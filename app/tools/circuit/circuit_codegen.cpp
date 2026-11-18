// circuit_codegen: ahead-of-time specialised kernels for the shipped circuit netlists (docs/CIRCUIT_ENGINE.md §10.44).
//
//   circuit_codegen <outdir> <CircuitUnit.cpp> [--shards S] [--hosts 44100,48000,88200,96000] [--sweep K] [--jobs J]
//                   [--only id,id] [--survey]
//
// Every unit bound in CircuitUnit.cpp's unitBindings() is compiled the way CircuitUnit::buildChannel compiles it (the
// internal rate and rate ladder of each host rate, LTE substep runtimes, heater hum both off and on where applicable,
// channel-0 readout probes and channel-1/CLI empty probes) at its catalogue defaults, at every switch / selector
// position and continuous control's ends, and at K seeded random knob settings, and reset (the pivot orders are chosen
// at reset, on the matrix values of those knobs). Every distinct sparse linear update structure
// (Circuit::codegenLinPatterns) and sparse nodal Newton structure (Circuit::codegenLuPatterns) becomes a straight-line
// kernel pair; <outdir>/circuit_kernels.cpp holds the sorted registries, circuit_kernels_<i>.cpp the kernels (S shards,
// compiled in parallel). --survey prints the structures per unit and writes nothing.
//
// The kernels are the interpreter's linPorts / linOut and spFactor / spStep (Engine.cpp, sparse paths) with the loops
// run out at generation time: the same floating-point operations on the same operands in the same order, in the same
// expression shapes. Since §10.66 they also leave out the interpreter's zero tests (an operation by an exact zero changes
// no value) and test the pivots without early returns, so each kernel is one branch-free block (CE_CG_OLD=1: as before).
#include "circuit/CircuitOversampler.hpp"
#include "circuit/Engine.hpp"
#include "circuit/library/EmbeddedLibrary.hpp"
#include "generated/ComponentCatalog.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace rig::ce;

// The generator runs on the interpreter: an empty kernel registry (CodegenStub.cpp's, linked here instead).
namespace rig::ce::cg {
const LinKernel* findLin(std::uint64_t, std::uint64_t) noexcept { return nullptr; }
int linKernelCount() noexcept { return 0; }
const LuKernel* findLu(std::uint64_t, std::uint64_t) noexcept { return nullptr; }
int luKernelCount() noexcept { return 0; }
const NewtonKernel* findNewton(std::uint64_t, std::uint64_t) noexcept { return nullptr; }
int newtonKernelCount() noexcept { return 0; }
} // namespace rig::ce::cg

namespace {

struct Binding {
    int table;
    std::string id;
};

std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

std::vector<Binding> bindings(const std::string& unitCpp) {
    const std::string src = slurp(unitCpp);
    std::vector<Binding> out;
    const auto a = src.find("unitBindings()");
    if (a == std::string::npos) return out;
    const auto b = src.find("return b;", a);
    const std::string body = src.substr(a, b - a);
    static const std::regex re(R"re(\{(\d+), "([^"]+)")re");
    std::set<std::pair<int, std::string>> seen;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), re); it != std::sregex_iterator(); ++it) {
        const int t = std::stoi((*it)[1].str());
        const std::string id = (*it)[2].str();
        if (seen.insert({t, id}).second) out.push_back({t, id});
    }
    return out;
}

const char* embedded(const std::string& id) {
    for (const auto& e : library::kEmbedded)
        if (id == e.id) return e.json;
    return nullptr;
}

// The catalogue unit's default control positions: quantizeFace=true matches CircuitUnit (constructor + controlFromParams);
// quantizeFace=false matches circuit_cli --table (tableControls).
std::vector<double> tableDefaults(const Netlist& n, int table, bool quantizeFace) {
    const rig::catalog::ComponentSchema* schema = nullptr;
    for (const auto& cs : rig::catalog::kAllComponents)
        if (cs.table_id == table) schema = &cs;
    std::vector<double> v;
    for (const auto& c : n.controls) {
        double x = c.def;
        bool matched = false;
        if (schema)
            for (const auto& pid : c.params) {
                const auto it = std::find_if(schema->params.begin(), schema->params.end(), [&](const auto& ps) { return ps.id == pid; });
                if (it == schema->params.end()) continue;
                x = std::clamp((static_cast<double>(static_cast<float>(it->default_value)) - c.paramMin) / (c.paramMax - c.paramMin), 0.0, 1.0);
                matched = true;
                break;
            }
        if (!matched && quantizeFace && c.face) {
            const double hi = c.options.size() > 2 ? static_cast<double>(c.options.size() - 1) : 1.0;
            const double raw = static_cast<double>(static_cast<float>(std::lround(c.def * hi)));
            x = std::clamp(raw / hi, 0.0, 1.0);
        }
        if (c.kind == "switch") x = x > 0.5 ? 1.0 : 0.0;
        v.push_back(x);
    }
    return v;
}

// Channel-0 readout probes exported by CircuitUnit::buildChannel (readouts.lfo and readouts.gr).
std::vector<std::string> readoutProbes(const char* text, const Netlist& n) {
    std::vector<std::string> probes;
    const nlohmann::json raw = nlohmann::json::parse(text, nullptr, false);
    if (!raw.is_object() || !raw.contains("readouts") || !raw["readouts"].is_object()) return probes;
    const nlohmann::json& ro = raw["readouts"];
    std::string lfo;
    if (ro.contains("lfo") && ro["lfo"].is_string()) lfo = ro["lfo"].get<std::string>();
    if (ro.contains("lfo") && ro["lfo"].is_object()) lfo = ro["lfo"].value("node", std::string());
    if (!lfo.empty()) probes.push_back(lfo);
    if (ro.contains("gr") && ro["gr"].is_object()) {
        const nlohmann::json& g = ro["gr"];
        const std::string law = g.value("law", std::string());
        bool okLaw = law == "ratio" || law == "vca";
        if (law == "table" && g.contains("table") && g["table"].is_array()) {
            int pts = 0;
            for (const auto& pt : g["table"])
                if (pt.is_array() && pt.size() == 2 && pt[0].is_number() && pt[1].is_number()) ++pts;
            okLaw = pts >= 2;
        }
        if (okLaw) {
            auto ground = [](const std::string& s) { return s.empty() || s == "0" || s == "gnd" || s == "GND" || s == "ground"; };
            std::string grNode, grRef;
            if (law == "vca") {
                const std::string part = g.value("part", std::string());
                for (const auto& p : n.parts) {
                    if (p.ref != part || p.type != "vca" || p.nodes.size() < 3) continue;
                    grNode = p.nodes[2];
                    grRef = p.nodes[1];
                }
            } else {
                if (g.contains("node") && g["node"].is_string()) grNode = g["node"].get<std::string>();
                if (g.contains("ref") && g["ref"].is_string()) grRef = g["ref"].get<std::string>();
            }
            if (!ground(grNode)) probes.push_back(grNode);
            if (!ground(grRef)) probes.push_back(grRef);
        }
    }
    return probes;
}

// CompileOptions::hum only adds heater-cathode leakage (rhk || chk) when the netlist has both a tube and a 45-65 Hz
// mains source (Engine.cpp). CompileOptions::noise and bbdNoise enter only through runtime bias / BbdBlock and never
// alter M, R, SparseLin, or SparseLu, so keeping them off skips the 1/f filter-bank fit during kernel generation.
bool hasMainsHum(const Netlist& n) {
    bool tube = false, mains = false;
    for (const auto& p : n.parts) {
        if (p.type == "triode" || p.type == "pentode") tube = true;
        else if (p.type == "V" && p.acAmp != 0.0 && p.acFreq > 45.0 && p.acFreq < 65.0) mains = true;
    }
    return tube && mains;
}

// CircuitUnit::onPrepare / buildChannel at a host rate (HQ, the default): internal rate and rate-ladder levels.
CompileOptions unitOptions(const Netlist& n, double host, bool withHum) {
    constexpr int kMaxOs = CircuitOversampler::kMaxFactor;
    int f = std::max(1, host >= 88000.0 ? n.oversampling / 2 : n.oversampling);
    const double minRate = n.minRate;
    if (minRate > 0)
        while (host * f < minRate - 1.0 && f < kMaxOs) f *= 2;
    CompileOptions o;
    o.sampleRate = host * f;
    o.multirate = false; // HQ
    o.settleMs = 0.0;    // the mains settle runs after the structures are fixed (reset): it does not change them
    o.noise = false;
    o.bbdNoise = false;
    o.hum = withHum;
    o.lteTol = kLiveLteTol;
    o.lteMaxPorts = kLiveLteMaxPorts;
    o.ladderBaseStage = 0;
    while ((1 << (o.ladderBaseStage + 1)) <= f) ++o.ladderBaseStage;
    for (const auto& [ref, rate] : n.partRates) {
        const double want = (o.sampleRate / f >= 88000.0 ? 0.5 : 1.0) * rate;
        int lv = 0;
        while ((f << (lv + 1)) <= want + 1e-9 && (f << (lv + 1)) <= (n.rateEvents ? 2 : 1) * kMaxOs) ++lv; // events: one level more (CircuitUnit, docs §10.62)
        if (lv > 0) o.partLevels.push_back({ref, lv});
    }
    return o;
}

struct FoundLin {
    cg::LinPattern p;
    std::vector<std::uint8_t> bakeable; // per R entry: ±1 at every occurrence
    std::set<std::string> where;
    int hits = 0;
};

struct FoundLu {
    cg::LuPattern p;
    std::set<std::string> where;
    int hits = 0;
};

struct FoundNewton {
    cg::NewtonPattern p;
    std::set<std::string> where;
    int hits = 0;
};

std::string hex(std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

// The cg::NewtonFn parameter list (Codegen.hpp).
#define NK_SIG                                                                                                                                         \
    "(const NlDevice* const* devs, int ndev, const int* bs, const int* bn, const double* __restrict K, const double* __restrict p, "                \
    "double* __restrict vpOut, double* __restrict curOut, const std::uint8_t* lim, const double* limA, const double* limB, const double* limC, "      \
    "int maxIt, double tol, int& itersOut, double& residualOut, const double* kcol, bool chordOn, nk::KaCache* kac, int nDyn, nk::KernelState* ks, " \
    "const double* dpPred, double* luScratch, nk::SparseLu* sp, const double* ptol) noexcept"

// Generation-time switches (docs §10.66). The kernels below are bit-identical either way (every value; the sign of an
// exact zero is the only freedom, and no output has shown one): CE_CG_OLD=1 emits every kernel as before §10.66 (stamp /
// injection loops, zero-multiplier tests, early pivot returns, the linear kernels' zero tests); CE_CG_LUNOZ=0..3 keeps (0)
// or drops (bit 1: the factor's updates, bit 2: the step's forward substitution) the LU's zero tests (default 3);
// CE_CG_LUPIVBR=1 keeps the factor's early returns; CE_CG_LINZB=1 keeps the linear kernels' zero tests.
const bool kCgOld = std::getenv("CE_CG_OLD") != nullptr;
const bool kLuStraight = !kCgOld;
const int kLuNoZeroBr = kCgOld ? 0 : std::getenv("CE_CG_LUNOZ") ? std::atoi(std::getenv("CE_CG_LUNOZ")) : 3;
// The factor's pivot tests: early returns or one flag returned at the end (a failed factorisation's values are never
// read: the caller falls back to the dense path), so the factor is one basic block.
const bool kLuPivBr = kCgOld || std::getenv("CE_CG_LUPIVBR") != nullptr;
const bool kLinNoZeroBr = !kCgOld && std::getenv("CE_CG_LINZB") == nullptr;

// ── emission ──
// SparseLin kernel pair: the interpreter's operations in its order (Engine.cpp: linPorts / linOut / spSolvePacked).
void emitLin(std::ostream& o, const FoundLin& f, const std::string& name) {
    const auto& p = f.p;
    const int n = p.n, sw = p.ns + p.nu;
    auto colTerms = [&](int c) {
        std::string s;
        for (int e = p.cOff[static_cast<std::size_t>(c)]; e < p.cOff[static_cast<std::size_t>(c) + 1]; ++e) {
            const int row = p.cRow[static_cast<std::size_t>(e)];
            const auto E = static_cast<std::size_t>(e);
            if (f.bakeable[E] && p.rVal[E] == 1.0) s += " b" + std::to_string(row) + " += wc;";
            else if (f.bakeable[E] && p.rVal[E] == -1.0) s += " b" + std::to_string(row) + " -= wc;";
            else s += " b" + std::to_string(row) + " += rv[" + std::to_string(e) + "] * wc;";
        }
        return s;
    };
    // rows of b a column has written so far in this function (kLinNoZeroBr: the forward substitution skips the others)
    std::vector<std::uint8_t> reach(static_cast<std::size_t>(n), 0);
    auto col = [&](int c) {
        if (p.cOff[static_cast<std::size_t>(c)] == p.cOff[static_cast<std::size_t>(c) + 1]) return std::string();
        for (int e = p.cOff[static_cast<std::size_t>(c)]; e < p.cOff[static_cast<std::size_t>(c) + 1]; ++e) reach[static_cast<std::size_t>(p.cRow[static_cast<std::size_t>(e)])] = 1;
        return "    { const double wc = w[" + std::to_string(c) + "]; " + (kLinNoZeroBr ? "{" : "if (wc != 0.0) {") + colTerms(c) + " } }\n";
    };
    auto decl = [&]() {
        std::string s = "    (void)lv; (void)uv; (void)rv; (void)inv; (void)w; (void)y; (void)bias; (void)bPart;\n    [[maybe_unused]] double";
        for (int i = 0; i < n; ++i) s += (i ? ", b" : " b") + std::to_string(i) + " = 0.0";
        return s + ";\n";
    };
    // rows the first sw columns write (bPart: the others stay exactly +0.0 in the interpreter's copy too)
    std::vector<std::uint8_t> touched(static_cast<std::size_t>(n), 0);
    for (int c = 0; c < sw; ++c)
        for (int e = p.cOff[static_cast<std::size_t>(c)]; e < p.cOff[static_cast<std::size_t>(c) + 1]; ++e) touched[static_cast<std::size_t>(p.cRow[static_cast<std::size_t>(e)])] = 1;
    auto forward = [&]() {
        std::string s;
        for (int k = 0; k < n; ++k) {
            const int e0 = p.lOff[static_cast<std::size_t>(k)], e1 = p.lOff[static_cast<std::size_t>(k) + 1];
            if (e0 == e1) continue;
            if (kLinNoZeroBr) {
                if (!reach[static_cast<std::size_t>(k)]) continue; // b_k is +0 here, as the zero test would find
                for (int e = e0; e < e1; ++e) reach[static_cast<std::size_t>(p.lIdx[static_cast<std::size_t>(e)])] = 1;
            }
            s += "    { const double bk = b" + std::to_string(k) + "; " + (kLinNoZeroBr ? "{" : "if (bk != 0.0) {");
            for (int e = e0; e < e1; ++e) s += " b" + std::to_string(p.lIdx[static_cast<std::size_t>(e)]) + " -= lv[" + std::to_string(e) + "] * bk;";
            s += " } }\n";
        }
        return s;
    };
    auto back = [&](const std::vector<int>& rows) {
        std::string s;
        for (int k : rows) {
            const std::string K = std::to_string(k);
            s += "    double v" + K + " = b" + K + ";";
            for (int e = p.uOff[static_cast<std::size_t>(k)]; e < p.uOff[static_cast<std::size_t>(k) + 1]; ++e)
                s += " v" + K + " -= uv[" + std::to_string(e) + "] * x" + std::to_string(p.uIdx[static_cast<std::size_t>(e)]) + ";";
            s += " const double x" + K + " = v" + K + " * inv[" + K + "];\n";
        }
        return s;
    };
    auto gather = [&](const std::vector<int>& A, const std::vector<int>& JA, const std::vector<int>& B, const std::vector<int>& JB) {
        std::string s;
        for (std::size_t k = 0; k < A.size(); ++k) {
            s += "    { double v = 0.0;";
            if (A[k] >= 0) s += " v += x" + std::to_string(A[k]) + ";";
            if (JA[k] >= 0) s += " v += w[" + std::to_string(p.ns + JA[k]) + "];";
            if (B[k] >= 0) s += " v -= x" + std::to_string(B[k]) + ";";
            if (JB[k] >= 0) s += " v -= w[" + std::to_string(p.ns + JB[k]) + "];";
            s += " y[" + std::to_string(k) + "] = bias ? v + bias[" + std::to_string(k) + "] : v; }\n";
        }
        return s;
    };
    const char* sig = "(const double* __restrict lv, const double* __restrict uv, const double* __restrict rv, const double* __restrict inv, "
                      "const double* __restrict w, double* __restrict y, const double* __restrict bias, double* __restrict bPart) noexcept";
    o << "// " << hex(p.h1) << hex(p.h2) << ": n " << n << ", ns " << p.ns << ", nu " << p.nu << ", nn " << p.nn << ", nuc " << p.nuc << ", ny " << p.ny
      << ", W " << p.W << ", L " << p.lIdx.size() << ", U " << p.uIdx.size() << ", R " << p.cRow.size() << "\n";
    for (const auto& w : f.where) o << "//   " << w << "\n";
    // linPorts: states and inputs, [bPart], cuts, solve over bPorts, port differences
    o << "void " << name << "_ports" << sig << " {\n" << decl();
    for (int c = 0; c < sw; ++c) o << col(c);
    o << "    if (bPart) {";
    for (int i = 0; i < n; ++i)
        if (touched[static_cast<std::size_t>(i)]) o << " bPart[" << i << "] = b" << i << ";";
    o << " }\n";
    for (int c = 0; c < p.nuc; ++c) o << col(sw + p.nn + c);
    o << forward() << back(p.bPorts) << gather(p.pA, p.pJA, p.pB, p.pJB) << "}\n";
    // linOut: from bPart (states and inputs already summed) or from zero, then the remaining columns, solve over bOut
    o << "void " << name << "_out" << sig << " {\n" << decl();
    reach = touched;
    o << "    if (bPart) {";
    for (int i = 0; i < n; ++i)
        if (touched[static_cast<std::size_t>(i)]) o << " b" << i << " = bPart[" << i << "];";
    o << " } else {\n";
    for (int c = 0; c < sw; ++c) o << "    " << col(c);
    o << "    }\n";
    for (int c = sw; c < p.W; ++c) o << col(c);
    o << forward() << back(p.bOut) << gather(p.oA, p.oJA, p.oB, p.oJB) << "}\n\n";
}

// SparseLu kernel pair: the interpreter's operations in its order (Engine.cpp: spFactor / spStep).
void emitLu(std::ostream& o, const FoundLu& f, const std::string& name) {
    const auto& p = f.p;
    const int n = p.n, nn = p.nn;
    const std::size_t nL = p.lIdx.size(), nU = p.uIdx.size(), nv = nL + nU + static_cast<std::size_t>(n);

    o << "// LU " << hex(p.h1) << hex(p.h2) << ": n " << n << ", nn " << nn << ", L " << nL << ", U " << nU << ", oQ " << p.oQ.size()
      << ", bRows " << p.bRows.size() << "\n";
    for (const auto& w : f.where) o << "//   " << w << "\n";

    // spFactor
    o << "bool " << name
      << "_factor(const double* __restrict snap, const double* __restrict cmaxS, const double (*__restrict Jd)[3], "
         "const int* __restrict live, int na, double* __restrict v, double* __restrict inv) noexcept {\n"
      << "    (void)cmaxS; (void)Jd; (void)live; (void)na; (void)inv;\n"
      << "    std::memcpy(v, snap, " << nv << "u * sizeof(double));\n";
    if (!kLuPivBr) o << "    bool bad = false;\n";
    bool anyStamp = false;
    std::vector<std::string> stampCase(static_cast<std::size_t>(nn));
    for (int a = 0; a < nn; ++a) {
        const int np = p.bn[static_cast<std::size_t>(a)];
        std::string s;
        for (int m = 0; m < np; ++m) {
            const auto k = static_cast<std::size_t>(a * 3 + m);
            const int e0 = p.stOff[k], e1 = p.stOff[k + 1];
            if (e0 == e1) continue;
            s += " { const double g = Jd[" + std::to_string(a) + "][" + std::to_string(m) + "]; if (g != 0.0) {";
            for (int e = e0; e < e1; ++e)
                s += " v[" + std::to_string(p.stPos[static_cast<std::size_t>(e)]) + (p.stSign[static_cast<std::size_t>(e)] > 0 ? "] += g;" : "] -= g;");
            s += " } }";
        }
        if (!s.empty()) {
            stampCase[static_cast<std::size_t>(a)] = std::move(s);
            anyStamp = true;
        }
    }
    if (anyStamp) {
        if (kLuStraight) {
            // straight-line, port by port in ascending order (the order of every live list) under a live flag: the
            // switch-in-a-loop form kept the right-hand side in loop-carried registers and took an indirect jump per port
            o << "    bool on[" << nn << "];\n    if (live) { for (int a = 0; a < " << nn << "; ++a) on[a] = false; for (int i = 0; i < na; ++i) on[live[i]] = true; }\n"
              << "    else { for (int a = 0; a < " << nn << "; ++a) on[a] = true; }\n";
            for (int a = 0; a < nn; ++a)
                if (!stampCase[static_cast<std::size_t>(a)].empty())
                    o << "    if (on[" << a << "]) {" << stampCase[static_cast<std::size_t>(a)] << " }\n";
        } else {
        o << "    { const int end = live ? na : " << nn << ";\n      for (int i = 0; i < end; ++i) {\n        switch (live ? live[i] : i) {\n";
        for (int a = 0; a < nn; ++a)
            if (!stampCase[static_cast<std::size_t>(a)].empty())
                o << "          case " << a << ":" << stampCase[static_cast<std::size_t>(a)] << " break;\n";
        o << "          default: break;\n        }\n      }\n    }\n";
        }
    }
    for (int k = 0; k < n; ++k) {
        const auto K = static_cast<std::size_t>(k);
        const std::uint8_t fl = p.kFl[K];
        const int d0 = p.dOff[K], d1 = p.dOff[K + 1];
        const int r0 = p.rOff[K], r1 = p.rOff[K + 1];
        if (fl == 0 && d0 == d1 && r0 == r1) continue;
        const std::size_t diag = nL + nU + K;
        if (fl & 2) {
            o << "    { const double pv = v[" << diag << "]; double cmax = std::max(std::abs(pv), cmaxS[" << k << "]);";
            for (int e = d0; e < d1; ++e) o << " cmax = std::max(cmax, std::abs(v[" << p.dIdx[static_cast<std::size_t>(e)] << "]));";
            if (kLuPivBr) o << " if (!(std::abs(pv) >= 1e-6 * cmax) || !(std::abs(pv) > 1e-300)) return false; }\n";
            else o << " bad |= !(std::abs(pv) >= 1e-6 * cmax) | !(std::abs(pv) > 1e-300); }\n";
        }
        if (fl & 1) {
            if (d0 < d1) {
                o << "    { const double iv = (inv[" << k << "] = 1.0 / v[" << diag << "]);";
                for (int e = d0; e < d1; ++e) o << " v[" << p.dIdx[static_cast<std::size_t>(e)] << "] *= iv;";
                o << " }\n";
            } else {
                o << "    inv[" << k << "] = 1.0 / v[" << diag << "];\n";
            }
        } else if (d0 < d1) {
            o << "    { const double iv = inv[" << k << "];";
            for (int e = d0; e < d1; ++e) o << " v[" << p.dIdx[static_cast<std::size_t>(e)] << "] *= iv;";
            o << " }\n";
        }
        for (int r = r0; r < r1; ++r) {
            const auto R = static_cast<std::size_t>(r);
            const int o0 = p.oOff[R], o1 = p.oOff[R + 1];
            if (o0 == o1) continue;
            o << "    { const double f = v[" << p.rE[R] << "]; " << (kLuNoZeroBr & 1 ? "{" : "if (f != 0.0) {");
            for (int q = o0; q < o1; ++q) {
                const auto Q = static_cast<std::size_t>(q);
                o << " v[" << p.oQ[Q] << "] -= f * v[" << p.oU[Q] << "];";
            }
            o << " } }\n";
        }
    }
    o << (kLuPivBr ? "    return true;\n}\n" : "    return !bad;\n}\n");

    // spStep
    o << "void " << name
      << "_step(const double* __restrict v, const double* __restrict inv, const double (*__restrict Jd)[3], "
         "const int* __restrict live, int na, const double* __restrict F, double* __restrict d) noexcept {\n"
      << "    (void)v; (void)inv; (void)Jd; (void)live; (void)na; (void)F; (void)d;\n"
      << "    double b[" << std::max(1, n) << "] = {};\n";
    bool anyInj = false;
    std::vector<std::string> injCase(static_cast<std::size_t>(nn));
    for (int a = 0; a < nn; ++a) {
        const auto A = static_cast<std::size_t>(a);
        const int pf = p.pFrom[A], pt = p.pTo[A];
        if (pf < 0 && pt < 0) continue;
        const int b0 = p.bs[A], np = p.bn[A];
        std::string s = " { double t = 0.0;";
        for (int m = 0; m < np; ++m) s += " t += Jd[" + std::to_string(a) + "][" + std::to_string(m) + "] * F[" + std::to_string(b0 + m) + "];";
        if (pf >= 0) s += " b[" + std::to_string(pf) + "] -= t;";
        if (pt >= 0) s += " b[" + std::to_string(pt) + "] += t;";
        s += " }";
        injCase[A] = std::move(s);
        anyInj = true;
    }
    if (anyInj) {
        if (kLuStraight) {
            o << "    bool on[" << nn << "];\n    if (live) { for (int a = 0; a < " << nn << "; ++a) on[a] = false; for (int i = 0; i < na; ++i) on[live[i]] = true; }\n"
              << "    else { for (int a = 0; a < " << nn << "; ++a) on[a] = true; }\n";
            for (int a = 0; a < nn; ++a)
                if (!injCase[static_cast<std::size_t>(a)].empty())
                    o << "    if (on[" << a << "]) {" << injCase[static_cast<std::size_t>(a)] << " }\n";
        } else {
        o << "    { const int end = live ? na : " << nn << ";\n      for (int i = 0; i < end; ++i) {\n        switch (live ? live[i] : i) {\n";
        for (int a = 0; a < nn; ++a)
            if (!injCase[static_cast<std::size_t>(a)].empty())
                o << "          case " << a << ":" << injCase[static_cast<std::size_t>(a)] << " break;\n";
        o << "          default: break;\n        }\n      }\n    }\n";
        }
    }
    // Rows of b that no injection reaches (structurally zero through the forward substitution): their columns are
    // skipped, as the zero test would skip them at run time (b starts at +0 and is never written there)
    std::vector<char> reach(static_cast<std::size_t>(n), 0);
    for (int a = 0; a < nn; ++a)
        if (!injCase[static_cast<std::size_t>(a)].empty()) {
            if (p.pFrom[static_cast<std::size_t>(a)] >= 0) reach[static_cast<std::size_t>(p.pFrom[static_cast<std::size_t>(a)])] = 1;
            if (p.pTo[static_cast<std::size_t>(a)] >= 0) reach[static_cast<std::size_t>(p.pTo[static_cast<std::size_t>(a)])] = 1;
        }
    for (int k = 0; k < n; ++k) {
        const int e0 = p.lOff[static_cast<std::size_t>(k)], e1 = p.lOff[static_cast<std::size_t>(k) + 1];
        if (e0 == e1) continue;
        if (kLuNoZeroBr & 2) {
            if (!reach[static_cast<std::size_t>(k)]) continue;
            for (int e = e0; e < e1; ++e) reach[static_cast<std::size_t>(p.lIdx[static_cast<std::size_t>(e)])] = 1;
        }
        o << "    { const double bk = b[" << k << "]; " << (kLuNoZeroBr & 2 ? "{" : "if (bk != 0.0) {");
        for (int e = e0; e < e1; ++e) o << " b[" << p.lIdx[static_cast<std::size_t>(e)] << "] -= v[" << e << "] * bk;";
        o << " } }\n";
    }
    for (int k : p.bRows) {
        const std::string K = std::to_string(k);
        o << "    double u" + K + " = b[" + K + "];";
        for (int e = p.uOff[static_cast<std::size_t>(k)]; e < p.uOff[static_cast<std::size_t>(k) + 1]; ++e)
            o << " u" << K << " -= v[" << (nL + static_cast<std::size_t>(e)) << "] * x" << p.uIdx[static_cast<std::size_t>(e)] << ";";
        o << " const double x" << K << " = u" << K << " * inv[" << k << "];\n";
    }
    for (int a = 0; a < nn; ++a) {
        const auto A = static_cast<std::size_t>(a);
        if (p.xP[A] < 0 && p.xN[A] < 0) {
            o << "    d[" << a << "] = F[" << a << "];\n";
        } else {
            o << "    { double t = F[" << a << "];";
            if (p.xP[A] >= 0) o << " t += x" << p.xP[A] << ";";
            if (p.xN[A] >= 0) o << " t -= x" << p.xN[A] << ";";
            o << " d[" << a << "] = t; }\n";
        }
    }
    o << "}\n\n";
}

// Newton kernel (docs §10.45): the group structure as compile-time constants, and the shared kernel template instantiated
// on it (nk::newtonKernel<NN, nk::StaticSpec<G>>, NewtonKernel.hpp). No arithmetic is emitted here: the kernel is the
// interpreter's source, so the operations and their order are the interpreter's by construction.
void emitNewton(std::ostream& o, const FoundNewton& f, const std::string& name) {
    const auto& p = f.p;
    const int n = p.n, nd = static_cast<int>(p.kind.size());
    const int NN = n <= 24 ? n : 0;
    auto list = [&](const std::vector<int>& v, const char* cast = nullptr) {
        std::string s;
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i) s += ", ";
            s += cast ? std::string(cast) + "(" + std::to_string(v[i]) + ")" : std::to_string(v[i]);
        }
        return s;
    };
    auto bools = [&](const std::vector<int>& v) {
        std::string s;
        for (std::size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + std::string(v[i] ? "true" : "false");
        return s;
    };
    bool anyA = false, anyB = false;
    for (int x : p.ccA) anyA |= x != 0;
    for (int x : p.ccB) anyB |= x != 0;
    std::vector<int> bn(static_cast<std::size_t>(n));
    for (int k = 0; k < n; ++k) bn[static_cast<std::size_t>(k)] = p.np[static_cast<std::size_t>(p.dev[static_cast<std::size_t>(k)])];
    o << "// NK " << hex(p.h1) << hex(p.h2) << ": n " << n << ", " << nd << " devices, NN " << NN << "\n";
    for (const auto& w : f.where) o << "//   " << w << "\n";
    o << "struct " << name << "_G {\n"
      << "    static constexpr int n = " << n << ", ndev = " << nd << ";\n"
      << "    static constexpr NlKind kind[" << nd << "] = {" << list(p.kind, "NlKind") << "};\n"
      << "    static constexpr int np[" << nd << "] = {" << list(p.np) << "};\n"
      << "    static constexpr int off[" << nd << "] = {" << list(p.off) << "};\n"
      << "    static constexpr bool shift[" << nd << "] = {" << bools(p.shift) << "};\n"
      << "    static constexpr std::uint8_t lim[" << n << "] = {" << list(p.lim) << "};\n"
      << "    static constexpr int bn[" << n << "] = {" << list(bn) << "};\n"
      << "    static constexpr int dev[" << n << "] = {" << list(p.dev) << "};\n"
      << "    static constexpr bool ccA[" << n << "] = {" << bools(p.ccA) << "};\n"
      << "    static constexpr bool ccB[" << n << "] = {" << bools(p.ccB) << "};\n"
      << "    static constexpr bool anyCcA = " << (anyA ? "true" : "false") << ", anyCcB = " << (anyB ? "true" : "false") << ";\n"
      << "};\n"
      << "bool " << name << NK_SIG << " {\n"
      << "    return nk::newtonKernel<" << NN << ", nk::StaticSpec<" << name << "_G>>(devs, ndev, bs, bn, K, p, vpOut, curOut, lim, limA, limB, limC, maxIt, tol, itersOut,\n"
      << "        residualOut, kcol, chordOn, kac, nDyn, ks, dpPred, luScratch, sp, ptol);\n}\n\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: circuit_codegen <outdir> <CircuitUnit.cpp> [--shards S] [--hosts a,b] [--sweep K] [--jobs J] [--only ids] [--survey]\n");
        return 2;
    }
    const std::string outDir = argv[1], unitCpp = argv[2];
    int shards = 16, sweep = 4, jobs = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    std::vector<double> hosts = {44100.0, 48000.0, 88200.0, 96000.0};
    std::set<std::string> only;
    bool survey = false;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        auto list = [&](const std::string& str) {
            std::vector<std::string> v;
            std::stringstream ss(str);
            std::string t;
            while (std::getline(ss, t, ',')) v.push_back(t);
            return v;
        };
        if (a == "--shards") shards = std::max(1, std::stoi(next()));
        else if (a == "--sweep") sweep = std::max(0, std::stoi(next()));
        else if (a == "--jobs") jobs = std::max(1, std::stoi(next()));
        else if (a == "--hosts") {
            hosts.clear();
            for (const auto& h : list(next())) hosts.push_back(std::stod(h));
        } else if (a == "--only") {
            for (const auto& x : list(next())) only.insert(x);
        } else if (a == "--survey") survey = true;
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    auto units = bindings(unitCpp);
    if (units.empty()) {
        std::fprintf(stderr, "no unit bindings in %s\n", unitCpp.c_str());
        return 1;
    }
    if (!only.empty())
        units.erase(std::remove_if(units.begin(), units.end(), [&](const Binding& b) { return !only.count(b.id) && !only.count(std::to_string(b.table)); }), units.end());

    struct BaseKey {
        long rate;
        bool hum;
        std::vector<std::pair<std::string, int>> levels;
        bool operator<(const BaseKey& o) const {
            if (rate != o.rate) return rate < o.rate;
            if (hum != o.hum) return hum < o.hum;
            return levels < o.levels;
        }
    };
    struct PreparedUnit {
        Binding u;
        Netlist n;
        std::vector<double> netDef;
        std::vector<std::vector<double>> configs;
        std::vector<std::pair<CompileOptions, std::string>> bases;
        std::vector<std::vector<std::string>> probeSets;
    };
    struct TaskItem {
        std::size_t ui, bi, pi;
    };

    const auto t0 = std::chrono::steady_clock::now();
    std::vector<PreparedUnit> prep;
    prep.reserve(units.size());
    std::vector<TaskItem> tasks;

    for (const auto& u : units) {
        const char* text = embedded(u.id);
        if (!text) continue;
        PreparedUnit pu;
        pu.u = u;
        std::string err;
        if (!parseNetlistText(text, pu.n, err)) {
            std::fprintf(stderr, "%s: %s\n", u.id.c_str(), err.c_str());
            continue;
        }
        const auto& n = pu.n;
        const rig::catalog::ComponentSchema* schema = nullptr;
        for (const auto& cs : rig::catalog::kAllComponents)
            if (cs.table_id == u.table) schema = &cs;

        for (const auto& c : n.controls) pu.netDef.push_back(c.kind == "switch" ? (c.def > 0.5 ? 1.0 : 0.0) : c.def);
        const std::vector<double> def = tableDefaults(n, u.table, true);
        const std::vector<double> cliDef = tableDefaults(n, u.table, false);
        auto addCfg = [&](const std::vector<double>& v) {
            if (std::find(pu.configs.begin(), pu.configs.end(), v) == pu.configs.end()) pu.configs.push_back(v);
        };
        addCfg(def);
        addCfg(cliDef);
        addCfg(pu.netDef);

        // Group controls by their CircuitUnit parameter binding so ganged controls (e.g. stereo input/TC selectors
        // sharing one parameter id) move together, and unexposed internal trimmers stay at their factory default.
        std::vector<std::string> paramKey(n.controls.size());
        std::vector<bool> exposed(n.controls.size(), false);
        std::map<std::string, std::vector<std::size_t>> paramGroups;
        std::vector<std::string> groupOrder;
        for (std::size_t c = 0; c < n.controls.size(); ++c) {
            const auto& ctl = n.controls[c];
            if (ctl.legacyOnly) continue;
            std::string key = ctl.id;
            bool matched = false;
            if (schema) {
                for (const auto& pid : ctl.params)
                    if (std::any_of(schema->params.begin(), schema->params.end(), [&](const auto& ps) { return ps.id == pid; })) {
                        key = pid;
                        matched = true;
                        break;
                    }
            }
            const bool faceExtra = ctl.face && (ctl.kind == "switch" || ctl.kind == "selector");
            paramKey[c] = key;
            exposed[c] = matched || faceExtra || !schema;
            if (!exposed[c]) continue;
            if (!paramGroups.count(key)) groupOrder.push_back(key);
            paramGroups[key].push_back(c);
        }

        for (const auto& key : groupOrder) {
            const auto& idxs = paramGroups[key];
            const auto& first = n.controls[idxs[0]];
            if (first.kind == "selector" && first.options.size() > 2) {
                const std::size_t nOpt = first.options.size();
                for (std::size_t j = 0; j < nOpt; ++j) {
                    auto v = def;
                    for (std::size_t c : idxs) v[c] = static_cast<double>(j) / static_cast<double>(nOpt - 1);
                    addCfg(v);
                }
            } else {
                for (double x : {0.0, 1.0}) {
                    auto v = def;
                    for (std::size_t c : idxs) v[c] = x;
                    addCfg(v);
                }
            }
        }

        std::mt19937_64 rng(0x5eed0000ull + static_cast<std::uint64_t>(u.table));
        for (int k = 0; k < sweep; ++k) {
            auto v = (k % 2 == 1 && pu.netDef != def) ? pu.netDef : def;
            for (const auto& key : groupOrder) {
                const auto& idxs = paramGroups[key];
                if (n.controls[idxs[0]].kind != "continuous") continue;
                const double val = std::uniform_real_distribution<double>(0.0, 1.0)(rng);
                for (std::size_t c : idxs) v[c] = val;
            }
            addCfg(v);
        }

        pu.probeSets = {{}};
        const std::vector<std::string> ch0Probes = readoutProbes(text, n);
        if (!ch0Probes.empty()) pu.probeSets.push_back(ch0Probes);

        std::set<BaseKey> seenBase;
        const std::vector<bool> humModes = hasMainsHum(n) ? std::vector<bool>{false, true} : std::vector<bool>{false};
        for (double host : hosts) {
            for (bool withHum : humModes) {
                CompileOptions base = unitOptions(n, host, withHum);
                const BaseKey bk{std::lround(base.sampleRate), base.hum, base.partLevels};
                if (!seenBase.insert(bk).second) continue;
                pu.bases.emplace_back(std::move(base), "host " + std::to_string(static_cast<int>(host)) + (withHum ? " +hum" : ""));
            }
        }

        const std::size_t ui = prep.size();
        for (std::size_t bi = 0; bi < pu.bases.size(); ++bi)
            for (std::size_t pi = 0; pi < pu.probeSets.size(); ++pi)
                tasks.push_back({ui, bi, pi});
        prep.push_back(std::move(pu));
    }

    struct UnitAcc {
        std::mutex mu;
        std::map<std::pair<std::uint64_t, std::uint64_t>, FoundLin> lin;
        std::map<std::pair<std::uint64_t, std::uint64_t>, FoundLu> lu;
        std::map<std::pair<std::uint64_t, std::uint64_t>, FoundNewton> nk;
        std::map<std::string, std::set<std::string>> perRuntime;
        int resets = 0, compiles = 0, failed = 0;
    };
    std::vector<std::unique_ptr<UnitAcc>> accs(prep.size());
    for (auto& a : accs) a = std::make_unique<UnitAcc>();

    std::atomic<std::size_t> nextTask{0};
    auto work = [&]() {
        for (;;) {
            const std::size_t ti = nextTask.fetch_add(1);
            if (ti >= tasks.size()) return;
            const auto [ui, bi, pi] = tasks[ti];
            const auto& pu = prep[ui];
            const auto& [base, baseTag] = pu.bases[bi];
            const auto& pr = pu.probeSets[pi];

            std::map<std::pair<std::uint64_t, std::uint64_t>, FoundLin> localLin;
            std::map<std::pair<std::uint64_t, std::uint64_t>, FoundLu> localLu;
            std::map<std::pair<std::uint64_t, std::uint64_t>, FoundNewton> localNk;
            std::map<std::string, std::set<std::string>> localRt;
            int resets = 0, compiles = 0, failed = 0;

            struct CachedCkt {
                std::unique_ptr<Circuit> c;
                std::vector<double> lastCtl;
            };
            std::vector<CachedCkt> cache;

            for (const auto& cfg : pu.configs) {
                CachedCkt* hit = nullptr;
                for (auto& entry : cache)
                    if (!entry.c->topologyStale(&cfg)) {
                        hit = &entry;
                        break;
                    }
                if (!hit) {
                    CompileOptions o = base;
                    o.probes = pr;
                    o.controls = cfg;
                    auto c = std::make_unique<Circuit>();
                    std::string err;
                    ++compiles;
                    if (!c->compile(pu.n, o, err)) {
                        ++failed;
                        continue;
                    }
                    cache.push_back({std::move(c), pu.netDef});
                    hit = &cache.back();
                    if (cfg != pu.netDef) {
                        for (std::size_t i = 0; i < cfg.size(); ++i) hit->c->setControlImmediate(static_cast<int>(i), cfg[i]);
                        hit->c->reset();
                        hit->lastCtl = cfg;
                    }
                } else if (cfg != hit->lastCtl) {
                    for (std::size_t i = 0; i < cfg.size(); ++i) hit->c->setControlImmediate(static_cast<int>(i), cfg[i]);
                    hit->c->reset();
                    hit->lastCtl = cfg;
                }
                ++resets;
                const std::string suffix = ", " + baseTag + (pr.empty() ? "" : " +ro");
                for (auto& p : hit->c->codegenLinPatterns()) {
                    const auto key = std::make_pair(p.h1, p.h2);
                    localRt["lin " + p.where.substr(0, p.where.find(", T"))].insert(hex(p.h1));
                    auto it = localLin.find(key);
                    const std::string where = pu.u.id + " (" + std::to_string(pu.u.table) + ") " + p.where + suffix;
                    if (it == localLin.end()) {
                        FoundLin f;
                        f.bakeable.resize(p.rVal.size());
                        for (std::size_t e = 0; e < p.rVal.size(); ++e) f.bakeable[e] = p.rVal[e] == 1.0 || p.rVal[e] == -1.0;
                        f.p = std::move(p);
                        f.where.insert(where);
                        f.hits = 1;
                        localLin.emplace(key, std::move(f));
                    } else {
                        auto& f = it->second;
                        for (std::size_t e = 0; e < p.rVal.size(); ++e) f.bakeable[e] = f.bakeable[e] && p.rVal[e] == f.p.rVal[e];
                        if (f.where.size() < 6) f.where.insert(where);
                        ++f.hits;
                    }
                }
                for (auto& p : hit->c->codegenNewtonPatterns()) {
                    const auto key = std::make_pair(p.h1, p.h2);
                    localRt["nk " + p.where.substr(0, p.where.find(", T"))].insert(hex(p.h1));
                    auto it = localNk.find(key);
                    const std::string where = pu.u.id + " (" + std::to_string(pu.u.table) + ") " + p.where + suffix;
                    if (it == localNk.end()) {
                        FoundNewton f;
                        f.p = std::move(p);
                        f.where.insert(where);
                        f.hits = 1;
                        localNk.emplace(key, std::move(f));
                    } else {
                        if (it->second.where.size() < 6) it->second.where.insert(where);
                        ++it->second.hits;
                    }
                }
                for (auto& p : hit->c->codegenLuPatterns()) {
                    const auto key = std::make_pair(p.h1, p.h2);
                    localRt["lu " + p.where.substr(0, p.where.find(", T"))].insert(hex(p.h1));
                    auto it = localLu.find(key);
                    const std::string where = pu.u.id + " (" + std::to_string(pu.u.table) + ") " + p.where + suffix;
                    if (it == localLu.end()) {
                        FoundLu f;
                        f.p = std::move(p);
                        f.where.insert(where);
                        f.hits = 1;
                        localLu.emplace(key, std::move(f));
                    } else {
                        auto& f = it->second;
                        if (f.where.size() < 6) f.where.insert(where);
                        ++f.hits;
                    }
                }
            }

            auto& acc = *accs[ui];
            std::lock_guard<std::mutex> lk(acc.mu);
            acc.resets += resets;
            acc.compiles += compiles;
            acc.failed += failed;
            for (const auto& [rt, sigs] : localRt) acc.perRuntime[rt].insert(sigs.begin(), sigs.end());
            for (auto& [key, f] : localLin) {
                auto it = acc.lin.find(key);
                if (it == acc.lin.end()) acc.lin.emplace(key, std::move(f));
                else {
                    auto& g = it->second;
                    for (std::size_t e = 0; e < g.bakeable.size(); ++e) g.bakeable[e] = g.bakeable[e] && f.bakeable[e] && f.p.rVal[e] == g.p.rVal[e];
                    for (const auto& w : f.where)
                        if (g.where.size() < 6) g.where.insert(w);
                    g.hits += f.hits;
                }
            }
            for (auto& [key, f] : localNk) {
                auto it = acc.nk.find(key);
                if (it == acc.nk.end()) acc.nk.emplace(key, std::move(f));
                else {
                    for (const auto& w : f.where)
                        if (it->second.where.size() < 6) it->second.where.insert(w);
                    it->second.hits += f.hits;
                }
            }
            for (auto& [key, f] : localLu) {
                auto it = acc.lu.find(key);
                if (it == acc.lu.end()) acc.lu.emplace(key, std::move(f));
                else {
                    auto& g = it->second;
                    for (const auto& w : f.where)
                        if (g.where.size() < 6) g.where.insert(w);
                    g.hits += f.hits;
                }
            }
        }
    };

    std::vector<std::thread> pool;
    for (int j = 0; j < jobs; ++j) pool.emplace_back(work);
    for (auto& t : pool) t.join();

    std::map<std::pair<std::uint64_t, std::uint64_t>, FoundLin> foundLin;
    std::map<std::pair<std::uint64_t, std::uint64_t>, FoundLu> foundLu;
    std::map<std::pair<std::uint64_t, std::uint64_t>, FoundNewton> foundNk;
    std::map<std::string, std::string> report;
    for (std::size_t ui = 0; ui < prep.size(); ++ui) {
        const auto& pu = prep[ui];
        auto& acc = *accs[ui];
        if (!acc.lin.empty() || !acc.lu.empty() || !acc.nk.empty() || acc.failed > 0) {
            std::ostringstream rep;
            rep << pu.u.id << " (" << pu.u.table << "): " << acc.resets << " resets (" << acc.compiles << " compiles, " << acc.failed << " failed), "
                << acc.lin.size() << " lin + " << acc.lu.size() << " lu + " << acc.nk.size() << " newton structures";
            for (const auto& [rt, sigs] : acc.perRuntime) rep << "; " << rt << ": " << sigs.size();
            report[pu.u.id + "#" + std::to_string(pu.u.table)] = rep.str();
        }
        for (auto& [key, f] : acc.lin) {
            auto it = foundLin.find(key);
            if (it == foundLin.end()) foundLin.emplace(key, std::move(f));
            else {
                auto& g = it->second;
                for (std::size_t e = 0; e < g.bakeable.size(); ++e) g.bakeable[e] = g.bakeable[e] && f.bakeable[e] && f.p.rVal[e] == g.p.rVal[e];
                for (const auto& w : f.where)
                    if (g.where.size() < 8) g.where.insert(w);
                g.hits += f.hits;
            }
        }
        for (auto& [key, f] : acc.nk) {
            auto it = foundNk.find(key);
            if (it == foundNk.end()) foundNk.emplace(key, std::move(f));
            else {
                for (const auto& w : f.where)
                    if (it->second.where.size() < 8) it->second.where.insert(w);
                it->second.hits += f.hits;
            }
        }
        for (auto& [key, f] : acc.lu) {
            auto it = foundLu.find(key);
            if (it == foundLu.end()) foundLu.emplace(key, std::move(f));
            else {
                auto& g = it->second;
                for (const auto& w : f.where)
                    if (g.where.size() < 8) g.where.insert(w);
                g.hits += f.hits;
            }
        }
    }

    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (survey) {
        for (const auto& [k, r] : report) std::fprintf(stderr, "%s\n", r.c_str());
    }
    std::fprintf(stderr, "circuit_codegen: %zu units, %zu sparse-lin + %zu sparse-lu + %zu newton structures, %.1f s\n", prep.size(), foundLin.size(), foundLu.size(),
                 foundNk.size(), secs);
    if (survey) return 0;

    // kernels, sharded round-robin across S shards; registries sorted by (h1, h2) for binary search
    std::vector<std::ostringstream> sh(static_cast<std::size_t>(shards));
    for (auto& shOut : sh)
        shOut << "// GENERATED by tools/circuit/circuit_codegen (docs/CIRCUIT_ENGINE.md §10.44). Do not edit.\n"
                 "#include \"circuit/Codegen.hpp\"\n#include \"circuit/NewtonKernel.hpp\"\n#include <algorithm>\n#include <cmath>\n#include <cstring>\n\nnamespace rig::ce::cg {\n\n";
    std::ostringstream reg;
    reg << "// GENERATED by tools/circuit/circuit_codegen (docs/CIRCUIT_ENGINE.md §10.44). Do not edit.\n"
           "#include \"circuit/Codegen.hpp\"\n#include <algorithm>\n#include <utility>\n\nnamespace rig::ce::cg {\n\n";
    std::ostringstream tableLin, tableLu, tableNk;
    int kLin = 0, kLu = 0, kNk = 0, shardIdx = 0;
    for (const auto& [key, f] : foundLin) {
        const std::string name = "lin" + std::to_string(kLin);
        auto& shOut = sh[static_cast<std::size_t>(shardIdx++ % shards)];
        emitLin(shOut, f, name);
        const char* sig = "(const double* __restrict, const double* __restrict, const double* __restrict, const double* __restrict, const double* __restrict, "
                          "double* __restrict, const double* __restrict, double* __restrict) noexcept;\n";
        reg << "void " << name << "_ports" << sig << "void " << name << "_out" << sig;
        std::vector<int> bi;
        std::vector<double> bv;
        for (std::size_t e = 0; e < f.bakeable.size(); ++e)
            if (f.bakeable[e]) bi.push_back(static_cast<int>(e)), bv.push_back(f.p.rVal[e]);
        reg << "static const int " << name << "_bi[] = {";
        for (std::size_t i = 0; i < bi.size(); ++i) reg << (i ? ", " : "") << bi[i];
        reg << (bi.empty() ? "0" : "") << "};\nstatic const double " << name << "_bv[] = {";
        for (std::size_t i = 0; i < bv.size(); ++i) reg << (i ? ", " : "") << (bv[i] > 0 ? "1.0" : "-1.0");
        reg << (bv.empty() ? "0.0" : "") << "};\n";
        tableLin << "    {0x" << hex(key.first) << "ull, 0x" << hex(key.second) << "ull, &" << name << "_ports, &" << name << "_out, " << bi.size() << ", " << name
                 << "_bi, " << name << "_bv},\n";
        ++kLin;
    }
    for (const auto& [key, f] : foundLu) {
        const std::string name = "lu" + std::to_string(kLu);
        auto& shOut = sh[static_cast<std::size_t>(shardIdx++ % shards)];
        emitLu(shOut, f, name);
        reg << "bool " << name
            << "_factor(const double* __restrict, const double* __restrict, const double (*__restrict)[3], const int* __restrict, int, double* __restrict, "
               "double* __restrict) noexcept;\n"
            << "void " << name
            << "_step(const double* __restrict, const double* __restrict, const double (*__restrict)[3], const int* __restrict, int, const double* __restrict, "
               "double* __restrict) noexcept;\n";
        tableLu << "    {0x" << hex(key.first) << "ull, 0x" << hex(key.second) << "ull, &" << name << "_factor, &" << name << "_step},\n";
        ++kLu;
    }
    for (const auto& [key, f] : foundNk) {
        const std::string name = "nk" + std::to_string(kNk);
        auto& shOut = sh[static_cast<std::size_t>(shardIdx++ % shards)];
        emitNewton(shOut, f, name);
        reg << "bool " << name << NK_SIG << ";\n";
        tableNk << "    {0x" << hex(key.first) << "ull, 0x" << hex(key.second) << "ull, &" << name << "},\n";
        ++kNk;
    }
    reg << "\nstatic const LinKernel kLin[] = {\n" << tableLin.str() << (kLin == 0 ? "    {0, 0, nullptr, nullptr, 0, nullptr, nullptr},\n" : "") << "};\n"
        << "const LinKernel* findLin(std::uint64_t h1, std::uint64_t h2) noexcept {\n"
        << "    const auto* end = kLin + " << kLin << ";\n"
        << "    const auto* it = std::lower_bound(kLin, end, std::pair<std::uint64_t, std::uint64_t>{h1, h2},\n"
        << "        [](const LinKernel& e, const std::pair<std::uint64_t, std::uint64_t>& q) { return e.h1 < q.first || (e.h1 == q.first && e.h2 < q.second); });\n"
        << "    return it != end && it->ports && it->h1 == h1 && it->h2 == h2 ? it : nullptr;\n}\n"
        << "int linKernelCount() noexcept { return " << kLin << "; }\n\n"
        << "static const LuKernel kLu[] = {\n" << tableLu.str() << (kLu == 0 ? "    {0, 0, nullptr, nullptr},\n" : "") << "};\n"
        << "const LuKernel* findLu(std::uint64_t h1, std::uint64_t h2) noexcept {\n"
        << "    const auto* end = kLu + " << kLu << ";\n"
        << "    const auto* it = std::lower_bound(kLu, end, std::pair<std::uint64_t, std::uint64_t>{h1, h2},\n"
        << "        [](const LuKernel& e, const std::pair<std::uint64_t, std::uint64_t>& q) { return e.h1 < q.first || (e.h1 == q.first && e.h2 < q.second); });\n"
        << "    return it != end && it->factor && it->h1 == h1 && it->h2 == h2 ? it : nullptr;\n}\n"
        << "int luKernelCount() noexcept { return " << kLu << "; }\n\n"
        << "static const NewtonKernel kNk[] = {\n" << tableNk.str() << (kNk == 0 ? "    {0, 0, nullptr},\n" : "") << "};\n"
        << "const NewtonKernel* findNewton(std::uint64_t h1, std::uint64_t h2) noexcept {\n"
        << "    const auto* end = kNk + " << kNk << ";\n"
        << "    const auto* it = std::lower_bound(kNk, end, std::pair<std::uint64_t, std::uint64_t>{h1, h2},\n"
        << "        [](const NewtonKernel& e, const std::pair<std::uint64_t, std::uint64_t>& q) { return e.h1 < q.first || (e.h1 == q.first && e.h2 < q.second); });\n"
        << "    return it != end && it->fn && it->h1 == h1 && it->h2 == h2 ? it : nullptr;\n}\n"
        << "int newtonKernelCount() noexcept { return " << kNk << "; }\n\n} // namespace rig::ce::cg\n";
    auto write = [&](const std::string& path, const std::string& text) {
        if (slurp(path) == text) return true;
        std::ofstream f(path, std::ios::binary);
        f << text;
        return static_cast<bool>(f);
    };
    bool ok = write(outDir + "/circuit_kernels.cpp", reg.str());
    for (int i = 0; i < shards; ++i) {
        sh[static_cast<std::size_t>(i)] << "} // namespace rig::ce::cg\n";
        ok = write(outDir + "/circuit_kernels_" + std::to_string(i) + ".cpp", sh[static_cast<std::size_t>(i)].str()) && ok;
    }
    if (!ok) {
        std::fprintf(stderr, "circuit_codegen: cannot write to %s\n", outDir.c_str());
        return 1;
    }
    return 0;
}
