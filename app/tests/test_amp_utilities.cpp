#undef NDEBUG // test checks and setup remain active in Release builds
#include "../src/dsp/AmpUtilities.hpp"
#include <cassert>
#include <limits>

int main() {
    rig::amp::AmpInputUtilities input;
    input.prepare(48000);
    // Sustain below the old implicit gate threshold is preserved exactly.
    for (int i = 0; i < 48000; ++i) assert(input.processDigital(1e-6f) == 1e-6f);
    input.setGateEnabled(true);
    for (int i = 0; i < 48000; ++i) input.processDigital(1e-6f);
    assert(input.processDigital(1e-6f) < 1e-8f);
    input.setGateEnabled(false);
    assert(input.processDigital(1e-6f) == 1e-6f);
    assert(input.setVoltageScale(rig::circuit::VoltageCalibration::assumed(2)));
    assert(input.processVolts(0.25f) == 0.5);
    assert(!input.setVoltageScale(rig::circuit::VoltageCalibration::assumed(0)));
    assert(!input.setVoltageScale(rig::circuit::VoltageCalibration::assumed(-1)));
    assert(input.processVolts(0.25f) == 0.5); // rejected edit kept last valid scale
    assert(input.processDigital(std::numeric_limits<float>::quiet_NaN()) == 0);
}
