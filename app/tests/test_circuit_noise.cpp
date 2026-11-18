// A small control-thread check; including the server also compiles both settings paths.
#include "audio/RealtimeAudioServer.hpp"
#include "circuit/CircuitUnit.hpp"
#include "dsp/ComponentModel.hpp"
#include <cstdio>
#include <cstdlib>

int main() {
    using namespace rig;
    const char* env = std::getenv("HEADROOM_CIRCUIT_NOISE");
    if (ce::circuitNoiseEnabled() != (env && std::string_view(env) == "1")) return 1;
    auto unit = ce::makeCircuitUnitAlways(*catalog::findByTableId(15000));
    if (!unit) return 2;
    ce::setCircuitNoiseEnabled(false);
    unit->prepare(48000, 64);
    if (ce::circuitNoiseNeedsRefresh(*unit)) return 3;
    ce::setCircuitNoiseEnabled(true);
    if (!ce::circuitNoiseNeedsRefresh(*unit) || !ce::refreshCircuitNoise(*unit) || ce::circuitNoiseNeedsRefresh(*unit)) return 4;
    ce::setCircuitNoiseEnabled(false);
    if (!ce::refreshCircuitNoise(*unit) || ce::circuitNoiseNeedsRefresh(*unit)) return 5;
    unit->setStringParameter(model::kModelParam, R"({"noise":false})");
    ce::setCircuitNoiseEnabled(true);
    if (ce::circuitNoiseNeedsRefresh(*unit)) return 6;
    std::puts("PASS: default/env, off/on/off circuit rebuilds, model noise veto");
}
