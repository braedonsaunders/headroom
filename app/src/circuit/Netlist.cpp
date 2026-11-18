#include "Netlist.hpp"
#include "Logic.hpp"
#include <map>

#include "ModelLibrary.hpp"
#include "library/EmbeddedLibrary.hpp"

#include <algorithm>
#include <complex>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>

namespace rig::ce {

using json = nlohmann::json;

// ───────────────────────────── values ─────────────────────────────

double Taper::operator()(double x) const noexcept {
    x = std::clamp(x, 0.0, 1.0);
    auto logLaw = [](double t, double m) {
        m = std::clamp(m, 0.01, 0.49);
        const double y = 1.0 / m - 1.0; // (b^0.5 - 1)/(b - 1) = m  ->  b = y^2
        const double b = y * y;
        return (std::pow(b, t) - 1.0) / (b - 1.0);
    };
    switch (kind) {
        case 'A': return logLaw(x, mid);
        case 'C': return 1.0 - logLaw(1.0 - x, mid);
        case 'W': return x < 0.5 ? 0.5 * logLaw(2.0 * x, mid) : 1.0 - 0.5 * logLaw(2.0 * (1.0 - x), mid);
        default: return x;
    }
}

std::optional<double> parseValue(std::string_view text, bool spice) {
    std::string s;
    for (char c : text)
        if (!std::isspace(static_cast<unsigned char>(c))) s.push_back(c);
    // Strip unit words / symbols (UTF-8 Ω and µ handled below).
    auto stripSuffix = [&](std::string_view suf) {
        if (s.size() > suf.size()) {
            std::string tail = s.substr(s.size() - suf.size());
            std::string lower = tail;
            for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lower == suf) s.resize(s.size() - suf.size());
        }
    };
    {
        const std::string ohm = "\xCE\xA9"; // Ω
        if (s.size() >= ohm.size() && s.compare(s.size() - ohm.size(), ohm.size(), ohm) == 0) s.resize(s.size() - ohm.size());
        const std::string micro = "\xC2\xB5"; // µ
        for (std::size_t p; (p = s.find(micro)) != std::string::npos;) s.replace(p, micro.size(), "u");
        const std::string micro2 = "\xCE\xBC"; // μ
        for (std::size_t p; (p = s.find(micro2)) != std::string::npos;) s.replace(p, micro2.size(), "u");
    }
    stripSuffix("ohms");
    stripSuffix("ohm");
    if (s.empty()) return std::nullopt;
    // Leading number.
    std::size_t i = 0;
    auto isNum = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '+'; };
    while (i < s.size() && (isNum(s[i]) || ((s[i] == 'e' || s[i] == 'E') && i > 0 && i + 1 < s.size() &&
                                            (std::isdigit(static_cast<unsigned char>(s[i + 1])) || s[i + 1] == '-' || s[i + 1] == '+'))))
        ++i;
    if (i == 0) return std::nullopt;
    double base = 0.0;
    try {
        std::size_t used = 0;
        base = std::stod(s.substr(0, i), &used);
        if (used != i) return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
    std::string rest = s.substr(i);
    if (rest.empty()) return base;
    std::string lower = rest;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    double mult = 1.0;
    std::size_t used = 1;
    if (lower.rfind("meg", 0) == 0) {
        mult = 1e6;
        used = 3;
    } else {
        switch (rest[0]) {
            case 'T': case 't': mult = 1e12; break;
            case 'G': case 'g': mult = 1e9; break;
            case 'M': mult = spice ? 1e-3 : 1e6; break;
            case 'm': mult = 1e-3; break;
            case 'k': case 'K': mult = 1e3; break;
            case 'u': case 'U': mult = 1e-6; break;
            case 'n': case 'N': mult = 1e-9; break;
            case 'p': case 'P': mult = 1e-12; break;
            case 'f': mult = 1e-15; break;
            case 'F': mult = spice ? 1e-15 : 1.0; break; // "F" alone = farad in electronics notation
            case 'R': case 'r': mult = 1.0; break;
            case 'V': case 'v': case 'H': case 'h': case 'A': case 'a': case 'S': case 's': mult = 1.0; used = rest.size(); break;
            default: return std::nullopt;
        }
    }
    std::string tail = rest.substr(std::min(used, rest.size()));
    // "4k7" style: digits after the multiplier are the fraction.
    if (!tail.empty() && std::isdigit(static_cast<unsigned char>(tail[0]))) {
        std::size_t j = 0;
        while (j < tail.size() && std::isdigit(static_cast<unsigned char>(tail[j]))) ++j;
        const double frac = std::stod("0." + tail.substr(0, j));
        if (base != std::floor(base)) return std::nullopt;
        base += (base < 0 ? -frac : frac);
        tail = tail.substr(j);
    }
    // Remaining must be a unit letter only.
    if (!tail.empty()) {
        std::string t = tail;
        for (auto& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (t != "f" && t != "h" && t != "v" && t != "a" && t != "s" && t != "hz") return std::nullopt;
    }
    return base * mult;
}

std::string formatValue(double v, std::string_view unit) {
    struct P { double m; const char* s; };
    static const P ps[] = {{1e9, "G"}, {1e6, "M"}, {1e3, "k"}, {1, ""}, {1e-3, "m"}, {1e-6, "µ"}, {1e-9, "n"}, {1e-12, "p"}};
    const double a = std::abs(v);
    char buf[64];
    if (a == 0.0) {
        std::snprintf(buf, sizeof buf, "0 %.*s", static_cast<int>(unit.size()), unit.data());
        return buf;
    }
    for (const auto& p : ps) {
        if (a >= p.m * 0.9999) {
            const double x = v / p.m;
            const int digits = std::abs(x) >= 100 ? 0 : (std::abs(x) >= 10 ? 1 : 2);
            std::snprintf(buf, sizeof buf, "%.*f", digits, x);
            std::string s = buf;
            if (s.find('.') != std::string::npos) {
                while (!s.empty() && s.back() == '0') s.pop_back();
                if (!s.empty() && s.back() == '.') s.pop_back();
            }
            return s + " " + p.s + std::string(unit);
        }
    }
    std::snprintf(buf, sizeof buf, "%.3g %.*s", v, static_cast<int>(unit.size()), unit.data());
    return buf;
}

// ───────────────────────────── netlist accessors ─────────────────────────────

const ControlDef* Netlist::control(std::string_view cid) const {
    for (const auto& c : controls)
        if (c.id == cid) return &c;
    return nullptr;
}
int Netlist::controlIndex(std::string_view cid) const {
    for (std::size_t i = 0; i < controls.size(); ++i)
        if (controls[i].id == cid) return static_cast<int>(i);
    return -1;
}
const Part* Netlist::part(std::string_view r) const {
    for (const auto& p : parts)
        if (p.ref == r) return &p;
    return nullptr;
}
Part* Netlist::part(std::string_view r) {
    for (auto& p : parts)
        if (p.ref == r) return &p;
    return nullptr;
}

double controlPosition(const Netlist& n, const std::string& id, const std::vector<double>& ctl) {
    const int k = n.controlIndex(id);
    if (k < 0) return 0.5;
    if (static_cast<std::size_t>(k) < ctl.size()) return std::clamp(ctl[static_cast<std::size_t>(k)], 0.0, 1.0);
    return std::clamp(n.controls[static_cast<std::size_t>(k)].def, 0.0, 1.0);
}

double valueAt(const Netlist& n, const ValueSpec& v, const std::vector<double>& ctl) {
    if (!v.variable()) return v.nominal;
    double x = controlPosition(n, v.control, ctl);
    if (v.invert) x = 1.0 - x;
    if (v.law == "lin") return v.min + (v.max - v.min) * x;
    return v.min * std::pow(v.max / v.min, x);
}

void potSegments(const Netlist& n, const Part& p, const std::vector<double>& ctl, double& lower, double& upper) {
    const double total = valueAt(n, p.value, ctl);
    double x = p.control.empty() ? 0.5 : controlPosition(n, p.control, ctl);
    if (p.invert) x = 1.0 - x;
    if (p.spanLo > 0.0 || p.spanHi < 1.0) x = std::clamp((x - p.spanLo) / (p.spanHi - p.spanLo), 0.0, 1.0);
    const double t = p.taper(x);
    // optional end resistance: rend at each end, the rest of the track swept by the taper (total pin-to-pin unchanged)
    const double re = std::clamp(p.rend, 0.0, 0.5 * total), span = total - 2.0 * re;
    lower = std::max(kPotEndOhms, re + span * t);
    upper = std::max(kPotEndOhms, re + span * (1.0 - t));
}

bool switchClosed(const Netlist& n, const Part& p, const std::vector<double>& ctl) {
    const double x = controlPosition(n, p.control, ctl);
    bool on = x >= 0.5;
    if (!p.positions.empty()) {
        int idx = 0;
        if (const ControlDef* cd = n.control(p.control)) {
            if (cd->options.size() > 1)
                idx = static_cast<int>(std::lround(x * static_cast<double>(cd->options.size() - 1)));
            else if (cd->paramMax > cd->paramMin)
                idx = static_cast<int>(std::lround(cd->paramMin + x * (cd->paramMax - cd->paramMin)));
            else
                idx = static_cast<int>(std::lround(x));
        } else {
            idx = static_cast<int>(std::lround(x));
        }
        on = std::find(p.positions.begin(), p.positions.end(), idx) != p.positions.end();
    }
    return p.invert ? !on : on;
}

bool netlistEndsInCab(const Netlist& n) {
    bool any = false;
    for (const auto& p : n.parts) {
        if (p.type != "speaker" || p.nodes.size() < 3) continue;
        any = true;
        for (const auto& nd : p.nodes) // the output is a speaker pin: a plain load (terminal volts) or the bare cone
            if (nd == n.io.output || (!n.io.outputR.empty() && nd == n.io.outputR)) return false;
    }
    return any;
}

int partArity(const std::string& t, int given) {
    if (t == "R" || t == "C" || t == "L" || t == "V" || t == "switch" || t == "D" || t == "Dpair") return 2;
    if (t == "pot" || t == "npn" || t == "pnp" || t == "triode" || t == "njf" || t == "pjf") return 3;
    if (t == "speaker") return 3;  // electro-mechanical loudspeaker: [+, -, vel] (vel = cone velocity node, m/s)
    if (t == "pentode" || t == "ldr" || t == "spring" || t == "bbd") return 4;
    if (t == "lamp" || t == "led" || t == "neon" || t == "cds" || t == "elpanel") return 2;
    if (t == "ota") return 6;
    if (t == "absi") return 4;     // NE570 rectifier law: sense+, sense-, inject into (from 0) — nodes [vin, vref, crect, "0"]
    if (t == "gaincell") return 6; // NE570 ΔG law: [gin, vref, r5a, r5b, inv, "0"]
    if (t == "vca") return 8;      // Blackmer VCA, THAT 2180 SIP pin order: [in, ec+, ec-, sym, v-, gnd, v+, out]
    if (t == "rms") return 8;      // RMS detector, THAT 2252 SIP pin order: [in, ibias, gnd, sym, v-, cap, out, v+]
    if (t == "dff") return 8;      // CMOS 4013 section: [d, clk, set, reset, q, qb, vdd, vss]
    if (t == "jkff") return 9;
    if (t == "ad536") return 14;   // AD536A, D-14 pin order: [vin, nc, -vs, cav, db, bufout, bufin, iout, rl, com, nc, nc, nc, +vs]     // CMOS 4027 section: [j, k, clk, set, reset, q, qb, vdd, vss]
    if (t == "nand") return 5;     // CMOS 4011 gate: [a, b, y, vdd, vss]
    if (t == "inv") return 4;      // CMOS 4069 / 4049 inverter: [a, y, vdd, vss]
    if (t == "tape") return 5;
    if (t == "transformer") return (given >= 4 && given % 2 == 0) ? given : 4;
    if (t == "opamp") return (given == 5) ? 5 : 3;
    if (t == "vcvs" || t == "vccs") return 4;
    return 0; // "sub" never reaches here: subcircuits are expanded first
}

// ───────────────────────────── JSON ─────────────────────────────

namespace {

bool readNumber(const json& v, double& out, std::string& error, const std::string& where) {
    if (v.is_number()) {
        out = v.get<double>();
        return std::isfinite(out);
    }
    if (v.is_string()) {
        if (auto x = parseValue(v.get<std::string>())) {
            out = *x;
            return true;
        }
    }
    error = where + ": not a number or engineering value: " + v.dump();
    return false;
}

bool readValue(const json& v, ValueSpec& out, std::string& error, const std::string& where) {
    if (v.is_object()) {
        if (!v.contains("nominal")) {
            error = where + ": value object needs \"nominal\"";
            return false;
        }
        if (!readNumber(v["nominal"], out.nominal, error, where + ".nominal")) return false;
        if (v.contains("control")) {
            out.control = v["control"].get<std::string>();
            if (!v.contains("min") || !v.contains("max")) {
                error = where + ": a controlled value needs min and max";
                return false;
            }
            if (!readNumber(v["min"], out.min, error, where + ".min") || !readNumber(v["max"], out.max, error, where + ".max"))
                return false;
            out.law = v.value("law", std::string("log"));
            out.invert = v.value("invert", false);
            if (out.law != "lin" && out.law != "log") {
                error = where + ": law must be lin or log";
                return false;
            }
            if (out.law == "log" && (out.min <= 0 || out.max <= 0)) {
                error = where + ": log law needs positive min/max";
                return false;
            }
        }
        return true;
    }
    return readNumber(v, out.nominal, error, where);
}

bool readTaper(const json& v, Taper& t, std::string& error, const std::string& where) {
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        if (s == "A" || s == "log" || s == "audio") t = {'A', 0.10};
        else if (s == "B" || s == "lin" || s == "linear") t = {'B', 0.5};
        else if (s == "C" || s == "rlog") t = {'C', 0.10};
        else if (s == "W") t = {'W', 0.10};
        else {
            error = where + ": unknown taper " + s;
            return false;
        }
        return true;
    }
    if (v.is_object()) {
        const std::string k = v.value("type", std::string("A"));
        t.kind = k.empty() ? 'A' : k[0];
        t.mid = v.value("mid", 0.10);
        return true;
    }
    error = where + ": bad taper";
    return false;
}

json valueToJson(const ValueSpec& v) {
    if (!v.variable()) return v.nominal;
    json j = {{"nominal", v.nominal}, {"control", v.control}, {"min", v.min}, {"max", v.max}, {"law", v.law}};
    if (v.invert) j["invert"] = true;
    return j;
}

bool parseModelJson(const std::string& name, const json& m, DeviceModel& d, std::string& error) {
    d.name = name;
    if (!m.is_object() || !m.contains("kind")) {
        error = "model " + name + ": needs an object with \"kind\"";
        return false;
    }
    d.kind = m["kind"].get<std::string>();
    const auto& defs = modelDefaults(d.kind);
    if (defs.empty()) {
        error = "model " + name + ": unknown kind " + d.kind;
        return false;
    }
    for (auto it = m.begin(); it != m.end(); ++it) {
        if (it.key() == "kind") continue;
        if (it.key() == "note") {
            d.note = it.value().get<std::string>();
            continue;
        }
        if (it.key() == "level" && it.value().is_string()) {
            d.p["level"] = it.value().get<std::string>() == "ideal" ? 0.0 : 1.0;
            continue;
        }
        if (!defs.count(it.key())) {
            error = "model " + name + ": unknown parameter " + it.key() + " for kind " + d.kind;
            return false;
        }
        double x = 0;
        if (!readNumber(it.value(), x, error, "model " + name + "." + it.key())) return false;
        d.p[it.key()] = x;
    }
    return true;
}

// Subcircuit expansion (docs §2.2): {"ref":"X1","type":"sub","of":"name","nodes":[...],"controls":{"inner":"outer"}}
// becomes the definition's parts with refs "X1.R1", internal nodes "X1.n", pins mapped to the instance's nodes and
// control names remapped. Ground stays ground. Nested instances expand recursively.
bool expandParts(const json& parts, const json& subs, const std::string& prefix, const std::map<std::string, std::string>& pins,
                 const std::map<std::string, std::string>& ctls, int depth, json& out, std::string& error) {
    if (depth > 8) {
        error = "subcircuits nested more than 8 deep";
        return false;
    }
    auto isGround = [](const std::string& x) {
        std::string l = x;
        for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return l == "0" || l == "gnd" || l == "ground";
    };
    auto mapNode = [&](const std::string& n) {
        if (auto it = pins.find(n); it != pins.end()) return isGround(it->second) ? std::string("0") : it->second;
        if (isGround(n)) return std::string("0");
        return prefix.empty() ? n : prefix + n;
    };
    auto mapCtl = [&](const std::string& c) {
        auto it = ctls.find(c);
        return it == ctls.end() ? c : it->second;
    };
    for (const auto& pj : parts) {
        if (!pj.is_object() || !pj.contains("type") || !pj.contains("ref")) {
            out.push_back(pj);
            continue;
        }
        json q = pj;
        const std::string ref = prefix + pj["ref"].get<std::string>();
        q["ref"] = ref;
        if (pj.contains("nodes") && pj["nodes"].is_array()) {
            json nd = json::array();
            for (const auto& n : pj["nodes"]) nd.push_back(mapNode(n.is_string() ? n.get<std::string>() : n.dump()));
            q["nodes"] = nd;
        }
        if (pj.contains("control") && pj["control"].is_string()) q["control"] = mapCtl(pj["control"].get<std::string>());
        if (pj.contains("light") && pj["light"].is_string()) q["light"] = mapNode(pj["light"].get<std::string>());
        if (pj.contains("value") && pj["value"].is_object() && pj["value"].contains("control"))
            q["value"]["control"] = mapCtl(pj["value"]["control"].get<std::string>());
        if (pj.contains("clock") && pj["clock"].is_object() && pj["clock"].contains("byControl") &&
            pj["clock"]["byControl"].is_object() && pj["clock"]["byControl"].contains("control") &&
            pj["clock"]["byControl"]["control"].is_string())
            q["clock"]["byControl"]["control"] = mapCtl(pj["clock"]["byControl"]["control"].get<std::string>());
        if (pj["type"] != "sub") {
            out.push_back(q);
            continue;
        }
        const std::string of = pj.value("of", std::string());
        static const json libSubs = [] {
            json all = json::object();
            for (const auto& b : library::kBlocks) {
                const json j = json::parse(b.json, nullptr, false);
                if (j.is_object() && j.contains("subcircuits"))
                    for (auto it = j["subcircuits"].begin(); it != j["subcircuits"].end(); ++it) all[it.key()] = it.value();
            }
            return all;
        }();
        const bool lib = of.rfind("lib:", 0) == 0;
        const std::string key = lib ? of.substr(4) : of;
        const json& pool = lib ? libSubs : subs;
        if (!pool.is_object() || !pool.contains(key)) {
            error = "part " + ref + ": unknown subcircuit " + of;
            return false;
        }
        const auto& def = pool[key];
        const auto& dpins = def.at("pins");
        if (!q.contains("nodes") || q["nodes"].size() != dpins.size()) {
            error = "part " + ref + ": subcircuit " + of + " has " + std::to_string(dpins.size()) + " pins";
            return false;
        }
        std::map<std::string, std::string> pm;
        for (std::size_t i = 0; i < dpins.size(); ++i) pm[dpins[i].get<std::string>()] = q["nodes"][i].get<std::string>();
        std::map<std::string, std::string> cm;
        if (pj.contains("controls"))
            for (auto it = pj["controls"].begin(); it != pj["controls"].end(); ++it) cm[it.key()] = mapCtl(it.value().get<std::string>());
        for (const auto& [k, v] : ctls)
            if (!cm.count(k)) cm[k] = v;
        json kids = json::array();
        if (!expandParts(def.at("parts"), subs, ref + ".", pm, cm, depth + 1, kids, error)) return false;
        // "parallel": m identical instances on the same pins = one instance with R, L ÷ m, C × m and m-fold
        // devices (exact, and m times cheaper than m instances).
        const double m = pj.value("parallel", 1.0);
        if (m != 1.0) {
            if (!(m >= 1.0) || std::floor(m) != m) {
                error = "part " + ref + ": \"parallel\" must be a whole number >= 1";
                return false;
            }
            for (auto& k : kids) {
                if (!k.is_object() || !k.contains("type")) continue;
                const std::string t = k["type"].get<std::string>();
                auto scale = [&](double f) {
                    if (!k.contains("value")) return true;
                    auto& v = k["value"];
                    std::optional<double> x;
                    if (v.is_number()) x = v.get<double>();
                    else if (v.is_string()) x = parseValue(v.get<std::string>());
                    if (!x) return false;
                    v = *x * f;
                    return true;
                };
                bool ok = true;
                if (t == "R" || t == "L") ok = scale(1.0 / m);
                else if (t == "C") ok = scale(m);
                else if (t == "triode" || t == "pentode" || t == "speaker") k["parallel"] = k.value("parallel", 1.0) * m;
                else if (t != "V") ok = false;
                if (!ok) {
                    error = "part " + ref + ": \"parallel\" supports subcircuits of R, L, C, V, triodes and pentodes (" + k.value("ref", std::string()) + " is " + t + ")";
                    return false;
                }
            }
        }
        // "series": m identical instances in series (the same current through each) = one instance with R, L × m and
        // C ÷ m: exact for a linear two-pin network (a 2x12 or 4x10 cabinet's drivers); the inner nodes are not kept.
        const double ms = pj.value("series", 1.0);
        if (ms != 1.0) {
            if (!(ms >= 1.0) || std::floor(ms) != ms || dpins.size() != 2) {
                error = "part " + ref + ": \"series\" must be a whole number >= 1, on a two-pin subcircuit";
                return false;
            }
            for (auto& k : kids) {
                if (!k.is_object() || !k.contains("type")) continue;
                const std::string t = k["type"].get<std::string>();
                std::optional<double> x;
                if (k.contains("value")) {
                    const auto& v = k["value"];
                    if (v.is_number()) x = v.get<double>();
                    else if (v.is_string()) x = parseValue(v.get<std::string>());
                }
                if ((t != "R" && t != "L" && t != "C") || !x) {
                    error = "part " + ref + ": \"series\" supports subcircuits of R, L and C (" + k.value("ref", std::string()) + " is " + t + ")";
                    return false;
                }
                k["value"] = t == "C" ? *x / ms : *x * ms;
            }
        }
        for (auto& k : kids) out.push_back(std::move(k));
    }
    return true;
}

const std::set<std::string> kModelTypes = {"D", "Dpair", "npn", "pnp", "njf", "pjf", "opamp", "triode", "pentode", "ldr", "spring",
                                           "ota", "lamp", "led", "neon", "cds", "elpanel", "bbd", "tape", "absi", "gaincell", "vca", "rms", "dff", "jkff", "nand", "inv", "ad536",
                                           "speaker"};
const std::map<std::string, std::string> kTypeKind = {{"D", "diode"}, {"Dpair", "diode"}, {"npn", "npn"},  {"pnp", "pnp"}, {"njf", "njf"}, {"pjf", "pjf"},
                                                      {"opamp", "opamp"}, {"triode", "triode"}, {"pentode", "pentode"}, {"ldr", "ldr"}, {"spring", "springtank"},
                                                      {"ota", "ota"}, {"lamp", "lamp"}, {"led", "led"}, {"neon", "neon"}, {"cds", "cds"},
                                                      {"elpanel", "elpanel"}, {"bbd", "bbd"}, {"tape", "tape"}, {"absi", "compander"},
                                                      {"gaincell", "compander"}, {"vca", "vca"}, {"rms", "rms"},
                                                      {"dff", "cmos"}, {"jkff", "cmos"}, {"ad536", "ad536"}, {"nand", "cmos"}, {"inv", "cmos"},
                                                      {"speaker", "speaker"}};

} // namespace

std::optional<DeviceModel> resolveModel(const Netlist& n, const std::string& name) {
    if (auto it = n.models.find(name); it != n.models.end()) {
        // Local models extend the built-in defaults for their kind.
        DeviceModel d = it->second;
        for (const auto& [k, v] : modelDefaults(d.kind))
            if (!d.p.count(k)) d.p[k] = v;
        return d;
    }
    const auto& lib = builtinModels();
    if (auto it = lib.find(name); it != lib.end()) {
        DeviceModel d = it->second;
        for (const auto& [k, v] : modelDefaults(d.kind))
            if (!d.p.count(k)) d.p[k] = v;
        return d;
    }
    return std::nullopt;
}

bool parseNetlist(const json& j, Netlist& out, std::string& error) {
    try {
        if (!j.is_object()) {
            error = "netlist must be a JSON object";
            return false;
        }
        Netlist n;
        n.schema = j.value("schema", std::string());
        if (n.schema != kSchemaV1) {
            error = "unsupported netlist schema \"" + n.schema + "\" (expected " + kSchemaV1 + ")";
            return false;
        }
        n.id = j.value("id", std::string());
        if (n.id.empty()) {
            error = "netlist needs an id";
            return false;
        }
        n.name = j.value("name", n.id);
        n.description = j.value("description", std::string());
        n.version = j.value("version", 1);
        n.oversampling = std::clamp(j.value("oversampling", 2), 1, 8);
        n.minRate = std::max(0.0, j.value("minRate", 0.0));
        if (j.contains("readouts") && j["readouts"].is_object()) {
            n.readouts = j["readouts"];
            if (n.readouts.contains("lfo") && n.readouts["lfo"].is_object()) n.lfoNode = n.readouts["lfo"].value("node", std::string()); // (§10.49)
        }
        n.lanes = j.value("lanes", true);
        if (j.contains("lte")) {
            const auto& l = j["lte"];
            if (!l.is_object()) {
                error = "lte must be an object";
                return false;
            }
            n.lteStart = l.value("start", std::string());
            if (!n.lteStart.empty() && n.lteStart != "be" && n.lteStart != "bdf2") {
                error = "lte.start must be be or bdf2";
                return false;
            }
            n.lteTol = std::max(0.0, l.value("tol", 0.0));
        }
        if (j.contains("newton")) {
            const auto& nt = j["newton"];
            if (!nt.is_object()) {
                error = "newton must be an object";
                return false;
            }
            n.newtonTol = std::max(0.0, nt.value("tol", 0.0));
            n.tubeNewtonTol = std::max(0.0, nt.value("tube", 0.0));
            n.newtonGroupTols.clear();
            if (nt.contains("groups")) { // docs §10.55
                if (!nt["groups"].is_object()) {
                    error = "newton.groups must be an object";
                    return false;
                }
                for (const auto& [ref, t] : nt["groups"].items())
                    if (t.is_number() && t.get<double>() > 0) n.newtonGroupTols.emplace_back(ref, t.get<double>());
            }
            n.newtonChordGate = std::max(0.0, nt.value("chordGate", 0.0)); // docs §10.50
            n.newtonQuadPorts = std::max(0, nt.value("quadPorts", 0));
            n.newtonSubPred = std::max(-1, nt.value("subPred", -1));
            n.newtonSubPredPorts = std::max(0, nt.value("subPredPorts", 0));
            n.newtonSubsteps = std::max(0.0, nt.value("substeps", 0.0)); // docs §10.64
        }
        n.solver = j.value("solver", std::string("auto"));
        if (n.solver != "auto" && n.solver != "dk" && n.solver != "wdf") {
            error = "solver must be auto, dk or wdf";
            return false;
        }
        if (j.contains("sources"))
            for (const auto& s : j["sources"])
                n.sources.push_back({s.value("title", std::string()), s.value("url", std::string()),
                                     s.value("licence", std::string()), s.value("note", std::string())});
        if (j.contains("io")) {
            const auto& io = j["io"];
            n.io.input = io.value("input", n.io.input);
            n.io.output = io.value("output", n.io.output);
            n.io.outputR = io.value("outputR", std::string());
            n.io.inputR = io.value("inputR", std::string());
            n.io.inputVoltsPerFS = io.value("inputVoltsPerFS", 1.0);
            n.io.outputVoltsPerFS = io.value("outputVoltsPerFS", 1.0);
            if (io.contains("sourceImpedance") && !readNumber(io["sourceImpedance"], n.io.sourceImpedance, error, "io.sourceImpedance"))
                return false;
            if (!(n.io.inputVoltsPerFS > 0) || !(n.io.outputVoltsPerFS > 0)) {
                error = "io volts-per-full-scale must be positive";
                return false;
            }
        }
        if (j.contains("models")) {
            for (auto it = j["models"].begin(); it != j["models"].end(); ++it) {
                DeviceModel d;
                if (!parseModelJson(it.key(), it.value(), d, error)) return false;
                n.models[it.key()] = d;
            }
        }
        if (j.contains("controls")) {
            for (const auto& c : j["controls"]) {
                ControlDef d;
                d.id = c.at("id").get<std::string>();
                d.label = c.value("label", d.id);
                d.kind = c.value("kind", std::string("continuous"));
                d.unit = c.value("unit", std::string());
                d.group = c.value("group", std::string());
                d.note = c.value("note", std::string());
                d.def = c.value("default", 0.5);
                d.legacyOnly = c.value("legacyOnly", false);
                d.mod = c.value("mod", false);
                d.modNote = c.value("modNote", std::string());
                d.face = c.value("face", false);
                if (c.contains("params"))
                    for (const auto& p : c["params"]) d.params.push_back(p.get<std::string>());
                if (c.contains("paramRange") && c["paramRange"].is_array() && c["paramRange"].size() == 2) {
                    d.paramMin = c["paramRange"][0].get<double>();
                    d.paramMax = c["paramRange"][1].get<double>();
                }
                if (c.contains("options"))
                    for (const auto& o : c["options"]) d.options.push_back(o.get<std::string>());
                if (d.kind != "continuous" && d.kind != "switch" && d.kind != "selector") {
                    error = "control " + d.id + ": kind must be continuous, switch or selector";
                    return false;
                }
                if (n.control(d.id)) {
                    error = "duplicate control id " + d.id;
                    return false;
                }
                n.controls.push_back(std::move(d));
            }
        }
        if (!j.contains("parts") || !j["parts"].is_array() || j["parts"].empty()) {
            error = "netlist needs a non-empty parts array";
            return false;
        }
        json flatParts = json::array();
        {
            const json subs = j.value("subcircuits", json::object());
            if (!expandParts(j["parts"], subs, "", {}, {}, 0, flatParts, error)) return false;
            // models declared inside subcircuits join the netlist's models
            for (auto it = subs.begin(); it != subs.end(); ++it)
                if (it.value().contains("models"))
                    for (auto mt = it.value()["models"].begin(); mt != it.value()["models"].end(); ++mt)
                        if (!n.models.count(mt.key())) {
                            DeviceModel d;
                            if (!parseModelJson(mt.key(), mt.value(), d, error)) return false;
                            n.models[mt.key()] = d;
                        }
        }
        std::set<std::string> refs;
        for (const auto& pj : flatParts) {
            Part p;
            p.ref = pj.at("ref").get<std::string>();
            p.type = pj.at("type").get<std::string>();
            const std::string where = "part " + p.ref;
            if (!refs.insert(p.ref).second) {
                error = "duplicate part ref " + p.ref;
                return false;
            }
            for (const auto& nd : pj.at("nodes")) p.nodes.push_back(nd.is_string() ? nd.get<std::string>() : nd.dump());
            const int arity = partArity(p.type, static_cast<int>(p.nodes.size()));
            if (arity == 0) {
                error = where + ": unknown part type " + p.type;
                return false;
            }
            if (static_cast<int>(p.nodes.size()) != arity) {
                error = where + ": type " + p.type + " needs " + std::to_string(arity) + " nodes";
                return false;
            }
            p.label = pj.value("label", std::string());
            p.note = pj.value("note", std::string());
            p.family = pj.value("family", std::string());
            p.package = pj.value("package", std::string());
            p.model = pj.value("model", std::string());
            p.control = pj.value("control", std::string());
            p.invert = pj.value("invert", false);
            p.tolerance = pj.value("tolerance", 0.0);
            if (pj.contains("rend") && !readNumber(pj["rend"], p.rend, error, where + ".rend")) return false;
            if (p.rend < 0) {
                error = where + ": rend must be >= 0";
                return false;
            }
            if (pj.contains("span")) {
                const auto& sp = pj["span"];
                if (p.type != "pot" || !sp.is_array() || sp.size() != 2 || !sp[0].is_number() || !sp[1].is_number() ||
                    sp[0].get<double>() < 0.0 || sp[1].get<double>() > 1.0 || sp[0].get<double>() >= sp[1].get<double>()) {
                    error = where + ": \"span\" must be [lo, hi] with 0 <= lo < hi <= 1 on a pot";
                    return false;
                }
                p.spanLo = sp[0].get<double>();
                p.spanHi = sp[1].get<double>();
            }
            p.parallel = std::max(1, pj.value("parallel", 1));
            p.series = std::max(1, pj.value("series", 1));
            if (p.type == "speaker" && (p.nodes[2] == "0" || p.nodes[2] == p.nodes[0] || p.nodes[2] == p.nodes[1])) {
                error = where + ": a speaker's third node is its cone-velocity node (not ground, not an electrical pin)";
                return false;
            }
            if (pj.contains("gain") && !readNumber(pj["gain"], p.gain, error, where + ".gain")) return false;
            const bool optical = p.type == "lamp" || p.type == "led" || p.type == "neon" || p.type == "cds" || p.type == "elpanel";
            if (optical) {
                p.light = pj.value("light", std::string());
                if (p.light.empty()) {
                    error = where + ": an optical part needs a \"light\" node";
                    return false;
                }
                if (pj.contains("couple") && !readNumber(pj["couple"], p.couple, error, where + ".couple")) return false;
            }
            if (p.type == "bbd") {
                const json ck = pj.value("clock", json::object());
                if (ck.contains("hz") && !readNumber(ck["hz"], p.clockHz, error, where + ".clock.hz")) return false;
                auto readPts = [&](const json& arr, std::vector<std::pair<double, double>>& dst, const std::string& ctx) {
                    for (const auto& pt : arr) {
                        double cv = 0, cf = 0;
                        if (!pt.is_array() || pt.size() != 2) {
                            error = ctx + ": clock points are [volts, hertz] pairs";
                            return false;
                        }
                        if (!readNumber(pt[0], cv, error, ctx) || !readNumber(pt[1], cf, error, ctx)) return false;
                        dst.emplace_back(cv, cf);
                    }
                    std::sort(dst.begin(), dst.end());
                    return true;
                };
                if (ck.contains("points") && !readPts(ck["points"], p.clock, where + ".clock")) return false;
                if (ck.contains("byControl") && ck["byControl"].is_object()) {
                    const auto& bc = ck["byControl"];
                    p.clockControl = bc.value("control", std::string());
                    if (p.clockControl.empty() || n.controlIndex(p.clockControl) < 0) {
                        error = where + ".clock.byControl: unknown control '" + p.clockControl + "'";
                        return false;
                    }
                    if (!bc.contains("rows") || !bc["rows"].is_array() || bc["rows"].empty()) {
                        error = where + ".clock.byControl: expected non-empty 'rows'";
                        return false;
                    }
                    for (const auto& rj : bc["rows"]) {
                        Part::ClockRow row;
                        if (!rj.is_object() || !rj.contains("at") || !rj.contains("points") || !rj["points"].is_array() || rj["points"].empty()) {
                            error = where + ".clock.byControl.rows: expected {'at': x, 'points': [...]}";
                            return false;
                        }
                        if (!readNumber(rj["at"], row.at, error, where + ".clock.byControl.at")) return false;
                        if (!readPts(rj["points"], row.points, where + ".clock.byControl.points")) return false;
                        p.clockRows.push_back(std::move(row));
                    }
                    std::sort(p.clockRows.begin(), p.clockRows.end(), [](const auto& a, const auto& b) { return a.at < b.at; });
                }
                if (p.clock.empty() && p.clockRows.empty() && !(p.clockHz > 0)) {
                    error = where + ": a bbd needs \"clock\": {\"points\": [[volts, hz], ...]}, {\"byControl\": ...}, or {\"hz\": f}";
                    return false;
                }
            }
            if (p.type == "transformer") {
                auto nums = [&](const char* key, std::vector<double>& v) {
                    if (!pj.contains(key)) return true;
                    for (const auto& x : pj[key]) {
                        double d = 0;
                        if (!readNumber(x, d, error, where + "." + key)) return false;
                        v.push_back(d);
                    }
                    return true;
                };
                if (!nums("turns", p.turns) || !nums("r", p.windR) || !nums("leak", p.windL)) return false;
                if (p.turns.size() * 2 != p.nodes.size()) {
                    error = where + ": one \"turns\" entry per winding (node pair) is required";
                    return false;
                }
                if (pj.contains("lm") && !readNumber(pj["lm"], p.lm, error, where + ".lm")) return false;
                if (pj.contains("lsat") && !readNumber(pj["lsat"], p.lsat, error, where + ".lsat")) return false;
                if (pj.contains("hc") && !readNumber(pj["hc"], p.hc, error, where + ".hc")) return false;
                if (pj.contains("hw") && !readNumber(pj["hw"], p.hw, error, where + ".hw")) return false;
            }
            if (pj.contains("ac")) {
                const auto& ac = pj["ac"];
                if (p.type != "V" || !ac.is_object()) {
                    error = where + ": \"ac\" is only valid on V sources";
                    return false;
                }
                if (!readNumber(ac.value("amplitude", json(0)), p.acAmp, error, where + ".ac.amplitude") ||
                    !readNumber(ac.value("freq", json(60)), p.acFreq, error, where + ".ac.freq"))
                    return false;
                p.acPhase = ac.value("phase", 90.0);
            }
            if (pj.contains("scale")) {
                const auto& sc = pj["scale"];
                if (p.type != "V" || !sc.is_object() || !sc.contains("control")) {
                    error = where + ": \"scale\" is only valid on V sources and needs a \"control\"";
                    return false;
                }
                p.scaleControl = sc["control"].get<std::string>();
                p.scaleMin = sc.value("min", 1.0);
                p.scaleMax = sc.value("max", 1.0);
                if (!(p.scaleMin > 0.0) || !(p.scaleMax > 0.0)) {
                    error = where + ".scale: min and max must be positive";
                    return false;
                }
            }
            if (pj.contains("ron") && !readNumber(pj["ron"], p.ron, error, where + ".ron")) return false;
            if (pj.contains("roff") && !readNumber(pj["roff"], p.roff, error, where + ".roff")) return false;
            if (pj.contains("positions")) {
                if (p.type != "switch" || !pj["positions"].is_array()) {
                    error = where + ": \"positions\" must be an integer array on a switch";
                    return false;
                }
                for (const auto& pos : pj["positions"]) {
                    if (!pos.is_number_integer()) {
                        error = where + ": \"positions\" entries must be integers";
                        return false;
                    }
                    p.positions.push_back(pos.get<int>());
                }
            }
            if (pj.contains("value")) {
                if (!readValue(pj["value"], p.value, error, where + ".value")) return false;
                p.hasValue = true;
            }
            if (pj.contains("taper") && !readTaper(pj["taper"], p.taper, error, where + ".taper")) return false;
            const bool needsValue = p.type == "R" || p.type == "C" || p.type == "L" || p.type == "V" || p.type == "pot";
            if (needsValue && !p.hasValue) {
                error = where + ": needs a value";
                return false;
            }
            if (needsValue && p.type != "V" && !(p.value.nominal > 0 || p.value.variable())) {
                error = where + ": value must be positive";
                return false;
            }
            if ((p.type == "pot" || p.type == "switch") && p.control.empty() && p.type == "switch") {
                error = where + ": a switch needs a control";
                return false;
            }
            if (kModelTypes.count(p.type)) {
                if (p.model.empty()) {
                    error = where + ": needs a model";
                    return false;
                }
            }
            n.parts.push_back(std::move(p));
        }
        // Models resolve and match their part types.
        for (const auto& p : n.parts) {
            if (!kModelTypes.count(p.type)) continue;
            auto m = resolveModel(n, p.model);
            if (!m) {
                error = "part " + p.ref + ": unknown model " + p.model;
                return false;
            }
            if (m->kind != kTypeKind.at(p.type) && !(p.type == "D" && m->kind == "vacdiode")) {
                error = "part " + p.ref + ": model " + p.model + " is a " + m->kind + ", not a " + kTypeKind.at(p.type);
                return false;
            }
        }
        // Controls referenced by parts exist.
        for (const auto& p : n.parts) {
            for (const auto* c : {&p.control, &p.value.control, &p.scaleControl})
                if (!c->empty() && !n.control(*c)) {
                    error = "part " + p.ref + ": unknown control " + *c;
                    return false;
                }
        }
        for (const auto& p : n.parts) {
            if (!p.positions.empty()) {
                const auto* cd = n.control(p.control);
                if (cd && !cd->options.empty()) {
                    const int nOpt = static_cast<int>(cd->options.size());
                    for (int pos : p.positions)
                        if (pos < 0 || pos >= nOpt) {
                            error = "part " + p.ref + ": position " + std::to_string(pos) + " out of range for control " + p.control;
                            return false;
                        }
                }
            }
        }
        if (j.contains("probes"))
            for (const auto& pr : j["probes"]) n.probes.push_back(pr.get<std::string>());
        if (j.contains("cuts"))
            for (const auto& c : j["cuts"]) {
                if (c.is_string()) {
                    n.cuts.push_back(c.get<std::string>());
                } else if (c.is_object() && c.contains("node")) {
                    const auto nd = c["node"].get<std::string>();
                    n.cuts.push_back(nd);
                    for (const auto& l : c.value("loads", json::array())) n.cutLoads[nd].push_back(l.get<std::string>());
                    if (c.value("chfHold", std::string()) == "group") n.chfGroupHold.push_back(nd);
                    if (c.value("delay", false)) {
                        if (!n.cutLoads.count(nd)) {
                            error = "cuts: a delay cut (" + nd + ") must name its loads";
                            return false;
                        }
                        n.delayCuts.push_back(nd);
                    }
                } else {
                    error = "cuts: entries are node names or {\"node\", \"loads\"}";
                    return false;
                }
            }
        if (j.contains("multirate")) {
            const auto& mr = j["multirate"];
            if (mr.contains("slow")) {
                const auto& sl = mr["slow"];
                n.slowRate = sl.value("rate", 0.0);
                if (!(n.slowRate > 0)) {
                    error = "multirate.slow: \"rate\" (Hz) is required";
                    return false;
                }
                for (const auto& x : sl.value("nodes", json::array())) n.slowNodes.push_back(x.get<std::string>());
            }
            if (mr.contains("oversampled"))
                for (const auto& r : mr["oversampled"]) {
                    const auto ref = r.get<std::string>();
                    if (!n.part(ref)) {
                        error = "multirate: unknown part " + ref;
                        return false;
                    }
                    n.oversampledParts.push_back(ref);
                }
            if (mr.contains("rates"))
                for (const auto& [ref, v] : mr["rates"].items()) {
                    const int f = v.get<int>();
                    if (!n.part(ref)) {
                        error = "multirate.rates: unknown part " + ref;
                        return false;
                    }
                    // up to 64 with "events": true (docs §10.62: event substeps, never a ladder rate above the oversampler's 32x)
                    if (f < 1 || f > (mr.value("events", false) ? 64 : 32) || (f & (f - 1))) {
                        error = "multirate.rates: " + ref + " must be a power of two from 1 to 32 (64 with events)";
                        return false;
                    }
                    n.partRates.push_back({ref, f});
                }
            if (mr.contains("events")) n.rateEvents = mr["events"].get<bool>(); // docs §10.48
        }
        if (j.contains("layout")) n.layout = j["layout"];
        out = std::move(n);
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = std::string("netlist: ") + e.what();
        return false;
    }
}

bool parseNetlistText(std::string_view text, Netlist& out, std::string& error) {
    json j = json::parse(text.begin(), text.end(), nullptr, false);
    if (j.is_discarded()) {
        error = "netlist: invalid JSON";
        return false;
    }
    return parseNetlist(j, out, error);
}

json toJson(const Netlist& n) {
    json j;
    j["schema"] = n.schema;
    j["id"] = n.id;
    j["version"] = n.version;
    j["name"] = n.name;
    if (!n.description.empty()) j["description"] = n.description;
    json src = json::array();
    for (const auto& s : n.sources) {
        json o = {{"title", s.title}};
        if (!s.url.empty()) o["url"] = s.url;
        if (!s.licence.empty()) o["licence"] = s.licence;
        if (!s.note.empty()) o["note"] = s.note;
        src.push_back(o);
    }
    j["sources"] = src;
    j["io"] = {{"input", n.io.input}, {"output", n.io.output}, {"inputVoltsPerFS", n.io.inputVoltsPerFS},
               {"outputVoltsPerFS", n.io.outputVoltsPerFS}};
    if (n.io.sourceImpedance > 0) j["io"]["sourceImpedance"] = n.io.sourceImpedance;
    if (!n.io.outputR.empty()) j["io"]["outputR"] = n.io.outputR;
    if (!n.io.inputR.empty()) j["io"]["inputR"] = n.io.inputR;
    j["oversampling"] = n.oversampling;
    if (n.minRate > 0) j["minRate"] = n.minRate;
    if (!n.lanes) j["lanes"] = false;
    if (!n.lteStart.empty()) j["lte"]["start"] = n.lteStart;
    if (n.lteTol > 0) j["lte"]["tol"] = n.lteTol;
    if (n.newtonTol > 0) j["newton"]["tol"] = n.newtonTol;
    if (n.tubeNewtonTol > 0) j["newton"]["tube"] = n.tubeNewtonTol;
    for (const auto& [ref, t] : n.newtonGroupTols) j["newton"]["groups"][ref] = t;
    if (n.newtonChordGate > 0) j["newton"]["chordGate"] = n.newtonChordGate;
    if (n.newtonQuadPorts > 0) j["newton"]["quadPorts"] = n.newtonQuadPorts;
    if (n.newtonSubPred >= 0) j["newton"]["subPred"] = n.newtonSubPred;
    if (n.newtonSubPredPorts > 0) j["newton"]["subPredPorts"] = n.newtonSubPredPorts;
    if (n.newtonSubsteps > 0) j["newton"]["substeps"] = n.newtonSubsteps;
    j["solver"] = n.solver;
    json models = json::object();
    for (const auto& [k, m] : n.models) {
        json o = {{"kind", m.kind}};
        for (const auto& [pk, pv] : m.p) o[pk] = pv;
        if (!m.note.empty()) o["note"] = m.note;
        models[k] = o;
    }
    j["models"] = models;
    json ctl = json::array();
    for (const auto& c : n.controls) {
        json o = {{"id", c.id}, {"label", c.label}, {"kind", c.kind}, {"default", c.def}};
        if (!c.params.empty()) o["params"] = c.params;
        if (c.paramMin != 0.0 || c.paramMax != 1.0) o["paramRange"] = {c.paramMin, c.paramMax};
        if (!c.unit.empty()) o["unit"] = c.unit;
        if (!c.group.empty()) o["group"] = c.group;
        if (!c.note.empty()) o["note"] = c.note;
        if (!c.options.empty()) o["options"] = c.options;
        if (c.legacyOnly) o["legacyOnly"] = true;
        if (c.mod) o["mod"] = true;
        if (!c.modNote.empty()) o["modNote"] = c.modNote;
        if (c.face) o["face"] = true;
        ctl.push_back(o);
    }
    j["controls"] = ctl;
    json parts = json::array();
    for (const auto& p : n.parts) {
        json o = {{"ref", p.ref}, {"type", p.type}, {"nodes", p.nodes}};
        if (p.hasValue) o["value"] = valueToJson(p.value);
        if (!p.model.empty()) o["model"] = p.model;
        if (!p.control.empty()) o["control"] = p.control;
        if (p.type == "pot") o["taper"] = {{"type", std::string(1, p.taper.kind)}, {"mid", p.taper.mid}};
        if (p.invert) o["invert"] = true;
        if (p.tolerance > 0) o["tolerance"] = p.tolerance;
        if (p.parallel > 1) o["parallel"] = p.parallel;
        if (p.series > 1) o["series"] = p.series;
        if (!p.light.empty()) o["light"] = p.light;
        if (p.type == "cds" && p.couple != 1.0) o["couple"] = p.couple;
        if (p.type == "bbd") {
            json ck = json::object();
            if (p.clockHz > 0) ck["hz"] = p.clockHz;
            if (!p.clock.empty()) {
                json pts = json::array();
                for (const auto& [cv, cf] : p.clock) pts.push_back({cv, cf});
                ck["points"] = pts;
            }
            if (!p.clockControl.empty() && !p.clockRows.empty()) {
                json rows = json::array();
                for (const auto& r : p.clockRows) {
                    json pts = json::array();
                    for (const auto& [cv, cf] : r.points) pts.push_back({cv, cf});
                    rows.push_back({{"at", r.at}, {"points", pts}});
                }
                ck["byControl"] = {{"control", p.clockControl}, {"rows", rows}};
            }
            o["clock"] = ck;
        }
        if (p.rend > 0) o["rend"] = p.rend;
        if (p.spanLo > 0.0 || p.spanHi < 1.0) o["span"] = {p.spanLo, p.spanHi};
        if (p.type == "switch") {
            o["ron"] = p.ron;
            o["roff"] = p.roff;
            if (!p.positions.empty()) o["positions"] = p.positions;
        }
        if (p.type == "vcvs" || p.type == "vccs") o["gain"] = p.gain;
        if (p.acAmp != 0) o["ac"] = {{"amplitude", p.acAmp}, {"freq", p.acFreq}, {"phase", p.acPhase}};
        if (!p.scaleControl.empty()) o["scale"] = {{"control", p.scaleControl}, {"min", p.scaleMin}, {"max", p.scaleMax}};
        if (p.type == "transformer") {
            o["turns"] = p.turns;
            if (!p.windR.empty()) o["r"] = p.windR;
            if (!p.windL.empty()) o["leak"] = p.windL;
            if (p.lm > 0) o["lm"] = p.lm;
            if (p.lsat > 0) o["lsat"] = p.lsat;
            if (p.hc > 0) o["hc"] = p.hc;
            if (p.hw > 0) o["hw"] = p.hw;
        }
        if (!p.label.empty()) o["label"] = p.label;
        if (!p.note.empty()) o["note"] = p.note;
        if (!p.family.empty()) o["family"] = p.family;
        if (!p.package.empty()) o["package"] = p.package;
        parts.push_back(o);
    }
    j["parts"] = parts;
    if (!n.probes.empty()) j["probes"] = n.probes;
    if (!n.oversampledParts.empty()) j["multirate"] = {{"oversampled", n.oversampledParts}};
    if (!n.slowNodes.empty()) j["multirate"]["slow"] = {{"rate", n.slowRate}, {"nodes", n.slowNodes}};
    for (const auto& [ref, f] : n.partRates) j["multirate"]["rates"][ref] = f;
    if (n.rateEvents) j["multirate"]["events"] = true;
    if (!n.cuts.empty()) {
        json cj = json::array();
        for (const auto& c : n.cuts) {
            auto it = n.cutLoads.find(c);
            if (it == n.cutLoads.end()) cj.push_back(c);
            else {
                json e = {{"node", c}, {"loads", it->second}};
                if (std::find(n.delayCuts.begin(), n.delayCuts.end(), c) != n.delayCuts.end()) e["delay"] = true;
                if (std::find(n.chfGroupHold.begin(), n.chfGroupHold.end(), c) != n.chfGroupHold.end()) e["chfHold"] = "group";
                cj.push_back(e);
            }
        }
        j["cuts"] = cj;
    }
    if (!n.layout.empty()) j["layout"] = n.layout;
    if (!n.readouts.empty()) j["readouts"] = n.readouts;
    return j;
}

bool applyOverrides(Netlist& n, const json& ov, std::string& error) {
    if (ov.is_null() || (ov.is_object() && ov.empty())) return true;
    if (!ov.is_object()) {
        error = "circuit overrides must be an object";
        return false;
    }
    json j = toJson(n);
    try {
        for (auto it = ov.begin(); it != ov.end(); ++it) {
            if (it.key() == "parts") {
                for (auto pit = it.value().begin(); pit != it.value().end(); ++pit) {
                    bool found = false;
                    for (auto& pj : j["parts"]) {
                        if (pj["ref"] != pit.key()) continue;
                        found = true;
                        for (auto f = pit.value().begin(); f != pit.value().end(); ++f) {
                            if (f.key() != "value" && f.key() != "model" && f.key() != "taper") {
                                error = "part " + pit.key() + ": field " + f.key() + " is not editable";
                                return false;
                            }
                            if (f.key() == "value" && pj.contains("value") && pj["value"].is_object() && !f.value().is_object()) {
                                // Editing the nominal of a controlled value keeps its law and rescales its range.
                                double nv = 0;
                                if (!readNumber(f.value(), nv, error, "part " + pit.key() + ".value")) return false;
                                const double old = pj["value"]["nominal"].get<double>();
                                const double k = old > 0 ? nv / old : 1.0;
                                pj["value"]["nominal"] = nv;
                                pj["value"]["min"] = pj["value"]["min"].get<double>() * k;
                                pj["value"]["max"] = pj["value"]["max"].get<double>() * k;
                            } else {
                                pj[f.key()] = f.value();
                            }
                        }
                    }
                    if (!found) {
                        error = "unknown part " + pit.key();
                        return false;
                    }
                }
            } else if (it.key() == "models") {
                for (auto mit = it.value().begin(); mit != it.value().end(); ++mit) j["models"][mit.key()] = mit.value();
            } else {
                error = "unknown circuit override section " + it.key();
                return false;
            }
        }
    } catch (const std::exception& e) {
        error = std::string("circuit overrides: ") + e.what();
        return false;
    }
    Netlist next;
    if (!parseNetlist(j, next, error)) return false;
    n = std::move(next);
    return true;
}

// ───────────────────────────── SPICE ─────────────────────────────

namespace {

std::string spiceNode(const std::string& s) {
    std::string l = s;
    for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (l == "0" || l == "gnd" || l == "ground") return "0";
    std::string o;
    for (char c : s) o.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    return "n_" + o;
}
std::string spiceName(const std::string& s) {
    std::string o;
    for (char c : s) o.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    return o;
}
std::string num(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.12g", v);
    return b;
}

// SPICE node names are case-insensitive, netlist node names are not (ENGINE_QUEUE 32): "nC2" and "nc2" are two nets in
// the engine, and one node in ngspice, which silently shorts them (the EHX Micro Synth's filter node onto its 4013's
// Q). The first name seen keeps its plain spelling; each later one that folds onto a taken name gets a "_c<k>" suffix.
// Deterministic (netlist order), and empty for a netlist without such pairs, so every other deck is unchanged.
std::map<std::string, std::string> spiceCaseRenames(const Netlist& n) {
    std::map<std::string, std::string> ren;
    std::set<std::string> seen;
    std::map<std::string, std::string> owner; // folded SPICE name -> netlist name that holds it
    const auto fold = [](std::string t) {
        for (auto& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return t;
    };
    std::vector<std::string> order{n.io.input, n.io.inputR, n.io.output};
    for (const auto& p : n.parts) {
        for (const auto& x : p.nodes) order.push_back(x);
        order.push_back(p.light);
    }
    for (const auto& x : order) {
        if (x.empty() || !seen.insert(x).second) continue;
        const std::string sn = spiceNode(x);
        if (sn == "0") continue;
        const auto [it, fresh] = owner.emplace(fold(sn), x);
        if (fresh || it->second == x) continue;
        for (int k = 2;; ++k) {
            const std::string cand = sn + "_c" + std::to_string(k);
            if (owner.emplace(fold(cand), x).second) {
                ren[x] = cand;
                break;
            }
        }
    }
    return ren;
}

} // namespace

std::string spiceNodeName(const std::string& node) { return spiceNode(node); }
std::string spiceNodeName(const Netlist& n, const std::string& node) {
    const auto ren = spiceCaseRenames(n);
    const auto it = ren.find(node);
    return it != ren.end() ? it->second : spiceNode(node);
}

std::string exportSpice(const Netlist& n, const SpiceExportOptions& opt) {
    std::ostringstream o;
    const auto renames = spiceCaseRenames(n);
    const auto snode = [&](const std::string& x) {
        const auto it = renames.find(x);
        return it != renames.end() ? it->second : spiceNode(x);
    };
    o << "* " << n.name << " (" << n.id << " v" << n.version << ") - exported by Headroom circuit engine\n";
    o << "* Identical device equations to the real-time engine (docs/CIRCUIT_ENGINE.md section 7)\n";
    for (const auto& [x, sx] : renames) o << "* node \"" << x << "\" is " << sx << " (SPICE names are case-insensitive)\n";
    // rshunt: a 1 TΩ shunt per node keeps ngspice's DC regular on capacitor-isolated nodes (the engine's DC uses gmin)
    o << ".options reltol=1e-5 abstol=1e-12 vntol=1e-8 gmin=1e-12 method=trap rshunt=1e12\n";
    const auto& ctl = opt.controls;
    // Known DC node voltages for op-amp supply pins.
    std::map<std::string, double> known;
    known["0"] = 0.0;
    for (const auto& p : n.parts)
        if (p.type == "V") known[snode(p.nodes[0])] = p.value.nominal;
    if (!opt.inputFile.empty()) {
        o << "AIN %v([" << snode(n.io.input) << "]) hr_input_file\n";
        o << ".model hr_input_file filesource (file=\"" << opt.inputFile
          << "\" amploffset=[0] amplscale=[1] timeoffset=0 timescale=1 timerelative=false amplstep=false)\n";
    } else if (!opt.inputSource.empty()) {
        o << "VIN " << snode(n.io.input) << " 0 " << opt.inputSource << "\n";
    }
    // io.inputR: a second driven input (the same file or source spec unless inputSourceR is given)
    if (!n.io.inputR.empty()) {
        if (!opt.inputFile.empty()) o << "AINR %v([" << snode(n.io.inputR) << "]) hr_input_file\n";
        else if (!opt.inputSourceR.empty() || !opt.inputSource.empty())
            o << "VINR " << snode(n.io.inputR) << " 0 " << (opt.inputSourceR.empty() ? opt.inputSource : opt.inputSourceR) << "\n";
    }
    std::set<std::string> modelsDone;
    // Optical light nodes (as Circuit::compile): 1 F with the first emitter's leak 1/τ_light.
    std::map<std::string, double> lightTau;
    for (const auto& p : n.parts)
        if ((p.type == "lamp" || p.type == "led" || p.type == "neon" || p.type == "elpanel") && !lightTau.count(p.light))
            if (auto m = resolveModel(n, p.model)) lightTau[p.light] = std::max(m->get("tau_light", 1e-4), 1e-6);
    for (const auto& [ln, tau] : lightTau)
        o << "C_lt_" << spiceName(ln) << " " << snode(ln) << " 0 1\nR_lt_" << spiceName(ln) << " " << snode(ln) << " 0 " << num(tau) << "\n";
    for (const auto& p : n.parts) {
        const std::string R = spiceName(p.ref);
        const auto nd = [&](int i) { return snode(p.nodes[static_cast<std::size_t>(i)]); };
        if (p.type == "R") o << "R" << R << " " << nd(0) << " " << nd(1) << " " << num(valueAt(n, p.value, ctl)) << "\n";
        else if (p.type == "C") o << "C" << R << " " << nd(0) << " " << nd(1) << " " << num(valueAt(n, p.value, ctl)) << "\n";
        else if (p.type == "L") {
            o << "L" << R << " " << nd(0) << " " << nd(1) << " " << num(valueAt(n, p.value, ctl));
            if (const auto it = opt.icL.find(p.ref); it != opt.icL.end()) o << " IC=" << num(it->second);
            o << "\n";
        }
        else if (p.type == "V" && p.acAmp != 0) {
            const double k = sourceScaleAt(n, p, ctl);
            o << "V" << R << " " << nd(0) << " " << nd(1) << " SIN(" << num(k * p.value.nominal) << " " << num(k * p.acAmp) << " " << num(p.acFreq) << " 0 0 " << num(p.acPhase) << ")\n";
        } else if (p.type == "V") o << "V" << R << " " << nd(0) << " " << nd(1) << " DC " << num(sourceScaleAt(n, p, ctl) * p.value.nominal) << "\n";
        else if (p.type == "pot") {
            double lo, up;
            potSegments(n, p, ctl, lo, up);
            if (p.nodes[0] != p.nodes[1]) o << "R" << R << "_a " << nd(0) << " " << nd(1) << " " << num(lo) << "\n";
            if (p.nodes[1] != p.nodes[2]) o << "R" << R << "_b " << nd(1) << " " << nd(2) << " " << num(up) << "\n";
        } else if (p.type == "switch") {
            o << "R" << R << " " << nd(0) << " " << nd(1) << " " << num(switchClosed(n, p, ctl) ? p.ron : p.roff) << "\n";
        } else if (p.type == "vcvs") {
            o << "E" << R << " " << nd(0) << " " << nd(1) << " " << nd(2) << " " << nd(3) << " " << num(p.gain) << "\n";
        } else if (p.type == "vccs") {
            o << "G" << R << " " << nd(0) << " " << nd(1) << " " << nd(2) << " " << nd(3) << " " << num(p.gain) << "\n";
        } else if ((p.type == "D" || p.type == "Dpair") && resolveModel(n, p.model)->kind == "vacdiode") {
            const auto m = *resolveModel(n, p.model);
            const double k = m.get("k", 3.6e-3), kn = m.get("knee", 0.5);
            const std::string z = "(V(" + nd(0) + "," + nd(1) + ")/" + num(kn) + ")";
            o << "B" << R << " " << nd(0) << " " << nd(1) << " I=" << num(k) << "*pow(" << num(kn) << "*(max(" << z << ",0)+ln(1+exp(-abs(" << z << ")))),1.5)\n";
        } else if (p.type == "D" || p.type == "Dpair") {
            const auto m = *resolveModel(n, p.model);
            const std::string mn = "D_" + spiceName(p.model);
            if (modelsDone.insert(mn).second)
                o << ".model " << mn << " D(IS=" << num(m.get("is", 1e-14)) << " N=" << num(m.get("n", 1)) << " RS=" << num(m.get("rs", 0))
                  << (m.get("bv", 0) > 0 ? " BV=" + num(m.get("bv", 0)) + " IBV=" + num(m.get("ibv", 1e-3)) + (m.get("nbv", 0) > 0 ? " NBV=" + num(m.get("nbv", 0)) : std::string()) : std::string())
                  << ")\n";
            o << "D" << R << " " << nd(0) << " " << nd(1) << " " << mn << "\n";
            if (p.type == "Dpair") o << "D" << R << "_r " << nd(1) << " " << nd(0) << " " << mn << "\n";
        } else if (p.type == "npn" || p.type == "pnp") {
            const auto m = *resolveModel(n, p.model);
            const std::string mn = "Q_" + spiceName(p.model);
            if (modelsDone.insert(mn).second) {
                o << ".model " << mn << " " << (p.type == "npn" ? "NPN" : "PNP") << "(IS=" << num(m.get("is", 1e-14)) << " BF=" << num(m.get("bf", 100))
                  << " BR=" << num(m.get("br", 1));
                if (m.get("ise", 0) > 0) o << " ISE=" << num(m.get("ise", 0)) << " NE=" << num(m.get("ne", 1.5));
                if (m.get("isc", 0) > 0) o << " ISC=" << num(m.get("isc", 0)) << " NC=" << num(m.get("nc", 2));
                if (m.get("vaf", 0) > 0) o << " VAF=" << num(m.get("vaf", 0));
                o << ")\n";
            }
            // base resistance as an explicit resistor to an internal base node (as the engine builds it)
            std::string b = nd(1);
            if (m.get("rb", 0) > 0) {
                b = "n_" + R + "_b";
                o << "R" << R << "_rb " << nd(1) << " " << b << " " << num(m.get("rb", 0)) << "\n";
            }
            o << "Q" << R << " " << nd(0) << " " << b << " " << nd(2) << " " << mn << "\n";
            if (m.get("cje", 0) > 0) o << "C" << R << "_je " << b << " " << nd(2) << " " << num(m.get("cje", 0)) << "\n";
            if (m.get("cjc", 0) > 0) o << "C" << R << "_jc " << b << " " << nd(0) << " " << num(m.get("cjc", 0)) << "\n";
            if (m.get("icbo", 0) > 0) // germanium ICBO: the engine's law (Devices.hpp), polarity-symmetric
                o << "B" << R << "_lk " << b << " " << nd(0) << " I=" << num(m.get("icbo", 0)) << "*tanh(V(" << b << "," << nd(0) << ")/"
                  << num(std::max(m.get("vleak", 0.2), 1e-3)) << ")\n";
        } else if (p.type == "njf" || p.type == "pjf") {
            const auto m = *resolveModel(n, p.model);
            const std::string mn = "J_" + spiceName(p.model);
            if (modelsDone.insert(mn).second)
                o << ".model " << mn << " " << (p.type == "njf" ? "NJF" : "PJF") << "(LEVEL=1 VTO=" << num(m.get("vto", -2)) << " BETA=" << num(m.get("beta", 1e-4))
                  << " LAMBDA=" << num(m.get("lambda", 0)) << " IS=" << num(m.get("is", 1e-14)) << " N=" << num(m.get("n", 1)) << " B=1)\n";
            const double ks = m.get("ksub", 0.0);
            if (ks <= 0) o << "J" << R << " " << nd(0) << " " << nd(1) << " " << nd(2) << " " << mn << "\n";
            else {
                // Subthreshold-smoothed level 1 (Devices.hpp NlKind::Jfet, "ksub"): channel as a B-source, gate junctions as diodes.
                const bool pch = p.type == "pjf";
                const std::string sg = pch ? "-1" : "1", d = nd(0), g = nd(1), sn = nd(2);
                const std::string vgs = "(" + sg + "*V(" + g + "," + sn + "))", vds = "(" + sg + "*V(" + d + "," + sn + "))";
                auto sp = [&](const std::string& x) {
                    const std::string z = "((" + x + "-(" + num(m.get("vto", -2)) + "))/" + num(ks) + ")";
                    return "(" + num(ks) + "*(max(" + z + ",0)+ln(1+exp(-abs(" + z + ")))))";
                };
                const std::string be = num(m.get("beta", 1e-4)), la = num(m.get("lambda", 0));
                auto f = [&](const std::string& vg, const std::string& u) {
                    return "(" + u + "<" + vg + " ? " + be + "*(1+" + la + "*" + u + ")*" + u + "*(2*" + vg + "-" + u + ") : " + be + "*(1+" + la + "*" + u + ")*" + vg + "*" + vg + ")";
                };
                const std::string vgn = sp(vgs), vgi = sp("(" + vgs + "-" + vds + ")"), un = vds, ui = "(-" + vds + ")";
                o << "B" << R << "_ch " << d << " " << sn << " I=" << sg << "*(" << vds << ">=0 ? " << f(vgn, un) << " : -" << f(vgi, ui) << ")\n";
                const std::string dm = "D_" + spiceName(p.model) + "_g";
                if (modelsDone.insert(dm).second) o << ".model " << dm << " D(IS=" << num(m.get("is", 1e-14)) << " N=" << num(m.get("n", 1)) << ")\n";
                if (!pch) o << "D" << R << "_gs " << g << " " << sn << " " << dm << "\nD" << R << "_gd " << g << " " << d << " " << dm << "\n";
                else o << "D" << R << "_gs " << sn << " " << g << " " << dm << "\nD" << R << "_gd " << d << " " << g << " " << dm << "\n";
            }
            if (m.get("cgs", 0) > 0) o << "C" << R << "_gs " << nd(1) << " " << nd(2) << " " << num(m.get("cgs", 0)) << "\n";
            if (m.get("cgd", 0) > 0) o << "C" << R << "_gd " << nd(1) << " " << nd(0) << " " << num(m.get("cgd", 0)) << "\n";
        } else if (p.type == "opamp") {
            const auto m = *resolveModel(n, p.model);
            std::string inp = nd(0);
            const std::string inn = nd(1), out = nd(2);
            // input errors (Engine.cpp, ENGINE_QUEUE 31) as plain elements: pin bias currents, a series offset source
            const double vos = m.get("vos", 0.0), ibp = m.get("ib", 0.0) + 0.5 * m.get("ios", 0.0), ibn = m.get("ib", 0.0) - 0.5 * m.get("ios", 0.0);
            if (ibp != 0.0 && inp != "0") o << "I" << R << "_ibp " << inp << " 0 DC " << num(ibp) << "\n";
            if (ibn != 0.0 && inn != "0") o << "I" << R << "_ibn " << inn << " 0 DC " << num(ibn) << "\n";
            if (vos != 0.0) {
                o << "V" << R << "_vos n_" << R << "_ip " << inp << " DC " << num(vos) << "\n";
                inp = "n_" + R + "_ip";
            }
            if (m.get("level", 1) < 0.5) {
                o << "E" << R << " " << out << " 0 " << inp << " " << inn << " 1e7\n";
                continue;
            }
            double vhi = m.get("vhi", 13.5), vlo = m.get("vlo", -13.5);
            const bool railNodes = p.nodes.size() == 5 && (!known.count(nd(3)) || !known.count(nd(4)));
            if (p.nodes.size() == 5 && !railNodes) {
                vhi = known[nd(3)] - m.get("swing_hi", 1.5);
                vlo = known[nd(4)] + m.get("swing_lo", 1.5);
            }
            const double c1 = m.get("cdom", 30e-12);
            const double gm = 2.0 * M_PI * m.get("gbw", 1e6) * c1;
            const double r1 = m.get("a0", 1e5) / gm;
            const double imax = m.get("slew", 1e6) * c1;
            const double ic = imax * 1e-3, vc = 0.05;
            const std::string x = "n_" + R + "_x";
            o << "B" << R << "_gm 0 " << x << " I=" << num(imax) << "*tanh(" << num(gm / imax) << "*(V(" << inp << ")-V(" << inn << ")))\n";
            o << "R" << R << "_1 " << x << " 0 " << num(r1) << "\n";
            o << "C" << R << "_1 " << x << " 0 " << num(c1) << "\n";
            if (!railNodes) {
                // a convergence hint only (not a constraint): ngspice's operating point can otherwise settle a follower's
                // swing node on the spurious R1·Imax solution (exp() in B-sources is range-limited)
                const auto hint = opt.nodeset.find(p.ref + "#x");
                o << ".nodeset v(" << x << ")=" << num(hint != opt.nodeset.end() ? hint->second : 0.5 * (vhi + vlo)) << "\n";
                o << "B" << R << "_cl " << x << " 0 I=" << num(ic) << "*(exp((V(" << x << ")-" << num(vhi) << ")/" << num(vc) << ")-exp((" << num(vlo)
                  << "-V(" << x << "))/" << num(vc) << "))\n";
                if (m.get("ilim", 0) > 0) { // output current limit (Engine.cpp, Devices.hpp OutLim)
                    const double il = m.get("ilim", 0), ro = std::max(m.get("rout", 100.0), 1e-3);
                    o << "E" << R << " n_" << R << "_xb 0 " << x << " 0 1\n";
                    o << "B" << R << "_lim n_" << R << "_xb " << out << " I=" << num(il) << "*tanh(V(n_" << R << "_xb," << out << ")/" << num(il * ro) << ")\n";
                } else {
                    o << "E" << R << " " << out << " 0 " << x << " 0 1\n";
                }
            } else { // supply pins on circuit nodes: the engine's relative clamps; the output's load current is drawn from
                     // the rails, gated by headroom (Engine.cpp railDraws: the engine applies it one sample late)
                const std::string vp = nd(3), vn = nd(4);
                // the same convergence hint: a follower on derived rails otherwise lands on the ±R1·Imax solution
                if (const auto hint = opt.nodeset.find(p.ref + "#x"); hint != opt.nodeset.end())
                    o << ".nodeset v(" << x << ")=" << num(hint->second) << "\n";
                o << "B" << R << "_ch " << x << " 0 I=" << num(ic) << "*exp((V(" << x << ")-V(" << vp << ")+" << num(m.get("swing_hi", 1.5)) << ")/" << num(vc) << ")\n";
                o << "B" << R << "_cn " << x << " 0 I=-" << num(ic) << "*exp((V(" << vn << ")+" << num(m.get("swing_lo", 1.5)) << "-V(" << x << "))/" << num(vc) << ")\n";
                o << "E" << R << " " << out << " 0 " << x << " 0 1\n";
                const double iq = std::max(m.get("iq", 1e-3), 0.0);
                const std::string io = "(-i(E" + R + "))"; // current sourced into the loads
                auto gate = [&](const std::string& h) { return "1/(1+exp(-((" + h + ")-0.2)/0.05))"; };
                if (vp != "0")
                    o << "B" << R << "_dp " << vp << " 0 I=" << gate("V(" + vp + ")-V(" + out + ")") << "*((" << io << "+abs(" << io << "))/2+" << num(iq) << ")\n";
                if (vn != "0")
                    o << "B" << R << "_dn 0 " << vn << " I=" << gate("V(" + out + ")-V(" + vn + ")") << "*((abs(" << io << ")-" << io << ")/2+" << num(iq) << ")\n";
            }
        } else if (p.type == "speaker") {
            // Engine.cpp "speaker" (Devices.hpp NlKind::Speaker): the same equations, written out whole (the engine splits
            // them into linear elements plus a residual law). Nodes: psi = flux / Le0 (A), x = excursion (mm), tv/tm = coil
            // and magnet temperature rise (K, the engine integrates them explicitly one sample behind the coil current).
            const SpeakerDerived sp = speakerDerived(*resolveModel(n, p.model), p.series, p.parallel);
            const std::string vel = nd(2), b = nd(1), n1 = "n_" + R + "_1", psi = "n_" + R + "_psi", xn = "n_" + R + "_xm";
            std::string a = n1;
            o << "R" << R << "_re " << nd(0) << " " << n1 << " " << num(sp.re) << "\n";
            if (sp.l2 > 0) {
                a = "n_" + R + "_a";
                o << "L" << R << "_l2 " << n1 << " " << a << " " << num(sp.l2) << "\n";
                if (sp.r2 > 0) o << "R" << R << "_r2 " << n1 << " " << a << " " << num(sp.r2) << "\n";
            }
            const std::string X = "V(" + xn + ")";
            const std::string xc = "min(max(" + X + "," + num(sp.xlo) + ")," + num(sp.xhi) + ")";
            const std::string inside = "u(" + X + "-(" + num(sp.xlo) + "))*u(" + num(sp.xhi) + "-" + X + ")";
            const std::string r = "(1+" + num(sp.lx1) + "*" + xc + "+" + num(sp.lx2) + "*" + xc + "*" + xc + ")";
            const std::string ic = "(V(" + psi + ")/" + r + ")";
            const std::string bl = "(" + num(sp.bl) + "*(1+" + xc + "*(" + num(sp.b[0]) + "+" + xc + "*(" + num(sp.b[1]) + "+" + xc + "*(" + num(sp.b[2]) + "+" + xc + "*" +
                                   num(sp.b[3]) + ")))))";
            const std::string fk = "(" + num(sp.kms * 1e-3) + "*" + X + "*(1+" + num(sp.k1) + "*" + X + "+" + num(sp.k2) + "*" + X + "*" + X + "+" + num(sp.k4) + "*" + X +
                                   "*" + X + "*" + X + "*" + X + "))";
            const std::string frel = "(" + num(0.5e3 * sp.le) + "*(" + num(sp.lx1) + "+" + num(2.0 * sp.lx2) + "*" + xc + ")*" + inside + "*" + ic + "*" + ic + ")";
            const std::string tv = "n_" + R + "_tv", tm = "n_" + R + "_tm";
            const std::string dre = sp.thermal ? "(" + num(sp.re * sp.alpha) + "*V(" + tv + "))" : "0";
            o << "C" << R << "_psi " << psi << " 0 " << num(sp.le) << "\n";
            o << "B" << R << "_psi 0 " << psi << " I=V(" << a << "," << b << ")-" << bl << "*V(" << vel << ")-" << dre << "*" << ic << "\n";
            o << "B" << R << "_coil " << a << " " << b << " I=" << ic << "\n";
            o << "C" << R << "_m " << vel << " 0 " << num(sp.mms) << "\n";
            o << "R" << R << "_m " << vel << " 0 " << num(1.0 / sp.rms) << "\n";
            o << "B" << R << "_f 0 " << vel << " I=" << bl << "*" << ic << "-" << fk << "+" << frel << "\n";
            o << "G" << R << "_x 0 " << xn << " " << vel << " 0 1000\n";
            o << "C" << R << "_x " << xn << " 0 1\n";
            if (sp.thermal) {
                o << "B" << R << "_h 0 " << tv << " I=(" << num(sp.re) << "+" << dre << ")*" << ic << "*" << ic << "/" << sp.coils << "\n";
                o << "C" << R << "_tv " << tv << " 0 " << num(sp.ctv) << "\n";
                if (sp.rtm > 0) {
                    o << "R" << R << "_tv " << tv << " " << tm << " " << num(sp.rtv) << "\n";
                    o << "C" << R << "_tm " << tm << " 0 " << num(sp.ctm) << "\n";
                    o << "R" << R << "_tm " << tm << " 0 " << num(sp.rtm) << "\n";
                } else {
                    o << "R" << R << "_tv " << tv << " 0 " << num(sp.rtv) << "\n";
                }
            }
        } else if (p.type == "spring") {
            // The tank's springs are not electrical: SPICE sees its input coil load and an idle (0 V) output source.
            const auto m = *resolveModel(n, p.model);
            o << "R" << R << "_in " << nd(0) << " n_" << R << "_c " << num(m.get("rin", 8)) << "\n";
            o << "L" << R << "_in n_" << R << "_c " << nd(1) << " " << num(m.get("lin", 1e-3)) << "\n";
            o << "R" << R << "_out " << nd(2) << " " << nd(3) << " " << num(m.get("rout", 2575)) << "\n";
        } else if (p.type == "ldr") {
            const auto m = *resolveModel(n, p.model);
            const std::string a = nd(0), b = nd(1), la = nd(2), lb = nd(3), light = "n_" + R + "_light";
            const double vb = m.get("vstrike", 65), rl = m.get("rlamp", 30e3), sm = m.get("smooth", 2), k = m.get("drive", 150), rel = m.get("release", 0.08);
            const std::string vl = "V(" + la + "," + lb + ")";
            const std::string z = "((abs(" + vl + ")-" + num(vb) + ")/" + num(sm) + ")";
            const std::string mag = "(" + num(sm / rl) + "*(max(" + z + ",0)+ln(1+exp(-abs(" + z + ")))))";
            o << "B" << R << "_n " << la << " " << lb << " I=sgn(" << vl << ")*" << mag << "\n";
            o << "B" << R << "_d 0 " << light << " I=" << num(k) << "*" << mag << "\n";
            o << "C" << R << "_l " << light << " 0 1\n";
            o << "R" << R << "_l " << light << " 0 " << num(rel) << "\n";
            const double gam = m.get("gamma", 0.9);
            o << "B" << R << "_r " << a << " " << b << " I=V(" << a << "," << b << ")*(" << num(1.0 / m.get("rdark", 1e7)) << "+(pow(max(V(" << light << "),0)+1e-4,"
              << num(gam) << ")-" << num(std::pow(1e-4, gam)) << ")/" << num(m.get("ron", 8e3)) << ")\n";
        } else if (p.type == "ota") {
            // Devices.hpp NlKind::OtaHalf: abc junction as a diode, the two output halves and input base currents as B-sources.
            const auto m = *resolveModel(n, p.model);
            const std::string out = nd(0), inp = nd(1), inn = nd(2), abc = nd(3), vm = nd(4), vp = nd(5);
            const double nvt = m.get("n", 1.0) * std::max(1.0, m.get("junctions", 1.0)) * kVt, is = m.get("is", 5.9e-16);
            const std::string mn = "D_" + spiceName(p.model) + "_abc";
            if (modelsDone.insert(mn).second) o << ".model " << mn << " D(IS=" << num(is) << " N=" << num(nvt / 0.02586491704) << ")\n";
            // The halves mirror the abc diode's own current, read through a 0 V sense source (ENGINE_QUEUE 32): the
            // diode's junction limiting then bounds them too, where a free exp(V(abc,vm)) in a B-source overflows on
            // the first Newton step of a seeded .op (the Micro Synth's A10..A14, abc at 0 V with vm at -10 V). The
            // diode's ngspice gmin (1e-12 S, the deck's .options) is taken back out, so the law is the same is*(e^x-1).
            o << "D" << R << "_abc " << abc << " n_" << R << "_abcs " << mn << "\n";
            o << "V" << R << "_abc n_" << R << "_abcs " << vm << " DC 0\n";
            const std::string iabc = "(" + num(m.get("mirror", 1.0)) + "*(I(V" + R + "_abc)-1e-12*V(" + abc + ",n_" + R + "_abcs)))";
            const std::string vd = "V(" + inp + "," + inn + ")";
            const double w = std::max(m.get("w", 0.1), 1e-3), va = m.get("va", 0.0), bf = m.get("bf", 0.0);
            for (int half = 0; half < 2; ++half) {
                const std::string sig = "(1/(1+exp(" + std::string(half == 0 ? "-" : "") + vd + "/" + num(kVt) + ")))";
                const std::string u = half == 0 ? "V(" + vp + "," + out + ")" : "V(" + out + "," + vm + ")";
                const std::string c = "(0.5*(1+tanh((" + u + "-" + num(half == 0 ? m.get("usat_hi", 1.5) : m.get("usat_lo", 0.6)) + ")/" + num(w) + ")))";
                const std::string e = va > 0 ? "*(1+" + u + "/" + num(va) + ")" : "";
                o << "B" << R << (half == 0 ? "_p " : "_m ") << (half == 0 ? vp + " " + out : out + " " + vm) << " I=" << iabc << "*" << sig << "*" << c << e << "\n";
                if (bf > 0) o << "B" << R << (half == 0 ? "_bp " : "_bm ") << (half == 0 ? inp : inn) << " " << vm << " I=" << iabc << "*" << sig << "/" << num(bf) << "\n";
            }
        } else if (p.type == "tape") {
            // The tape is not electrical: SPICE sees the record head's R + L load and each playback head as R + L behind an
            // idle (0 V) source.
            const auto m = *resolveModel(n, p.model);
            o << "R" << R << "_rr " << nd(0) << " n_" << R << "_rx " << num(m.get("rrec", 100)) << "\n";
            o << "L" << R << "_rl n_" << R << "_rx " << nd(1) << " " << num(m.get("lrec", 10e-3)) << "\n";
            for (int h = 0; h < 3; ++h) {
                if (p.nodes[static_cast<std::size_t>(2 + h)] == "0") continue;
                const std::string k = std::to_string(h + 1);
                o << "V" << R << "_p" << k << " n_" << R << "_src" << k << " 0 DC 0\n";
                o << "R" << R << "_pr" << k << " n_" << R << "_src" << k << " n_" << R << "_pl" << k << " " << num(m.get("rpb", 300)) << "\n";
                o << "L" << R << "_pl" << k << " n_" << R << "_pl" << k << " " << nd(2 + h) << " " << num(m.get("lpb", 0.1)) << "\n";
            }
        } else if (p.type == "bbd") {
            // The bucket chain is not electrical: SPICE sees the outputs as sources behind rout at the DC level the engine
            // reports (SpiceExportOptions::nodeset "<ref>#src1"), and the input's optional capacitance.
            const auto m = *resolveModel(n, p.model);
            const auto h = opt.nodeset.find(p.ref + "#src1");
            const double vdc = h != opt.nodeset.end() ? h->second : 0.0;
            for (int k = 1; k <= 2; ++k) {
                if (k == 2 && (p.nodes[2] == "0" || p.nodes[2] == p.nodes[1])) continue;
                const std::string src = "n_" + R + "_src" + std::to_string(k);
                o << "V" << R << "_s" << k << " " << src << " 0 DC " << num(vdc) << "\n";
                o << "R" << R << "_o" << k << " " << src << " " << nd(k) << " " << num(m.get("rout", 2e3)) << "\n";
            }
            if (m.get("cin", 0) > 0) o << "C" << R << "_in " << nd(0) << " 0 " << num(m.get("cin", 0)) << "\n";
        } else if (p.type == "absi") { // NE570 rectifier (Devices.hpp AbsI)
            const auto m = *resolveModel(n, p.model);
            const double dl = m.get("delta", 1e-3);
            const std::string v = "V(" + nd(0) + "," + nd(1) + ")";
            o << "B" << R << " " << nd(3) << " " << nd(2) << " I=(sqrt(" << v << "*" << v << "+" << num(dl * dl) << ")-" << num(dl) << ")/" << num(m.get("r1", 10e3)) << "\n";
        } else if (p.type == "gaincell") { // NE570 ΔG cell (Devices.hpp GainCell)
            const auto m = *resolveModel(n, p.model);
            o << "B" << R << " " << nd(5) << " " << nd(4) << " I=" << num(2.0 / m.get("r5", 10e3)) << "*max(V(" << nd(2) << "," << nd(3) << "),0)*tanh(V(" << nd(0) << ","
              << nd(1) << ")/" << num(m.get("r2", 20e3) * m.get("i1", 140e-6)) << ")\n";
        } else if (p.type == "vca") {
            // Engine.cpp "vca": internal amplifier (high-gain E, as the ideal op-amp), bias generator, four two-junction
            // Shockley paths (Devices.hpp DiodeLight law, no gmin term) behind their ohmic resistances.
            const auto m = *resolveModel(n, p.model);
            const std::string in = nd(0), ecp = nd(1), ecn = nd(2), sym = nd(3), vm = nd(4), gnd = nd(5), vp = nd(6), out = nd(7);
            const double v2 = std::max(m.get("mvdb", 6.1), 1e-3) * 1e-3 * 20.0 / std::log(10.0);
            const double iq = std::max(m.get("iq", 20e-6), 1e-12), v0 = 1.3, rs = std::max(m.get("rs", 0.0), 0.0), rd = m.get("rd", 0.0);
            const double is = iq * std::exp(-v0 / v2);
            const std::string x = "n_" + R + "_x", v3 = "n_" + R + "_v3";
            if (const auto h = opt.nodeset.find(p.ref + "#x"); h != opt.nodeset.end()) o << ".nodeset v(" << x << ")=" << num(h->second) << "\n";
            o << "E" << R << "_x " << x << " 0 " << gnd << " " << in << " 1e7\n";
            o << "B" << R << "_v3 " << v3 << " 0 V=V(" << x << ")-" << num(2.0 * (v0 + iq * rs)) << "\n";
            if (m.get("rsym", 25.0) > 0 && p.nodes[3] != p.nodes[1]) o << "R" << R << "_sym " << sym << " " << ecp << " " << num(m.get("rsym", 25.0)) << "\n";
            if (m.get("rc", 0.0) > 0) {
                if (ecp != gnd) o << "R" << R << "_cp " << ecp << " " << gnd << " " << num(m.get("rc", 0.0)) << "\n";
                if (ecn != gnd) o << "R" << R << "_cn " << ecn << " " << gnd << " " << num(m.get("rc", 0.0)) << "\n";
            }
            struct Path {
                const char* tag;
                std::string top, base, coll;
                bool pnp;
                double r, is;
            };
            const double vcz = m.get("vczero", 0.0), bfc = m.get("bf", 0.0);
            const Path paths[4] = {{"1", x, ecp, in, true, rs, is},
                                   {"2", x, ecn, out, true, std::max(rs - 0.5 * rd, 0.0), is * std::exp(-vcz / v2)},
                                   {"3", v3, ecn, in, false, rs, is * std::exp(vcz / v2)},
                                   {"4", v3, sym, out, false, std::max(rs + 0.5 * rd, 0.0), is * std::exp(m.get("vsym", 0.0) / v2)}};
            for (const auto& q : paths) {
                std::string e = q.top;
                if (q.r > 0) {
                    e = "n_" + R + "_e" + q.tag;
                    o << "R" << R << "_r" << q.tag << " " << q.top << " " << e << " " << num(q.r) << "\n";
                }
                const std::string vj = q.pnp ? "V(" + e + "," + q.base + ")" : "V(" + q.base + "," + e + ")";
                o << "B" << R << "_q" << q.tag << " " << (q.pnp ? e + " " + q.coll : q.coll + " " + e) << " I=" << num(q.is) << "*(exp(" << vj << "/" << num(v2) << ")-1)\n";
                if (bfc > 0) // base current of the control-pin transistor
                    o << "B" << R << "_b" << q.tag << " " << (q.pnp ? e + " " + q.base : q.base + " " + e) << " I=" << num(q.is / bfc) << "*(exp(" << vj << "/" << num(v2) << ")-1)\n";
            }
            if (m.get("icc", 0.0) > 0 && vp != vm) o << "I" << R << "_cc " << vp << " " << vm << " DC " << num(m.get("icc", 0.0)) << "\n";
            if (m.get("isc", 0.0) > 0) { // output transistors' collector-base junctions (Engine.cpp: output compliance)
                const std::string mn = "D_" + R + "_cb";
                o << ".model " << mn << " D(IS=" << num(m.get("isc", 0.0)) << " N=" << num(kVt / 0.02586491704) << ")\n";
                o << "D" << R << "_cb2 " << out << " " << ecn << " " << mn << "\nD" << R << "_cb4 " << sym << " " << out << " " << mn << "\n";
            }
            if (m.get("v5", 0.0) > 0 && vm != "0" && vm != gnd) { // pin 5 held v5 below GND, its current from V+
                const std::string n5 = "n_" + R + "_v5";
                const double r5 = std::max(m.get("r5", 20.0), 1e-3);
                o << "B" << R << "_v5 " << n5 << " 0 V=V(" << gnd << ")-" << num(m.get("v5", 0.0)) << "\n";
                o << "R" << R << "_r5 " << n5 << " " << vm << " " << num(r5) << "\n";
                if (vp != "0") o << "G" << R << "_i5 " << vp << " " << n5 << " " << n5 << " " << vm << " " << num(1.0 / r5) << "\n";
            }
        } else if (p.type == "dff" || p.type == "jkff" || p.type == "nand" || p.type == "inv") {
            // CMOS 4000B (Engine.cpp, Devices.hpp CmosOut / MosHalf, Logic.hpp): the channel law as .func definitions; the
            // logic as continuous-time behavioural sources (input sigmoids 1 mV wide, a master–slave pair of 1 ns tracking
            // states for the flip-flop) delayed by the propagation delay through matched lossless lines, giving the same
            // drive λ the engine places as band-limited steps.
            const auto m = *resolveModel(n, p.model);
            const int nin = p.type == "jkff" ? 5 : p.type == "dff" ? 4 : (p.type == "nand" ? 2 : 1), nout = p.type == "dff" || p.type == "jkff" ? 2 : 1;
            const std::string vdd = nd(nin + nout), vss = nd(nin + nout + 1);
            // Intermediate quantities are behavioural voltage nodes (exact; they keep ngspice's expressions small): the
            // overdrive vov = κ·softplus((vgs − Vt)/κ) and the channel law f(vov, vds) as a .func of the overdrive.
            const std::string f = "c" + R;
            const double kap = std::max(m.get("kappa", 0.05), 1e-3), vt0 = m.get("vt", 1.659);
            auto vovExpr = [&](const std::string& vgs) {
                const std::string z = "((" + vgs + "-" + num(vt0) + ")/" + num(kap) + ")";
                return num(kap) + "*(max(" + z + ",0)+ln(1+exp(-abs(" + z + "))))";
            };
            o << ".func " << f << "_f(vov,x,b) {b/(1+" << num(m.get("theta", 0.2016)) << "*vov)*(vov*min(x,vov)-0.5*min(x,vov)*min(x,vov))*(1+" << num(m.get("lambda", 0.01527))
              << "*x)}\n";
            if (m.get("cin", 0) > 0)
                for (int k = 0; k < nin; ++k)
                    if (nd(k) != vss) o << "C" << R << "_in" << k << " " << nd(k) << " " << vss << " " << num(m.get("cin", 0)) << "\n";
            if (p.type == "inv" && m.get("ub", 0) > 0.5) {
                const std::string a = nd(0), y = nd(1);
                // per channel: overdrive at the gate–source and gate–drain voltages; the source / drain swap for vds < 0 is
                // f(vov_gs, max(vds,0)) − f(vov_gd, max(−vds,0)) (f(·, 0) = 0)
                struct Ch {
                    const char* tag;
                    std::string g, d, s;
                    double b;
                } chs[2] = {{"n", "V(" + a + "," + vss + ")", "V(" + y + "," + vss + ")", "", m.get("betan", m.get("beta", 1.215e-3))},
                            {"p", "V(" + vdd + "," + a + ")", "V(" + vdd + "," + y + ")", "", m.get("betap", m.get("beta", 1.215e-3))}};
                for (const auto& c : chs) {
                    const std::string gs = "n_" + R + "_g" + c.tag, gd = "n_" + R + "_d" + c.tag;
                    o << "B" << R << "_g" << c.tag << " " << gs << " 0 V=" << vovExpr(c.g) << "\n";
                    o << "B" << R << "_h" << c.tag << " " << gd << " 0 V=" << vovExpr(c.g + "-" + c.d) << "\n";
                    const std::string from = std::string(c.tag) == "n" ? y : vdd, to = std::string(c.tag) == "n" ? vss : y;
                    // with the engine's exact linear split across the drain port (a conductance β, subtracted again)
                    o << "R" << R << "_gs" << c.tag << " " << from << " " << to << " " << num(1.0 / c.b) << "\n";
                    o << "B" << R << "_" << c.tag << " " << from << " " << to << " I=" << f << "_f(V(" << gs << "),max(" << c.d << ",0)," << num(c.b) << ")-" << f << "_f(V(" << gd
                      << "),max(-" << c.d << ",0)," << num(c.b) << ")-" << num(c.b) << "*" << c.d << "\n";
                }
                continue;
            }
            const double w = 1e-3; // input decision width (V)
            const std::string thr = "(V(" + vss + ")+" + num(m.get("vth", 0.5)) + "*V(" + vdd + "," + vss + "))";
            // An input wired to a logic output pin reads that output's drive before the band-limiting latency (the engine
            // passes such edges cell to cell at their exact times: Logic.hpp nets); others compare the pin with VDD·vth.
            auto sigIn = [&](int k) {
                for (const auto& q : n.parts) {
                    if (q.type != "dff" && q.type != "jkff" && q.type != "nand" && q.type != "inv") continue;
                    const auto qm = resolveModel(n, q.model);
                    if (q.type == "inv" && qm && qm->get("ub", 0) > 0.5) continue;
                    const int qin = q.type == "jkff" ? 5 : q.type == "dff" ? 4 : (q.type == "nand" ? 2 : 1), qout = q.type == "dff" || q.type == "jkff" ? 2 : 1;
                    for (int j = 0; j < qout; ++j)
                        if (q.nodes[static_cast<std::size_t>(qin + j)] == p.nodes[static_cast<std::size_t>(k)])
                            return "(0.5*(1+tanh((V(n_" + spiceName(q.ref) + "_ta" + std::to_string(j) + ")-0.5)/" + num(w) + ")))";
                }
                return "(0.5*(1+tanh((V(" + nd(k) + ")-" + thr + ")/" + num(w) + ")))";
            };
            std::string raw[2];
            if (p.type == "inv") raw[0] = "1-" + sigIn(0);
            else if (p.type == "nand") raw[0] = "1-" + sigIn(0) + "*" + sigIn(1);
            else {
                const std::string ms = "n_" + R + "_m", ss = "n_" + R + "_s";
                const bool jk = p.type == "jkff";
                const int ci = jk ? 2 : 1; // clock input index
                const std::string qs = "(0.5*(1+tanh((V(" + ss + ")-0.5)/" + num(w) + ")))";
                // the master's data: D, or for the 4027 the truth table's J·Q̄ + K̄·Q of the slave's state
                const std::string sd = jk ? "(" + sigIn(0) + "*(1-" + qs + ")+(1-" + sigIn(1) + ")*" + qs + ")" : sigIn(0);
                const std::string sc = sigIn(ci), sS = sigIn(ci + 1), sR = sigIn(ci + 2);
                // non-overlapping enables: the master closes 10 mV below the clock threshold, the slave opens at it, so a
                // slow clock edge never makes the pair transparent (the engine's edge is the crossing of the threshold itself)
                std::string mc = sc;
                {
                    const std::string ck = sc.find("_ta") != std::string::npos ? sc : "";
                    if (ck.empty()) mc = "(0.5*(1+tanh((V(" + nd(ci) + ")-" + thr + "+0.01)/" + num(w) + ")))";
                    else mc = sc.substr(0, sc.find("-0.5)")) + "-0.49)/" + num(w) + ")))";
                }
                o << "C" << R << "_m " << ms << " 0 1e-9\nC" << R << "_s " << ss << " 0 1e-9\n";
                if (m.get("q0", 0.0) > 0.5) o << ".ic v(" << ss << ")=1 v(" << ms << ")=1\n";
                o << "B" << R << "_m 0 " << ms << " I=(1-" << mc << ")*(" << sd << "-V(" << ms << "))+" << sS << "*(1-V(" << ms << "))-" << sR << "*V(" << ms << ")\n";
                o << "B" << R << "_s 0 " << ss << " I=" << sc << "*(V(" << ms << ")-V(" << ss << "))+" << sS << "*(1-V(" << ss << "))-" << sR << "*V(" << ss << ")\n";
                raw[0] = sS + "+(1-" + sS + ")*(1-" + sR + ")*" + qs;
                raw[1] = sR + "+(1-" + sR + ")*(1-" + sS + ")*(1-" + qs + ")";
            }
            // propagation delay at the supply's DC value (the engine follows a moving supply sample by sample)
            double vs = 10.0;
            if (known.count(vdd) && known.count(vss)) vs = known[vdd] - known[vss];
            else if (known.count(vdd) && vss == "0") vs = known[vdd];
            const double tp[3] = {m.get("tpd5", 125e-9), m.get("tpd10", 60e-9), m.get("tpd15", 45e-9)};
            const double x = std::log(std::clamp(vs, 3.0, 20.0));
            const double y = x < std::log(10.0) ? std::log(tp[0]) + (std::log(tp[1]) - std::log(tp[0])) * (x - std::log(5.0)) / (std::log(10.0) - std::log(5.0))
                                                : std::log(tp[1]) + (std::log(tp[2]) - std::log(tp[1])) * (x - std::log(10.0)) / (std::log(15.0) - std::log(10.0));
            const double td = std::exp(y);
            const double eps = std::max(2.0 * m.get("iddq", 5e-9), 1e-12);
            const std::string vs_ = "n_" + R + "_vs"; // full-drive overdrive of the output stage (the supply's)
            o << "B" << R << "_vs " << vs_ << " 0 V=" << vovExpr("V(" + vdd + "," + vss + ")") << "\n";
            for (int j = 0; j < nout; ++j) {
                const std::string J = std::to_string(j), lam = "n_" + R + (j == 0 ? "_l" : "_lb"), pin = nd(nin + j);
                const std::string ra = "n_" + R + "_raw" + J, ta = "n_" + R + "_ta" + J, tb = "n_" + R + "_tb" + J;
                o << "B" << R << "_r" << J << " " << ra << " 0 V=" << raw[j] << "\n";
                // the delay as a one-pole lag of time constant tpd: its centroid (DC group delay) is tpd, the same edge
                // time for every audio-band consequence (the residual is O((ω·tpd)²) < 1e-4 at 20 kHz); a lossless line
                // would hold ngspice to steps below tpd for the whole run
                // then the engine's band-limiting latency (the minBLEP centroid, a constant) as a lossless line
                o << "R" << R << "_ts" << J << " " << ra << " " << ta << " 1\n";
                o << "C" << R << "_td" << J << " " << ta << " 0 " << num(td) << "\n";
                if (opt.logicRate > 0) {
                    const double D = sharedMinBlep()->delay / opt.logicRate;
                    o << "E" << R << "_tx" << J << " n_" << R << "_tx" << J << " 0 " << ta << " 0 1\n";
                    o << "R" << R << "_tr" << J << " n_" << R << "_tx" << J << " n_" << R << "_ty" << J << " 50\n";
                    o << "T" << R << "_t" << J << " n_" << R << "_ty" << J << " 0 " << tb << " 0 Z0=50 TD=" << num(D) << "\n";
                    o << "R" << R << "_tl" << J << " " << tb << " 0 50\n";
                    o << "B" << R << "_l" << J << " " << lam << " 0 V=2*V(" << tb << ")\n";
                } else {
                    o << "B" << R << "_l" << J << " " << lam << " 0 V=V(" << ta << ")\n";
                }
                const std::string dl = "n_" + R + "_dl" + J, ion = "n_" + R + "_io" + J;
                o << "B" << R << "_dl" << J << " " << dl << " 0 V=V(" << lam << ")*V(" << vdd << "," << pin << ")-(1-V(" << lam << "))*V(" << pin << "," << vss << ")\n";
                // sgn(Δ)·f(|Δ|) written as Δ·f(|Δ|)/|Δ| (finite at Δ = 0, with a non-zero slope there: ngspice's
                // derivative of sgn(Δ)·f(|Δ|) is 0 at exactly Δ = 0, which stalls it on an unloaded pin at a rail)
                {
                    const std::string D = "V(" + dl + ")", U = "abs(" + D + ")", VV = "V(" + vs_ + ")";
                    const std::string r = "min(1," + VV + "/max(" + U + ",1e-30))", mm = "min(" + U + "," + VV + ")";
                    o << "B" << R << "_io" << J << " " << ion << " 0 V=" << D << "*" << num(m.get("beta", 1.215e-3)) << "/(1+" << num(m.get("theta", 0.2016)) << "*" << VV << ")*(" << VV
                      << "*" << r << "-0.5*" << mm << "*" << r << ")*(1+" << num(m.get("lambda", 0.01527)) << "*" << U << ")\n";
                }
                const std::string io = "V(" + ion + ")";
                o << "B" << R << "_u" << J << " " << vdd << " " << pin << " I=0.5*(sqrt(" << io << "*" << io << "+" << num(eps * eps) << ")+" << io << ")\n";
                // the engine's exact linear split (Engine.cpp: a conductance β across the sink port, subtracted from the
                // law): the same total current, and a pin with no other load stays regular for ngspice too
                const double bta = m.get("beta", 1.215e-3);
                o << "R" << R << "_gs" << J << " " << pin << " " << vss << " " << num(1.0 / bta) << "\n";
                o << "B" << R << "_d" << J << " " << pin << " " << vss << " I=0.5*(sqrt(" << io << "*" << io << "+" << num(eps * eps) << ")-" << io << ")-" << num(bta)
                  << "*V(" << pin << "," << vss << ")\n";
            }
        } else if (p.type == "ad536") {
            // Engine.cpp "ad536": the same law as B-sources (Devices.hpp NlKind::Ad536)
            const auto m = *resolveModel(n, p.model);
            const std::string vin = nd(0), vneg = nd(2), cav = nd(3), db = nd(4), bo = nd(5), bi = nd(6), iout = nd(7), rl = nd(8), com = nd(9), vpos = nd(13);
            const double r1 = std::max(m.get("r1", 25e3), 1.0), r4 = std::max(m.get("r4", 50e3), 1.0);
            const double i0 = std::max(m.get("vfloor", 1e-3), 1e-9) / r4, dl = 0.01 * i0, i4m = std::max(m.get("i4max", 0.0), 0.0);
            const double k = std::max(m.get("mvdb", 3.0), 1e-3) * 1e-3 * 20.0 / std::log(10.0), iref = std::max(m.get("iref", 20e-6), 1e-12);
            const std::string dbv = "n_" + R + "_db", i3 = "(V(" + vpos + "," + cav + ")/" + num(r1) + ")";
            const std::string i3e = "(0.5*(" + i3 + "+sqrt(" + i3 + "*" + i3 + "+" + num(dl * dl) + ")))";
            const std::string raw = "((V(" + vin + "," + com + ")*V(" + vin + "," + com + ")*" + num(1.0 / (r4 * r4)) + "+" + num(i0 * i0) + ")/" + i3e + ")";
            o << "R" << R << "_in " << vin << " " << com << " " << num(std::max(m.get("rin", 16.67e3), 1.0)) << "\n";
            for (int k : {1, 10, 11, 12})
                if (nd(k) != com && nd(k) != "0") o << "R" << R << "_nc" << k << " " << nd(k) << " " << com << " 1e9\n";
            o << "R" << R << "_r1 " << vpos << " " << cav << " " << num(r1) << "\n";
            o << "B" << R << "_sq " << cav << " " << vneg << " I=" << (i4m > 0 ? raw + "/(1+" + raw + "/" + num(i4m) + ")" : raw) << "\n";
            o << "R" << R << "_db " << dbv << " " << com << " 1\n";
            o << "B" << R << "_lg " << com << " " << dbv << " I=-" << num(k) << "*ln(" << i3e << "/" << num(iref) << ")\n";
            o << "E" << R << "_db " << db << " 0 " << dbv << " 0 1\n";
            o << "G" << R << "_io " << vpos << " " << iout << " " << vpos << " " << cav << " " << num(2.0 / r1) << "\n";
            if (iout != rl) o << "R" << R << "_r2 " << iout << " " << rl << " " << num(std::max(m.get("r2", 25e3), 1.0)) << "\n";
            if (bo != "0" && bo != bi) {
                if (bi != com) o << "R" << R << "_bi " << bi << " " << com << " 1e8\n";
                o << "E" << R << "_bo " << bo << " 0 " << bi << " 0 1\n";
            }
            if (m.get("icc", 0.0) > 0 && vpos != vneg) o << "I" << R << "_cc " << vpos << " " << vneg << " DC " << num(m.get("icc", 0.0)) << "\n";
        } else if (p.type == "rms") {
            // Engine.cpp "rms": OA1 (high-gain E) with r0 = 1 kΩ, the squared current into a 1 Ω node (Devices.hpp RmsSq),
            // Q6 into CAP (RmsLog), the pin-2 junction string as a native diode, OA3 as OUT = V(cap) − vofs.
            const auto m = *resolveModel(n, p.model);
            const std::string in = nd(0), ib = nd(1), gnd = nd(2), sym = nd(3), vm = nd(4), cap = nd(5), out = nd(6), vp = nd(7);
            // The junctions run at the physical thermal voltage vtj (default: the 2252's, 6.1 mV/dB at a 35 °C chip); mvdb
            // is only the output slope, an output gain mvdb / (2·vtj·ln10/20) inside the OA3 buffer (exactly 1 for 6.1).
            const double vt = m.get("vtj", 0.0) > 0 ? m.get("vtj", 0.0) : 6.1e-3 * 10.0 / std::log(10.0);
            double gain = std::max(m.get("mvdb", 6.1), 1e-3) / (vt * 1e3 * std::log(10.0) / 10.0);
            if (std::abs(gain - 1.0) < 1e-12) gain = 1.0;
            const double nb = std::max(m.get("nb", 3.0), 1.0), r0 = 1e3, iref = 1e-3;
            const double isb = std::max(m.get("ibref", 24e-6), 1e-12) * std::exp(-m.get("vbias", 2.1) / (nb * vt));
            const double vofs = m.get("vofs", 1.4), kgeo = std::max(m.get("kgeo", 2.9), 1e-6), iflr = std::max(m.get("iflr", 16e-9), 0.0);
            const std::string x = "n_" + R + "_x", sq = "n_" + R + "_sq";
            o << "E" << R << "_x " << x << " 0 " << gnd << " " << in << " 1e7\n";
            o << "R" << R << "_0 " << in << " " << x << " " << num(r0) << "\n";
            o << "R" << R << "_sq " << sq << " 0 1\n";
            if (sym != gnd) o << "R" << R << "_sym " << sym << " " << gnd << " 1e7\n";
            const std::string mn = "D_" + R + "_bias";
            o << ".model " << mn << " D(IS=" << num(isb) << " N=" << num(nb * vt / 0.02586491704) << ")\n";
            o << "D" << R << "_bias " << ib << " " << vm << " " << mn << "\n";
            const std::string vx = "V(" + x + "," + gnd + ")";
            o << "B" << R << "_sq 0 " << sq << " I=(exp(2*(V(" << sym << "," << in << ")-" << num(m.get("vsym", 0.0)) << ")/" << num(vt) << ")*pow(max(-" << vx << ",0),2)+pow(max("
              << vx << ",0),2))*" << num(1.0 / (r0 * r0 * iref * iref)) << "+" << num(iflr * iflr / (iref * iref)) << "\n";
            o << "B" << R << "_q6 " << vp << " " << cap << " I=max(V(" << sq << "),0)*" << num(iref * iref * kgeo * kgeo) << "/(" << num(isb) << "*exp(V(" << ib << "," << vm << ")/"
              << num(nb * vt) << "))*exp(-(V(" << cap << "," << gnd << ")-" << num(vofs) << ")/" << num(vt) << ")\n";
            o << "B" << R << "_o " << out << " 0 V=" << (gain == 1.0 ? std::string() : num(gain) + "*(") << "V(" << cap << ")-" << num(vofs) << (gain == 1.0 ? "" : ")") << "\n";
            if (m.get("icc", 0.0) > 0 && vp != vm) o << "I" << R << "_cc " << vp << " " << vm << " DC " << num(m.get("icc", 0.0)) << "\n";
        } else if (p.type == "lamp" || p.type == "led" || p.type == "neon" || p.type == "cds" || p.type == "elpanel") {
            // Devices.hpp LampTh / DiodeLight / Neon + LightDrive / CdsRate + CdsCell; light and state nodes are 1 F.
            const auto m = *resolveModel(n, p.model);
            const std::string a = nd(0), b = nd(1), L = snode(p.light);
            const double gl = p.type == "cds" ? 0.0 : 1.0 / lightTau[p.light];
            if (p.type == "lamp") {
                const double vr = m.get("vrated", 28), ir = m.get("irated", 0.04), al = m.get("alpha", 1.214);
                const double tha = std::pow(std::clamp(m.get("rcold_ratio", 1.0 / 13.5), 1e-3, 1.0), 1.0 / al);
                const double rad = 1.0 / (1.0 - std::pow(tha, 4.0)), tau0 = std::max(m.get("tau", 0.02), 1e-5) * (al + 4.0 * rad);
                const std::string th = "n_" + R + "_theta", thc = "max(V(" + th + "),0.02)";
                const std::string g = "(" + num(ir / vr) + "*pow(" + thc + ",-" + num(al) + "))";
                o << "C" << R << "_th " << th << " 0 1\n";
                o << "B" << R << "_l " << a << " " << b << " I=V(" << a << "," << b << ")*" << g << "\n";
                o << "B" << R << "_th 0 " << th << " I=" << num(1.0 / tau0) << "*(V(" << a << "," << b << ")*V(" << a << "," << b << ")*" << g << "*" << num(1.0 / (vr * ir))
                  << "-(pow(" << thc << ",4)-" << num(std::pow(tha, 4.0)) << ")*" << num(rad) << ")\n";
                o << "B" << R << "_e 0 " << L << " I=" << num(gl) << "*exp(-" << num(m.get("tlambda", 20250) / m.get("thot", 2500)) << "*(1/" << thc << "-1))\n";
            } else if (p.type == "led") {
                std::string an = a;
                if (m.get("rs", 0) > 0) {
                    an = "n_" + R + "_j";
                    o << "R" << R << "_rs " << a << " " << an << " " << num(m.get("rs", 0)) << "\n";
                }
                const std::string mn = "D_" + spiceName(p.model);
                if (modelsDone.insert(mn).second) o << ".model " << mn << " D(IS=" << num(m.get("is", 1e-19)) << " N=" << num(m.get("n", 1.6)) << ")\n";
                o << "D" << R << " " << an << " " << b << " " << mn << "\n";
                o << "B" << R << "_e 0 " << L << " I=" << num(gl / std::max(m.get("iref", 0.01), 1e-9) * m.get("is", 1e-19)) << "*(exp(V(" << an << "," << b << ")/"
                  << num(m.get("n", 1.6) * kVt) << ")-1)\n";
            } else if (p.type == "elpanel") {
                // The ElLight normalisation k is the engine's (compile-time period average); export it via the same formula.
                const double cp = m.get("cp", 3e-9), rs = m.get("rs", 1e4), bb = m.get("b", 20.0), ve = m.get("veps", 0.5), dl = 1e-3;
                const double w = 2.0 * M_PI * m.get("fref", 1000.0), vpk = std::sqrt(2.0) * m.get("vref", 60.0);
                const std::complex<double> z(rs, -1.0 / (w * cp));
                const std::complex<double> ipk = vpk / z, vcpk = ipk / std::complex<double>(0.0, w * cp);
                double avg = 0;
                for (int q = 0; q < 512; ++q) {
                    const double ph = 2.0 * M_PI * (q + 0.5) / 512.0;
                    const double iv = std::abs(ipk) * std::sin(ph), vc = std::abs(vcpk) * std::sin(ph + std::arg(vcpk) - std::arg(ipk));
                    avg += std::sqrt(iv * iv * rs * rs + dl * dl) / rs * std::exp(-bb / (std::sqrt(std::sqrt(vc * vc + dl * dl) + ve)));
                }
                avg /= 512.0;
                const std::string mid = "n_" + R + "_m", vr = "V(" + a + "," + mid + ")", vc = "V(" + mid + "," + b + ")";
                o << "R" << R << "_s " << a << " " << mid << " " << num(rs) << "\nC" << R << "_p " << mid << " " << b << " " << num(cp) << "\n";
                o << "B" << R << "_e 0 " << L << " I=" << num(gl / std::max(avg, 1e-30) / rs) << "*sqrt(" << vr << "*" << vr << "+" << num(dl * dl) << ")*exp(-" << num(bb)
                  << "/sqrt(sqrt(" << vc << "*" << vc << "+" << num(dl * dl) << ")+" << num(ve) << "))\n";
            } else if (p.type == "neon") {
                const double vb = m.get("vstrike", 65), rl = m.get("rlamp", 30e3), sm = m.get("smooth", 2);
                const std::string vl = "V(" + a + "," + b + ")", z = "((abs(" + vl + ")-" + num(vb) + ")/" + num(sm) + ")";
                const std::string mag = "(" + num(sm / rl) + "*(max(" + z + ",0)+ln(1+exp(-abs(" + z + ")))))";
                o << "B" << R << "_n " << a << " " << b << " I=sgn(" << vl << ")*" << mag << "\n";
                o << "B" << R << "_e 0 " << L << " I=" << num(gl / std::max(m.get("iref", 1e-3), 1e-9)) << "*" << mag << "\n";
            } else {
                const double taul = std::max(m.get("tau_l", 2.0), 1e-3), tauh = m.get("tau_h", 0.0), mh = m.get("mh", 0.0), mr = m.get("mr", 0.0);
                const bool hist = tauh > 0 && (mh > 0 || mr > 0);
                const std::string nn = "n_" + R + "_n", hn = "n_" + R + "_h", hv = hist ? "max(V(" + hn + "),0)" : "0";
                o << "C" << R << "_n " << nn << " 0 1\nR" << R << "_n " << nn << " 0 " << num(taul) << "\n";
                if (hist) o << "C" << R << "_h " << hn << " 0 1\nR" << R << "_h " << hn << " 0 " << num(tauh) << "\nG" << R << "_h 0 " << hn << " " << nn << " 0 " << num(1.0 / tauh) << "\n";
                o << "B" << R << "_r 0 " << nn << " I=" << num(m.get("kr", 200)) << "/(1+" << num(hist ? mr : 0.0) << "*" << hv << ")*(" << num(std::max(p.couple, 0.0)) << "*max(V(" << L
                  << "),0)-V(" << nn << ")*abs(V(" << nn << ")))\n";
                const double ex = 2.0 * m.get("gamma", 1.0);
                o << "B" << R << "_c " << a << " " << b << " I=V(" << a << "," << b << ")*(" << num(1.0 / std::max(m.get("rdark", 10e6), 1.0)) << "+(pow(max(V(" << nn << "),0)+1e-4,"
                  << num(ex) << ")-" << num(std::pow(1e-4, ex)) << ")/" << num(std::max(m.get("ron", 1e3), 1e-3)) << "/(1+" << num(hist ? mh : 0.0) << "*" << hv << "))\n";
            }
        } else if (p.type == "transformer") {
            const std::string core = "n_" + R + "_core";
            for (std::size_t k = 0; k < p.turns.size(); ++k) {
                std::string a = nd(static_cast<int>(2 * k));
                const std::string b = nd(static_cast<int>(2 * k + 1)), K = std::to_string(k);
                if (k < p.windR.size() && p.windR[k] > 0) {
                    o << "R" << R << "_r" << K << " " << a << " n_" << R << "_r" << K << " " << num(p.windR[k]) << "\n";
                    a = "n_" + R + "_r" + K;
                }
                if (k < p.windL.size() && p.windL[k] > 0) {
                    o << "L" << R << "_l" << K << " " << a << " n_" << R << "_l" << K << " " << num(p.windL[k]);
                    if (const auto it = opt.icL.find(p.ref + "#l" + K); it != opt.icL.end()) o << " IC=" << num(it->second);
                    o << "\n";
                    a = "n_" + R + "_l" + K;
                }
                // sense source (branch current) + winding VCVS + MMF injection into the core node
                o << "V" << R << "_s" << K << " " << a << " n_" << R << "_w" << K << " 0\n";
                o << "E" << R << "_w" << K << " n_" << R << "_w" << K << " " << b << " " << core << " 0 " << num(p.turns[k]) << "\n";
                o << "F" << R << "_f" << K << " " << core << " 0 V" << R << "_s" << K << " " << num(p.turns[k]) << "\n";
            }
            if (p.lm > 0) {
                const std::string flux = "n_" + R + "_flux";
                const double ls = p.lsat > 0 ? p.lsat : 1e6;
                o << "G" << R << "_l 0 " << flux << " " << core << " 0 1\n";
                o << "C" << R << "_l " << flux << " 0 1\n";
                o << "B" << R << "_m " << core << " 0 I=-" << num(ls / p.lm) << "*sinh(max(min(V(" << flux << ")/" << num(ls) << ",40),-40))\n";
                if (p.hc > 0) o << "* " << R << ": core hysteresis (hc " << num(p.hc) << " A) is not exported; the deck is anhysteretic\n";
            } else {
                o << "R" << R << "_cg " << core << " 0 1e12\n";
            }
        } else if (p.type == "pentode") {
            const auto m = *resolveModel(n, p.model);
            const std::string pl = nd(0), g2 = nd(1), g1 = nd(2), k = nd(3);
            const std::string vg1 = "V(" + g1 + "," + k + ")", vpk = "V(" + pl + "," + k + ")", vg2 = "V(" + g2 + "," + k + ")";
            const double par = p.parallel;
            const double mu = m.get("mu", 8.7), ex = m.get("ex", 1.35), kg1 = m.get("kg1", 1460) / par, kg2 = m.get("kg2", 4500) / par, kp = m.get("kp", 48), kvb = m.get("kvb", 12);
            const std::string z = num(kp) + "*(" + num(1.0 / mu) + "+" + vg1 + "/max(" + vg2 + ",1e-3))";
            const std::string e1 = "(max(" + vg2 + ",1e-3)/" + num(kp) + "*(max(" + z + ",0)+ln(1+exp(-abs(" + z + ")))))";
            o << "B" << R << "_p " << pl << " " << k << " I=(" << vg2 << ">1e-3)*" << num(2.0 / kg1) << "*pow(max(" << e1 << ",0)," << num(ex) << ")*atan(" << vpk << "/" << num(kvb) << ")\n";
            // screen: space current Q·(1/kg2 + 2/kg1·(π/2 − atan(vpk/kvb))) (see Devices.hpp)
            o << "B" << R << "_s " << g2 << " " << k << " I=(" << vg2 << ">1e-3)*pow(max(" << e1 << ",0)," << num(ex) << ")*(" << num(1.0 / kg2) << "+"
              << num(2.0 / kg1) << "*(" << num(0.5 * M_PI) << "-atan(" << vpk << "/" << num(kvb) << ")))\n";
            const double gs = m.get("gscale", 1.5e-3) * par, gk = m.get("gknee", 0.08);
            const std::string zg = "(" + vg1 + "/" + num(gk) + ")";
            o << "B" << R << "_g " << g1 << " " << k << " I=" << num(gs) << "*pow(" << num(gk) << "*(max(" << zg << ",0)+ln(1+exp(-abs(" << zg << ")))),1.5)\n";
            if (m.get("cg1k", 0) > 0) o << "C" << R << "_gk " << g1 << " " << k << " " << num(par * m.get("cg1k", 0)) << "\n";
            if (m.get("cg1p", 0) > 0) o << "C" << R << "_gp " << g1 << " " << pl << " " << num(par * m.get("cg1p", 0)) << "\n";
            if (m.get("cpk", 0) > 0) o << "C" << R << "_pk " << pl << " " << k << " " << num(par * m.get("cpk", 0)) << "\n";
        } else if (p.type == "triode") {
            const auto m = *resolveModel(n, p.model);
            const std::string pl = nd(0), g = nd(1), k = nd(2);
            const std::string vpk = "V(" + pl + "," + k + ")", vgk = "V(" + g + "," + k + ")";
            const double par = p.parallel;
            const double mu = m.get("mu", 100), ex = m.get("ex", 1.4), kg1 = m.get("kg1", 1060) / par, kp = m.get("kp", 600), kvb = m.get("kvb", 300);
            // E1 = vpk/kp * softplus(kp*(1/mu + (vgk+vgo)/sqrt(kvb + vpk^2)));  Ip = 2*max(E1,0)^ex/kg1. vgo (contact-potential
            // offset) is written only when the card sets it, so vgo = 0 decks are unchanged.
            const double vgo = m.get("vgo", 0.0);
            const std::string vgp = vgo != 0.0 ? "(" + vgk + "+" + num(vgo) + ")" : vgk;
            const std::string z = num(kp) + "*(" + num(1.0 / mu) + "+" + vgp + "/sqrt(" + num(kvb) + "+" + vpk + "*" + vpk + "))";
            const std::string sp = "(max(" + z + ",0)+ln(1+exp(-abs(" + z + "))))";
            const std::string e1 = "(" + vpk + "/" + num(kp) + "*" + sp + ")";
            o << "B" << R << "_p " << pl << " " << k << " I=" << num(2.0 / kg1) << "*pow(max(" << e1 << ",0)," << num(ex) << ")\n";
            const double gs = m.get("gscale", 6.2e-4) * par, gk = m.get("gknee", 0.06);
            const std::string zg = "(" + vgk + "/" + num(gk) + ")";
            o << "B" << R << "_g " << g << " " << k << " I=" << num(gs) << "*pow(" << num(gk) << "*(max(" << zg << ",0)+ln(1+exp(-abs(" << zg << ")))),1.5)\n";
            if (m.get("cgk", 0) > 0) o << "C" << R << "_gk " << g << " " << k << " " << num(par * m.get("cgk", 0)) << "\n";
            if (m.get("cgp", 0) > 0) o << "C" << R << "_gp " << g << " " << pl << " " << num(par * m.get("cgp", 0)) << "\n";
            if (m.get("cpk", 0) > 0) o << "C" << R << "_pk " << pl << " " << k << " " << num(par * m.get("cpk", 0)) << "\n";
        }
    }
    // Full-node seeding (ENGINE_QUEUE 32): every netlist node ("#"-free key) given in opt.nodeset gets a .nodeset,
    // but only if the deck actually has that node. Engine-internal nodes ("#") have their own hints above (#x, #src1)
    // or no counterpart in the deck (e.g. a diode's Rs node is a native RS here), and a .nodeset on a missing vector
    // makes ngspice abort the run.
    {
        std::set<std::string> nodes;
        const std::string deck = o.str();
        std::string tok;
        for (char c : deck) {
            if (std::isspace(static_cast<unsigned char>(c)) || c == '(' || c == ')' || c == ',' || c == '=') {
                if (!tok.empty()) nodes.insert(tok);
                tok.clear();
            } else {
                tok.push_back(c);
            }
        }
        if (!tok.empty()) nodes.insert(tok);
        for (const auto& [name, v] : opt.nodeset) {
            if (name.find('#') != std::string::npos || !std::isfinite(v)) continue;
            const std::string sx = snode(name);
            if (sx != "0" && nodes.count(sx)) o << ".nodeset v(" << sx << ")=" << num(v) << "\n";
        }
        for (const auto& [name, v] : opt.ic) { // `.tran … uic` initial state (transformer flux nodes: n_<ref>_flux)
            const auto hash = name.find('#');
            if (!std::isfinite(v) || (hash != std::string::npos && name.compare(hash, std::string::npos, "#flux") != 0)) continue;
            const std::string sx = hash == std::string::npos ? snode(name) : "n_" + spiceName(name.substr(0, hash)) + "_flux";
            if (sx != "0" && nodes.count(sx)) o << ".ic v(" << sx << ")=" << num(v) << "\n";
        }
    }
    if (!opt.analysis.empty()) o << opt.analysis << "\n";
    return o.str();
}

bool importSpice(std::string_view text, Netlist& out, SpiceImportReport& report, std::string& error) {
    // Logical lines (join '+' continuations, drop comments except "*@pot" annotations).
    std::vector<std::string> lines;
    {
        std::istringstream in{std::string(text)};
        std::string l;
        bool first = true;
        while (std::getline(in, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (first) { // SPICE: the first line is the title
                first = false;
                if (!l.empty() && l[0] != '.' && !std::isalpha(static_cast<unsigned char>(l[0]))) continue;
                if (!l.empty() && l[0] == '*') continue;
            }
            if (l.empty()) continue;
            if (l[0] == '+' && !lines.empty()) {
                lines.back() += " " + l.substr(1);
                continue;
            }
            if (l.rfind("*@", 0) == 0) {
                lines.push_back(l);
                continue;
            }
            if (l[0] == '*') continue;
            if (auto c = l.find(';'); c != std::string::npos) l = l.substr(0, c);
            lines.push_back(l);
        }
    }
    Netlist n;
    n.id = "imported";
    n.name = "Imported SPICE netlist";
    std::map<std::string, std::pair<std::string, std::map<std::string, double>>> models;
    auto tok = [](const std::string& l) {
        // Whitespace, commas and parentheses all separate tokens ("D(IS=1n N=2)" -> D, IS=1n, N=2).
        std::vector<std::string> t;
        std::string cur;
        for (char c : l) {
            if (std::isspace(static_cast<unsigned char>(c)) || c == ',' || c == '(' || c == ')') {
                if (!cur.empty()) t.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) t.push_back(cur);
        // re-join "KEY = VALUE" written with spaces
        std::vector<std::string> o;
        for (std::size_t i = 0; i < t.size(); ++i) {
            if (t[i] == "=" && !o.empty() && i + 1 < t.size()) {
                o.back() += "=" + t[i + 1];
                ++i;
            } else if (!t[i].empty() && t[i].back() == '=' && i + 1 < t.size()) {
                o.push_back(t[i] + t[i + 1]);
                ++i;
            } else {
                o.push_back(t[i]);
            }
        }
        return o;
    };
    auto upper = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    };
    auto node = [](const std::string& s) { return s == "0" ? std::string("0") : s; };
    // First pass: models.
    for (const auto& l : lines) {
        if (upper(l).rfind(".MODEL", 0) != 0) continue;
        auto t = tok(l);
        if (t.size() < 3) continue;
        std::map<std::string, double> params;
        for (std::size_t i = 3; i < t.size(); ++i) {
            auto eq = t[i].find('=');
            if (eq == std::string::npos) continue;
            if (auto v = parseValue(t[i].substr(eq + 1), true)) params[upper(t[i].substr(0, eq))] = *v;
        }
        models[t[1]] = {upper(t[2]), params};
    }
    std::set<std::string> usedModels;
    bool ioAnnotated = false;
    for (const auto& l : lines) {
        if (l.rfind("*@io", 0) == 0) {
            // *@io INPUT OUTPUT
            auto t = tok(l.substr(4));
            if (t.size() >= 2) {
                n.io.input = node(t[0]);
                n.io.output = node(t[1]);
                ioAnnotated = true;
            }
            continue;
        }
        if (l.rfind("*@pot", 0) == 0) {
            // *@pot REF TOTAL PIN1 WIPER PIN3 TAPER CONTROL   (annotation for pots, which SPICE lacks)
            auto t = tok(l.substr(5));
            if (t.size() < 7) {
                report.warnings.push_back("ignored malformed pot annotation: " + l);
                continue;
            }
            Part p;
            p.ref = t[0];
            p.type = "pot";
            p.nodes = {node(t[2]), node(t[3]), node(t[4])};
            if (auto v = parseValue(t[1], true)) p.value.nominal = *v;
            p.hasValue = true;
            std::string e;
            readTaper(json(t[5]), p.taper, e, p.ref);
            p.control = t[6];
            if (!n.control(p.control)) {
                ControlDef cd;
                cd.id = cd.label = p.control;
                n.controls.push_back(cd);
            }
            n.parts.push_back(p);
            continue;
        }
        if (l[0] == '.') {
            const std::string u = upper(tok(l)[0]);
            if (u != ".MODEL" && u != ".END" && u != ".OPTIONS" && u != ".OPTION")
                report.warnings.push_back("ignored directive: " + l);
            continue;
        }
        auto t = tok(l);
        if (t.empty()) continue;
        const char kind = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0][0])));
        Part p;
        p.ref = t[0];
        auto needs = [&](std::size_t k) {
            if (t.size() < k) {
                report.warnings.push_back("ignored short line: " + l);
                return false;
            }
            return true;
        };
        switch (kind) {
            case 'R': case 'C': case 'L': {
                if (!needs(4)) continue;
                p.type = std::string(1, kind);
                p.nodes = {node(t[1]), node(t[2])};
                auto v = parseValue(t[3], true);
                if (!v) {
                    report.warnings.push_back("ignored (bad value): " + l);
                    continue;
                }
                p.value.nominal = *v;
                p.hasValue = true;
                break;
            }
            case 'V': {
                if (!needs(4)) continue;
                p.type = "V";
                p.nodes = {node(t[1]), node(t[2])};
                {
                    // "VIN" (any case) or a non-DC source is the audio input port.
                    std::string ur = upper(p.ref);
                    bool isAc = false;
                    for (std::size_t q = 3; q < t.size(); ++q) {
                        const std::string uq = upper(t[q]);
                        isAc |= uq == "AC" || uq == "SIN" || uq == "PWL" || uq == "PULSE";
                    }
                    if (ur.rfind("VIN", 0) == 0 || isAc) {
                        n.io.input = p.nodes[0];
                        continue;
                    }
                }
                std::size_t k = 3;
                if (upper(t[k]) == "DC" && t.size() > 4) ++k;
                auto v = parseValue(t[k], true);
                if (!v) {
                    report.warnings.push_back("non-DC source treated as the input port: " + l);
                    n.io.input = p.nodes[0];
                    continue;
                }
                p.value.nominal = *v;
                p.hasValue = true;
                if (p.nodes[1] != "0") {
                    report.warnings.push_back("floating voltage source not supported in v1: " + l);
                    continue;
                }
                break;
            }
            case 'D': {
                if (!needs(4)) continue;
                p.type = "D";
                p.nodes = {node(t[1]), node(t[2])};
                p.model = t[3];
                usedModels.insert(t[3]);
                break;
            }
            case 'Q': {
                if (!needs(5)) continue;
                p.nodes = {node(t[1]), node(t[2]), node(t[3])};
                p.model = t[4];
                auto it = models.find(t[4]);
                p.type = (it != models.end() && it->second.first == "PNP") ? "pnp" : "npn";
                usedModels.insert(t[4]);
                break;
            }
            case 'E': case 'G': {
                if (!needs(6)) continue;
                p.type = kind == 'E' ? "vcvs" : "vccs";
                p.nodes = {node(t[1]), node(t[2]), node(t[3]), node(t[4])};
                if (auto v = parseValue(t[5], true)) p.gain = *v;
                break;
            }
            default:
                report.warnings.push_back("unsupported element (v1 subset is R C L V D Q E G + *@pot): " + l);
                continue;
        }
        n.parts.push_back(std::move(p));
    }
    for (const auto& name : usedModels) {
        auto it = models.find(name);
        if (it == models.end()) {
            if (!builtinModels().count(name)) {
                error = "model " + name + " is neither defined in the netlist nor in the built-in library";
                return false;
            }
            continue;
        }
        DeviceModel d;
        d.name = name;
        const auto& [k, prm] = it->second;
        d.kind = k == "D" ? "diode" : (k == "NPN" ? "npn" : (k == "PNP" ? "pnp" : ""));
        if (d.kind.empty()) {
            report.warnings.push_back("model " + name + " of type " + k + " not supported");
            continue;
        }
        for (const auto& [pk, pv] : prm) {
            std::string lk = pk;
            for (auto& c : lk) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (modelDefaults(d.kind).count(lk)) d.p[lk] = pv;
            else report.warnings.push_back("model " + name + ": parameter " + pk + " ignored (not in the v1 device law)");
        }
        n.models[name] = d;
    }
    if (!ioAnnotated) {
        // Output: a node literally called out / n_out / vout, else the input stays and the user picks one.
        for (const auto& p : n.parts)
            for (const auto& nd : p.nodes) {
                std::string l = nd;
                for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (l == "out" || l == "n_out" || l == "vout" || l == "output") n.io.output = nd;
            }
        if (!n.part("__none__")) {
            bool found = false;
            for (const auto& p : n.parts)
                for (const auto& nd : p.nodes) found |= nd == n.io.output;
            if (!found) report.warnings.push_back("no output node found: add '*@io <input> <output>' to the netlist");
        }
    }
    // Round-trip through JSON to run the full validator.
    json j = toJson(n);
    return parseNetlist(j, out, error);
}

double sourceScaleAt(const Netlist& n, const Part& p, const std::vector<double>& ctl) {
    if (p.scaleControl.empty()) return 1.0;
    return sourceScale(controlPosition(n, p.scaleControl, ctl), p.scaleMin, p.scaleMax);
}

std::vector<std::string> lintNetlist(const nlohmann::json& raw, const Netlist& n) {
    std::vector<std::string> w;
    // Units: look at the value text as written.
    std::map<std::string, std::string> rawValue;
    if (raw.is_object() && raw.contains("parts") && raw["parts"].is_array())
        for (const auto& p : raw["parts"]) {
            if (!p.is_object() || !p.contains("ref")) continue;
            const auto& v = p.contains("value") ? p["value"] : json();
            const json& nom = v.is_object() && v.contains("nominal") ? v["nominal"] : v;
            if (nom.is_string()) rawValue[p["ref"].get<std::string>()] = nom.get<std::string>();
            else if (nom.is_number()) rawValue[p["ref"].get<std::string>()] = nom.dump();
        }
    auto bareNumber = [](const std::string& s) {
        if (s.empty()) return false;
        for (char ch : s)
            if (!(std::isdigit(static_cast<unsigned char>(ch)) || ch == '.' || ch == 'e' || ch == 'E' || ch == '-' || ch == '+')) return false;
        return true;
    };
    for (const auto& p : n.parts) {
        if (p.type == "C") {
            auto it = rawValue.find(p.ref);
            // A plain decimal with no unit and no exponent ("0.01", "0.047") is the classic µF-in-farads slip; values
            // written in scientific notation ("6.65e-11") are deliberate SI and pass.
            if (it != rawValue.end() && bareNumber(it->second) && it->second.find_first_of("eE") == std::string::npos) {
                const double x = std::atof(it->second.c_str());
                if (x >= 1e-6 && x < 1.0)
                    w.push_back(p.ref + ": capacitor value \"" + it->second + "\" has no unit and is read as " + formatValue(x, "F") +
                                "; write it with a unit (\"" + formatValue(x * 1e-6, "F") + "\" if the schematic means µF)");
            }
            // 10 mF: solid-state power-amp reservoirs are real at 3300-10000 µF (the JC-120's C85/C86 3300 µF / 50 V);
            // a "m"/"u" slip lands far above that.
            if (p.value.nominal > 10e-3) w.push_back(p.ref + ": capacitor of " + formatValue(p.value.nominal, "F") + " (above 10 mF) is almost certainly a unit error");
        }
        if (p.type == "R" || p.type == "pot") {
            // "1m" is 1 mΩ in SI (milli); a schematic's "1M" is meg. A resistor written with a lone lower-case m and
            // below 1 Ω is almost always that slip (a 1 MΩ feedback resistor turned into a short).
            auto it = rawValue.find(p.ref);
            // also the RKM form "1m2" (1.2 mΩ) where the drawing means 1M2
            auto milliSlip = [](const std::string& v) {
                if (v.empty() || v.find("meg") != std::string::npos) return false;
                if (v.back() == 'm') return true;
                const auto k = v.find('m');
                return k != std::string::npos && k > 0 && k + 1 < v.size() && std::isdigit(static_cast<unsigned char>(v[k - 1])) &&
                       std::isdigit(static_cast<unsigned char>(v[k + 1]));
            };
            if (it != rawValue.end() && milliSlip(it->second) && p.value.nominal < 1.0)
                w.push_back(p.ref + ": resistor value \"" + it->second + "\" is read as " + formatValue(p.value.nominal, "Ω") +
                            " (m = milli); write it with M (\"1M2\") or meg if the schematic means megohms");
        }
        if (p.type == "L" && p.value.nominal > 100.0)
            w.push_back(p.ref + ": inductor of " + formatValue(p.value.nominal, "H") + " (above 100 H) is almost certainly a unit error");
    }
    // Node names that differ only in case ("nC2" / "nc2"): two nets to the engine, easy to mistake for one when read,
    // and one node to plain SPICE (exportSpice renames the later one, ENGINE_QUEUE 32).
    {
        std::map<std::string, std::string> folded;
        std::set<std::string> warned; // one warning per spelling
        for (const auto& p : n.parts)
            for (const auto& nd : p.nodes) {
                std::string f = nd;
                for (auto& c : f) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                const auto [it, fresh] = folded.emplace(f, nd);
                if (!fresh && it->second != nd && f != "gnd" && f != "ground" && warned.insert(nd).second)
                    w.push_back("nodes \"" + it->second + "\" and \"" + nd + "\" differ only in case (" + p.ref +
                                "); they are two nets, check that this is meant");
            }
    }
    // Mains nodes: every non-ground node of a V source with an AC term.
    std::set<std::string> acNodes;
    for (const auto& p : n.parts)
        if (p.type == "V" && std::abs(p.acAmp) >= 20.0 && p.acFreq >= 40.0 && p.acFreq <= 70.0) // mains windings, not LFO/test generators
            for (const auto& nd : p.nodes)
                if (nd != "0" && nd != "gnd") acNodes.insert(nd);
    static const std::set<std::string> kMainsOk = {"V", "transformer", "D", "Dpair", "vacdiode", "fuse", "L"};
    for (const auto& p : n.parts) {
        bool touches = false;
        for (const auto& nd : p.nodes) touches |= acNodes.count(nd) > 0;
        if (!touches || kMainsOk.count(p.type)) continue;
        if (p.type == "R" && p.value.nominal <= 1000.0) continue; // winding / limiting resistance
        if (p.type == "R" && p.ref.rfind("RKICK", 0) == 0) continue;
        w.push_back(p.ref + " (" + p.type + ") connects to an AC-driven node (" + [&] {
            for (const auto& nd : p.nodes) if (acNodes.count(nd)) return nd;
            return std::string();
        }() + "): check for a node-name collision with a mains winding");
    }
    // Preamp plate loads on the raw reservoir (three instances: Supro, AC15, and a hum path in the 5E3 drafts). The
    // reservoir is the node a rectifier charges directly from a mains winding; triode plate resistors belong on a
    // filtered rail further down the chain, or ripple rides straight into the signal (a direct-coupled stage
    // passes it to the phase inverter and the output tubes amplify it differentially).
    {
        std::set<std::string> reservoir;
        for (const auto& p : n.parts) {
            if ((p.type != "D" && p.type != "vacdiode" && p.type != "rectifier") || p.nodes.size() < 2) continue;
            const std::string& a = p.nodes[0];
            const std::string& k = p.nodes[p.nodes.size() - 1];
            if (acNodes.count(a)) reservoir.insert(k);
        }
        std::set<std::string> plates;
        for (const auto& p : n.parts)
            if ((p.type == "triode" || p.type == "pentode") && !p.nodes.empty()) plates.insert(p.nodes[0]);
        for (const auto& p : n.parts) {
            if (p.type != "R" || p.nodes.size() != 2) continue;
            for (int s2 = 0; s2 < 2; ++s2)
                if (reservoir.count(p.nodes[static_cast<std::size_t>(s2)]) && plates.count(p.nodes[static_cast<std::size_t>(1 - s2)]))
                    w.push_back(p.ref + ": plate load from " + p.nodes[static_cast<std::size_t>(1 - s2)] + " goes straight to the rectifier's reservoir node " +
                                p.nodes[static_cast<std::size_t>(s2)] + " (unfiltered ripple); preamp plates belong on a filtered rail");
        }
    }
    // Push-pull output transformers without winding capacitance (ENGINE_QUEUE item 12): an ideal transformer's
    // leakage rings at the plate edges and the engine and ngspice then resolve large-signal edges differently (the
    // Supro's driven gate was 13.5 % until a 470 pF + 2.2 kΩ network went from each plate end to the centre tap).
    for (const auto& p : n.parts) {
        if (p.type != "transformer" || p.nodes.size() < 6 || p.nodes[1] != p.nodes[2]) continue; // centre-tapped primary
        for (int end : {0, 3}) {
            const std::string& pl = p.nodes[static_cast<std::size_t>(end)];
            bool hasC = false;
            for (const auto& q : n.parts)
                if (q.type == "C" && std::find(q.nodes.begin(), q.nodes.end(), pl) != q.nodes.end()) hasC = true;
            if (!hasC) w.push_back(p.ref + ": plate end " + pl + " has no winding capacitance (add ~470 pF + 2.2 kΩ to the centre tap, as on the other amps)");
        }
    }
    return w;
}

} // namespace rig::ce

