#pragma once
// Leslie 122 rotor block (catalogue 31000): the two rotating sound sources of the cabinet and the stereo microphone
// pair that hears them. The 122's amplifier and its 800 Hz dividing network are a circuit (library netlist
// leslie-122-31000, run by CircuitUnit); its two speaker-level outputs feed this block:
//   horn  = the treble band, the voltage across the compression driver (io.output)  -> the horn rotor
//   drum  = the bass band, the voltage across the 15-inch driver (io.outputR)       -> the drum (bass rotor)
// There is no crossover here: the circuit's dividing network makes the split.
//
// Each rotor: a speed that approaches its chorale / tremolo target exponentially with separate acceleration and
// braking time constants (the motors and belts), an angle, and per microphone a direct path and a cabinet-wall
// reflection. Each path is a variable delay (the rotating mouth's distance to the microphone: Doppler), a directivity
// gain and a directivity low-pass (the mouth facing the microphone is louder and brighter). The two microphones
// stand at +-spread/2 around the cabinet front.
//
// Published figures used (the 122 service manual gives none of these):
//   speeds and time constants, measured on a Leslie 122: J. Herrera, C. Hanson, J. S. Abel, "Discrete Time Emulation
//   of the Leslie Speaker", AES 127th Convention (2009), paper 7925: horn 0.672 Hz chorale / 7.056 Hz tremolo,
//   drum 5.955 Hz tremolo; horn accelerates with tau 0.161 s and brakes with 0.321 s, drum 4.127 s / 1.371 s.
//   The paper's drum chorale figure (0.101 Hz) is widely flagged as a misprint; the drum's chorale speed is Leslie's
//   own 40 rpm (0.667 Hz). The rotors turn in opposite directions.
//   horn radius 19.2 cm, drum radius 22 cm (the effective radii used with those measurements).
// Fitted (no published data): the directivity depths and low-pass corners, the reflection path, the cabinet colour.
//
// Real-time: no allocation after prepare(), no locks. Deterministic: every state advances per sample, so rendering a
// block as consecutive sub-blocks is bit-identical (RackGraph pipelining). Control values written by setParams() are
// read once per begin() call; derived coefficients are recomputed on the audio thread when they change.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

namespace rig::dsp {

struct LeslieRotorParams { // the catalogue's 15 parameters (Pwr is the host's bypass), raw 0..1 values
    float speed = 0.5f;       // SF: < 0.5 chorale (slow), >= 0.5 tremolo (fast)
    float balance = 0.5f;     // B: 0 drum only, 0.5 as built, 1 horn only
    float pan = 0.5f;         // P: stereo balance of the microphone pair
    float distance = 0.5f;    // D: microphone distance from the rotor axes, 0.35 m .. 1.6 m (0.75 m at 0.5)
    float hornSlow = 0.5f;    // TS: horn chorale speed, 0 = stopped (brake), 0.5 = 0.672 Hz, 1 = 2x
    float hornFast = 0.5f;    // TF: horn tremolo speed, 0.5x .. 1.5x of 7.056 Hz
    float hornAccel = 0.5f;   // TA: horn acceleration, 0.5 = the measured time constants, 0 = 4x slower, 1 = 4x faster
    float hornSpread = 0.5f;  // TSp: angle between the horn's two microphones, 0 .. 180 degrees
    float drumSlow = 0.5f;    // BS
    float drumFast = 0.5f;    // BF
    float drumAccel = 0.5f;   // BA
    float drumSpread = 0.5f;  // BSp
    float cab = 0.5f;         // CTn: cabinet colour (top cut 14 kHz .. 3.5 kHz, low-mid body)
    float mix = 0.5f;         // Wt: dry input / rotor microphones
};

class LeslieRotors {
public:
    // Published speeds (Hz) and time constants (s); see the header comment.
    static constexpr double kHornSlowHz = 0.672, kHornFastHz = 7.056;
    static constexpr double kDrumSlowHz = 40.0 / 60.0, kDrumFastHz = 5.955;
    static constexpr double kHornAccS = 0.161, kHornDecS = 0.321;
    static constexpr double kDrumAccS = 4.127, kDrumDecS = 1.371;
    static constexpr double kHornRadiusM = 0.192, kDrumRadiusM = 0.22;
    static constexpr double kSoundMps = 343.0;
    // Gain from the circuit's speaker-level outputs (in units of its outputVoltsPerFS) to the microphones: the unit's
    // output at its defaults matches the earlier behavioural rotary unit within 1 dB (test_circuit_unit).
    static constexpr float kMakeup = 1.5f;

    void prepare(double sampleRate) {
        fs_ = sampleRate > 1000.0 ? sampleRate : 48000.0;
        // longest path: 1.6 m + 0.22 m + the reflection's 0.45 m, plus the interpolator's reach
        std::size_t need = static_cast<std::size_t>(std::ceil(fs_ * 2.4 / kSoundMps)) + 8;
        std::size_t n = 16;
        while (n < need) n <<= 1;
        for (auto& r : rot_) {
            r.line.assign(n, 0.0f);
            r.mask = n - 1;
        }
        lastIn_ = {};
        lastIn_.speed = -1.0f; // force the coefficient pass
        reset();
    }

    void reset() noexcept {
        for (int k = 0; k < 2; ++k) {
            Rotor& r = rot_[k];
            std::fill(r.line.begin(), r.line.end(), 0.0f);
            r.w = 0;
            r.angle = k == 0 ? 0.0 : 0.25; // a fixed start, so a render is reproducible
            r.lp.fill(0.0f);
            r.primed = false;
        }
        cab_.fill(0.0f);
        primed_ = false;
    }

    // Control thread (or before process). Plain stores: begin() reads them once per call.
    void setParams(const LeslieRotorParams& p) noexcept { in_ = p; }

    // Horn rotor angle, 0..1 (the face's rotor lamp / live readout).
    float hornPhase() const noexcept { return static_cast<float>(rot_[0].angle); }
    // Drum rotor angle, 0..1 (it turns the other way: the angle falls).
    float drumPhase() const noexcept { return static_cast<float>(rot_[1].angle); }
    double rotorHz(int k) const noexcept { return rot_[k & 1].hz; }

    // Audio thread: begin() once per rendered block (or sub-block), then tick() once per sample. horn and drum are the
    // circuit's band outputs in units of its outputVoltsPerFS; dryL / dryR the unit's input at the same sample (the
    // caller reads them before it writes the outputs, so an in-place block works).
    void begin() noexcept { update(); }
    void tick(float dl, float dr, float horn, float drum, float& outL, float& outR) noexcept {
        const float x[2] = {horn, drum};
        // control glides (about 30 ms), so a knob drag never steps a delay or a gain
        for (int j = 0; j < kNumGlide; ++j) cur_[j] += glide_ * (tgt_[j] - cur_[j]);
        const float dist = cur_[kDist];
        float yl = 0.0f, yr = 0.0f;
        for (int k = 0; k < 2; ++k) {
            Rotor& r = rot_[k];
            const Shape& s = shape_[k];
            // motor: first-order approach to the target speed, accelerating or braking
            const double kk = r.target > r.hz ? r.kAcc : r.kDec;
            r.hz += kk * (r.target - r.hz);
            r.angle += r.dir * r.hz / fs_;
            r.angle -= std::floor(r.angle);
            r.line[r.w & r.mask] = x[k];
            const double th = 2.0 * std::numbers::pi * r.angle;
            const float ca = static_cast<float>(std::cos(th)), sa = static_cast<float>(std::sin(th));
            const float half = cur_[k == 0 ? kHornHalf : kDrumHalf]; // half the microphones' angle, radians
            const float ch = std::cos(half), sh = std::sin(half);
            const float g = cur_[k == 0 ? kHornGain : kDrumGain];
            const float R = static_cast<float>(s.radius);
            for (int m = 0; m < 2; ++m) {
                // the mouth's direction relative to microphone m (L at -half, R at +half)
                const float sm = m == 0 ? -sh : sh;
                const float c = ca * ch + sa * sm;  // cos(theta - phi_m)
                const float sn = sa * ch - ca * sm; // sin(theta - phi_m)
                // direct path
                const float a = dist - R * c, b = R * sn;
                const float d1 = std::sqrt(a * a + b * b);
                const float w = 0.5f + 0.5f * c; // 1 = the mouth faces the microphone
                const float y1 = r.read(static_cast<float>(d1 * fs_ / kSoundMps));
                float& l1 = r.lp[static_cast<std::size_t>(m * 2)];
                l1 += (s.lpBack + (s.lpFront - s.lpBack) * w) * (y1 - l1);
                const float dir1 = s.floor + (1.0f - s.floor) * (s.squared ? w * w : w);
                float y = dir1 * (dist / d1) * l1;
                // the wall reflection: the mouth facing away from the microphone, a longer path
                const float a2 = dist + R * c;
                const float d2 = std::sqrt(a2 * a2 + b * b) + kReflM;
                const float y2 = r.read(static_cast<float>(d2 * fs_ / kSoundMps));
                float& l2 = r.lp[static_cast<std::size_t>(m * 2 + 1)];
                const float w2 = 1.0f - w;
                l2 += (s.lpBack + (s.lpFront - s.lpBack) * w2) * kReflDark * (y2 - l2);
                const float dir2 = s.floor + (1.0f - s.floor) * (s.squared ? w2 * w2 : w2);
                y += cur_[kRefl] * dir2 * l2;
                (m == 0 ? yl : yr) += g * y;
            }
            ++r.w;
        }
        // cabinet colour: two one-pole low-passes plus a low-mid body (80-400 Hz band)
        for (int m = 0; m < 2; ++m) {
            float& v = m == 0 ? yl : yr;
            float* st = &cab_[static_cast<std::size_t>(m * 4)];
            st[0] += cabA_ * (v - st[0]);
            st[1] += cabA_ * (st[0] - st[1]);
            st[2] += bodyHi_ * (v - st[2]);
            st[3] += bodyLo_ * (v - st[3]);
            v = st[1] + cur_[kBody] * (st[2] - st[3]);
        }
        const float mix = cur_[kMix], wet = mix * kMakeup;
        outL = (1.0f - mix) * dl + wet * yl * cur_[kPanL];
        outR = (1.0f - mix) * dr + wet * yr * cur_[kPanR];
    }

    // State hand-off to a fresh copy (a knob edit re-prepares a copy of the unit): the rotors keep turning, the
    // delay lines keep their history, the glides continue from where they were.
    void adoptRuntimeStateFrom(LeslieRotors& o) noexcept {
        if (o.fs_ != fs_) return;
        for (int k = 0; k < 2; ++k) {
            Rotor& r = rot_[k];
            Rotor& p = o.rot_[k];
            if (r.line.size() != p.line.size()) return;
            std::swap(r.line, p.line);
            r.w = p.w;
            r.angle = p.angle;
            r.hz = p.hz;
            r.lp = p.lp;
            r.primed = p.primed;
        }
        cab_ = o.cab_;
        cur_ = o.cur_;
        primed_ = o.primed_;
    }

private:
    struct Shape {
        double radius;
        float floor;    // directivity gain with the mouth facing away
        bool squared;   // a narrower beam (the horn)
        float lpFront;  // one-pole coefficient at 48 kHz-equivalent corners, set in update()
        float lpBack;
    };
    struct Rotor {
        std::vector<float> line;
        std::size_t mask = 0, w = 0;
        double angle = 0, hz = 0, target = 0, kAcc = 0, kDec = 0, dir = 1;
        std::array<float, 4> lp{}; // per microphone: direct and reflection directivity low-passes
        bool primed = false;
        float read(float delay) const noexcept { // 4-point Hermite
            const float pos = static_cast<float>(w) - delay;
            const float fl = std::floor(pos);
            const float f = pos - fl;
            const auto i = static_cast<std::ptrdiff_t>(fl);
            auto at = [&](std::ptrdiff_t j) { return line[static_cast<std::size_t>(j) & mask]; };
            const float xm = at(i - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
            const float c1 = 0.5f * (x1 - xm), c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
            const float c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
            return ((c3 * f + c2) * f + c1) * f + x0;
        }
    };
    enum Glide { kDist, kHornHalf, kDrumHalf, kHornGain, kDrumGain, kPanL, kPanR, kMix, kRefl, kBody, kNumGlide };

    static constexpr float kReflM = 0.45f;   // extra path of the wall reflection (fitted)
    static constexpr float kReflDark = 0.5f; // the reflection's directivity low-pass runs at half the direct one's

    float onePole(double hz) const noexcept { return static_cast<float>(1.0 - std::exp(-2.0 * std::numbers::pi * hz / fs_)); }

    // Audio thread, once per begin(): derived values from the control snapshot when it changed.
    void update() noexcept {
        const LeslieRotorParams p = in_;
        if (!primed_ || !same(p, lastIn_)) {
            lastIn_ = p;
            auto c01 = [](float v) { return std::clamp(v, 0.0f, 1.0f); };
            const bool fast = p.speed >= 0.5f;
            const double accel[2] = {std::pow(4.0, 1.0 - 2.0 * c01(p.hornAccel)), std::pow(4.0, 1.0 - 2.0 * c01(p.drumAccel))};
            const double slow[2] = {kHornSlowHz * 2.0 * c01(p.hornSlow), kDrumSlowHz * 2.0 * c01(p.drumSlow)};
            const double quick[2] = {kHornFastHz * (0.5 + c01(p.hornFast)), kDrumFastHz * (0.5 + c01(p.drumFast))};
            const double tauA[2] = {kHornAccS, kDrumAccS}, tauD[2] = {kHornDecS, kDrumDecS};
            for (int k = 0; k < 2; ++k) {
                Rotor& r = rot_[k];
                r.dir = k == 0 ? 1.0 : -1.0;
                r.target = fast ? quick[k] : slow[k];
                r.kAcc = 1.0 - std::exp(-1.0 / (tauA[k] * accel[k] * fs_));
                r.kDec = 1.0 - std::exp(-1.0 / (tauD[k] * accel[k] * fs_));
                if (!r.primed) { // a fresh unit starts at its running speed
                    r.hz = r.target;
                    r.primed = true;
                }
            }
            tgt_[kDist] = static_cast<float>(0.35 * std::pow(1.6 / 0.35, c01(p.distance)));
            tgt_[kHornHalf] = 0.5f * std::numbers::pi_v<float> * c01(p.hornSpread);
            tgt_[kDrumHalf] = 0.5f * std::numbers::pi_v<float> * c01(p.drumSpread);
            tgt_[kHornGain] = std::min(1.0f, 2.0f * c01(p.balance));
            tgt_[kDrumGain] = std::min(1.0f, 2.0f * (1.0f - c01(p.balance)));
            tgt_[kPanL] = std::min(1.0f, 2.0f * (1.0f - c01(p.pan)));
            tgt_[kPanR] = std::min(1.0f, 2.0f * c01(p.pan));
            tgt_[kMix] = c01(p.mix);
            tgt_[kRefl] = 0.15f + 0.3f * c01(p.distance); // farther away: more of the room
            tgt_[kBody] = 0.6f * c01(p.cab);
            cabA_ = onePole(14000.0 * std::pow(0.25, c01(p.cab)) * 1.55); // two poles: -3 dB near the named corner
            bodyHi_ = onePole(400.0);
            bodyLo_ = onePole(80.0);
            shape_[0].lpFront = onePole(18000.0);
            shape_[0].lpBack = onePole(2200.0);
            shape_[1].lpFront = onePole(6000.0);
            shape_[1].lpBack = onePole(1500.0);
            glide_ = onePole(1.0 / (2.0 * std::numbers::pi * 0.03));
        }
        if (!primed_) {
            cur_ = tgt_;
            primed_ = true;
        }
    }
    static bool same(const LeslieRotorParams& a, const LeslieRotorParams& b) noexcept {
        return a.speed == b.speed && a.balance == b.balance && a.pan == b.pan && a.distance == b.distance &&
               a.hornSlow == b.hornSlow && a.hornFast == b.hornFast && a.hornAccel == b.hornAccel &&
               a.hornSpread == b.hornSpread && a.drumSlow == b.drumSlow && a.drumFast == b.drumFast &&
               a.drumAccel == b.drumAccel && a.drumSpread == b.drumSpread && a.cab == b.cab && a.mix == b.mix;
    }

    double fs_ = 48000.0;
    LeslieRotorParams in_{}, lastIn_{};
    std::array<Rotor, 2> rot_{};
    std::array<Shape, 2> shape_{{{kHornRadiusM, 0.3f, true, 0.0f, 0.0f}, {kDrumRadiusM, 0.5f, false, 0.0f, 0.0f}}};
    std::array<float, kNumGlide> tgt_{}, cur_{};
    std::array<float, 8> cab_{};
    float cabA_ = 1.0f, bodyHi_ = 0.0f, bodyLo_ = 0.0f, glide_ = 1.0f;
    bool primed_ = false;
};

} // namespace rig::dsp
