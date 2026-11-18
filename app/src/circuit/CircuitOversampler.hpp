#pragma once
// Oversampler for circuit units: a cascade of half-band IIR stages up to 32x (amp::Oversampler stops at 8x).
// Stage 1 is the steep design that sets the passband; stage 2 and the rest only have to reject images that sit
// far from the audio band. For 2x, 4x and 8x it runs the same stages in the same order as amp::Oversampler, so
// those factors are bit-identical; 16x and 32x add further copies of the last stage.
//
// Why 32x: hard-switching circuits (a silicon Fuzz Face at 0.5 V) produce edges whose harmonics fall only about
// 6 dB per octave, so aliasing drops slowly with rate. The aliasing gate (tests/test_aliasing.cpp) measured the
// Fuzz Face's worst image at -35 dB at 8x, -43 dB at 16x and -62 dB at 32x.
#include "dsp/AmpModeling.hpp"

#include <array>

namespace rig::ce {

class CircuitOversampler {
public:
    static constexpr int kMaxFactor = 32;
    static constexpr int kMaxStages = 5;

    // Control thread only (computes the half-band coefficients).
    void prepare(int factor) {
        stages_ = 0;
        while ((1 << (stages_ + 1)) <= factor && stages_ < kMaxStages) ++stages_;
        factor_ = 1 << stages_;
        const auto c1 = amp::HalfbandDesign::compute(10, 0.035);
        const auto c2 = amp::HalfbandDesign::compute(6, 0.20);
        const auto c3 = amp::HalfbandDesign::compute(4, 0.30);
        // Stages beyond 8x (only used for hard-switching circuits) carry the stronger 6-coefficient design: a near-
        // square wave puts its full harmonic energy at their images, and the 4-coefficient stage's stopband set a
        // -60 dB floor that more oversampling could not get under (tests/test_aliasing.cpp).
        const auto c4 = amp::HalfbandDesign::compute(6, 0.20);
        for (int ch = 0; ch < 2; ++ch) {
            up1_[ch].setCoefs(c1), dn1_[ch].setCoefs(c1);
            up2_[ch].setCoefs(c2), dn2_[ch].setCoefs(c2);
            up3_[0][ch].setCoefs(c3), dn3_[0][ch].setCoefs(c3); // stage 3 (8x): as amp::Oversampler
            for (int s = 1; s < kMaxStages - 2; ++s) up4_[s - 1][ch].setCoefs(c4), dn4_[s - 1][ch].setCoefs(c4);
        }
    }
    void reset() {
        for (int ch = 0; ch < 2; ++ch) {
            up1_[ch].reset(), dn1_[ch].reset(), up2_[ch].reset(), dn2_[ch].reset();
            up3_[0][ch].reset(), dn3_[0][ch].reset();
            for (int s = 0; s < kMaxStages - 3; ++s) up4_[s][ch].reset(), dn4_[s][ch].reset();
        }
    }
    int factor() const noexcept { return factor_; }

    // Writes factor() samples into out (out must hold kMaxFactor).
    void up(int ch, float x, float* out) noexcept {
        out[0] = x;
        int n = 1;
        float tmp[kMaxFactor];
        for (int s = 0; s < stages_; ++s) {
            for (int i = 0; i < n; ++i) upStage(s, ch, out[i], tmp[2 * i], tmp[2 * i + 1]);
            n *= 2;
            for (int i = 0; i < n; ++i) out[i] = tmp[i];
        }
    }
    // Reads factor() samples from in (overwritten) and returns one.
    float down(int ch, float* in) noexcept {
        int n = factor_;
        for (int s = stages_ - 1; s >= 0; --s) {
            n /= 2;
            for (int i = 0; i < n; ++i) in[i] = dnStage(s, ch, in[2 * i], in[2 * i + 1]);
        }
        return in[0];
    }

private:
    void upStage(int s, int ch, float x, float& a, float& b) noexcept {
        if (s == 0) up1_[ch].up(x, a, b);
        else if (s == 1) up2_[ch].up(x, a, b);
        else if (s == 2) up3_[0][ch].up(x, a, b);
        else up4_[s - 3][ch].up(x, a, b);
    }
    float dnStage(int s, int ch, float a, float b) noexcept {
        if (s == 0) return dn1_[ch].down(a, b);
        if (s == 1) return dn2_[ch].down(a, b);
        if (s == 2) return dn3_[0][ch].down(a, b);
        return dn4_[s - 3][ch].down(a, b);
    }
    int stages_ = 0, factor_ = 1;
    std::array<amp::HalfbandIIR<10>, 2> up1_{}, dn1_{};
    std::array<amp::HalfbandIIR<6>, 2> up2_{}, dn2_{};
    std::array<std::array<amp::HalfbandIIR<4>, 2>, 1> up3_{}, dn3_{};
    std::array<std::array<amp::HalfbandIIR<6>, 2>, kMaxStages - 3> up4_{}, dn4_{};
};

} // namespace rig::ce
