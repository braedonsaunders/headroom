#pragma once
#include "PowerCircuit.hpp"
#include "PreampCircuit.hpp"
#include "json.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace rig::circuit {
// Only executable topologies are accepted. Names identify subcircuit choices,
// never a claim that the composed model replicates a whole commercial amp.
class CircuitDefinition {
public:
  enum class Topology { MarshallPair, BassmanStackPair };
  struct Scalar {
    std::string id, label, type, unit, provenance;
    double value, minimum, maximum;
    bool editable = true;
  };
  struct Applied {
    PreampCircuit::Config preamp;
    PowerConfig power;
    double master = 0.5, bass = 0.5, mid = 0.5, treble = 0.5;
    double feedback = 0.4, presence = 0.5, depth = 0.5;
    double interfaceC = 22e-9, interfaceR = 1000000, loadR = 1000, loadL = 1;
  };
  static constexpr std::string_view schemaId = "rig.circuit-amp.v1";
  static std::string_view topologyId(Topology t) noexcept {
    return t == Topology::MarshallPair
               ? "paired-triode.marshall2204-stack+reduced-push-pull.v1"
               : "paired-triode.5f6a-stack+reduced-push-pull.v1";
  }
  explicit CircuitDefinition(Topology t = Topology::MarshallPair)
      : topology_(t) {
    applied_.preamp = t == Topology::MarshallPair
                          ? PreampCircuit::marshall2204Pair()
                          : PreampCircuit::bassman5f6aToneStack();
  }
  Topology topology() const noexcept { return topology_; }
  const Applied &applied() const noexcept { return applied_; }
  // This is the single scalar registry used for validation, serialization and
  // editor metadata. References address the same config consumed by the DSP.
  std::vector<Scalar> scalars() const {
    auto copy = *this;
    std::vector<Scalar> out;
    copy.visit([&](const char *id, const char *label, const char *type,
                   const char *unit, double &v, double lo, double hi,
                   const char *provenance) {
      out.push_back({id, label, type, unit, provenance, v, lo, hi, true});
    });
    return out;
  }
  bool setScalar(std::string_view id, double value, std::string &error) {
    bool found = false, valid = false;
    visit([&](const char *key, const char *, const char *, const char *,
              double &v, double lo, double hi, const char *) {
      if (id == key) {
        found = true;
        valid = std::isfinite(value) && value >= lo && value <= hi;
        if (valid)
          v = value;
      }
    });
    if (!found)
      error = "Unknown component ID: " + std::string(id);
    else if (!valid)
      error = "Nonfinite or out-of-range component: " + std::string(id);
    else
      error.clear();
    return found && valid;
  }
  // Allocation-free performance controls share the applied config registry.
  bool setPerformanceControls(double master, double bass, double mid,
                              double treble, double feedback, double presence,
                              double depth) noexcept {
    for (double value : {master, bass, mid, treble, feedback, presence, depth})
      if (!std::isfinite(value) || value < 0 || value > 1)
        return false;
    applied_.master = master;
    applied_.bass = bass;
    applied_.mid = mid;
    applied_.treble = treble;
    applied_.feedback = feedback;
    applied_.presence = presence;
    applied_.depth = depth;
    return true;
  }
  bool valid(std::string &error) const {
    if (topology_ != Topology::MarshallPair &&
        topology_ != Topology::BassmanStackPair) {
      error = "Unsupported topology";
      return false;
    }
    for (const auto &p : scalars())
      if (!std::isfinite(p.value) || p.value < p.minimum ||
          p.value > p.maximum) {
        error = "Invalid component: " + p.id;
        return false;
      }
    error.clear();
    return true;
  }
  nlohmann::json toJson() const {
    nlohmann::json values = nlohmann::json::object();
    for (const auto &s : scalars())
      values[s.id] = s.value;
    return {{"schema", schemaId},
            {"topology", topologyId(topology_)},
            {"components", values}};
  }
  // Transactional import: a malformed document never changes destination.
  static bool fromJson(const nlohmann::json &j, CircuitDefinition &destination,
                       std::string &error) {
    try {
      if (!j.is_object() || j.size() != 3 || j.at("schema") != nlohmann::json(schemaId)) {
        error = "Unsupported circuit definition schema";
        return false;
      }
      const auto topology = j.at("topology").get<std::string>();
      Topology t;
      if (topology == topologyId(Topology::MarshallPair))
        t = Topology::MarshallPair;
      else if (topology == topologyId(Topology::BassmanStackPair))
        t = Topology::BassmanStackPair;
      else {
        error = "Unsupported executable topology";
        return false;
      }
      CircuitDefinition next(t);
      const auto &components = j.at("components");
      if (!components.is_object() ||
          components.size() != next.scalars().size()) {
        error = "Complete component object required";
        return false;
      }
      for (auto it = components.begin(); it != components.end(); ++it) {
        if (!it.value().is_number()) {
          error = "Numeric component value required: " + it.key();
          return false;
        }
        if (!next.setScalar(it.key(), it.value().get<double>(), error))
          return false;
      }
      if (!next.valid(error))
        return false;
      destination = std::move(next);
      return true;
    } catch (const std::exception &e) {
      error = std::string("Invalid circuit definition: ") + e.what();
      return false;
    }
  }
  nlohmann::json editorSchema() const {
    auto j = toJson();
    j["parameters"] = nlohmann::json::array();
    for (const auto &p : scalars())
      j["parameters"].push_back({{"id", p.id},
                                 {"label", p.label},
                                 {"type", p.type},
                                 {"unit", p.unit},
                                 {"value", p.value},
                                 {"min", p.minimum},
                                 {"max", p.maximum},
                                 {"editable", true},
                                 {"provenance", p.provenance}});
    j["capabilities"] = {{"scalarEdits", true},
                         {"arbitraryTopologyEdits", false},
                         {"physicalVoltageIO", true},
                         {"hardwareCalibrated", false},
                         {"neuralDevice", false}};
    j["netlist"] = connectivity();
    j["description"] =
        "Solved coupled triode pair and passive tone stack; empirical "
        "triode/pentode device curves; reduced push-pull "
        "power/transformer/reactive load. Shared supply uses partitioned "
        "sample coupling. Master wiper is unloaded at a feed-forward voltage "
        "boundary; interface blocking RC is explicit. Not a whole hardware "
        "amplifier replica.";
    return j;
  }
  // Fixed connectivity describes the implemented solve, not editable wires.
  nlohmann::json connectivity() const {
    using J = nlohmann::json;
    J nodes = J::array({"ground",        "input",         "input-cap",
                        "input-grid",    "plate",         "cathode",
                        "follower",      "stack-input",   "slope",
                        "treble-top",    "bass-top",      "mid",
                        "stack-output",  "mid-top",       "master-wiper",
                        "phase-input",   "reservoir",     "screen-supply",
                        "preamp-supply", "phase-plate-a", "phase-plate-b",
                        "output-grid-a", "output-grid-b", "primary-coordinate",
                        "speaker-output"});
    J devices = J::array();
    auto branch = [&](const char *id, const char *type, J ports, J parameters) {
      devices.push_back({{"id", id},
                         {"type", type},
                         {"ports", ports},
                         {"parameters", parameters}});
    };
    auto scalarBranch = [&](const char *id, const char *type, const char *a,
                            const char *b, const char *scalar) {
      branch(id, type, J::array({a, b}), {{"valueRef", scalar}});
    };
    scalarBranch("input-coupling", "capacitor", "input", "input-cap",
                 "preamp.inputC");
    scalarBranch("input-stopper", "resistor", "input-cap", "input-grid",
                 "preamp.gridR");
    scalarBranch("input-leak", "resistor", "input-grid", "ground",
                 "preamp.gridLeakR");
    scalarBranch("plate-load", "resistor", "preamp-supply", "plate",
                 "preamp.plateR");
    scalarBranch("cathode-bias", "resistor", "cathode", "ground",
                 "preamp.cathodeR");
    scalarBranch("cathode-bypass", "capacitor", "cathode", "ground",
                 "preamp.cathodeC");
    scalarBranch("follower-load", "resistor", "follower", "ground",
                 "preamp.followerR");
    branch("triode-a", "empirical-triode",
           J::array({"plate", "input-grid", "cathode"}),
           {{"law", "TubeDevice"}});
    branch("triode-b", "empirical-triode",
           J::array({"preamp-supply", "plate", "follower"}),
           {{"law", "TubeDevice"}});
    branch("direct-link", "resistor", J::array({"follower", "stack-input"}),
           {{"ohm", 10}});
    scalarBranch("tone-slope", "resistor", "stack-input", "slope",
                 "preamp.slopeR");
    scalarBranch("tone-treble-cap", "capacitor", "stack-input", "treble-top",
                 "preamp.trebleC");
    scalarBranch("tone-bass-cap", "capacitor", "slope", "bass-top",
                 "preamp.bassC");
    scalarBranch("tone-mid-cap", "capacitor", "slope", "mid", "preamp.midC");
    branch("tone-treble-pot", "potentiometer",
           J::array({"treble-top", "stack-output", "bass-top"}),
           {{"resistanceRef", "preamp.trebleR"},
            {"positionRef", "control.treble"},
            {"segmentFloorOhms", 10}});
    branch("tone-bass-pot", "rheostat", J::array({"bass-top", "mid-top"}),
           {{"resistanceRef", "preamp.bassR"},
            {"positionRef", "control.bass"},
            {"law", "R*(1-position), minimum 10 ohm; wiper tied to top"}});
    branch("tone-mid-pot", "potentiometer",
           J::array({"mid-top", "mid", "ground"}),
           {{"resistanceRef", "preamp.midR"},
            {"positionRef", "control.mid"},
            {"segmentFloorOhms", 10}});
    branch("master", "unloaded-wiper-pot",
           J::array({"stack-output", "master-wiper", "ground"}),
           {{"resistanceRef", "master.resistance"},
            {"positionRef", "control.master"},
            {"boundary", "total resistance loads solved stack; unilateral "
                         "unloaded wiper voltage port"}});
    scalarBranch("interstage-blocking", "capacitor", "master-wiper",
                 "phase-input", "interface.couplingC");
    scalarBranch("interstage-leak", "resistor", "phase-input", "ground",
                 "interface.gridLeakR");
    branch(
        "reservoir-source", "thevenin-source",
        J::array({"ground", "reservoir"}),
        {{"voltageRef", "power.sourceV"}, {"resistanceRef", "power.sourceR"}});
    scalarBranch("reservoir-capacitor", "capacitor", "reservoir", "ground",
                 "power.reservoirC");
    scalarBranch("screen-feed", "resistor", "reservoir", "screen-supply",
                 "power.screenR");
    scalarBranch("screen-capacitor", "capacitor", "screen-supply", "ground",
                 "power.screenC");
    scalarBranch("preamp-feed", "resistor", "screen-supply", "preamp-supply",
                 "power.preampR");
    scalarBranch("preamp-capacitor", "capacitor", "preamp-supply", "ground",
                 "power.preampC");
    branch("phase-inverter", "solved-reduced-triode-pair",
           J::array({"phase-input", "speaker-output", "preamp-supply",
                     "phase-plate-a", "phase-plate-b", "ground"}),
           {{"parameterRefs",
             J::array({"power.phasePlateA", "power.phasePlateB",
                       "power.phaseCathodeR", "power.phaseTailR",
                       "power.phaseReferenceV", "control.feedback",
                       "control.presence", "control.depth"})},
            {"boundary",
             "input grid-source conduction and feedback filters are reduced"}});
    branch("output-coupling-a", "biased-grid-coupling",
           J::array({"phase-plate-a", "output-grid-a", "ground"}),
           {{"parameterRefs",
             J::array({"power.couplingC", "power.gridLeakR", "power.biasV"})}});
    branch("output-coupling-b", "biased-grid-coupling",
           J::array({"phase-plate-b", "output-grid-b", "ground"}),
           {{"parameterRefs",
             J::array({"power.couplingC", "power.gridLeakR", "power.biasV"})}});
    branch(
        "output-pair", "empirical-pentode-pair-transformer",
        J::array({"output-grid-a", "output-grid-b", "reservoir",
                  "screen-supply", "primary-coordinate", "speaker-output",
                  "ground"}),
        {{"law", "PowerDevices"},
         {"boundary",
          "reduced differential primary solve; fixed ideal turns conversion"},
         {"primaryToSecondaryImpedanceRatio", 500}});
    branch(
        "reactive-load", "referred-parallel-r-l-resonant-branch",
        J::array({"primary-coordinate", "ground"}),
        {{"parameterRefs", J::array({"load.reflectedR", "load.inductance"})},
         {"seriesResonanceOhms", 6000},
         {"seriesResonanceHenries", 0.6},
         {"seriesResonanceFarads", 6.6e-6},
         {"boundary", "values referred to differential primary coordinate"}});
    branch("regularizer-stack", "resistor", J::array({"stack-input", "ground"}),
           {{"ohm", 1e10}});
    branch("regularizer-slope", "resistor", J::array({"slope", "ground"}),
           {{"ohm", 1e10}});
    return {{"editableConnections", false},
            {"topology", topologyId(topology_)},
            {"nodes", nodes},
            {"devices", devices},
            {"supplyCoupling", "partitioned sample voltage/current exchange; "
                               "not simultaneous whole-circuit solve"}};
  }

private:
  Topology topology_;
  Applied applied_{};
  template <class F> void visit(F &&f) {
    const char *pre = topology_ == Topology::MarshallPair
                          ? "Published Marshall2204 pair/stack component "
                            "choice; empirical software device model"
                          : "Generic software pair component choice; published "
                            "Fender5F6A stack values";
    const char *power =
        "Reduced power circuit engineering value; not measured hardware";
    const char *interface = "Explicit reduced interstage network";
#define P(id, label, field, lo, hi)                                            \
  f("preamp." id, label, "resistor", "ohm", applied_.preamp.field, lo, hi, pre)
    P("gridR", "Grid stopper", gridR, 10, 1e8);
    P("gridLeakR", "Input grid leak", gridLeakR, 10, 1e8);
    P("plateR", "Triode plate resistor", plateR, 10, 1e8);
    P("cathodeR", "Triode cathode resistor", cathodeR, 10, 1e8);
    P("followerR", "Follower cathode resistor", followerR, 10, 1e8);
    P("slopeR", "Tone slope resistor", slopeR, 10, 1e8);
    P("trebleR", "Treble potentiometer", trebleR, 10, 1e8);
    P("bassR", "Bass potentiometer", bassR, 10, 1e8);
    P("midR", "Mid potentiometer", midR, 10, 1e8);
#undef P
#define C(id, label, field, lo)                                                \
  f("preamp." id, label, "capacitor", "F", applied_.preamp.field, lo, 1e-3, pre)
    C("inputC", "Input coupling capacitor", inputC, 1e-12);
    C("cathodeC", "Cathode bypass capacitor", cathodeC, 0);
    C("trebleC", "Treble capacitor", trebleC, 1e-12);
    C("bassC", "Bass capacitor", bassC, 1e-12);
    C("midC", "Mid capacitor", midC, 1e-12);
#undef C
#define W(id, label, field, unit, lo, hi)                                      \
  f("power." id, label,                                                        \
    unit[0] == 'F'   ? "capacitor"                                             \
    : unit[0] == 'V' ? "voltage-source"                                        \
                     : "resistor",                                             \
    unit, applied_.power.field, lo, hi, power)
    W("sourceV", "Reservoir source", reservoirSourceVolts, "V", 100, 600);
    W("sourceR", "Reservoir resistance", reservoirResistanceOhms, "ohm", 1,
      2000);
    W("reservoirC", "Reservoir capacitor", reservoirCapacitanceFarads, "F",
      1e-6, .01);
    W("screenR", "Screen supply resistor", screenResistanceOhms, "ohm", 1,
      10000);
    W("screenC", "Screen supply capacitor", screenCapacitanceFarads, "F", 1e-6,
      .01);
    W("preampR", "Preamp supply resistor", preampResistanceOhms, "ohm", 100,
      200000);
    W("preampC", "Preamp supply capacitor", preampCapacitanceFarads, "F", 1e-6,
      .01);
    W("phasePlateA", "Phase inverter plate A", phasePlateAOhms, "ohm", 1000,
      500000);
    W("phasePlateB", "Phase inverter plate B", phasePlateBOhms, "ohm", 1000,
      500000);
    W("phaseCathodeR", "Phase inverter cathode", phaseCathodeOhms, "ohm", 10,
      10000);
    W("phaseTailR", "Phase inverter tail", phaseTailOhms, "ohm", 100, 100000);
    W("phaseReferenceV", "Tail reference", phaseTailReferenceVolts, "V", 0, 80);
    W("gridLeakR", "Output grid leak", outputGridLeakOhms, "ohm", 10000,
      1000000);
    W("couplingC", "Output coupling capacitor", outputCouplingFarads, "F", 1e-9,
      1e-6);
    W("biasV", "Output grid bias", outputBiasVolts, "V", -100, -5);
#undef W
    f("master.resistance", "Master pot total load", "potentiometer", "ohm",
      applied_.preamp.loadR, 10, 1e8, interface);
    f("interface.couplingC", "Interstage DC blocking capacitor", "capacitor",
      "F", applied_.interfaceC, 1e-9, 1e-5, interface);
    f("interface.gridLeakR", "Interstage grid leak", "resistor", "ohm",
      applied_.interfaceR, 10000, 1e7, interface);
    f("load.reflectedR", "Reflected half-primary resistance", "resistor", "ohm",
      applied_.loadR, 100, 10000, power);
    f("load.inductance", "Reflected load inductance", "inductor", "H",
      applied_.loadL, .05, 10, power);
#define K(field)                                                               \
  f("control." #field, #field, "control", "ratio", applied_.field, 0, 1,       \
    "Normalized software control of the documented subcircuit")
    K(master);
    K(bass);
    K(mid);
    K(treble);
    K(feedback);
    K(presence);
    K(depth);
#undef K
  }
};
} // namespace rig::circuit
