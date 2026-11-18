#include "circuit/CircuitOversampler.hpp"
#include "CircuitUnit.hpp"

#include "Denormals.hpp"
#include "Engine.hpp"
#include "LaneBlock.hpp"
#include "../core/RtPool.hpp"
#include "ModelLibrary.hpp"
#include "library/EmbeddedLibrary.hpp"

#include "../dsp/AmpModeling.hpp"
#include "../dsp/ComponentModel.hpp"
#include "../dsp/LeslieRotors.hpp"
#include "../dsp/OutputTrim.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cfenv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <thread>

namespace rig::ce {

namespace {
std::atomic<bool>& noiseSetting() noexcept {
    static std::atomic<bool> enabled{[] {
        const char* v = std::getenv("HEADROOM_CIRCUIT_NOISE");
        return v && std::string_view(v) == "1";
    }()};
    return enabled;
}
}
bool circuitNoiseEnabled() noexcept { return noiseSetting().load(std::memory_order_relaxed); }
void setCircuitNoiseEnabled(bool enabled) noexcept { noiseSetting().store(enabled, std::memory_order_relaxed); }

constexpr double kSettleMs = 450.0; // mains settle for circuits with AC sources (CompileOptions::settleMs)
// CE_DOUBLESETTLE=1: every channel resets (and settles) again after its compile at the same controls, as before §10.52.
// CE_LOADPROBE=1: print a state digest before and after that second reset (developer).
const bool kDoubleSettle = [] {
    const char* s = std::getenv("CE_DOUBLESETTLE");
    return s && std::string_view(s) == "1";
}();
const bool kLoadProbe = std::getenv("CE_LOADPROBE") != nullptr;

// Per-instance noise stream (docs §3.9): two units of the same netlist and serial (two nominal Twins in one rig, or in
// two plug-in instances) must not hiss in lockstep. Each unit takes a value from a process-wide counter mixed with a
// process-start salt, once, at construction; only the noise seed uses it (the circuit is unchanged). Tests set
// HEADROOM_NOISE_FIXED_SEED for the old reproducible seed (serial and channel only).
namespace {
std::atomic<std::uint64_t> gNoiseInstances{0};
const std::uint64_t kNoiseSalt = [] {
    std::random_device rd;
    return (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd()) ^
           static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}();
std::uint64_t newNoiseInstance() {
    if (std::getenv("HEADROOM_NOISE_FIXED_SEED")) return 0;
    std::uint64_t z = kNoiseSalt + 0x9E3779B97F4A7C15ull * (gNoiseInstances.fetch_add(1, std::memory_order_relaxed) + 1); // splitmix64
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return (z ^ (z >> 31)) | 1; // never 0 (0 = the fixed seed)
}
} // namespace


using json = nlohmann::json;

const std::vector<UnitBinding>& unitBindings() {
    static const std::vector<UnitBinding> b = {
        {15000, "ibanez-ts808", ""},
        {30000, "fender-ab763-twin-reverb", ""}, // the complete amp: supply, reverb, vibrato, power stage, speakers
        // Wave A: complete amps (supply, preamp, PI, power stage, output transformer, speakers), behind the flag
        {172000, "fender-ab763-deluxe-reverb", ""},
        {62000, "fender-5f6a-bassman", ""},
        {34000, "marshall-1959-plexi", ""},
        {92000, "marshall-1959-plexi-variac", ""}, // the same 1959 circuit with its mains dropped to ~90 VAC
        {101000, "marshall-jcm800-2203", ""},       // the JMP 2203 (1975-80) is the same circuit the JCM800 2203 continued
        {56000, "marshall-jcm800-2203", ""},
        {38000, "vox-ac30-tb", ""},
        // Wave B pedals
        {17000, "boss-ds1", ""},             // DS1-A circuit (1994 onward); the unit is labelled accordingly
        {43000, "ehx-big-muff-pi", ""},
        {41000, "dallas-fuzz-face-si", ""},
        {166000, "dallas-fuzz-face-ge", ""},
        {42000, "proco-rat2", ""},
        // Wave C
        {75000, "mxr-phase90-script", ""},
        {71000, "dunlop-crybaby-gcb95", ""},
        {72000, "vox-v846-clyde-mccoy", ""},
        {48000, "mxr-dyna-comp", ""},
        {20000, "ehx-small-stone", ""},
        {164000, "shinei-univibe", ""},
        {74000, "ehx-electric-mistress", ""},
        {142000, "roland-dimension-d", ""},
        {78000, "roland-re201", ""},
        {82000, "ehx-deluxe-memory-man", ""},
        {29000, "boss-ce1", ""},
        {123000, "urei-1176ln", ""},
        {124000, "teletronix-la2a", ""},
        // Wave D (netlists SPICE-gated in test_circuit_engine)
        {40000, "dallas-rangemaster", ""},
        {65000, "mxr-m133-micro-amp", ""},
        {169000, "fuzz-face-vintage-ge", ""},
        {174000, "interfax-harmonic-percolator", ""},
        {167000, "ibanez-ts9dx", ""},
        {79000, "fender-5e3-tweed-deluxe", ""}, // V1 split cathodes + no PI-grid cut: 0 Newton failures
        {87000, "hiwatt-dr103", ""},          // Hiwatt DR103 (wave D), schematic-exact (no buffers)
        {152000, "supro-1624t-dual-tone", ""},     // OT winding capacitance added: driven 0.014 % vs ngspice
        {21000, "dunlop-crybaby-21000", ""},       // Cry Baby (wave E): the GCB-95 board, 399 Hz-2.24 kHz sweep
        {111000, "butler-tube-driver", ""},        // BK Butler Tube Driver (wave E): real starved 12AX7 cascade
        {122000, "soldano-slo100-122000", ""},     // Soldano SLO-100: OD loop miswire fixed, B+ to chart
        {134000, "peavey-5150-134000", ""},       // Peavey 5150 Block Letter: presence/resonance NFB shelves from the 1992 drawing
        {35000, "boss-oc2-35000", ""},            // Boss OC-2 Octave: CD4013 divider, JFET synchronous-detector voices
        {133000, "ssl-4000e-dynamics-133000", ""}, // SSL 4000E channel dynamics (82E10 Rev 15a): feed-forward compressor + gate/expander on one dbx 2151
        {130000, "ssl-4000g-bus-comp-130000", ""}, // SSL 4000G quad bus compressor (82E26 x4 + 82E27, 1990-92 G sheets): linked stereo, 202X audio + side-chain VCAs
        {125000, "dbx-160vu-125000", ""},          // dbx 160 (AKG-era sheet, dwg 260182): VCA (200) + RMS (208) feed-forward, 1:1-inf:1 hard knee
        {170000, "eae-surveyor-170000", ""},       // EAE Surveyor / Intersound IVP (wave E5): Tube Voice push-pull, 42TM018 fitted
        {165000, "boss-hm2-heavy-metal", ""},      // Boss HM-2 (wave D): clip-stage bias fixed, 2SK30A/M5218L cards
        {13000, "4ms-tremulus-lune-13000", ""},    // optical tremolo (wave E3): 4ms Tremulus Lune, single source (Aion)
        {47000, "mu-tron-iii-47000", ""},          // Mu-Tron III (wave E3): GEO/GGG replica, Keen GEO 117 switch notes
        {27000, "fender-6g15-27000", ""},          // Fender 6G15 Reverb (H-FA schematic + layout): 6K6GT (RCA-fitted card) into a 4AB3C1B tank
        {176000, "vox-ac15-handwired", ""},        // Vox AC15 (wave D): EF86 + Top Boost-era Vibravox, full rate
        {54000, "ampeg-svt-1969", ""},             // Ampeg SVT 1969 (wave E): power transformer-fed supply, starts at its periodic steady state (docs §3.10)
        {46000, "boss-mt2-metal-zone", ""},        // Boss MT-2 (wave E pass 23): Q010 emitter-follower gyrator read from the 1991 service notes; VB cut, DK solver
        {45000, "tech21-sansamp-gt2", ""},         // SansAmp GT2 (wave E pass 23): Luja trace + Tonepad throw positions, three 3-position selectors
        {84000, "marshall-jh1-jackhammer", ""},    // Marshall JH-1 Jackhammer: factory drawing Rev 2 24.6.98 (single source); Wien-bridge contour
        {121000, "ssl-4000e-eq-121000", ""},       // SSL 4000 E channel EQ, card 82E242 (black knob): T82242.70 + Nov 85 ECO; centre-tapped boost/cut pots via pot spans
        {144000, "mutron-biphase-144000", ""},     // Mu-Tron Bi-Phase (ARP set, rev 2 1-8-79): two 6-stage photocell phasors, two sweep generators; not a barber-pole
        {91000, "marshall-1959rr-91000", ""},      // "Hot-Rod Plexi" = Marshall's factory 1959RR cascade (1959RR-01-60-02 Iss 2); EL34 pairs folded
        {163000, "soldano-slo100-mk2-163000", ""}, // SLO-100 MK II: Rev E 11/2019 KiCad redraw, the only source; 6L6 pairs folded, bdf2 tol 8
        {81000, "bogner-uberschall-81000", ""},     // Bogner Uberschall: two independent traces (2007 Rev A primary, 2012 for the supply); EL34 pairs folded, bdf2 tol 8
        {49000, "roland-jc-120", ""},               // Roland JC-120, 1982 revision: both discrete power amps, MN3007 chorus/vibrato, stereo spkL/spkR (~125-130 % of a core)
        {151000, "friedman-be100-151000", ""},     // Friedman BE-100 V2 Rev 1.5 (lossless redraw, single source): Clean / BE / HBE on gainMode
        {162000, "bogner-ecstasy-101b-162000", ""}, // Bogner Ecstasy 101B (blueguitar traces + schematicheaven OD redraw): Ch1 clean / Ch2 / Ch3 on gainMode; EL34 pairs folded
        {16000, "mesa-dual-rectifier-16000", ""},  // Mesa Dual Rectifier 3-channel Solo Head (catalogue: 3-Channel); Mode = Clean/Raw/Vintage/Modern (~1.4-1.8 cores)
        {96000, "mesa-dual-rectifier-revc-96000", ""},  // Dual Rectifier Rev C (first production): the 16000 netlist with the documented Rev C stack, presence and cathode deltas (wave-e notes)
        {24000, "fairchild-670-24000", ""},        // Fairchild 670 (factory A-96039): both channels, S301 L-R / lateral-vertical matrix, 6386 two-triode card, tube-regulated supply
        {80000, "orange-or120-1972-80000", ""},    // Orange OR120 Graphic MkII (1972 redraw + 1973 parts list): FAC stepped low cut, H.F. Drive boost trap, cathodyne PI; EL34 pairs folded, bias -46.9 V (owner-measured)
        {31000, "leslie-122-31000", "leslie-rotors"}, // Leslie 122: the circuit amp + 800 Hz dividing network feed the horn / drum rotor block (dsp/LeslieRotors.hpp)
        // circuit-only units (new catalogue entries, no legacy model)
        {179000, "klon-centaur", "", true},          // "Transparent Charge-Pump Overdrive"
        {180000, "marshall-bluesbreaker", "", true}, // "British Blues-Breaker Overdrive"           // LM308 on the real R10/C11 filtered rail (rail-node op-amp)
    };
    return b;
}

std::string libraryNetlist(std::string_view id) {
    for (const auto& e : library::kEmbedded) {
        if (id != e.id) continue;
        if (const char* dir = std::getenv("HEADROOM_CIRCUIT_DIR")) {
            std::ifstream f(std::string(dir) + "/" + e.file, std::ios::binary);
            if (f) {
                std::stringstream s;
                s << f.rdbuf();
                return s.str();
            }
        }
        return e.json;
    }
    return {};
}

std::vector<std::string> libraryIds() {
    std::vector<std::string> v;
    for (const auto& e : library::kEmbedded) v.emplace_back(e.id);
    return v;
}

static const UnitBinding* bindingFor(int32_t t) noexcept {
    for (const auto& b : unitBindings())
        if (b.tableId == t) return &b;
    return nullptr;
}

bool hasCircuitFor(int32_t t) noexcept { return bindingFor(t) != nullptr; }

// Clean cut (2026-11-09): every bound unit runs on the circuit engine. There is no user-facing switch and no
// fallback to the behavioural model. Units still in kPendingValidation are bound for development only and stay
// on their old model until they pass the SPICE gate. CPU is never a reason to hold a unit back (the user's rule):
// the top bar shows the engine load and the CPU work continues without touching quality.
// HEADROOM_LEGACY_MODELS=1 is a tool-only hook so circuit_ab can render the old model for A/B reports.
// 2026-11-10: the Small Stone (20000), RE-201 (78000) and Deluxe Memory Man (82000) passed the SPICE gate at the
// normal thresholds and left the list; no unit is pending now (list a table id here to hold one back again).
// Developer override for rate experiments (aliasing gate, tests/test_aliasing.cpp): a floor on every unit's internal
// rate. Read once at load, never on the audio thread.
static const double kMinRateOverride = [] {
    const char* v = std::getenv("HEADROOM_MINRATE");
    return v ? std::atof(v) : 0.0;
}();
static constexpr int kMaxOs = CircuitOversampler::kMaxFactor;

// Unit-to-unit variation ("serial" in the model JSON; docs/CIRCUIT_ENGINE.md §8). Serial 0 is the nominal, golden
// unit (bit-identical to no variation). Any other serial draws every constant part value within its published
// tolerance, and gives every valve its own copy of its card with the spread real tubes show: cathode-current
// constant kg1 ±15 % and µ ±5 % (3σ). The draw is deterministic per (serial, part ref), so a preset recalls the
// same unit.
static std::uint64_t splitmix(std::uint64_t& s) {
    std::uint64_t z = (s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
static double unitNormal(std::uint64_t& s) { // standard normal, clipped to ±3σ
    const double u1 = (static_cast<double>(splitmix(s) >> 11) + 0.5) * 0x1.0p-53;
    const double u2 = static_cast<double>(splitmix(s) >> 11) * 0x1.0p-53;
    return std::clamp(std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2), -3.0, 3.0);
}
static void applyUnitVariation(Netlist& n, std::uint64_t serial) {
    if (serial == 0) return;
    for (auto& p : n.parts) {
        std::uint64_t s = serial * 0x100000001b3ull;
        for (char ch : p.ref) s = (s ^ static_cast<unsigned char>(ch)) * 0x100000001b3ull;
        if (p.tolerance > 0 && !p.value.variable() && p.value.nominal != 0.0)
            p.value.nominal *= 1.0 + p.tolerance * unitNormal(s) / 3.0;
        if (p.type == "triode" || p.type == "pentode") {
            auto m = resolveModel(n, p.model);
            if (!m) continue;
            DeviceModel own = *m;
            own.name = p.model + "#" + p.ref;
            if (own.p.count("kg1")) own.p["kg1"] *= 1.0 + 0.15 * unitNormal(s) / 3.0;
            if (own.p.count("mu")) own.p["mu"] *= 1.0 + 0.05 * unitNormal(s) / 3.0;
            n.models[own.name] = own;
            p.model = own.name;
        }
    }
}

// Topology edits from the component page's circuit editor (docs/CIRCUIT_ENGINE.md §6): a small diff over the unit's
// netlist, stored in the model JSON next to the value edits ("circuit": {"topology": {...}, "parts": {...}}), so a
// preset or My Component keeps only the user's changes and a factory revision still reaches every part they did not
// touch. Applied in this order, each step on the result of the one before:
//   "rename": {"old": "new"}   nets renamed everywhere (parts, io, probes, cuts, layout hints); a rename onto an
//                              existing net merges the two. Simultaneous (no chains): node' = rename[node] or node.
//   "remove": ["REF", ...]     parts deleted (and dropped from multirate / cut-load lists)
//   "wire":   {"REF": [nodes]} an existing part's full pin list (connect, disconnect = a fresh net, move a pin)
//   "add":    {"REF": {part}}  new parts, the netlist's own part JSON without "ref" (type, nodes, value, model, ...)
// Then every list that names a node or part no longer present is pruned, and the result is parsed and validated like
// any netlist. Errors name the edit. Nothing here runs on the audio thread.
static bool applyTopology(Netlist& n, const json& t, std::string& err) {
    if (!t.is_object()) {
        err = "topology edits must be an object";
        return false;
    }
    for (auto it = t.begin(); it != t.end(); ++it)
        if (it.key() != "rename" && it.key() != "remove" && it.key() != "wire" && it.key() != "add") {
            err = "unknown topology section \"" + it.key() + "\"";
            return false;
        }
    auto isGround = [](const std::string& s) { return s == "0" || s == "gnd" || s == "GND" || s == "ground"; };
    json j = toJson(n);
    try {
        // rename
        std::map<std::string, std::string> rn;
        if (t.contains("rename")) {
            if (!t["rename"].is_object()) {
                err = "rename must map old net names to new ones";
                return false;
            }
            for (auto it = t["rename"].begin(); it != t["rename"].end(); ++it) {
                if (!it.value().is_string() || it.value().get<std::string>().empty()) {
                    err = "rename " + it.key() + ": the new name must be a non-empty string";
                    return false;
                }
                const std::string to = it.value().get<std::string>();
                if (isGround(it.key()) || isGround(to)) {
                    err = "rename " + it.key() + ": ground cannot be renamed, and no net can be renamed onto ground (wire the pins to 0 instead)";
                    return false;
                }
                rn[it.key()] = to;
            }
        }
        auto ren = [&](const std::string& s) {
            auto f = rn.find(s);
            return f == rn.end() ? s : f->second;
        };
        if (!rn.empty()) {
            for (auto& pj : j["parts"])
                for (auto& nd : pj["nodes"]) nd = ren(nd.get<std::string>());
            for (const char* k : {"input", "output", "outputR", "inputR"})
                if (j["io"].contains(k)) j["io"][k] = ren(j["io"][k].get<std::string>());
            if (j.contains("probes"))
                for (auto& p : j["probes"]) p = ren(p.get<std::string>());
            if (j.contains("cuts"))
                for (auto& c : j["cuts"]) {
                    if (c.is_string()) c = ren(c.get<std::string>());
                    else if (c.is_object() && c.contains("node")) c["node"] = ren(c["node"].get<std::string>());
                }
            if (j.contains("multirate") && j["multirate"].contains("slow") && j["multirate"]["slow"].contains("nodes"))
                for (auto& x : j["multirate"]["slow"]["nodes"]) x = ren(x.get<std::string>());
            if (j.contains("layout") && j["layout"].is_object())
                for (const char* k : {"columns", "rows"})
                    if (j["layout"].contains(k) && j["layout"][k].is_object()) {
                        json m = json::object();
                        for (auto it = j["layout"][k].begin(); it != j["layout"][k].end(); ++it) m[ren(it.key())] = it.value();
                        j["layout"][k] = m;
                    }
        }
        // nets (and optical light nodes) present before the edits: only names that vanish because of them are pruned;
        // lists may also name engine-internal nodes ("REF#light") that no part lists, and those stay
        auto netsOf = [&](std::set<std::string>& nodes, std::set<std::string>* refs) {
            for (const auto& pj : j["parts"]) {
                if (refs) refs->insert(pj["ref"].get<std::string>());
                for (const auto& nd : pj["nodes"]) nodes.insert(nd.get<std::string>());
                if (pj.contains("light") && pj["light"].is_string()) nodes.insert(pj["light"].get<std::string>());
            }
        };
        std::set<std::string> had;
        netsOf(had, nullptr);
        auto findPart = [&](const std::string& ref) -> json* {
            for (auto& pj : j["parts"])
                if (pj["ref"] == ref) return &pj;
            return nullptr;
        };
        // remove
        std::set<std::string> removed;
        if (t.contains("remove")) {
            if (!t["remove"].is_array()) {
                err = "remove must be a list of part refs";
                return false;
            }
            for (const auto& r : t["remove"]) {
                const std::string ref = r.is_string() ? r.get<std::string>() : r.dump();
                if (!findPart(ref)) {
                    err = "remove " + ref + ": no such part";
                    return false;
                }
                removed.insert(ref);
                json keep = json::array();
                for (auto& pj : j["parts"])
                    if (pj["ref"] != ref) keep.push_back(std::move(pj));
                j["parts"] = std::move(keep);
            }
        }
        // wire
        auto readNodes = [&](const json& v, const std::string& where, std::vector<std::string>& out) {
            if (!v.is_array() || v.empty()) {
                err = where + ": nodes must be a non-empty list of net names";
                return false;
            }
            for (const auto& x : v) {
                if (!x.is_string() || x.get<std::string>().empty()) {
                    err = where + ": every pin needs a net name";
                    return false;
                }
                out.push_back(x.get<std::string>());
            }
            return true;
        };
        if (t.contains("wire")) {
            if (!t["wire"].is_object()) {
                err = "wire must map part refs to their pin lists";
                return false;
            }
            for (auto it = t["wire"].begin(); it != t["wire"].end(); ++it) {
                json* pj = findPart(it.key());
                if (!pj) {
                    err = "wire " + it.key() + ": no such part" + (removed.count(it.key()) ? " (it was removed)" : "");
                    return false;
                }
                std::vector<std::string> nodes;
                if (!readNodes(it.value(), "wire " + it.key(), nodes)) return false;
                if (nodes.size() != (*pj)["nodes"].size()) {
                    err = "wire " + it.key() + ": " + std::to_string(nodes.size()) + " pins given, the part has " + std::to_string((*pj)["nodes"].size());
                    return false;
                }
                (*pj)["nodes"] = nodes;
            }
        }
        // add
        if (t.contains("add")) {
            if (!t["add"].is_object()) {
                err = "add must map new part refs to part objects";
                return false;
            }
            for (auto it = t["add"].begin(); it != t["add"].end(); ++it) {
                const std::string& ref = it.key();
                if (ref.empty() || ref.find_first_of(". \t#") != std::string::npos) {
                    err = "add \"" + ref + "\": a part ref is a plain name (no spaces, dots or #)";
                    return false;
                }
                if (findPart(ref)) {
                    err = "add " + ref + ": a part with that ref already exists";
                    return false;
                }
                if (!it.value().is_object() || !it.value().contains("type") || !it.value()["type"].is_string()) {
                    err = "add " + ref + ": needs a part type";
                    return false;
                }
                std::vector<std::string> nodes;
                if (!readNodes(it.value().value("nodes", json()), "add " + ref, nodes)) return false;
                json pj = it.value();
                pj.erase("ref");
                pj["ref"] = ref;
                pj["nodes"] = nodes;
                j["parts"].push_back(std::move(pj));
            }
        }
        // prune what names a vanished node or part
        std::set<std::string> nodes, refs;
        netsOf(nodes, &refs);
        auto gone = [&](const std::string& nd) { return had.count(nd) && !nodes.count(nd); };
        if (j.contains("probes")) {
            json keep = json::array();
            for (const auto& p : j["probes"])
                if (!gone(p.get<std::string>()) && std::find(keep.begin(), keep.end(), p) == keep.end()) keep.push_back(p);
            j["probes"] = keep;
        }
        if (j.contains("cuts")) {
            json keep = json::array();
            for (auto c : j["cuts"]) {
                const std::string nd = c.is_string() ? c.get<std::string>() : c.value("node", std::string());
                if (gone(nd)) continue;
                if (c.is_object() && c.contains("loads")) {
                    json loads = json::array();
                    for (const auto& l : c["loads"])
                        if (refs.count(l.get<std::string>()) || !removed.count(l.get<std::string>())) loads.push_back(l);
                    if (loads.empty()) continue; // its loads are gone: no cut left to make
                    c["loads"] = loads;
                }
                keep.push_back(c);
            }
            if (keep.empty()) j.erase("cuts");
            else j["cuts"] = keep;
        }
        if (j.contains("multirate")) {
            if (j["multirate"].contains("oversampled")) {
                json keep = json::array();
                for (const auto& r : j["multirate"]["oversampled"])
                    if (!removed.count(r.get<std::string>()) || refs.count(r.get<std::string>())) keep.push_back(r);
                j["multirate"]["oversampled"] = keep;
            }
            if (j["multirate"].contains("slow")) {
                json keep = json::array();
                for (const auto& x : j["multirate"]["slow"].value("nodes", json::array()))
                    if (!gone(x.get<std::string>())) keep.push_back(x);
                if (keep.empty()) j["multirate"].erase("slow");
                else j["multirate"]["slow"]["nodes"] = keep;
            }
        }
    } catch (const std::exception& e) {
        err = std::string("topology edits: ") + e.what();
        return false;
    }
    Netlist next;
    if (!parseNetlist(j, next, err)) return false;
    n = std::move(next);
    return true;
}

// kLiveLteTol (Engine.hpp): the LTE substep tolerance (CIRCUIT_ENGINE.md §3.1, §10.16; ENGINE_QUEUE 25/26)

static bool pendingValidation(int32_t t) noexcept {
    (void)t;
    return false;
}

bool circuitEngineEnabledFor(int32_t t) noexcept {
    if (!hasCircuitFor(t)) return false;
    if (bindingFor(t)->circuitOnly) return true;
    if (const char* legacy = std::getenv("HEADROOM_LEGACY_MODELS"); legacy && std::string_view(legacy) == "1") return false;
    return !pendingValidation(t);
}

namespace {

const char* unitOf(const std::string& type) {
    if (type == "R" || type == "pot" || type == "switch") return "Ω";
    if (type == "C") return "F";
    if (type == "L") return "H";
    if (type == "V") return "V";
    return "";
}

// Post section of an amp unit whose preamp is the circuit (behavioural until its circuit lands, see §8 Wave A).
struct PostSection {
    std::string kind;
    amp::PowerAmp power[2];
    double inScale = 1.0 / 25.0; // preamp volts -> power-amp drive units
    double outTrim = 1.0;
    float sag = 0.5f, vac = 0.5f, bias = 0.5f;
    bool active() const noexcept { return !kind.empty(); }
    void prepare(double isr) {
        if (kind != "fender-twin-power") return;
        // The Twin's existing power-section voicing (TubeAmpComponents.cpp, case 30000): 6L6 quad push-pull,
        // NFB damping 0.60, speaker resonance 110 Hz. Kept identical so the A/B isolates the preamp change.
        amp::PowerSpec p;
        p.drive = 0.85f;
        p.sag = 0.15f;
        p.knee = 1.25f;
        p.asym = 0.04f;
        p.ot_hp_hz = 40.0f;
        p.ot_lp_hz = 11000.0f;
        p.presence_hz = 3500.0f;
        p.presence_db = 0.0f;
        p.depth_hz = 95.0f;
        p.depth_db = 0.0f;
        p.damping = 0.60f;
        p.spk_fs = 110.0f;
        for (auto& pw : power) {
            pw.prepare(isr, p);
            pw.setControls(0.55f, 0.5f, 0.5f, sag, vac, bias);
        }
        outTrim = 0.5;
    }
};

class CircuitUnit final : public SchemaComponentBase, public model::Introspectable {
public:
    CircuitUnit(const catalog::ComponentSchema& schema, const UnitBinding& b) : SchemaComponentBase(schema), binding_(b) {
        params_[model::kModelParam].kind = catalog::ParamKind::String;
        std::string err;
        loadOk_ = parseNetlistText(libraryNetlist(b.netlistId), base_, err);
        if (!loadOk_) error_ = err;
        net_ = base_;
        bindControls();
        // Face-exported controls (netlist "face") start at the netlist default's option index, so the face shows the
        // position the circuit is actually in (constructor only: params_ is not touched from here on).
        for (std::size_t i = 0; i < net_.controls.size(); ++i) {
            const auto& c = net_.controls[i];
            if (c.face && binds_[i].param == c.id && !params_.count(c.id))
                params_[c.id].num_val = static_cast<float>(std::lround(binds_[i].lo + c.def * (binds_[i].hi - binds_[i].lo)));
        }
        // "leslie-rotors": the circuit's two band outputs (io.output horn, io.outputR drum) drive the rotor block, whose
        // stereo microphones are the unit's output; the catalogue's rotor parameters go to the block (onProcessControlRate)
        rotors_ = std::string_view(b.post) == "leslie-rotors";
        if (!rotors_) post_.kind = b.post;
        // Live readouts for the unit's face (docs/CIRCUIT_ENGINE.md "Live readouts"), read from the circuit's own nodes:
        //   "lfo": "<node>"  the node whose oscillation drives the face's LFO lamp (an oscillator node or a lamp's light
        //                    state, e.g. "ROACH#light")
        //   "gr": {...}      the gain reduction the face's GR meter shows (see parseGrReadout)
        if (loadOk_) {
            const json raw = json::parse(libraryNetlist(b.netlistId), nullptr, false);
            if (raw.is_object() && raw.contains("readouts") && raw["readouts"].is_object()) {
                const json& ro = raw["readouts"];
                if (ro.contains("lfo") && ro["lfo"].is_string()) lfoNodeName_ = ro["lfo"].get<std::string>();
                if (ro.contains("lfo") && ro["lfo"].is_object()) { // {"node": "<node>", "minSwing": volts}
                    lfoNodeName_ = ro["lfo"].value("node", std::string());
                    lfoMinSwing_ = std::max(0.0, ro["lfo"].value("minSwing", 0.0));
                }
                if (ro.contains("gr") && ro["gr"].is_object()) parseGrReadout(ro["gr"]);
            }
        }
    }

    // Audio thread, after processAudio: the circuit's own LFO phase and gain reduction, measured from its nodes
    // (UnitMeters). lfo_phase -2 = the oscillator is not running (switched off), -1 (untouched) = not known yet.
    void liveReadout(UnitLiveReadout& r) const noexcept override {
        if (lfo_.node >= 0) r.lfo_phase = lfo_.phase;
        if (gr_.node >= 0 && gr_.valid) r.gain_reduction_db = gr_.db;
        if (rotors_) r.lfo_phase = rotor_.hornPhase(), r.lfo2_phase = rotor_.drumPhase(); // the horn and drum rotors' angles
    }

    // /api/diag (EngineDiag.hpp): both channels' solver counters, read after processAudio (the right channel's core
    // has joined by then).
    bool solverStats(UnitSolverStats& s) const noexcept override {
        if (!ckt_[0]) return false;
        for (const auto& c : ckt_) {
            if (!c) continue;
            const auto& d = c->diagnostics();
            s.samples += d.samples;
            s.newton_iterations += d.newtonIterations;
            s.homotopies += d.homotopies;
            s.failures += d.failures;
            s.ptc_rescues += d.ptcRescues;
            s.half_steps += d.substeps;
            s.lte_substeps += d.lteSubsteps;
            s.max_iterations = std::max(s.max_iterations, d.maxIterations);
        }
        s.internal_rate = isr_;
        s.oversampling = osFactor_;
        s.multirate = multirate_;
        return true;
    }

    // Panel switches and selectors the legacy catalogue has no parameter for (netlist control "face": true), e.g.
    // the AC15's input jack and Vib-Trem footswitch: exported as stepped extra parameters so the face shows them.
    std::vector<ExtraParam> extraParams() const override {
        std::vector<ExtraParam> out;
        for (std::size_t i = 0; i < net_.controls.size() && i < binds_.size(); ++i) {
            const auto& c = net_.controls[i];
            if (!c.face || c.legacyOnly || binds_[i].param != c.id || (c.kind != "switch" && c.kind != "selector")) continue;
            std::vector<std::string> labels = c.options;
            if (labels.size() < 2) labels = {"Off", "On"};
            const double n1 = static_cast<double>(labels.size() - 1);
            out.push_back({c.id, c.label, labels, static_cast<float>(std::lround(c.def * n1))});
        }
        return out;
    }

    // The switch the UI shows for a legacy on/off parameter: the position of the netlist control it drives, read the
    // way the circuit reads it (controlFromParams; a two-way selector by its lround index, like switchClosed). An
    // inverted parameter range (the Twin's RvOn drives reverb_off with "paramRange": [1, 0]) flips it back to the
    // parameter's own sense. Stored-only (legacyOnly) and continuous controls have no position: the base rule.
    int switchState(std::string_view id) const override {
        for (std::size_t i = 0; i < net_.controls.size() && i < binds_.size(); ++i) {
            const auto& c = net_.controls[i];
            const auto& b = binds_[i];
            if (b.param != id || c.legacyOnly) continue;
            const double x = controlFromParams(i);
            bool at1;
            if (b.sw) at1 = x > 0.5;
            else if (c.kind == "selector" && c.options.size() == 2) at1 = std::lround(x) >= 1;
            else continue;
            return at1 != (b.hi < b.lo) ? 1 : 0;
        }
        // no circuit meaning (stored only): the catalogue convention, an untouched 0.5 is off
        if (!params_.count(std::string(id))) return -1;
        return getParamOr(id, 0.0f) > 0.5f ? 1 : 0;
    }

    std::vector<ExportedParam> exportParams() const override {
        auto v = SchemaComponentBase::exportParams();
        v.erase(std::remove_if(v.begin(), v.end(), [](const ExportedParam& e) {
                    return e.id == model::kAnalysisParam || (e.id == model::kModelParam && e.str.empty());
                }), v.end());
        return v;
    }

    bool noiseNeedsRefresh() const noexcept { return compiledNoise_ != (bbdNoise_ && circuitNoiseEnabled()); }
    bool refreshNoise() { return !noiseNeedsRefresh() || rebuildTopology(); }

    void adoptRuntimeStateFrom(IComponent& previous) noexcept override {
        auto* o = dynamic_cast<CircuitUnit*>(&previous);
        if (!o || o->tableId() != tableId() || o->isr_ != isr_ || o->osFactor_ != osFactor_ || o->multirate_ != multirate_) return;
        for (int c = 0; c < 2; ++c)
            if (ckt_[c] && o->ckt_[c] && !ckt_[c]->adoptStateFrom(*o->ckt_[c])) ckt_[c]->adoptStateByName(*o->ckt_[c]); // part edit
        os_ = o->os_;
        spkLoad_ = o->spkLoad_;
        for (int c = 0; c < 2; ++c) post_.power[c].adoptRuntimeStateFrom(o->post_.power[c]);
        if (rotors_) rotor_.adoptRuntimeStateFrom(o->rotor_);
        trim_.adoptRuntimeStateFrom(o->trim_);
    }

    SpeakerImpedanceProfile speakerImpedanceProfile() const noexcept override { return {}; }
    // Mono or stereo path: the rack's per-block AudioBlock::mono_input, else chosen per call (onProcessAudio).
    int subBlockSafety() const noexcept override { return 1; }
    // The mono path copies the left output to the right, and a stereo circuit without io.outputR plays io.output on both
    // sides; one with io.outputR (a stereo chorus, a stereo amp) makes two different sides.
    bool monoPreserving() const noexcept override { return !ckt_[0] || !ckt_[0]->hasOutputR(); }
    void setSpeakerLoadProfile(const SpeakerImpedanceProfile& p) noexcept override {
        // Audio thread, every block, with the following cab's profile (invalid: no cab follows). The quality switch
        // does not come from here: the rack sets cabFollows on the control thread (setCabFollows) and re-prepares.
        spkLoad_ = p;
        for (auto& c : ckt_)
            if (c) c->setSpeakerLoad(p.valid ? p.fs_hz : 0.0, p.q_res, p.le_scale);
        if (post_.active() && p.valid)
            for (auto& pw : post_.power) pw.setSpeakerLoadProfile(p.fs_hz, p.q_res, p.le_scale);
    }

    // Quality "hq" is the default for an amp with no cab after it (docs/CIRCUIT_ENGINE.md §10.10a, §10.12). The
    // rack (RackGraph::syncCabFollow) calls setCabFollows on the control thread before every plan publish and
    // re-prepares a detached copy when cabFollowNeedsPrepare, so the switch takes effect at once.
    int cabFollows() const noexcept override { return cabFollows_; }
    // Compile + settle read only this unit's netlist and the engine's lock-guarded shared tables (IComponent.hpp).
    bool preparesIndependently() const noexcept override { return true; }
    // The netlist ends in speaker + cab (docs/CIRCUIT_ENGINE.md §10.21): RackGraph adds / keeps no second cabinet.
    bool includesCabinet() const noexcept override { return loadOk_ && netlistEndsInCab(net_); }
    void setCabFollows(int cab) noexcept override { cabFollows_ = cab; }
    bool cabFollowNeedsPrepare(int cab) const noexcept override {
        return isr_ > 0 && !hqExplicit_ && cab >= 0 && cab != cabFollows_ && wantHq(cab) != hq_;
    }
    // A default prepare leaves hq_ as it is now (cabFollows_ is -1), so the answer is the one above without the isr_ test.
    bool cabFollowNeedsPrepareBeforePrepare(int cab) const noexcept override {
        return isr_ <= 0 && !hqExplicit_ && cab >= 0 && cab != cabFollows_ && wantHq(cab) != hq_;
    }

    std::string modelJson() const override;
    std::string curvesJson() const override;

protected:
    void onStringParameterChanged(std::string_view id, std::string_view value) override {
        if (id != model::kModelParam) return;
        Netlist next = base_;
        std::string err;
        std::vector<std::string> warnings;
        // A rejected edit changes nothing: the previous circuit keeps playing, and the stored model string goes back to
        // the one that circuit came from (so a preset saved now, or the page reading the slot, gets what is playing).
        auto keepPrevious = [&] { params_[model::kModelParam].str_val = acceptedModel_; };
        std::optional<Netlist> topoBase; // the netlist before the topology edits (the editor draws edits over it)
        json lintRaw = json::object();   // part JSON as the user wrote it, for the authoring lint (value units)
        if (!value.empty()) {
            json j = json::parse(value.begin(), value.end(), nullptr, false);
            if (j.is_discarded() || !j.is_object()) { // malformed: keep the last valid circuit
                keepPrevious();
                return;
            }
            if (j.contains("quality") && j["quality"].is_string()) { // "hq" | "standard" (see hq_)
                const bool hq = j["quality"].get<std::string>() == "hq";
                hqExplicit_ = true;
                if (hq != hq_) {
                    hq_ = hq;
                    if (isr_ > 0) onPrepare(isr_ / osFactor_, 0);
                }
            }
            // "noise": false switches the bucket-brigade noise off (on by default at the datasheet level)
            if (j.contains("noise") && j["noise"].is_boolean()) bbdNoise_ = j["noise"].get<bool>();
            if (j.contains("serial") && j["serial"].is_number_integer()) serial_ = static_cast<std::uint64_t>(std::max<std::int64_t>(0, j["serial"].get<std::int64_t>()));
            if (j.contains("circuit") && j["circuit"].is_object()) {
                json ov = j["circuit"];
                // A whole user circuit: SPICE text (the engine's own importer) or a JSON netlist.
                if (ov.contains("spice") || ov.contains("netlist")) {
                    Netlist user;
                    if (ov.contains("spice")) {
                        SpiceImportReport rep;
                        if (!ov["spice"].is_string() || !importSpice(ov["spice"].get<std::string>(), user, rep, err)) {
                            error_ = "SPICE import: " + err;
                            keepPrevious();
                            return;
                        }
                        warnings = rep.warnings;
                    } else if (!parseNetlist(ov["netlist"], user, err)) {
                        error_ = "netlist: " + err;
                        keepPrevious();
                        return;
                    } else if (ov["netlist"].contains("parts")) {
                        lintRaw["parts"] = ov["netlist"]["parts"];
                    }
                    user.id = base_.id + ".user";
                    if (user.name == "Imported SPICE netlist") user.name = j.value("name", std::string("Imported circuit"));
                    user.io.inputVoltsPerFS = base_.io.inputVoltsPerFS;
                    user.io.outputVoltsPerFS = base_.io.outputVoltsPerFS;
                    user.oversampling = base_.oversampling;
                    // The host unit's knobs (and presets) drive the imported controls in order.
                    for (std::size_t i = 0; i < user.controls.size() && i < base_.controls.size(); ++i)
                        if (user.controls[i].params.empty()) {
                            user.controls[i].params = base_.controls[i].params;
                            if (user.controls[i].label == user.controls[i].id) user.controls[i].label = base_.controls[i].label;
                        }
                    next = std::move(user);
                    ov.erase("spice");
                    ov.erase("netlist");
                }
                // Topology edits (the circuit editor: add, delete, rewire, rename), before the value edits so an added
                // part's value can be edited like any other.
                if (ov.contains("topology")) {
                    topoBase = next;
                    if (!applyTopology(next, ov["topology"], err)) {
                        error_ = "circuit edit: " + err;
                        keepPrevious();
                        return;
                    }
                    if (ov["topology"].contains("add") && ov["topology"]["add"].is_object()) {
                        if (!lintRaw.contains("parts")) lintRaw["parts"] = json::array();
                        for (auto it = ov["topology"]["add"].begin(); it != ov["topology"]["add"].end(); ++it) {
                            json pj = it.value();
                            pj["ref"] = it.key();
                            lintRaw["parts"].push_back(pj);
                        }
                    }
                    ov.erase("topology");
                }
                if (!ov.empty() && !applyOverrides(next, ov, err)) {
                    error_ = err;
                    keepPrevious();
                    return;
                }
            }
        }
        std::vector<std::string> lint;
        if (topoBase || lintRaw.contains("parts")) lint = lintNetlist(lintRaw, next);
        applyUnitVariation(next, serial_);
        // Transactional: an edit that does not compile, converge or survive the smoke render changes nothing.
        Netlist prevNet = net_;
        auto prevBinds = binds_;
        net_ = std::move(next);
        bindControls();
        if (isr_ > 0 && !rebuild(!value.empty())) {
            const std::string why = error_;
            net_ = std::move(prevNet);
            binds_ = std::move(prevBinds);
            error_ = why;
            keepPrevious();
            return;
        }
        importWarnings_ = std::move(warnings);
        lint_ = std::move(lint);
        topoBase_ = std::move(topoBase);
        acceptedModel_ = std::string(value);
        error_.clear();
    }

    // Control thread (every parameter edit, preset and copy goes through setParameter there, on a unit no published plan
    // holds): note when the switch and selector positions want another partition than the circuit's
    // (CompileOptions::openSwitches); the next processControlRate, which the rack runs right after, rebuilds it.
    void onParameterChanged(std::string_view, float) override {
        if (!ckt_[0] || topoPending_) return;
        std::vector<double> v(binds_.size());
        for (std::size_t i = 0; i < binds_.size(); ++i) v[i] = controlFromParams(i);
        topoPending_ = ckt_[0]->topologyStale(&v);
    }

    void onPrepare(double sr, uint32_t) override {
        // Internal rate (2026-11-09 aliasing audit, work/circuit-netlists/ENGINE_QUEUE.md): a dimed JCM800 aliased at
        // -24 dB re total below 20 kHz at 2x and -43 dB at 4x; a compressor or chorus measured the same at both. Each
        // netlist's minRate records which side of that line it is on.
        {
            int f = std::max(1, sr >= 88000.0 ? net_.oversampling / 2 : net_.oversampling);
            if (!hqExplicit_ && cabFollows_ >= 0) hq_ = wantHq(cabFollows_);
            // The netlist's minRate comes from the aliasing audit (circuits that measurably alias less at 4x). Others
            // measured identical at 2x and 4x, so the extra rate would cost CPU for no audible difference.
            const double minRate = std::max(net_.minRate, kMinRateOverride);
            if (minRate > 0 && hq_) // "standard" (opt-in) keeps the netlist's own factor and its multirate split
                while (sr * f < minRate - 1.0 && f < kMaxOs) f *= 2;
            osFactor_ = f;
        }
        isr_ = sr * osFactor_;
        // Multirate netlists oversample only their listed partitions: the circuit takes host-rate samples itself.
        if (!hqExplicit_ && cabFollows_ >= 0) hq_ = wantHq(cabFollows_);
        multirate_ = osFactor_ == 2 && !net_.oversampledParts.empty() && !hq_;
        os_.prepare(multirate_ ? 1 : osFactor_);
        if (rotors_) rotor_.prepare(sr); // host rate: the rotors follow the circuit's down-sampled outputs
        trim_.prepare(sr);
        rebuild();
    }

    void onReset() override {
        os_.reset();
        for (auto& c : ckt_)
            if (c) c->reset();
        for (auto& pw : post_.power) pw.reset();
        if (rotors_) rotor_.reset();
        trim_.reset();
    }

    void onProcessControlRate() override {
        if (rotors_) {
            dsp::LeslieRotorParams p;
            p.speed = getParamOr("SF", 0.5f), p.balance = getParamOr("B", 0.5f), p.pan = getParamOr("P", 0.5f);
            p.distance = getParamOr("D", 0.5f), p.cab = getParamOr("CTn", 0.5f), p.mix = getParamOr("Wt", 0.5f);
            p.hornSlow = getParamOr("TS", 0.5f), p.hornFast = getParamOr("TF", 0.5f);
            p.hornAccel = getParamOr("TA", 0.5f), p.hornSpread = getParamOr("TSp", 0.5f);
            p.drumSlow = getParamOr("BS", 0.5f), p.drumFast = getParamOr("BF", 0.5f);
            p.drumAccel = getParamOr("BA", 0.5f), p.drumSpread = getParamOr("BSp", 0.5f);
            rotor_.setParams(p);
        }
        // An output level the circuit has no part for (the SSL EQ card's Output): a gain after the circuit
        if (trimLaw_) trim_.setDb(trimLaw_->db(getParamOr(trimLaw_->param, trimLaw_->def)));
        if (!ckt_[0]) return;
        // A switch or selector moved a contact the circuit was compiled open or closed for (CompileOptions::openSwitches,
        // docs/CIRCUIT_ENGINE.md §10.29): build the circuit for the new positions and hand the state over. Only set by
        // onParameterChanged, which the rack calls on the control thread on a unit no published plan holds, and the rack
        // runs processControlRate there right after (RackGraph: ComponentEdit, detachForEdit, applyPresetParams), so
        // this never runs on the audio thread.
        if (topoPending_) {
            topoPending_ = false;
            rebuildTopology();
        }
        for (std::size_t i = 0; i < binds_.size(); ++i) {
            const double v = controlFromParams(i);
            for (auto& c : ckt_)
                if (c) c->setControlTarget(static_cast<int>(i), v);
        }
        if (post_.active()) {
            const float sag = getParamOr("Sg", 0.5f), vac = getParamOr("Vr", 0.5f), bias = getParamOr("Bia", 0.5f);
            if (sag != post_.sag || vac != post_.vac || bias != post_.bias) {
                post_.sag = sag;
                post_.vac = vac;
                post_.bias = bias;
                for (auto& pw : post_.power) pw.setControls(0.55f, 0.5f, 0.5f, sag, vac, bias);
            }
        }
    }

    void onProcessAudio(AudioBlock& block) override {
        renderAudio(block);
        if (trimLaw_) trim_.process(block.out, block.num_channels, block.num_frames);
    }

private:
    void renderAudio(AudioBlock& block) noexcept {
        const ScopedFlushDenormals ftz;
        const uint32_t nf = block.num_frames;
        if (!ckt_[0]) {
            block.copyInToOut();
            return;
        }
        if (ckt_[0]->hasOutputR() || ckt_[0]->hasInputR()) {
            processStereoCircuit(block);
            if (block.hostBlockEnd()) trackLfo(block.hostFrames()), trackGr(block.hostFrames()); // once per host block
            return;
        }
        // Mono or stereo for the whole host block: the rack decides it before anything renders (AudioBlock::mono_input),
        // so every sub-block of a pipelined block takes the path the whole block takes when rendered in one call.
        const bool stereo = block.num_channels > 1 && block.in[1] && block.in[0] != block.in[1] &&
                            (block.mono_input >= 0 ? block.mono_input == 0 : !std::equal(block.in[0], block.in[0] + nf, block.in[1]));
        const int nch = stereo ? 2 : 1;
        // Stereo: the right channel's circuit renders on another core while the left renders here (each channel has
        // its own Circuit, oversampler state and power section; nothing is shared).
        if (nch == 2) {
            ChanJob job{this, &block, 1};
            RtPool::Join j;
            const bool forked = !block.offline && RtPool::instance().fork(j, &CircuitUnit::runChannel, &job);
            processChannel(block, 0);
            if (forked) RtPool::instance().join(j);
            else processChannel(block, 1);
        } else {
            processChannel(block, 0);
        }
        if (nch == 1)
            for (uint32_t c = 1; c < block.num_channels; ++c)
                if (block.out[c] && block.out[c] != block.out[0]) std::copy(block.out[0], block.out[0] + nf, block.out[c]);
        if (block.hostBlockEnd()) trackLfo(block.hostFrames()), trackGr(block.hostFrames()); // once per host block
    }

    struct ChanJob {
        CircuitUnit* unit;
        AudioBlock* block;
        int ch;
    };
    static void runChannel(void* p) {
        const ScopedFlushDenormals ftz; // the pool thread has its own FPU mode
        auto& j = *static_cast<ChanJob*>(p);
        j.unit->processChannel(*j.block, j.ch);
    }
    // A stereo circuit (io.outputR and/or io.inputR): one instance. Without inputR it gets a mono input (the mean of a
    // stereo pair); with it, L drives io.input and R drives io.inputR (a mono host feeds both). Without outputR both
    // sides play io.output. The oversampler's two channel states carry the left and right signals: channel 0 / 1
    // up-samplers feed the inputs, channel 0 / 1 down-samplers carry the outputs.
    void processStereoCircuit(AudioBlock& block) noexcept {
        const uint32_t nf = block.num_frames;
        const int f = os_.factor();
        const double inV = net_.io.inputVoltsPerFS, outS = 1.0 / net_.io.outputVoltsPerFS;
        const float* inL = block.in[0];
        const float* inR = block.num_channels > 1 && block.in[1] ? block.in[1] : nullptr;
        float* outL = block.out[0];
        float* outR = block.num_channels > 1 ? block.out[1] : nullptr;
        if (!inL || !outL) return;
        Circuit& c = *ckt_[0];
        const bool trueIn = c.hasInputR(), trueOut = c.hasOutputR();
        const LaneBlock lanes(c, !block.offline); // intra-unit lanes (docs/CIRCUIT_ENGINE.md §10.33): a second core
        if (rotors_) rotor_.begin();
        for (uint32_t i = 0; i < nf; ++i) {
            const bool split = inR && inR != inL;
            const float xr = trueIn ? (split ? inR[i] : inL[i]) : 0.0f;
            const float x = trueIn ? inL[i] : (split ? 0.5f * (inL[i] + inR[i]) : inL[i]);
            const float dryL = inL[i], dryR = split ? inR[i] : inL[i]; // read before the (possibly in-place) writes
            float yL, yR;
            if (multirate_) {
                if (trueIn) c.setInputR(static_cast<double>(xr) * inV);
                const double yl = c.processHost(static_cast<double>(x) * inV);
                yL = static_cast<float>(yl * outS);
                yR = static_cast<float>((trueOut ? c.outputR() : yl) * outS);
            } else {
                float up[CircuitOversampler::kMaxFactor], upR[CircuitOversampler::kMaxFactor], upIn[CircuitOversampler::kMaxFactor];
                os_.up(0, x, up);
                if (trueIn) os_.up(1, xr, upIn);
                for (int k = 0; k < f; ++k) {
                    if (trueIn) c.setInputR(static_cast<double>(upIn[k]) * inV);
                    const double yl = c.process(static_cast<double>(up[k]) * inV);
                    up[k] = static_cast<float>(yl * outS);
                    upR[k] = static_cast<float>((trueOut ? c.outputR() : yl) * outS);
                }
                yL = os_.down(0, up);
                yR = os_.down(1, upR);
            }
            if (rotors_) rotor_.tick(dryL, dryR, yL, yR, yL, yR); // horn / drum bands -> the rotors' microphones
            outL[i] = yL;
            if (outR && outR != outL) outR[i] = yR;
        }
    }
    void processChannel(AudioBlock& block, int ch) noexcept {
        const uint32_t nf = block.num_frames;
        const int f = os_.factor();
        const double inV = net_.io.inputVoltsPerFS, outS = 1.0 / net_.io.outputVoltsPerFS;
        const float* in = block.in[ch] ? block.in[ch] : block.in[0];
        float* out = block.out[ch];
        Circuit& c = *ckt_[ch];
        if (!in || !out) return;
        const LaneBlock lanes(c, !block.offline); // intra-unit lanes (docs/CIRCUIT_ENGINE.md §10.33): a second core
        if (multirate_) {
            for (uint32_t i = 0; i < nf; ++i) {
                double y = c.processHost(static_cast<double>(in[i]) * inV) * outS;
                if (post_.active()) y = post_.power[ch].process(static_cast<float>(y * post_.inScale)) * post_.outTrim;
                out[i] = static_cast<float>(y);
            }
            return;
        }
        float up[CircuitOversampler::kMaxFactor];
        for (uint32_t i = 0; i < nf; ++i) {
            os_.up(ch, in[i], up);
            for (int k = 0; k < f; ++k) {
                double y = c.process(static_cast<double>(up[k]) * inV) * outS;
                if (post_.active()) y = post_.power[ch].process(static_cast<float>(y * post_.inScale)) * post_.outTrim;
                up[k] = static_cast<float>(y);
            }
            out[i] = os_.down(ch, up);
        }
    }

    struct Bind {
        std::string param; // parameter id read for this control
        double lo = 0, hi = 1;
        bool sw = false;
        double def = 0.5;
    };

    void bindControls() {
        binds_.clear();
        for (const auto& c : net_.controls) {
            Bind b;
            b.def = c.def;
            b.sw = c.kind == "switch";
            b.param = c.id;
            for (const auto& p : c.params)
                if (std::any_of(schema_.params.begin(), schema_.params.end(), [&](const auto& s) { return s.id == p; })) {
                    b.param = p;
                    b.lo = c.paramMin;
                    b.hi = c.paramMax;
                    break;
                }
            // A face-exported control (netlist "face", no legacy parameter) is an extra stepped parameter whose raw
            // value is the option index (extraParams below): map 0..n-1 onto 0..1.
            if (b.param == c.id && c.face && c.options.size() > 2) b.hi = static_cast<double>(c.options.size() - 1);
            binds_.push_back(b);
        }
    }

    double controlFromParams(std::size_t i) const {
        const auto& b = binds_[i];
        const float raw = getParamOr(b.param, static_cast<float>(b.lo + b.def * (b.hi - b.lo)));
        const double x = std::clamp((raw - b.lo) / (b.hi - b.lo), 0.0, 1.0);
        if (b.sw) return x > 0.5 ? 1.0 : 0.0; // legacy catalogue switches default to 0.5 = off
        return x;
    }

    // Builds both channel circuits; swaps them in only if everything succeeds. A user-edited circuit must also
    // survive a 50 ms full-scale smoke render (finite, |v| < 1 kV) and an estimated CPU cap (docs §6 safety).
    bool rebuild(bool userCircuit = false) {
        if (!loadOk_) return false;
        compiledNoise_ = bbdNoise_ && circuitNoiseEnabled();
        std::unique_ptr<Circuit> next[2];
        // a stereo circuit (io.outputR and/or io.inputR) is one instance rendering both sides; otherwise one circuit per channel
        const int nCkt = net_.io.outputR.empty() && net_.io.inputR.empty() ? 2 : 1;
        std::string err[2];
        if (nCkt == 2 && !tl_prepareForAdoption && parallelChannelPrepare() && !periodicSettle(net_.id)) {
            // The two channels settle on two cores (the settle is most of a circuit amp's load time). The right channel
            // starts without settleFrom, which only acts when the left one ran settlePeriodic (a transformer-fed supply):
            // such a unit's right channel is built again below from the left's periodic state, exactly as in the serial
            // order, and later builds of that netlist go serial from the start.
            fenv_t env;
            std::fegetenv(&env); // the same floating-point mode (FTZ/DAZ on x86) as this thread
            bool okR = false;
            std::exception_ptr failR;
            std::thread right([&] {
                std::fesetenv(&env);
                try {
                    okR = buildChannel(1, next[1], nullptr, err[1]);
                } catch (...) {
                    failR = std::current_exception();
                }
            });
            bool ok = false;
            std::exception_ptr failL;
            try {
                ok = buildChannel(0, next[0], nullptr, err[0]);
            } catch (...) {
                failL = std::current_exception();
            }
            right.join();
            if (failL) std::rethrow_exception(failL);
            if (failR) std::rethrow_exception(failR);
            if (ok && next[0]->hasPeriodicState()) {
                periodicSettle(net_.id, true);
                okR = buildChannel(1, next[1], next[0].get(), err[1]);
            }
            if (!ok || !okR) {
                error_ = !ok ? err[0] : err[1];
                return false;
            }
        } else {
            for (int c = 0; c < nCkt; ++c)
                if (!buildChannel(c, next[c], c == 1 ? next[0].get() : nullptr, err[c])) {
                    error_ = err[c];
                    return false;
                }
        }
        return finishRebuild(next, userCircuit);
    }

    // The same circuit at new switch / selector positions (onParameterChanged): each channel compiled for them (no
    // settle) and handed the running circuit's state (Circuit::adoptStateFrom across the topology), then swapped in.
    // Control thread, on a unit that is not rendering. A failed build keeps the running circuit (its contacts compiled
    // open stay open until the next rebuild).
    bool rebuildTopology() {
        if (!loadOk_ || isr_ <= 0 || !ckt_[0]) return false;
        std::unique_ptr<Circuit> next[2];
        std::string err;
        const bool oldNoise = compiledNoise_;
        compiledNoise_ = bbdNoise_ && circuitNoiseEnabled();
        topoRebuild_ = true;
        bool ok = true;
        for (int c = 0; c < 2 && ok; ++c)
            if (ckt_[c]) ok = buildChannel(c, next[c], nullptr, err) && next[c]->adoptStateFrom(*ckt_[c]);
        topoRebuild_ = false;
        if (!ok) { compiledNoise_ = oldNoise; return false; }
        for (int c = 0; c < 2; ++c)
            if (next[c]) ckt_[c] = std::move(next[c]);
        grRest_ = grRestNext_;
        grRestFor_ = ckt_[0].get();
        return true;
    }

    // Two-core channel builds (rebuild): on unless HEADROOM_SERIAL_PREPARE=1 (A/B and null tests).
    static bool parallelChannelPrepare() noexcept {
        static const bool on = [] {
            const char* s = std::getenv("HEADROOM_SERIAL_PREPARE");
            return !(s && std::string_view(s) == "1");
        }();
        return on;
    }
    // Netlists whose left channel ran settlePeriodic (their right channel warm-starts from it, so it builds second).
    static bool periodicSettle(const std::string& id, bool learned = false) {
        static std::mutex m;
        static std::set<std::string> ids;
        std::lock_guard<std::mutex> lock(m);
        if (learned) ids.insert(id);
        return ids.count(id) != 0;
    }

    // One channel's circuit: compiled, set to the unit's controls and at its operating point (settled when the unit
    // settles). Reads the unit's configuration only; channel 0 also records the GR readout's nodes and zero, which
    // channel 1 does not read, so rebuild may build the two at once.
    bool buildChannel(int c, std::unique_ptr<Circuit>& out, const Circuit* settleFrom, std::string& err) {
        out = std::make_unique<Circuit>();
        CompileOptions o;
        o.sampleRate = isr_;
        o.multirate = multirate_;
        // mains-fed supplies start from their running state, not the crest point; an edit copy skips this (its
        // state is replaced by the live unit's at the swap, see tl_prepareForAdoption)
        o.settleMs = tl_prepareForAdoption || topoRebuild_ ? 0.0 : kSettleMs; // (a topology rebuild adopts the running state too)
        // the partition at the unit's switch and selector positions (CompileOptions::openSwitches)
        for (std::size_t i = 0; i < binds_.size(); ++i) o.controls.push_back(controlFromParams(i));
        // a transformer-fed supply's periodic state (settlePeriodic) is computed once: the second channel starts
        // from the first one's (no effect on other circuits)
        o.settleFrom = settleFrom;
        o.bbdNoise = compiledNoise_;
        // Global opt-in AND the model JSON "noise" flag cover thermal, shot, tube,
        // heater hum and BBD noise; each instance and channel draws its own stream.
        o.noise = o.hum = compiledNoise_;
        o.noiseSeed = serial_ * 2 + static_cast<std::uint64_t>(c) + 1 + noiseInstance_ * 0x9E3779B97F4A7C15ull;
        // Error-controlled substeps (§10.16) stay off in the live unit until their tolerance is tuned so every amp
        // runs under one core and comparator LFOs start correctly (ENGINE_QUEUE 25); tests and tools keep the default.
        o.lteTol = kLiveLteTol;
        o.lteMaxPorts = kLiveLteMaxPorts;
        // Rate ladder (netlist "multirate.rates", docs §3.7): the groups that alias run faster than the rest.
        o.ladderBaseStage = 0;
        while ((1 << (o.ladderBaseStage + 1)) <= osFactor_) ++o.ladderBaseStage;
        if (hq_ && !multirate_)
            for (const auto& [ref, f] : net_.partRates) {
                const double want = (isr_ / osFactor_ >= 88000.0 ? 0.5 : 1.0) * f; // factor of this host
                int lv = 0;
                // event-local groups (docs §10.48) take the level as substeps, never as a ladder rate: one more level allowed (§10.62)
                while ((osFactor_ << (lv + 1)) <= want + 1e-9 && (osFactor_ << (lv + 1)) <= (net_.rateEvents ? 2 * kMaxOs : kMaxOs)) ++lv;
                if (lv > 0) o.partLevels.push_back({ref, lv});
            }
        if (c == 0 && !lfoNodeName_.empty()) o.probes.push_back(lfoNodeName_);
        if (c == 0) resolveGrReadout(o.probes);
        if (!out->compile(net_, o, err)) return false;
        // compile() ends in reset() at o.controls, which are these same positions, and with the supply settle
        // (o.settleMs). Setting them again and resetting repeated that whole settle, most of a circuit amp's load time
        // (docs §10.52). Skipped where the compile settled at exactly these positions. It stays where nothing settled
        // (no mains, an edit copy, a topology rebuild: a DC solve) and on a transformer-fed supply, whose second reset
        // is a short warm start from the first one's periodic state (settlePeriodic) and keeps its state as before.
        bool again = kDoubleSettle || !out->settlesSupply() || out->hasPeriodicState();
        for (std::size_t i = 0; i < binds_.size() && !again; ++i) again = out->controlValue(static_cast<int>(i)) != controlFromParams(i);
        const std::uint64_t d1 = kLoadProbe ? out->stateDigest() : 0;
        if (again) {
            for (std::size_t i = 0; i < binds_.size(); ++i) out->setControlImmediate(static_cast<int>(i), controlFromParams(i));
            if (!out->reset()) {
                err = "the DC operating point did not converge";
                return false;
            }
        }
        if (kLoadProbe) { // developer (CE_LOADPROBE=1, with CE_DOUBLESETTLE=1): does the second reset change the state?
            const std::uint64_t d2 = out->stateDigest();
            std::fprintf(stderr, "loadprobe %s ch%d settles %d reset again %d: %016llx -> %016llx %s\n", net_.id.c_str(), c, out->settlesSupply() ? 1 : 0, again ? 1 : 0,
                         static_cast<unsigned long long>(d1), static_cast<unsigned long long>(d2), d1 == d2 ? "SAME" : "DIFF");
        }
        // The GR meter's zero: its node at the operating point with no signal (as a meter's zero adjust is set)
        if (c == 0) grRestOf(*out);
        if (spkLoad_.valid) out->setSpeakerLoad(spkLoad_.fs_hz, spkLoad_.q_res, spkLoad_.le_scale);
        return true;
    }

    // rebuild, once both channels are built: a user circuit's smoke test, then the swap.
    bool finishRebuild(std::unique_ptr<Circuit> (&next)[2], bool userCircuit) {
        if (userCircuit) {
            Circuit smoke;
            CompileOptions o;
            o.sampleRate = isr_;
            o.noise = o.hum = o.bbdNoise = bbdNoise_ && circuitNoiseEnabled();
            std::string err;
            if (!smoke.compile(net_, o, err)) {
                error_ = err;
                return false;
            }
            double peak = 0;
            const int n = static_cast<int>(0.05 * isr_);
            for (int i = 0; i < n; ++i) {
                const double y = smoke.process(net_.io.inputVoltsPerFS * std::sin(2 * M_PI * 220.0 * i / isr_));
                if (!std::isfinite(y)) {
                    error_ = "the circuit produced a non-finite output in the smoke test";
                    return false;
                }
                peak = std::max(peak, std::abs(y));
            }
            if (peak > 1000.0) {
                error_ = "the circuit output exceeded 1 kV in the smoke test";
                return false;
            }
            // Edit budget: a user circuit may cost what the unit's own (stock) circuit costs plus 25 %, and at least
            // 2e5 flops/sample. Heavy stock units (the Fairchild 670, about 3.1e5) stay editable; an edit cannot make
            // a unit much heavier than the hardware it models.
            if (stockFlops_ <= 0 && haveStock_) { // same compile options as the smoke circuit, so like is compared with like
                Circuit stock;
                std::string serr;
                if (stock.compile(stockNet_, o, serr)) {
                    // and the same smoke render: flopsPerSample() scales the Newton work by the measured iterations per
                    // sample (2 when nothing has run), so an unrun stock circuit read as cheaper than the identical
                    // circuit after the full-scale smoke tone (the Fairchild 670: 1.3e5 unrun, 3.1e5 limiting hard)
                    for (int i = 0; i < n; ++i) stock.process(stockNet_.io.inputVoltsPerFS * std::sin(2 * M_PI * 220.0 * i / isr_));
                    stockFlops_ = stock.flopsPerSample();
                }
            }
            const double budget = std::max(2.0e5, 1.25 * stockFlops_);
            if (smoke.flopsPerSample() > budget) {
                error_ = "the circuit is too expensive to run in real time (estimated " + std::to_string(static_cast<long>(smoke.flopsPerSample())) + " flops/sample)";
                return false;
            }
        }
        ckt_[0] = std::move(next[0]);
        ckt_[1] = std::move(next[1]);
        if (!userCircuit) stockNet_ = net_, haveStock_ = true, stockFlops_ = 0; // the edit budget's reference (measured lazily)
        grRest_ = grRestNext_;
        grRestFor_ = ckt_[0].get();
        post_.prepare(multirate_ ? isr_ / 2 : isr_);
        error_.clear();
        return true;
    }

    // LFO phase from a node voltage, once per block: a slowly releasing min/max envelope gives the mid level, rising
    // crossings of it (with hysteresis, so noise or hum on the node cannot double-count) give the period, and the phase
    // is the time since the last crossing over that period. The node is "stopped" (phase -2: the face's lamp rests)
    // while its swing is below the netlist's minSwing (or a 1e-4 relative floor), or when no crossing has come for
    // 2.5 periods: an oscillator switched off (a vibrato footswitch) or one that does not run. Until two crossings have
    // been seen the phase is unknown (-1) and the face runs from its rate knob; until two consecutive cycles agree within
    // 35 % it counts as stopped (audio or supply ripple riding on the node swings it irregularly).
    struct LfoTrack {
        const Circuit* bound = nullptr;
        int node = -1;
        double lo = 0, hi = 0, since = 0, period = 0;
        bool primed = false, armed = false, crossed = false;
        int steady = 0; // consecutive cycles whose length agreed with the period (an oscillator is regular)
        float phase = -1.0f;
    };
    void trackLfo(uint32_t nf) noexcept {
        if (lfoNodeName_.empty() || !ckt_[0]) return;
        const Circuit* c = ckt_[0].get();
        if (lfo_.bound != c) { // resolved once per (re)built circuit, never allocates
            lfo_ = LfoTrack{};
            lfo_.bound = c;
            const auto& names = c->nodeNames();
            for (std::size_t i = 0; i < names.size(); ++i)
                if (names[i] == lfoNodeName_) lfo_.node = static_cast<int>(i);
        }
        if (lfo_.node < 0) return;
        const double v = c->nodeVoltage(lfo_.node);
        if (!std::isfinite(v)) return;
        if (!lfo_.primed) {
            lfo_.lo = lfo_.hi = v;
            lfo_.primed = true;
        }
        const double host = std::max(1.0, isr_ / osFactor_);
        const double rel = std::min(1.0, 0.5 * nf / host); // ~2 s release
        lfo_.hi = v > lfo_.hi ? v : lfo_.hi - (lfo_.hi - v) * rel;
        lfo_.lo = v < lfo_.lo ? v : lfo_.lo + (v - lfo_.lo) * rel;
        const double mid = 0.5 * (lfo_.hi + lfo_.lo), span = lfo_.hi - lfo_.lo;
        const double floor = std::max(lfoMinSwing_, 1e-4 * std::max(1.0, std::abs(mid)));
        lfo_.since += nf;
        if (span <= floor) { // not swinging: forget the period, so a restart is measured afresh
            lfo_.period = 0;
            lfo_.crossed = lfo_.armed = false;
            lfo_.steady = 0;
            lfo_.phase = -2.0f;
            return;
        }
        if (v < mid - 0.2 * span) lfo_.armed = true;
        else if (lfo_.armed && v >= mid && lfo_.since >= 0.05 * host) { // a rising crossing (LFOs run below 20 Hz: a
            lfo_.armed = false;                                             // faster one is audio riding on the node)
            if (lfo_.crossed) {
                if (lfo_.period > 0 && std::abs(lfo_.since - lfo_.period) <= 0.35 * lfo_.period) {
                    lfo_.period = 0.5 * (lfo_.period + lfo_.since);
                    lfo_.steady = std::min(lfo_.steady + 1, 4);
                } else { // irregular: a new period to confirm (audio or supply ripple on the node is not an LFO)
                    lfo_.period = lfo_.since;
                    lfo_.steady = 0;
                }
            }
            lfo_.crossed = true;
            lfo_.since = 0;
        }
        if (lfo_.period <= 0) {
            lfo_.phase = lfo_.since > 30.0 * host ? -2.0f : -1.0f; // no crossings for 30 s (slower than any LFO): stopped
            return;
        }
        if (lfo_.since > 2.5 * lfo_.period) {
            lfo_.period = 0;
            lfo_.crossed = false;
            lfo_.steady = 0;
            lfo_.phase = -2.0f;
            return;
        }
        if (lfo_.steady == 0) { // not (yet) a regular oscillation
            lfo_.phase = -2.0f;
            return;
        }
        lfo_.phase = static_cast<float>(std::min(0.999, lfo_.since / lfo_.period));
    }

    // Gain reduction for the face's GR meter, read from the circuit (netlist "readouts": {"gr": {...}}):
    //   {"node": "n", "law": "ratio"}             a meter node whose voltage falls as the gain is reduced (a photocell
    //                                             meter bridge): GR = 20·log10(v_rest / v), v_rest = the node at the
    //                                             operating point with no signal (where a meter's zero adjust is set)
    //   {"part": "U1", "law": "vca"}              a Blackmer VCA: GR = (Ec− − Ec+) / mvdb from its control pins and card
    //   {"node": "n", "law": "table", "table": [[v, dB], ...]}   a gain cell's control voltage mapped to the cell's
    //                                             gain reduction, measured from this circuit (test_circuit_unit checks it)
    // "ref": "<node>" reads node − ref. Once per block, no allocation; the result is smoothed over ~10 ms.
    enum class GrLaw { None, Ratio, Vca, Table };
    struct GrSpec {
        GrLaw law = GrLaw::None;
        std::string node, ref, part;
        double dbPerVolt = 0;
        std::vector<std::pair<double, double>> table;
    };
    void parseGrReadout(const json& g) {
        GrSpec s;
        const std::string law = g.value("law", std::string());
        s.law = law == "ratio" ? GrLaw::Ratio : law == "vca" ? GrLaw::Vca : law == "table" ? GrLaw::Table : GrLaw::None;
        if (g.contains("node") && g["node"].is_string()) s.node = g["node"].get<std::string>();
        if (g.contains("ref") && g["ref"].is_string()) s.ref = g["ref"].get<std::string>();
        if (g.contains("part") && g["part"].is_string()) s.part = g["part"].get<std::string>();
        if (g.contains("table") && g["table"].is_array())
            for (const auto& pt : g["table"])
                if (pt.is_array() && pt.size() == 2 && pt[0].is_number() && pt[1].is_number())
                    s.table.emplace_back(pt[0].get<double>(), pt[1].get<double>());
        std::sort(s.table.begin(), s.table.end());
        if (s.law == GrLaw::Table && s.table.size() < 2) s.law = GrLaw::None;
        grSpec_ = std::move(s);
    }
    // Control thread (rebuild): the nodes to read in the circuit being built, and the probes that export them.
    void resolveGrReadout(std::vector<std::string>& probes) {
        grNode_.clear();
        grRef_.clear();
        grK_ = 0;
        if (grSpec_.law == GrLaw::None) return;
        auto ground = [](const std::string& s) { return s.empty() || s == "0" || s == "gnd" || s == "GND" || s == "ground"; };
        if (grSpec_.law == GrLaw::Vca) { // pins (2180 SIP order): in, ec+, ec-, ...
            for (const auto& p : net_.parts) {
                if (p.ref != grSpec_.part || p.type != "vca" || p.nodes.size() < 3) continue;
                grNode_ = p.nodes[2];
                grRef_ = p.nodes[1];
                double mvdb = 6.1;
                if (auto m = resolveModel(net_, p.model); m && m->p.count("mvdb")) mvdb = m->p.at("mvdb");
                grK_ = 1000.0 / std::max(mvdb, 1e-3);
            }
        } else {
            grNode_ = grSpec_.node;
            grRef_ = grSpec_.ref;
        }
        if (ground(grRef_)) grRef_.clear();
        if (ground(grNode_)) grNode_.clear();
        if (!grNode_.empty()) probes.push_back(grNode_);
        if (!grRef_.empty()) probes.push_back(grRef_);
    }
    double grRead(const Circuit& c, int node, int ref) const noexcept {
        return c.nodeVoltage(node) - (ref > 0 ? c.nodeVoltage(ref) : 0.0);
    }
    int nodeIndex(const Circuit& c, const std::string& name) const noexcept { // no allocation
        if (name.empty()) return -1;
        const auto& names = c.nodeNames();
        for (std::size_t i = 0; i < names.size(); ++i)
            if (names[i] == name) return static_cast<int>(i);
        return -1;
    }
    // Control thread, right after the operating point: the meter's zero (law "ratio").
    void grRestOf(const Circuit& c) {
        grRestNext_ = std::numeric_limits<double>::quiet_NaN();
        const int n = nodeIndex(c, grNode_), r = nodeIndex(c, grRef_);
        if (n > 0) grRestNext_ = grRead(c, n, r);
    }
    struct GrTrack {
        const Circuit* bound = nullptr;
        int node = -1, ref = -1;
        double rest = 0, db = 0;
        bool valid = false;
    };
    void trackGr(uint32_t nf) noexcept {
        if (grSpec_.law == GrLaw::None || !ckt_[0]) return;
        const Circuit* c = ckt_[0].get();
        if (gr_.bound != c) { // resolved once per (re)built circuit
            gr_ = GrTrack{};
            gr_.bound = c;
            gr_.node = nodeIndex(*c, grNode_);
            gr_.ref = nodeIndex(*c, grRef_);
            gr_.rest = grRestFor_ == c ? grRest_ : std::numeric_limits<double>::quiet_NaN();
            if (gr_.node <= 0 || (grSpec_.law == GrLaw::Ratio && !(std::abs(gr_.rest) > 1e-9))) gr_.node = -1;
        }
        if (gr_.node < 0) return;
        const double v = grRead(*c, gr_.node, gr_.ref);
        if (!std::isfinite(v)) return;
        double db = 0;
        switch (grSpec_.law) {
            case GrLaw::Ratio: {
                const double x = v / gr_.rest;
                db = x > 1e-3 ? -20.0 * std::log10(x) : 60.0;
                break;
            }
            case GrLaw::Vca: db = v * grK_; break;
            case GrLaw::Table: {
                const auto& t = grSpec_.table;
                if (v <= t.front().first) db = t.front().second;
                else if (v >= t.back().first) db = t.back().second;
                else
                    for (std::size_t i = 1; i < t.size(); ++i)
                        if (v <= t[i].first) {
                            const double f = (v - t[i - 1].first) / std::max(1e-12, t[i].first - t[i - 1].first);
                            db = t[i - 1].second + f * (t[i].second - t[i - 1].second);
                            break;
                        }
                break;
            }
            case GrLaw::None: return;
        }
        db = std::clamp(db, 0.0, 60.0);
        const double a = gr_.valid ? 1.0 - std::exp(-static_cast<double>(nf) / (0.010 * std::max(1.0, isr_ / osFactor_))) : 1.0;
        gr_.db += a * (db - gr_.db);
        gr_.valid = true;
    }

    std::string lfoNodeName_;
    double lfoMinSwing_ = 0.0; // readouts.lfo.minSwing: the swing (V) below which the oscillator counts as stopped
    LfoTrack lfo_;
    GrSpec grSpec_;
    std::string grNode_, grRef_; // resolved for the current netlist (rebuild)
    double grK_ = 0;             // vca: dB per volt
    double grRest_ = 0, grRestNext_ = 0;
    double stockFlops_ = 0; // the stock circuit's cost (flops/sample): the edit budget's reference
    Netlist stockNet_;      // the stock netlist (control thread only)
    bool haveStock_ = false;
    const Circuit* grRestFor_ = nullptr; // the circuit grRest_ was measured on
    GrTrack gr_;
    UnitBinding binding_;
    std::vector<std::string> importWarnings_;
    std::vector<std::string> lint_;     // authoring lint of a user-edited circuit (lintNetlist), shown by the editor
    std::optional<Netlist> topoBase_;   // the netlist before the accepted topology edits (none: no topology edits)
    std::string acceptedModel_;         // the model string the running circuit came from (restored on a rejected edit)
    Netlist base_, net_;
    bool loadOk_ = false;
    std::string error_;
    std::unique_ptr<Circuit> ckt_[2];
    bool topoPending_ = false; // control thread: the parameters want another switch topology (onParameterChanged)
    bool topoRebuild_ = false; // buildChannel: a topology rebuild (no settle; the old circuit's state is adopted)
    CircuitOversampler os_; // up to 32x (hard-switching circuits; tests/test_aliasing.cpp)
    int osFactor_ = 2;
    double isr_ = 0;
    bool multirate_ = false;
    // Quality: "hq" runs the whole circuit at the oversampled rate (preamp included) instead of the multirate split
    // (only the netlist's oversampled partitions at 2x). Off by default; intended for units not followed by a cab,
    // where the host-rate preamp's top-octave roll-off (docs/CIRCUIT_ENGINE.md §10.8) would be audible.
    bool hq_ = true;
    std::uint64_t serial_ = 0;     // unit variation seed (0 = nominal); see applyUnitVariation
    std::uint64_t noiseInstance_ = newNoiseInstance(); // this unit's noise stream (0 under HEADROOM_NOISE_FIXED_SEED)
    bool hqExplicit_ = false;      // quality set through the model JSON (overrides the cab rule)
    int cabFollows_ = -1;          // control thread: -1 unknown, 0 no cab follows, 1 a cab follows (setCabFollows)
    bool compiledNoise_ = false;
    bool bbdNoise_ = true;         // noise and hum (model JSON "noise"): CompileOptions::bbdNoise, ::noise, ::hum
    // hq only changes anything where a multirate split exists (an oversampled netlist with listed partitions).
    // HQ (whole circuit oversampled) is the default for every multirate netlist, cab or not (2026-11-09): with the
    // preamp at the host rate, a cranked Plexi aliased at -15.6 dB re total below 20 kHz (-23 dB at 2x), audible as
    // digital harshness. "standard" (power stage only at 2x) stays available through the model JSON for CPU-bound hosts.
    // HQ whenever the netlist asks for more than the base rate: an oversampled-parts list, a minRate (the aliasing
    // audit's 4x) or a rate ladder. Before 2026-11-13 only the parts list counted, so nine amps (Uberschall, SVT, BE-100,
    // Supro, ...) rendered at 2x behind a cab while their aliasing gate measured 4x (docs §10.36).
    bool wantHq(int /*cab*/) const noexcept {
        return (net_.oversampling >= 2 && !net_.oversampledParts.empty()) || net_.minRate > 0 || !net_.partRates.empty();
    }
    SpeakerImpedanceProfile spkLoad_{};
    std::vector<Bind> binds_;
    PostSection post_;
    // Output level outside the netlist (dsp/OutputTrim.hpp): only units whose circuit has no part for their Output knob
    const dsp::OutputTrimLaw* trimLaw_ = dsp::outputTrimLaw(schema_.table_id);
    dsp::OutputTrim trim_{};
    bool rotors_ = false;       // binding post "leslie-rotors" (31000): the rotor block follows the circuit
    dsp::LeslieRotors rotor_;
};

// UI metadata: what kind of physical part this is and what it looks like (for illustrated part views). Netlist
// "family" / "package" fields override the heuristics.
std::pair<std::string, std::string> partLook(const Netlist& n, const Part& p, double value) {
    std::string family, package;
    const std::string mk = p.model.empty() ? std::string() : (resolveModel(n, p.model) ? resolveModel(n, p.model)->kind : std::string());
    auto is = [&](std::initializer_list<const char*> names) {
        for (const char* x : names)
            if (p.model == x) return true;
        return false;
    };
    if (p.type == "R") { family = "resistor"; package = "axial"; }
    else if (p.type == "pot") { family = "potentiometer"; package = "panel-pot"; }
    else if (p.type == "switch") { family = "switch"; package = "toggle"; }
    else if (p.type == "L") { family = value >= 0.5 ? "choke" : "inductor"; package = value >= 0.5 ? "frame-choke" : "wound-inductor"; }
    else if (p.type == "C") {
        if (value >= 1e-6) { family = "electrolytic"; package = "radial-can"; }
        else if (value >= 1e-9) { family = "film"; package = "box-film"; }
        else { family = "ceramic"; package = "disc"; }
    } else if (p.type == "V") { family = "source"; package = p.acAmp != 0 ? "mains-transformer" : "supply-rail"; }
    else if (p.type == "transformer") { family = "transformer"; package = "frame-transformer"; }
    else if (p.type == "ldr") { family = "optocoupler"; package = "neon-ldr"; }
    else if (p.type == "spring") { family = "spring"; package = "reverb-tank"; }
    else if (p.type == "speaker") { family = "speaker"; package = "guitar-speaker"; }
    else if (p.type == "ota") { family = "ota"; package = is({"CA3080", "CA3094"}) ? "DIP-8" : "DIP-16"; } // LM13700/13600: DIP-16
    else if (p.type == "bbd") { family = "bbd"; package = is({"SAD1024"}) ? "DIP-16" : "DIP-8"; }
    else if (p.type == "vca") { family = "vca"; package = is({"DBX202"}) ? "potted-module" : "SIP-8"; }
    else if (p.type == "rms") { family = "rms-detector"; package = "SIP-8"; }
    else if (p.type == "lamp") { family = "incandescent-lamp"; package = "T-1¾-wire-lead"; }
    else if (p.type == "led") { family = "led-opto"; package = "vactrol"; }
    else if (p.type == "neon") { family = "neon-lamp"; package = "NE-2"; }
    else if (p.type == "cds") { family = "photocell"; package = is({"VTL5C3_CELL"}) ? "vactrol" : "cds-disc"; }
    else if (p.type == "elpanel") { family = "el-panel"; package = "T4B"; }
    else if (p.type == "tape") { family = "tape-transport"; package = "tape-heads"; }
    else if (p.type == "absi" || p.type == "gaincell") { family = "compander"; package = "DIP-16"; }
    else if (p.type == "opamp") { family = "op-amp"; package = is({"IDEAL_OPAMP"}) ? "ideal" : "DIP-8"; }
    else if (p.type == "npn" || p.type == "pnp") {
        // germanium parts (saturation current in the 0.01-1 µA range, or an ICBO leakage term) are metal-can TO-1 types
        const auto dm = resolveModel(n, p.model);
        const bool ge = dm && (dm->get("icbo", 0.0) > 0 || dm->get("is", 0.0) >= 1e-9);
        family = std::string(p.type == "npn" ? "bjt-npn" : "bjt-pnp") + (ge ? "-germanium" : "");
        package = ge ? "TO-1" : "TO-92";
    } else if (p.type == "njf" || p.type == "pjf") { family = p.type == "njf" ? "jfet-n" : "jfet-p"; package = "TO-92"; }
    else if (p.type == "triode") { family = "triode"; package = "noval-B9A"; }
    else if (p.type == "pentode") { family = "power-pentode"; package = is({"EL84"}) ? "noval-B9A" : "octal"; }
    else if (p.type == "D" || p.type == "Dpair") {
        if (mk == "vacdiode") { family = "vacuum-rectifier"; package = "octal"; }
        else if (is({"1N34A"})) { family = "germanium-diode"; package = "DO-7"; }
        else if (is({"LED_RED", "LED_GREEN"})) { family = "led"; package = "T-1¾"; }
        else if (is({"1N4001"})) { family = "rectifier-diode"; package = "DO-41"; }
        else if (is({"BAT41"})) { family = "schottky-diode"; package = "DO-35"; }
        else { family = "silicon-diode"; package = "DO-35"; }
    } else { family = p.type; package = ""; }
    if (!p.family.empty()) family = p.family;
    if (!p.package.empty()) package = p.package;
    return {family, package};
}

json partValueJson(const Circuit& c, const Netlist& n, std::size_t pi) {
    const Part& p = n.parts[pi];
    json o = {{"ref", p.ref}, {"type", p.type}, {"nodes", p.nodes}};
    if (!p.label.empty()) o["label"] = p.label;
    if (!p.note.empty()) o["note"] = p.note;
    const std::string base = "circuit.parts." + p.ref;
    std::vector<double> ctl;
    for (int k = 0; k < c.controlCount(); ++k) ctl.push_back(c.controlValue(k));
    if (p.hasValue) {
        const double pv = c.partValue(static_cast<int>(pi));
        const double v = p.type == "V" ? p.value.nominal : (pv > 0.0 ? pv : valueAt(n, p.value, ctl));
        o["value"] = v;
        o["nominal"] = p.value.nominal;
        o["text"] = formatValue(v, unitOf(p.type));
        o["unit"] = unitOf(p.type);
        if (p.value.variable()) o["law"] = {{"control", p.value.control}, {"min", p.value.min}, {"max", p.value.max}, {"law", p.value.law}};
        if (p.type != "V")
            o["edit"] = {{"path", base + ".value"}, {"min", p.value.nominal / 10}, {"max", p.value.nominal * 10}, {"step", p.value.nominal / 100}, {"scale", "log"}};
    }
    if (p.tolerance > 0) o["tolerance"] = p.tolerance;
    if (p.type == "speaker") { // identical drivers folded into one equivalent (series x parallel)
        o["series"] = p.series;
        o["parallel"] = p.parallel;
    }
    {
        const auto [family, package] = partLook(n, p, o.value("value", 0.0));
        o["family"] = family;   // UI: kind of physical part ("electrolytic", "triode", "op-amp", …)
        o["package"] = package; // UI: what it looks like ("radial-can", "noval-B9A", "DIP-8", "TO-92", …)
    }
    if (p.type == "pot") {
        double lo, up;
        potSegments(n, p, ctl, lo, up);
        o["taper"] = std::string(1, p.taper.kind);
        o["position"] = controlPosition(n, p.control, ctl);
        o["segments"] = {lo, up};
    }
    if (p.type == "switch") {
        o["closed"] = switchClosed(n, p, ctl);
        if (!p.positions.empty()) o["positions"] = p.positions;
    }
    if (!p.control.empty()) o["control"] = p.control;
    if (!p.model.empty()) {
        o["model"] = p.model;
        if (auto m = resolveModel(n, p.model)) {
            o["model_kind"] = m->kind;
            json opts = json::array();
            for (const auto& [name, lm] : builtinModels())
                if (lm.kind == m->kind) opts.push_back({{"v", name}, {"label", name}});
            o["edit"] = {{"path", base + ".model"}, {"options", opts}};
        }
    }
    return o;
}

std::string CircuitUnit::modelJson() const {
    json m;
    m["kind"] = "circuit";
    m["post_section"] = binding_.post;
    m["backend"] = "circuit-engine";
    m["verification"] = "spice-validated";
    m["verification_scope"] =
        "The real-time solver is checked against ngspice running the same netlist and device equations (CTest circuit_engine). "
        "Device parameters are public datasheet/SPICE/paper values; no hardware unit was measured.";
    m["oversampling"] = osFactor_;
    m["multirate"] = multirate_;
    m["quality"] = hq_ ? "hq" : "standard";
    m["ends_in_cab"] = includesCabinet(); // speaker + cab in the circuit: the rack adds no cabinet after this unit
    m["noise"] = bbdNoise_;
    m["serial"] = serial_;
    m["internal_rate"] = isr_;
    m["custom"] = !getStringParameter(model::kModelParam).empty();
    if (!error_.empty()) m["error"] = error_;
    if (!importWarnings_.empty()) m["import_warnings"] = importWarnings_;
    const Circuit* c = ckt_[0].get();
    json ci;
    ci["id"] = net_.id;
    ci["name"] = net_.name;
    ci["version"] = net_.version;
    ci["description"] = net_.description;
    json src = json::array();
    for (const auto& s : net_.sources) src.push_back({{"title", s.title}, {"url", s.url}});
    ci["sources"] = src;
    ci["io"] = {{"input", net_.io.input}, {"output", net_.io.output}, {"inputVoltsPerFS", net_.io.inputVoltsPerFS}, {"outputVoltsPerFS", net_.io.outputVoltsPerFS}};
    json ctls = json::array();
    for (std::size_t i = 0; i < net_.controls.size(); ++i) {
        const auto& d = net_.controls[i];
        json o = {{"id", d.id}, {"label", d.label}, {"kind", d.kind}, {"value", c ? c->controlValue(static_cast<int>(i)) : d.def}, {"param", binds_[i].param}};
        if (!d.options.empty()) o["options"] = d.options;
        if (!d.note.empty()) o["note"] = d.note;
        if (!d.group.empty()) o["group"] = d.group;
        if (d.legacyOnly) o["legacyOnly"] = true;
        if (d.mod) o["mod"] = true;
        if (!d.modNote.empty()) o["modNote"] = d.modNote;
        ctls.push_back(o);
    }
    ci["controls"] = ctls;
    json parts = json::array();
    if (c)
        for (std::size_t i = 0; i < net_.parts.size(); ++i) parts.push_back(partValueJson(*c, net_, i));
    ci["parts"] = parts;
    json models = json::object();
    std::set<std::string> used;
    for (const auto& p : net_.parts)
        if (!p.model.empty()) used.insert(p.model);
    for (const auto& name : used)
        if (auto md = resolveModel(net_, name)) {
            json pj = json::object();
            for (const auto& [k, v] : md->p) pj[k] = v;
            models[name] = {{"kind", md->kind}, {"params", pj}, {"note", md->note}};
        }
    ci["models"] = models;
    {
        // The device library by kind, for the circuit editor's parts palette and model pickers.
        json lib = json::object();
        for (const auto& [name, lm] : builtinModels()) {
            if (!lib.contains(lm.kind)) lib[lm.kind] = json::array();
            lib[lm.kind].push_back(name);
        }
        ci["model_library"] = lib;
    }
    ci["probes"] = net_.probes;
    ci["layout"] = net_.layout;
    if (!lint_.empty()) ci["lint"] = lint_;
    if (c) {
        json groups = json::array();
        for (std::size_t g = 0; g < c->groups().size(); ++g) {
            const auto& gr = c->groups()[g];
            json refs = json::array();
            for (int p : gr.parts) refs.push_back(net_.parts[static_cast<std::size_t>(p)].ref);
            groups.push_back({{"index", g}, {"solver", solverName(gr.solver)}, {"unknowns", gr.unknowns.size()}, {"states", gr.states.size()},
                              {"nonlinear_ports", gr.nn}, {"parts", refs}});
        }
        ci["groups"] = groups;
        json nodes = json::array();
        for (const auto& nm : c->nodeNames()) nodes.push_back(nm);
        ci["nodes"] = nodes;
        ci["warnings"] = c->warnings();
        const auto& d = c->diagnostics();
        ci["diagnostics"] = {{"samples", d.samples}, {"newton_iterations", d.newtonIterations}, {"homotopies", d.homotopies}, {"failures", d.failures}};
    }
    m["circuit"] = ci;
    // Generic block view (one block per solve group) for pages without the circuit view.
    json blocks = json::array();
    if (c)
        for (std::size_t g = 0; g < c->groups().size(); ++g) {
            const auto& gr = c->groups()[g];
            json vals = json::array();
            for (int p : gr.parts) {
                const auto pj = partValueJson(*c, net_, static_cast<std::size_t>(p));
                json v = {{"k", pj["ref"]}, {"v", pj.value("text", pj.value("model", std::string()))}, {"raw", pj.value("value", 0.0)}};
                if (pj.contains("edit")) v["edit"] = pj["edit"];
                if (pj.contains("note")) v["note"] = pj["note"];
                vals.push_back(v);
            }
            std::string label = "Stage " + std::to_string(g + 1);
            for (int p : gr.parts)
                if (!net_.parts[static_cast<std::size_t>(p)].label.empty()) {
                    label = net_.parts[static_cast<std::size_t>(p)].label;
                    break;
                }
            blocks.push_back({{"id", "g" + std::to_string(g)}, {"type", gr.nn ? "circuit" : "filter"}, {"label", label},
                              {"sub", std::string(solverName(gr.solver))}, {"values", vals}, {"section", "Circuit"}, {"fidelity", "circuit-solved"}});
        }
    if (post_.active())
        blocks.push_back({{"id", "power"}, {"type", "power"}, {"label", "Power amp (behavioural)"}, {"sub", "6L6 push-pull, reactive load"},
                          {"values", json::array()}, {"section", "Power"}, {"fidelity", "behavioural"}});
    m["blocks"] = blocks;
    m["capabilities"] = {{"scalarEdits", true}, {"arbitraryTopologyEdits", true}, {"physicalVoltageIO", true}, {"hardwareCalibrated", false}, {"neuralDevice", false}};
    m["netlist"] = toJson(net_);
    // The circuit editor draws the user's topology edits ("circuit.topology") over the netlist they were made on.
    if (topoBase_) m["base_netlist"] = toJson(*topoBase_);
    return m.dump();
}

// Steady-state node waveforms for the component page's probes (a private circuit, never the live one).
std::string CircuitUnit::curvesJson() const {
    if (!loadOk_ || isr_ <= 0) return "{}";
    Circuit c;
    CompileOptions o;
    o.sampleRate = isr_;
    o.noise = o.hum = o.bbdNoise = bbdNoise_ && circuitNoiseEnabled();
    o.exportAllNodes = true;
    o.settleMs = kSettleMs;
    std::string err;
    if (!c.compile(net_, o, err)) return json({{"error", err}}).dump();
    for (std::size_t i = 0; i < binds_.size(); ++i) c.setControlImmediate(static_cast<int>(i), controlFromParams(i));
    c.reset();
    json dc = json::object(); // operating point
    for (std::size_t n = 1; n < c.nodeNames().size(); ++n)
        if (c.nodeNames()[n].find('#') == std::string::npos) dc[c.nodeNames()[n]] = c.nodeVoltage(static_cast<int>(n));
    const double level = 0.25 * net_.io.inputVoltsPerFS, freq = 330.0;
    const int settle = static_cast<int>(0.08 * isr_);
    const int span = static_cast<int>(2.0 * isr_ / freq);
    const int stride = std::max(1, span / 240);
    // Every netlist node can be scoped: trace them all (internal '#' nodes excluded); input first.
    std::vector<int> ids;
    std::vector<std::string> names{net_.io.input};
    for (std::size_t nd = 1; nd < c.nodeNames().size(); ++nd) {
        const auto& nm = c.nodeNames()[nd];
        if (nm.find('#') == std::string::npos && nm != net_.io.input) names.push_back(nm);
    }
    for (const auto& nm : names) ids.push_back(c.node(nm));
    json traces = json::object();
    std::vector<std::vector<double>> data(ids.size());
    for (int i = 0; i < settle + span; ++i) {
        const double vin = level * std::sin(2.0 * M_PI * freq * i / isr_);
        c.process(vin);
        if (i >= settle && (i - settle) % stride == 0)
            for (std::size_t k = 0; k < ids.size(); ++k) data[k].push_back(ids[k] == 0 ? 0.0 : (k == 0 ? vin : c.nodeVoltage(ids[k])));
    }
    for (std::size_t k = 0; k < ids.size(); ++k)
        if (ids[k] >= 0) traces[names[k]] = data[k];
    json t = json::array();
    for (std::size_t i = 0; i < data[0].size(); ++i) t.push_back(1000.0 * static_cast<double>(i * static_cast<std::size_t>(stride)) / isr_);
    return json({{"probes", {{"t_ms", t}, {"traces", traces}, {"level_v", level}, {"freq_hz", freq}, {"defaults", net_.probes}}}, {"dc", dc}}).dump();
}

} // namespace

bool circuitNoiseNeedsRefresh(const IComponent& unit) noexcept {
    const auto* c = dynamic_cast<const CircuitUnit*>(&unit);
    return c && c->noiseNeedsRefresh();
}
bool refreshCircuitNoise(IComponent& unit) {
    auto* c = dynamic_cast<CircuitUnit*>(&unit);
    return !c || c->refreshNoise();
}

std::unique_ptr<IComponent> makeCircuitUnitAlways(const catalog::ComponentSchema& schema) {
    const auto* b = bindingFor(schema.table_id);
    if (!b) return nullptr;
    return std::make_unique<CircuitUnit>(schema, *b);
}

std::unique_ptr<IComponent> makeCircuitUnit(const catalog::ComponentSchema& schema) {
    if (!circuitEngineEnabledFor(schema.table_id)) return nullptr;
    return makeCircuitUnitAlways(schema);
}

} // namespace rig::ce
