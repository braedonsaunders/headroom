#include "Wdf.hpp"

#include "Engine.hpp"

#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <set>

namespace rig::ce {

bool WdfGroup::eligible(const Circuit& c, const Group& g) {
    if (g.nn > 1 || !g.cutIn.empty()) return false;
    if (g.nn == 1) {
        const auto& d = c.devices_[static_cast<std::size_t>(g.devices[0])];
        if (d.kind != NlKind::Diode && d.kind != NlKind::DiodePair) return false;
    } else {
        return false; // linear groups: the state-space form is equivalent and cheaper; WDF only on request
    }
    for (int pi : g.prims)
        if (c.prims_[static_cast<std::size_t>(pi)].be || c.prims_[static_cast<std::size_t>(pi)].kind == Prim::Winding) return false; // trapezoidal leaves; no branch unknowns
    return true;
}

bool WdfGroup::build(const Circuit& c, const Group& g, std::string& error) {
    const int N = static_cast<int>(c.nodeNames_.size());
    tree_.clear();
    ports_.clear();
    post_.clear();
    exports_.clear();
    ctrlPrims_.clear();
    std::set<int> pinned = {0};
    inOf_.assign(static_cast<std::size_t>(N), -1);
    nu_ = static_cast<int>(g.inputs.size());
    for (int j = 0; j < nu_; ++j) {
        pinned.insert(g.inputs[static_cast<std::size_t>(j)]);
        inOf_[static_cast<std::size_t>(g.inputs[static_cast<std::size_t>(j)])] = j;
    }
    struct Edge {
        int x, y, t;
        bool alive;
    };
    std::vector<Edge> edges;
    std::vector<std::pair<int, int>> leafEnds; // leaf tree index -> (x, y)
    leafOfState_.assign(g.states.size(), -1);
    leafSign_.assign(g.states.size(), 1.0);
    std::map<int, int> leafOfPrim;
    for (int pi : g.prims) {
        const auto& p = c.prims_[static_cast<std::size_t>(pi)];
        if (p.kind == Prim::G || p.kind == Prim::Cap || p.kind == Prim::Ind) {
            if (p.a == p.b) continue;
            TNode t;
            t.kind = TNode::Leaf;
            t.prim = pi;
            t.elem = p.kind == Prim::G ? 0 : (p.kind == Prim::Cap ? 1 : 2);
            tree_.push_back(t);
            const int ti = static_cast<int>(tree_.size()) - 1;
            leafOfPrim[pi] = ti;
            edges.push_back({p.a, p.b, ti, true});
        } else {
            ctrlPrims_.push_back(pi);
            for (int nd : {p.a, p.b, p.c, p.d}) pinned.insert(nd);
        }
    }
    for (std::size_t k = 0; k < g.states.size(); ++k) leafOfState_[k] = leafOfPrim.at(g.states[k]);
    for (std::size_t t = 0; t < tree_.size(); ++t) leafEnds.push_back({edges[t].x, edges[t].y});
    int rx = -1, ry = -1;
    if (g.nn == 1) {
        rootDevice_ = g.devices[0];
        const auto& d = c.devices_[static_cast<std::size_t>(rootDevice_)];
        rx = d.port[0].from;
        ry = d.port[0].to;
        is_ = d.p[0];
        nvt_ = d.p[1];
        pair_ = d.kind == NlKind::DiodePair;
        gShift_ = d.gShift[0]; // the linear split's conductance, stamped as a leaf across the root: the law gives it back
        gmin_ = d.kind == NlKind::Diode ? kGmin * (1.0 - d.p[3]) : kGmin;
        if (d.port[0].cp != rx || d.port[0].cn != ry) {
            error = "WDF root must be a two-terminal device";
            return false;
        }
    }
    // Series-parallel reduction.
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t i = 0; i < edges.size() && !changed; ++i) {
            if (!edges[i].alive) continue;
            for (std::size_t j = i + 1; j < edges.size(); ++j) {
                if (!edges[j].alive) continue;
                const bool same = edges[i].x == edges[j].x && edges[i].y == edges[j].y;
                const bool rev = edges[i].x == edges[j].y && edges[i].y == edges[j].x;
                if (!same && !rev) continue;
                TNode t;
                t.kind = TNode::Parallel;
                t.c1 = edges[i].t;
                t.c2 = edges[j].t;
                t.p1 = 1;
                t.p2 = same ? 1 : -1;
                tree_.push_back(t);
                edges[i].t = static_cast<int>(tree_.size()) - 1;
                edges[j].alive = false;
                changed = true;
                break;
            }
        }
        if (changed) continue;
        std::map<int, std::vector<int>> inc;
        for (std::size_t i = 0; i < edges.size(); ++i)
            if (edges[i].alive) {
                inc[edges[i].x].push_back(static_cast<int>(i));
                inc[edges[i].y].push_back(static_cast<int>(i));
            }
        for (auto& [nd, list] : inc) {
            if (pinned.count(nd) || nd == rx || nd == ry || list.size() != 2) continue;
            auto& e1 = edges[static_cast<std::size_t>(list[0])];
            auto& e2 = edges[static_cast<std::size_t>(list[1])];
            const int p = e1.x == nd ? e1.y : e1.x;
            const int q = e2.x == nd ? e2.y : e2.x;
            if (p == nd || q == nd) continue;
            TNode t;
            t.kind = TNode::Series;
            t.c1 = e1.t;
            t.c2 = e2.t;
            t.p1 = (e1.x == p && e1.y == nd) ? 1 : -1; // composite p→q = (p→nd) + (nd→q)
            t.p2 = (e2.x == nd && e2.y == q) ? 1 : -1;
            tree_.push_back(t);
            e1 = {p, q, static_cast<int>(tree_.size()) - 1, true};
            e2.alive = false;
            changed = true;
            break;
        }
    }
    // Post-order of every subtree.
    std::vector<char> isChild(tree_.size(), 0);
    for (const auto& t : tree_)
        if (t.kind != TNode::Leaf) {
            isChild[static_cast<std::size_t>(t.c1)] = 1;
            isChild[static_cast<std::size_t>(t.c2)] = 1;
        }
    std::function<void(int)> visit = [&](int t) {
        const auto& n = tree_[static_cast<std::size_t>(t)];
        if (n.kind != TNode::Leaf) {
            visit(n.c1);
            visit(n.c2);
        }
        post_.push_back(t);
    };
    for (const auto& e : edges)
        if (e.alive) {
            visit(e.t);
            ports_.push_back({e.t, e.x, e.y});
        }
    if (g.nn == 1) {
        rootPort_ = static_cast<int>(ports_.size());
        ports_.push_back({-1, rx, ry});
    } else {
        rootPort_ = -1;
    }
    // Core nodes.
    std::set<int> core;
    auto addCore = [&](int nd) {
        if (nd > 0 && inOf_[static_cast<std::size_t>(nd)] < 0) core.insert(nd);
    };
    for (const auto& p : ports_) {
        addCore(p.x);
        addCore(p.y);
    }
    for (int pi : ctrlPrims_) {
        const auto& p = c.prims_[static_cast<std::size_t>(pi)];
        for (int nd : {p.a, p.b, p.c, p.d}) addCore(nd);
    }
    coreNodes_.assign(core.begin(), core.end());
    coreOf_.assign(static_cast<std::size_t>(N), -1);
    for (std::size_t i = 0; i < coreNodes_.size(); ++i) coreOf_[static_cast<std::size_t>(coreNodes_[i])] = static_cast<int>(i);
    // Export paths through leaves to a core / known node.
    std::map<int, std::vector<std::pair<int, int>>> adj; // node -> (leaf, other)
    for (std::size_t t = 0; t < leafEnds.size(); ++t) {
        adj[leafEnds[t].first].push_back({static_cast<int>(t), leafEnds[t].second});
        adj[leafEnds[t].second].push_back({static_cast<int>(t), leafEnds[t].first});
    }
    for (int e : g.exports) {
        ExportPath path;
        if (coreOf_[static_cast<std::size_t>(e)] >= 0) {
            path.startCore = coreOf_[static_cast<std::size_t>(e)];
            exports_.push_back(path);
            continue;
        }
        std::map<int, std::pair<int, int>> parent; // node -> (leaf, prev node)
        std::deque<int> q{e};
        parent[e] = {-1, -1};
        int found = -1;
        while (!q.empty() && found < 0) {
            const int m = q.front();
            q.pop_front();
            for (auto [leaf, other] : adj[m]) {
                if (parent.count(other)) continue;
                parent[other] = {leaf, m};
                if (other == 0 || coreOf_[static_cast<std::size_t>(other)] >= 0 || inOf_[static_cast<std::size_t>(other)] >= 0) {
                    found = other;
                    break;
                }
                q.push_back(other);
            }
        }
        if (found < 0) {
            error = "WDF: no path to node " + c.nodeNames_[static_cast<std::size_t>(e)];
            return false;
        }
        if (found > 0) {
            if (coreOf_[static_cast<std::size_t>(found)] >= 0) path.startCore = coreOf_[static_cast<std::size_t>(found)];
            else path.startInput = inOf_[static_cast<std::size_t>(found)];
        }
        // walk back from `found` to e: V[closer] = V[farther] ± v_leaf
        for (int m = found; m != e;) {
            const auto [leaf, prev] = parent[m];
            // step from m (known) to prev (closer to e)
            const auto [lx, ly] = leafEnds[static_cast<std::size_t>(leaf)];
            path.steps.push_back({leaf, prev == lx ? 1.0 : -1.0});
            m = prev;
        }
        exports_.push_back(path);
    }
    const auto nc = coreNodes_.size(), np = ports_.size();
    M_.assign(nc * nc, 0.0);
    rhs_.assign(nc * (np + static_cast<std::size_t>(nu_) + 1), 0.0);
    X_.assign(rhs_.size(), 0.0);
    piv_.assign(std::max<std::size_t>(nc, 1), 0);
    S_.assign(np * np, 0.0);
    Su_.assign(np * static_cast<std::size_t>(nu_), 0.0);
    Qn_.assign(nc * np, 0.0);
    Qnu_.assign(nc * static_cast<std::size_t>(nu_), 0.0);
    aPort_.assign(np, 0.0);
    bPort_.assign(np, 0.0);
    return true;
}

void WdfGroup::refresh(const Circuit& c, const Group& g) noexcept {
    const double T = g.T > 0 ? g.T : c.T_;
    for (int t : post_) {
        auto& n = tree_[static_cast<std::size_t>(t)];
        if (n.kind == TNode::Leaf) {
            const auto& p = c.prims_[static_cast<std::size_t>(n.prim)];
            // slot < 0: a fixed internal element (Prim::fixed), e.g. the 1 GΩ linear split compile stamps across a
            // junction on a DC-floating node. Reading slots_[-1] here left the core singular (refresh gave up, S = 0)
            // and the group silent: the MT-2 clipper's `clip` read 0 V (2026-11-11).
            const double v = p.slot >= 0 ? c.slots_[static_cast<std::size_t>(p.slot)].value : p.fixed;
            const double R = n.elem == 0 ? v : (n.elem == 1 ? T / (2.0 * v) : 2.0 * v / T);
            if (n.elem != 0 && n.R != R && n.R > 0) n.z += (R - n.R) * n.iLast; // keep branch voltage continuous
            n.R = R;
        } else {
            const auto& a = tree_[static_cast<std::size_t>(n.c1)];
            const auto& b = tree_[static_cast<std::size_t>(n.c2)];
            n.R = n.kind == TNode::Series ? a.R + b.R : a.R * b.R / (a.R + b.R);
        }
        n.G = 1.0 / n.R;
    }
    const int nc = static_cast<int>(coreNodes_.size()), np = static_cast<int>(ports_.size());
    const int C = np + nu_ + 1; // columns: ports, inputs, test current
    auto kclRow = [&](int nd) {
        if (nd <= 0) return -1;
        const int k = coreOf_[static_cast<std::size_t>(nd)];
        if (k < 0) return -1;
        return c.nodeKind_[static_cast<std::size_t>(nd)] == 3 ? k : -1;
    };
    auto stampAll = [&](bool withRoot) {
        std::fill(M_.begin(), M_.end(), 0.0);
        std::fill(rhs_.begin(), rhs_.end(), 0.0);
        auto addV = [&](int rw, int nd, double coef) {
            if (rw < 0 || nd <= 0) return;
            const int u = coreOf_[static_cast<std::size_t>(nd)];
            if (u >= 0) M_[static_cast<std::size_t>(rw * nc + u)] += coef;
            else if (inOf_[static_cast<std::size_t>(nd)] >= 0) rhs_[static_cast<std::size_t>(rw * C + np + inOf_[static_cast<std::size_t>(nd)])] -= coef;
        };
        for (int k = 0; k < np; ++k) {
            if (k == rootPort_ && !withRoot) continue;
            const auto& p = ports_[static_cast<std::size_t>(k)];
            const double G = p.tree >= 0 ? tree_[static_cast<std::size_t>(p.tree)].G : 1.0 / Rroot_;
            const int ra = kclRow(p.x), rb = kclRow(p.y);
            addV(ra, p.x, G);
            addV(ra, p.y, -G);
            addV(rb, p.y, G);
            addV(rb, p.x, -G);
            // branch current G(v − a) leaving x: the source term a·G goes to the RHS of x
            if (ra >= 0) rhs_[static_cast<std::size_t>(ra * C + k)] += G;
            if (rb >= 0) rhs_[static_cast<std::size_t>(rb * C + k)] -= G;
        }
        for (int pi : ctrlPrims_) {
            const auto& p = c.prims_[static_cast<std::size_t>(pi)];
            switch (p.kind) {
                case Prim::Vcvs: {
                    const int rw = coreOf_[static_cast<std::size_t>(p.a)];
                    addV(rw, p.a, 1.0);
                    addV(rw, p.c, -p.fixed);
                    addV(rw, p.d, p.fixed);
                    break;
                }
                case Prim::Nullor: {
                    const int rw = coreOf_[static_cast<std::size_t>(p.a)];
                    addV(rw, p.c, 1.0);
                    addV(rw, p.d, -1.0);
                    break;
                }
                case Prim::Vccs: {
                    const int ra = kclRow(p.a), rb = kclRow(p.b);
                    addV(ra, p.c, p.fixed);
                    addV(ra, p.d, -p.fixed);
                    addV(rb, p.c, -p.fixed);
                    addV(rb, p.d, p.fixed);
                    break;
                }
                default: break;
            }
        }
    };
    auto solve = [&]() {
        std::copy(rhs_.begin(), rhs_.end(), X_.begin());
        if (nc == 0) return true;
        if (!la::luFactor(M_.data(), nc, piv_.data())) return false;
        std::vector<double>* none = nullptr;
        (void)none;
        for (int col = 0; col < C; ++col) {
            // strided solve on column col
            for (int k = 0; k < nc; ++k) {
                const int p = piv_[static_cast<std::size_t>(k)];
                if (p != k) std::swap(X_[static_cast<std::size_t>(k * C + col)], X_[static_cast<std::size_t>(p * C + col)]);
            }
            for (int i = 1; i < nc; ++i) {
                double s = X_[static_cast<std::size_t>(i * C + col)];
                for (int k = 0; k < i; ++k) s -= M_[static_cast<std::size_t>(i * nc + k)] * X_[static_cast<std::size_t>(k * C + col)];
                X_[static_cast<std::size_t>(i * C + col)] = s;
            }
            for (int i = nc - 1; i >= 0; --i) {
                double s = X_[static_cast<std::size_t>(i * C + col)];
                for (int k = i + 1; k < nc; ++k) s -= M_[static_cast<std::size_t>(i * nc + k)] * X_[static_cast<std::size_t>(k * C + col)];
                X_[static_cast<std::size_t>(i * C + col)] = s / M_[static_cast<std::size_t>(i * nc + i)];
            }
        }
        return true;
    };
    auto nodeV = [&](int nd, int col) -> double {
        if (nd <= 0) return 0.0;
        const int k = coreOf_[static_cast<std::size_t>(nd)];
        if (k >= 0) return X_[static_cast<std::size_t>(k * C + col)];
        const int j = inOf_[static_cast<std::size_t>(nd)];
        return (j >= 0 && col == np + j) ? 1.0 : 0.0;
    };
    if (rootPort_ >= 0) {
        // Thévenin resistance seen by the root: other ports as their resistances, sources off, 1 A test.
        stampAll(false);
        const auto& rp = ports_[static_cast<std::size_t>(rootPort_)];
        const int rx = kclRow(rp.x), ry = kclRow(rp.y);
        for (auto& v : rhs_) v = 0.0;
        if (rx >= 0) rhs_[static_cast<std::size_t>(rx * C + C - 1)] += 1.0;
        if (ry >= 0) rhs_[static_cast<std::size_t>(ry * C + C - 1)] -= 1.0;
        if (solve()) {
            const double rth = nodeV(rp.x, C - 1) - nodeV(rp.y, C - 1);
            if (std::isfinite(rth) && rth > 1e-9) Rroot_ = rth;
        }
        logIsRonNvt_ = std::log(is_ * Rroot_ / nvt_);
        invNvt_ = 1.0 / nvt_;
    }
    stampAll(true);
    if (!solve()) return;
    for (int k = 0; k < np; ++k) {
        const auto& p = ports_[static_cast<std::size_t>(k)];
        for (int m = 0; m < np; ++m) {
            const double q = nodeV(p.x, m) - nodeV(p.y, m);
            S_[static_cast<std::size_t>(k * np + m)] = 2.0 * q - (k == m ? 1.0 : 0.0);
        }
        for (int j = 0; j < nu_; ++j) Su_[static_cast<std::size_t>(k * nu_ + j)] = 2.0 * (nodeV(p.x, np + j) - nodeV(p.y, np + j));
    }
    for (int i = 0; i < nc; ++i) {
        for (int m = 0; m < np; ++m) Qn_[static_cast<std::size_t>(i * np + m)] = X_[static_cast<std::size_t>(i * C + m)];
        for (int j = 0; j < nu_; ++j) Qnu_[static_cast<std::size_t>(i * nu_ + j)] = X_[static_cast<std::size_t>(i * C + np + j)];
    }
}

void WdfGroup::process(Circuit& c, const Group& g, double* w, double* out) noexcept {
    const int ns = static_cast<int>(g.states.size());
    const double* u = w + ns;
    // Up: reflected waves of every subtree.
    for (int t : post_) {
        auto& n = tree_[static_cast<std::size_t>(t)];
        switch (n.kind) {
            case TNode::Leaf: n.b = n.elem == 0 ? 0.0 : (n.elem == 1 ? n.z : -n.z); break;
            case TNode::Series: {
                const auto& a = tree_[static_cast<std::size_t>(n.c1)];
                const auto& b = tree_[static_cast<std::size_t>(n.c2)];
                n.b = n.p1 * a.b + n.p2 * b.b;
                break;
            }
            case TNode::Parallel: {
                const auto& a = tree_[static_cast<std::size_t>(n.c1)];
                const auto& b = tree_[static_cast<std::size_t>(n.c2)];
                n.b = (n.p1 * a.b * a.G + n.p2 * b.b * b.G) * n.R;
                break;
            }
        }
    }
    const int np = static_cast<int>(ports_.size());
    for (int k = 0; k < np; ++k)
        if (ports_[static_cast<std::size_t>(k)].tree >= 0) aPort_[static_cast<std::size_t>(k)] = tree_[static_cast<std::size_t>(ports_[static_cast<std::size_t>(k)].tree)].b;
    auto scatter = [&](int k) {
        double s = 0;
        const double* Sr = &S_[static_cast<std::size_t>(k * np)];
        for (int m = 0; m < np; ++m) s += Sr[m] * aPort_[static_cast<std::size_t>(m)];
        const double* Su = nu_ ? &Su_[static_cast<std::size_t>(k * nu_)] : nullptr;
        for (int j = 0; j < nu_; ++j) s += Su[j] * u[j];
        return s;
    };
    double iRoot = 0;
    if (rootPort_ >= 0) {
        aPort_[static_cast<std::size_t>(rootPort_)] = 0.0; // S_rr = 0: the root's own wave does not reflect back
        const double aEl = scatter(rootPort_);
        const double R = Rroot_;
        // Root: v + R·i(v) = aEl. Warm start from the previous sample (junction-limited Newton, usually 1-2
        // steps); the Wright-omega closed form seeds it whenever the warm start is far off (large jumps).
        auto f = [&](double v, double& df) {
            const double x = (pair_ ? std::abs(v) : v) * invNvt_;
            double d1;
            const double e1 = limexp(x, d1);
            if (pair_) {
                const double e2 = x < 40.0 ? 1.0 / e1 : 0.0;
                df = is_ * invNvt_ * (d1 + e2) + kGmin - gShift_;
                return (v < 0 ? -1.0 : 1.0) * is_ * (e1 - e2) + (kGmin - gShift_) * v;
            }
            df = is_ * invNvt_ * d1 + gmin_ - gShift_;
            return is_ * (e1 - 1.0) + (gmin_ - gShift_) * v;
        };
        auto omegaSeed = [&]() {
            const double sgn = pair_ && aEl < 0 ? -1.0 : 1.0;
            const double A = pair_ ? std::abs(aEl) : aEl;
            const double x = logIsRonNvt_ + (A + is_ * R) * invNvt_;
            const double i0 = nvt_ / R * wrightOmega(x) - is_;
            return sgn * (A - R * i0);
        };
        double v = vRoot_ + (vRoot_ - vRoot2_);
        bool ok = false;
        for (int pass = 0; pass < 2 && !ok; ++pass) {
            if (pass == 1) v = omegaSeed();
            for (int it = 0; it < 8; ++it) {
                double df;
                const double fi = f(v, df);
                const double step = (v + R * fi - aEl) / (1.0 + R * df);
                if (pass == 0 && std::abs(step) > 0.25) break; // far from the solution: reseed
                v -= step;
                if (std::abs(step) < 1e-9 * (1.0 + std::abs(v))) {
                    ok = true;
                    break;
                }
            }
        }
        vRoot2_ = vRoot_;
        iRoot = (aEl - v) / R; // consistent with the final v
        vRoot_ = v;
        aPort_[static_cast<std::size_t>(rootPort_)] = 2.0 * v - aEl; // element reflection = adaptor incident
    }
    for (int k = 0; k < np; ++k) bPort_[static_cast<std::size_t>(k)] = scatter(k);
    for (int k = 0; k < np; ++k)
        if (ports_[static_cast<std::size_t>(k)].tree >= 0) tree_[static_cast<std::size_t>(ports_[static_cast<std::size_t>(k)].tree)].a = bPort_[static_cast<std::size_t>(k)];
    // Down: incident waves to every child; leaves update their states.
    for (auto it = post_.rbegin(); it != post_.rend(); ++it) {
        auto& n = tree_[static_cast<std::size_t>(*it)];
        switch (n.kind) {
            case TNode::Leaf:
                n.iLast = (n.a - n.b) / (2.0 * n.R);
                if (n.elem != 0) n.z = n.a;
                break;
            case TNode::Series: {
                auto& a = tree_[static_cast<std::size_t>(n.c1)];
                auto& b = tree_[static_cast<std::size_t>(n.c2)];
                const double i = (n.a - n.b) / (2.0 * n.R);
                a.a = a.b + 2.0 * n.p1 * a.R * i;
                b.a = b.b + 2.0 * n.p2 * b.R * i;
                break;
            }
            case TNode::Parallel: {
                auto& a = tree_[static_cast<std::size_t>(n.c1)];
                auto& b = tree_[static_cast<std::size_t>(n.c2)];
                const double v = 0.5 * (n.a + n.b);
                a.a = 2.0 * n.p1 * v - a.b;
                b.a = 2.0 * n.p2 * v - b.b;
                break;
            }
        }
    }
    if (g.nn == 1) w[ns + nu_] = iRoot;
    // Exports.
    for (std::size_t e = 0; e < exports_.size(); ++e) {
        const auto& p = exports_[e];
        double v = 0;
        if (p.startCore >= 0) {
            const double* q = &Qn_[static_cast<std::size_t>(p.startCore * np)];
            for (int m = 0; m < np; ++m) v += q[m] * aPort_[static_cast<std::size_t>(m)];
            const double* qu = nu_ ? &Qnu_[static_cast<std::size_t>(p.startCore * nu_)] : nullptr;
            for (int j = 0; j < nu_; ++j) v += qu[j] * u[j];
        } else if (p.startInput >= 0) {
            v = u[p.startInput];
        }
        for (const auto& s : p.steps) {
            const auto& l = tree_[static_cast<std::size_t>(s.leaf)];
            v += s.sign * 0.5 * (l.a + l.b);
        }
        out[e] = v;
    }
    (void)c;
}

void WdfGroup::resetFrom(const Circuit& c, const Group& g, const double* s, const double* vc) noexcept {
    for (std::size_t k = 0; k < g.states.size(); ++k) {
        auto& n = tree_[static_cast<std::size_t>(leafOfState_[k])];
        const auto& p = c.prims_[static_cast<std::size_t>(g.states[k])];
        n.z = s[k] * n.R;
        n.iLast = p.kind == Prim::Cap ? vc[k] / n.R - s[k] : vc[k] / n.R + s[k];
    }
}

void WdfGroup::stateTo(const Circuit& c, const Group& g, double* s, double* vc) const noexcept {
    for (std::size_t k = 0; k < g.states.size(); ++k) {
        const auto& n = tree_[static_cast<std::size_t>(leafOfState_[k])];
        const auto& p = c.prims_[static_cast<std::size_t>(g.states[k])];
        s[k] = n.z / n.R;
        vc[k] = (p.kind == Prim::Cap ? n.iLast + s[k] : n.iLast - s[k]) * n.R;
    }
}

void WdfGroup::adoptFrom(const WdfGroup& o) noexcept {
    if (o.tree_.size() != tree_.size()) return;
    for (std::size_t i = 0; i < tree_.size(); ++i) {
        tree_[i].z = o.tree_[i].z;
        tree_[i].iLast = o.tree_[i].iLast;
    }
}

} // namespace rig::ce
