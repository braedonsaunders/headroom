#pragma once
// Behavioural blocks inside a circuit (docs/CIRCUIT_ENGINE.md §4): elements whose physics is not electrical but whose
// ports are. A block senses an electrical input (a coil current) and drives an electrical output through a Thévenin
// source one sample later, which is exact for blocks with inherent delay (a spring tank's 30-40 ms flight time).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace rig::ce {

// Spring reverb tank (Accutronics/Hammond type, e.g. 4AB3C1B: two springs). Per spring: input high-pass, the wave's
// flight from the input transducer to the output transducer (a delay line plus a dispersive cascade of first-order
// allpasses: low frequencies travel slower, the characteristic "drip" chirp), a reflection at the output end, the flight
// back, and a reflection at the input end, with the losses of each round trip set by the decay time. The output
// transducer sees the forward wave, so the first arrival comes after one flight time and the echoes follow every round
// trip (2× the flight): t, 3t, 5t... After Välimäki, Parker & Abel, "Parametric spring reverberation effect", JAES 2010
// (structure simplified; parameters are physical: flight delay, T60, dispersion, bandwidth).
class SpringTank {
public:
    struct Params {
        double delay1 = 0.0334, delay2 = 0.0392; // s: one-way flight time per spring (4AB3: "long" 2-spring)
        double delay3 = 0.0;                      // s: a third spring (0 = two-spring tank)
        double t60 = 2.9;                         // s
        double disp = 0.62;                       // allpass coefficient (dispersion strength)
        int stages = 36;                          // allpasses per spring per round trip (half each way)
        double lp = 4300, hp = 90;                // Hz: transducer/spring bandwidth
        double gain = 1.0;                        // output scaling (volts at the output transducer per amp of drive)
    };
    void prepare(double fs, const Params& p) {
        p_ = p;
        fs_ = fs;
        ns_ = p.delay3 > 0 ? 3 : 2;
        for (int s = 0; s < ns_; ++s) {
            auto& sp = spring_[s];
            const double td = s == 0 ? p.delay1 : (s == 1 ? p.delay2 : p.delay3);
            const int half = std::max(1, p.stages / 2);
            const double apDelay = half * (1.0 + p.disp) / (1.0 - p.disp); // low-frequency group delay of one half-cascade (samples)
            for (int dir = 0; dir < 2; ++dir) {
                auto& ln = sp.line[dir];
                ln.len = std::max(2, static_cast<int>(std::lround(td * fs - apDelay)));
                ln.buf.assign(static_cast<std::size_t>(ln.len), 0.0);
                ln.ap.assign(static_cast<std::size_t>(half), {0.0, 0.0});
                ln.pos = 0;
            }
            sp.fb = std::pow(10.0, -3.0 * 2.0 * td / p.t60); // per round trip (2 flights)
            sp.lpz = sp.hpz = sp.hpx = 0;
        }
        lpA_ = std::exp(-2.0 * M_PI * p.lp / fs);
        hpA_ = std::exp(-2.0 * M_PI * p.hp / fs);
    }
    void reset() {
        for (auto& sp : spring_) {
            for (auto& ln : sp.line) {
                std::fill(ln.buf.begin(), ln.buf.end(), 0.0);
                for (auto& a : ln.ap) a = {0.0, 0.0};
                ln.pos = 0;
            }
            sp.lpz = sp.hpz = sp.hpx = 0;
        }
    }
    double process(double x) noexcept {
        double y = 0;
        static constexpr double w2[2] = {0.55, 0.45}, w3[3] = {0.36, 0.33, 0.31};
        for (int s = 0; s < ns_; ++s) {
            auto& sp = spring_[s];
            // input transducer bandwidth (1st-order HP)
            const double h = hpA_ * (sp.hpz + x - sp.hpx);
            sp.hpx = x;
            sp.hpz = h;
            auto& fwd = sp.line[0];
            auto& bwd = sp.line[1];
            const double arrive = fwd.buf[static_cast<std::size_t>(fwd.pos)]; // forward wave at the output transducer
            const double back = bwd.buf[static_cast<std::size_t>(bwd.pos)];   // reflected wave back at the input end
            sp.lpz = (1.0 - lpA_) * arrive + lpA_ * sp.lpz;                   // losses rise with frequency (once per trip)
            fwd.push(h + sp.fb * back, p_.disp);                              // input end: drive + reflection
            bwd.push(sp.lpz, p_.disp);                                        // output end: reflection
            y += (ns_ == 3 ? w3[s] : w2[s]) * sp.lpz;
        }
        return p_.gain * y;
    }

private:
    struct Ap {
        double x1, y1;
    };
    struct Line {
        std::vector<double> buf;
        std::vector<Ap> ap;
        int len = 2, pos = 0;
        void push(double u, double c) noexcept {
            for (auto& a : ap) { // dispersion: y = c·u + x1 − c·y1
                const double o = c * u + a.x1 - c * a.y1;
                a.x1 = u;
                a.y1 = o;
                u = o;
            }
            buf[static_cast<std::size_t>(pos)] = u;
            if (++pos >= len) pos = 0;
        }
    };
    struct Spring {
        Line line[2]; // 0: input → output transducer, 1: back
        double fb = 0, lpz = 0, hpz = 0, hpx = 0;
    };
    Params p_;
    double fs_ = 96000, lpA_ = 0, hpA_ = 0;
    Spring spring_[3];
    int ns_ = 2;
};

// Tape loop with one record and up to three playback heads (Roland RE-201; DEVICES.md §6.3, v1). The tape is a
// spatial buffer: the record head writes the magnetisation at the tape coordinate s(t) = ∫v dt (os cells per engine
// sample at full speed), each playback head reads at a fixed distance behind it, so a speed change moves every echo's
// delay and pitch exactly as a real transport does (a speed-type delay, Zavalishin & Parker DAFx-18).
//  * Record (v1, coordinator decision): ideal AC bias approximated by the anhysteretic curve m = L((H + α·Ms·m)/a)
//    (Langevin L; Chowdhury DAFx-19 tape constants Ms, a, α), H = drive·i_rec. Explicit bias and Jiles–Atherton
//    hysteresis are v2.
//  * Playback: the head voltage is gain·dΦ/dt of the flux read under it (Faraday), after the playback losses (gap,
//    spacing and coating thickness: Chowdhury DAFx-19 / Bertram) applied as a spatial FIR: their time-domain cut-off
//    scales with the tape speed by construction.
//  * Speed: v = 1/(1 + (range − 1)·(1 − x)) of full speed for the Repeat Rate position x (delay linear in x, A), slewed
//    by the motor servo (τ, A), times (1 + wow/flutter): 3.5 Hz pinch wheel, 22 Hz capstan (DAFX 2nd ed. via Kaloinen)
//    plus drift.
class TapeLoop {
public:
    struct Params {
        double delays[3] = {0.069, 0.131, 0.189}; // s, head 1..3 at full speed
        int heads = 3;
        double range = 2.57;  // slowest delay / fastest delay
        double ms = 3.5e5, a = 22e3, alpha = 1.6e-3; // A/m, A/m, -
        double drive = 4e8;   // A/m per ampere of record-head current
        double gain = 5e-7;   // V per (unit magnetisation per second)
        double v0 = 0.19;     // m/s at full speed (A: not found)
        double gap = 5e-6, spacing = 3e-6, thickness = 10e-6; // m
        double wow = 1e-3, flutter = 5e-4; // fractional speed modulation amplitudes
        double servo = 0.25;  // s, motor speed response
        std::uint32_t seed = 7;
    };
    static constexpr int kOs = 2;   // tape cells per engine sample at full speed
    static constexpr int kTaps = 127; // playback-loss FIR (odd, linear phase; spacing and coating kernels span tens of cells)

    void prepare(double fs, const Params& p) {
        p_ = p;
        fs_ = fs;
        double dmax = 0;
        for (int h = 0; h < p.heads; ++h) dmax = std::max(dmax, p.delays[h]);
        size_ = 1;
        while (size_ < static_cast<std::int64_t>(dmax * fs * kOs) + 4 * kTaps + 64) size_ <<= 1;
        buf_.assign(static_cast<std::size_t>(size_ + kTaps + 2), 0.0);
        for (int h = 0; h < 3; ++h) dist_[h] = p.delays[h] * fs * kOs;
        // spatial loss FIR: cell length dx = v0/(fs·kOs); magnitude over spatial frequency k (rad/m), windowed design
        const double dx = p.v0 / (fs * kOs);
        const int M = 256;
        double mag[M + 1];
        for (int i = 0; i <= M; ++i) {
            const double k = M_PI / dx * i / M; // up to the cell Nyquist
            const double g = k * p.gap / 2, t = k * p.thickness;
            const double gl = g > 1e-12 ? std::sin(g) / g : 1.0;
            const double tl = t > 1e-12 ? (1.0 - std::exp(-t)) / t : 1.0;
            mag[i] = gl * std::exp(-k * p.spacing) * tl;
        }
        const int c = kTaps / 2;
        double sum = 0;
        for (int n = 0; n < kTaps; ++n) {
            double h = 0;
            for (int i = 0; i <= M; ++i) h += (i == 0 || i == M ? 0.5 : 1.0) * mag[i] * std::cos(M_PI * i / M * (n - c));
            h /= M;
            const double w = 0.54 + 0.46 * std::cos(M_PI * (n - c) / (c + 1));
            fir_[n] = h * w;
            sum += fir_[n];
        }
        for (double& f : fir_) f /= sum; // unity at DC (long wavelengths are lossless)
    }
    void reset(double x) {
        std::fill(buf_.begin(), buf_.end(), 0.0);
        s_ = 0;
        v_ = speedFor(x);
        t_ = 0;
        m_ = 0;
        mprev_ = 0;
        lp_ = 0;
        for (auto& r : rd_) r = 0;
        for (auto& r : prev_) r = 0;
        rng_ = p_.seed ? p_.seed : 7u;
        drift_ = 0;
        driftRaw_ = 0;
    }
    // One engine sample: record-head current, Repeat Rate position (0..1). Writes the head voltages for sample k + 1.
    void process(double irec, double x, double* out) noexcept {
        // speed: servo-slewed target × wow / flutter / drift
        const double T = 1.0 / fs_;
        v_ += (speedFor(x) - v_) * (T / (p_.servo + T));
        t_ += T;
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        // drift: Ornstein–Uhlenbeck process, unit variance, corner 0.3 Hz, smoothed by 10 Hz capstan inertia (A)
        constexpr double th = 2.0 * M_PI * 0.3;
        driftRaw_ += -th * driftRaw_ * T + std::sqrt(2.0 * th * T) * ((rng_ & 0xffffff) / 16777216.0 - 0.5) * 3.4641016;
        drift_ += (driftRaw_ - drift_) * (1.0 - std::exp(-2.0 * M_PI * 10.0 * T));
        const double mod = p_.wow * (std::sin(2 * M_PI * 3.5 * t_) + 0.5 * drift_) + p_.flutter * std::sin(2 * M_PI * 22.0 * t_ + 1.3);
        const double v = v_ * (1.0 + mod);
        // record: anhysteretic magnetisation (a few fixed-point/Newton steps on m = L((H + α·Ms·m)/a))
        const double H = p_.drive * irec;
        double m = m_;
        for (int it = 0; it < 4; ++it) {
            const double z = (H + p_.alpha * p_.ms * m) / p_.a;
            double L, dL;
            if (std::abs(z) < 1e-3) {
                L = z / 3.0;
                dL = 1.0 / 3.0;
            } else {
                const double th = std::tanh(z);
                L = 1.0 / th - 1.0 / z;
                dL = 1.0 / (z * z) - (1.0 - th * th) / (th * th);
            }
            const double f = m - L, df = 1.0 - dL * p_.alpha * p_.ms / p_.a;
            m -= f / df;
        }
        // write the cells the head passed during this sample (linear interpolation in time; low-passed to the
        // spatial Nyquist at this speed: a one-pole at ≈ 0.4·v·kOs·fs/2)
        const double fcw = 0.4 * std::max(v, 0.05) * kOs * 0.5 * fs_;
        const double aw = std::exp(-2.0 * M_PI * fcw * T);
        const double mw = lp_ = (1.0 - aw) * m + aw * lp_;
        const double s0 = s_, s1 = s_ + v * kOs;
        for (double c = std::floor(s0) + 1.0; c <= s1; c += 1.0) {
            const double mu = (c - s0) / (s1 - s0);
            const double val = mprev_ + mu * (mw - mprev_);
            const auto idx = static_cast<std::size_t>(static_cast<std::int64_t>(c) & (size_ - 1));
            buf_[idx] = val;
            if (idx < static_cast<std::size_t>(kTaps + 2)) buf_[static_cast<std::size_t>(size_) + idx] = val;
        }
        mprev_ = mw;
        m_ = m;
        s_ = s1;
        // playback: read one sample ahead (the block drives the next sample), loss FIR, then d/dt.
        // fir_ is symmetric around kTaps/2 and buf_ mirrors its first kTaps + 2 cells at the end, so each head is a
        // contiguous dot product vectorised by the compiler.
        const double ahead = s1 + v * kOs;
        for (int h = 0; h < p_.heads; ++h) {
            const double pos = ahead - dist_[h];
            const double fl = std::floor(pos), fr = pos - fl;
            const auto base = static_cast<std::size_t>((static_cast<std::int64_t>(fl) - kTaps / 2) & (size_ - 1));
            const double* __restrict bp = buf_.data() + base;
            double accA = 0, accB = 0;
            {
#pragma clang fp reassociate(on)
                for (int k = 0; k < kTaps; ++k) {
                    const double fk = fir_[k];
                    accA += fk * bp[k];
                    accB += fk * bp[k + 1];
                }
            }
            rd_[h] = accA + fr * (accB - accA);
            out[h] = p_.gain * (rd_[h] - prev_[h]) * fs_;
            prev_[h] = rd_[h];
        }
    }
    double speed() const noexcept { return v_; }

private:
    double speedFor(double x) const noexcept { return 1.0 / (1.0 + (p_.range - 1.0) * (1.0 - std::clamp(x, 0.0, 1.0))); }
    Params p_;
    double fs_ = 48000;
    std::int64_t size_ = 1;
    std::vector<double> buf_;
    double dist_[3] = {};
    double fir_[kTaps] = {};
    double s_ = 0, v_ = 1, t_ = 0, m_ = 0, mprev_ = 0, lp_ = 0, drift_ = 0, driftRaw_ = 0;
    double rd_[3] = {}, prev_[3] = {};
    std::uint32_t rng_ = 7;
};

// Bucket-brigade delay line (MN3002 / MN3005 / MN3007 / MN3207 / SAD1024): docs/CIRCUIT_ENGINE.md §10.12,
// work/circuit-netlists/wave-c/DEVICES.md §2.2. A BBD is a sampled-data device clocked far above the engine's Nyquist
// (37-85 kHz in a CE-1), so it cannot be an electrical element; it is a block between electrical ports.
//  * Clock: a phase accumulator φ (clock periods) advanced by f_clk(t)·T, f_clk from the sensed control voltage through
//    the unit's VCO law (a piecewise-linear table fitted from the real oscillator in ngspice). The LFO is circuit-level.
//  * Input: one bucket sample per clock period, taken at the exact sub-sample instant φ crosses an integer, by centred
//    6-point Lagrange interpolation of the input node's history (the input timeline runs two engine samples behind).
//  * Buckets: a FIFO of N/2 samples; delay = N/(2·f_clk). Charge-transfer loss as a Poisson FIR in the BBD domain,
//    e^{−a}·Σ a^k/k!·z^{−k} with a = N·ε (Weckler 1977: −17.4·N·ε dB at f_clk/2); a static 2nd-order nonlinearity
//    (Raffel & Smith DAFx-10: not clipping) relative to the window centre, hard-limited at the input window; optional
//    white noise per bucket sample (A-weighted datasheet level).
//  * Output: the device's full-period-hold staircase (the two outputs summed, Holters & Parker DAFx-18), stepping
//    half a clock period after the bucket leaves, written with band-limited steps (integrated windowed sinc, cut-off
//    0.45·fs) so clock images above the engine's Nyquist do not fold back. Output events are predicted up to the
//    kernel half-width ahead with the current clock (a few samples of an LFO-rate sweep), so neither the kernel nor
//    the engine's one-sample block coupling adds latency.
class BbdLine {
public:
    struct Params {
        int stages = 1024;
        double eps = 0.1;              // N·ε (total)
        double a2 = 0.0;               // 1/V: y = u − a2·u², u = input − window centre
        double vlo = -1e9, vhi = 1e9;  // input window (absolute volts): hard limit
        double vshift = 0.0;           // output DC shift (output = bucket − vshift)
        double noise = 0.0;            // white noise per bucket sample (V rms); 0 = off
        std::uint32_t seed = 1;
    };
    static constexpr int kHalf = 8;    // BLEP half-width (samples)
    static constexpr int kPhases = 64; // BLEP table resolution per sample

    using Law = std::vector<std::pair<double, double>>;
    using ControlRows = std::vector<std::pair<double, Law>>;

    void prepare(double fs, const Params& p, Law law, double fixedHz, ControlRows rows = {}) {
        p_ = p;
        fs_ = fs;
        T_ = 1.0 / fs;
        law_ = std::move(law);
        rows_ = std::move(rows);
        fixed_ = fixedHz;
        half_ = std::max(1, p.stages / 2);
        std::int64_t cap = 64;
        while (cap < static_cast<std::int64_t>(half_ + 64)) cap <<= 1;
        ringMask_ = cap - 1;
        ring_.assign(static_cast<std::size_t>(cap), 0.0);
        // Poisson FIR taps for a = N·ε
        const double a = std::max(p.eps, 0.0);
        double t = std::exp(-a);
        for (int k = 0; k < 6; ++k) {
            fir_[k] = t;
            t *= a / (k + 1);
        }
        // band-limited step table: BL(τ) = ∫_{−∞}^{τ} w(x)·2fc·sinc(2fc·x) dx over |τ| ≤ kHalf, normalised to 1
        const int n = 2 * kHalf * kPhases + 1;
        blep_.assign(static_cast<std::size_t>(n + 1), 0.0);
        const double fc = 0.45;
        double acc = 0;
        std::vector<double> k(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            const double x = (i - kHalf * kPhases) / static_cast<double>(kPhases);
            const double sx = x == 0 ? 1.0 : std::sin(2.0 * M_PI * fc * x) / (2.0 * M_PI * fc * x);
            const double w = 0.42 + 0.5 * std::cos(M_PI * x / kHalf) + 0.08 * std::cos(2.0 * M_PI * x / kHalf); // Blackman
            k[static_cast<std::size_t>(i)] = 2.0 * fc * sx * w;
        }
        for (int i = 0; i < n; ++i) { // trapezoidal running integral: blep_[i] is the step at exactly τ_i
            acc += k[static_cast<std::size_t>(i)];
            blep_[static_cast<std::size_t>(i)] = acc - 0.5 * k[static_cast<std::size_t>(i)];
        }
        for (auto& b : blep_) b /= acc;
        blep_[static_cast<std::size_t>(n)] = 1.0;
        acc_.assign(kRing, 0.0);
        lev_.assign(kRing, 0.0);
    }
    // Multirate: the line runs at the host rate (re-derives only the sample period; the kernel is in samples).
    void rateChanged(double fs) noexcept {
        fs_ = fs;
        T_ = 1.0 / fs;
    }
    static double evalLaw(const Law& law, double vctl, double fixed) noexcept {
        if (law.empty()) return std::max(fixed, 1.0);
        if (vctl <= law.front().first) return std::max(law.front().second, 1.0);
        if (vctl >= law.back().first) return std::max(law.back().second, 1.0);
        auto it = std::upper_bound(law.begin(), law.end(), std::make_pair(vctl, -1e300));
        const auto& b = *it;
        const auto& a = *(it - 1);
        return std::max(a.second + (b.second - a.second) * (vctl - a.first) / (b.first - a.first), 1.0);
    }
    double clockHz(double vctl, double ctl = 0.5) const noexcept {
        if (rows_.empty()) return evalLaw(law_, vctl, fixed_);
        if (ctl <= rows_.front().first) return evalLaw(rows_.front().second, vctl, fixed_);
        if (ctl >= rows_.back().first) return evalLaw(rows_.back().second, vctl, fixed_);
        auto it = std::upper_bound(rows_.begin(), rows_.end(), ctl, [](double x, const auto& r) { return x < r.first; });
        const auto& b = *it;
        const auto& a = *(it - 1);
        const double f0 = evalLaw(a.second, vctl, fixed_), f1 = evalLaw(b.second, vctl, fixed_);
        const double t = b.first > a.first ? (ctl - a.first) / (b.first - a.first) : 0.0;
        return std::max(f0 + (f1 - f0) * t, 1.0);
    }
    // DC transfer (operating point): output for a constant input.
    double dcOut(double vin) const noexcept { return shape(vin) - p_.vshift; }
    void reset(double vinDc, double vctl, double ctl = 0.5) {
        const double s = shape(vinDc);
        std::fill(ring_.begin(), ring_.end(), s);
        for (double& h : hist_) h = vinDc;
        phi_ = 0.0;
        wr_ = 0;
        fPrev_ = clockHz(vctl, ctl);
        level_ = s - p_.vshift;
        lastY_ = level_;
        std::fill(acc_.begin(), acc_.end(), 0.0);
        std::fill(lev_.begin(), lev_.end(), 0.0);
        now_ = 0;
        nextOut_ = -static_cast<std::int64_t>(half_);
        rng_ = p_.seed ? p_.seed : 1u;
    }
    // One engine sample: the input node and the control node at sample k; returns the output for sample k + 1.
    double process(double vin, double vctl, double ctl = 0.5) noexcept {
        for (int q = 0; q < 5; ++q) hist_[q] = hist_[q + 1];
        hist_[5] = vin;
        const double f = clockHz(vctl, ctl);
        // input timeline: the interval (k−3, k−2] at the previous sample's clock (centred 6-point interpolation)
        const double dphi = fPrev_ * T_;
        const double phi0 = phi_;
        phi_ += dphi;
        for (double c = std::floor(phi0) + 1.0; c <= phi_; c += 1.0) {
            const double mu = (c - phi0) / dphi; // 0..1 in (k−3, k−2]
            write(shape(interp(mu)));
        }
        fPrev_ = f;
        // output events through time (k + 1) + kHalf, on the input timeline (now at k − 2): 3 + kHalf samples ahead
        const double rate = f * T_; // clock periods per sample
        const double horizon = phi_ + (3.0 + kHalf) * rate;
        for (;;) {
            const double phiEv = static_cast<double>(nextOut_ + half_) + 0.5;
            if (phiEv > horizon) break;
            if (nextOut_ > wr_) break; // not written yet (only when the delay is shorter than the lookahead)
            const double tEv = static_cast<double>(now_) - 2.0 + (phiEv - phi_) / rate; // in samples
            const double y = emit(nextOut_);
            step(tEv, y - lastY_);
            lastY_ = y;
            ++nextOut_;
        }
        // advance the output to sample k + 1
        ++now_;
        const std::size_t i = static_cast<std::size_t>(now_ & (kRing - 1));
        level_ += lev_[i];
        lev_[i] = 0.0;
        const double out = level_ + acc_[i];
        acc_[i] = 0.0;
        return out;
    }

private:
    static constexpr std::int64_t kRing = 64; // ≥ 2·kHalf + lookahead
    double shape(double v) const noexcept {
        const double c = 0.5 * (p_.vlo + p_.vhi), u = std::clamp(v, p_.vlo, p_.vhi) - c; // window centre (0 when unbounded)
        return c + u - p_.a2 * u * u;
    }
    double interp(double mu) const noexcept { // 6-point Lagrange through hist[0..5] at x = 2 + mu (0 divisions)
        const double d0 = mu + 2.0, d1 = mu + 1.0, d2 = mu, d3 = mu - 1.0, d4 = mu - 2.0, d5 = mu - 3.0;
        const double p1 = d0, p2 = p1 * d1, p3 = p2 * d2, p4 = p3 * d3, p5 = p4 * d4;
        const double s4 = d5, s3 = s4 * d4, s2 = s3 * d3, s1 = s2 * d2, s0 = s1 * d1;
        return hist_[0] * (s0 * (-1.0 / 120.0)) + hist_[1] * (p1 * s1 * (1.0 / 24.0)) + hist_[2] * (p2 * s2 * (-1.0 / 12.0)) +
               hist_[3] * (p3 * s3 * (1.0 / 12.0)) + hist_[4] * (p4 * s4 * (-1.0 / 24.0)) + hist_[5] * (p5 * (1.0 / 120.0));
    }
    void write(double s) noexcept {
        ++wr_;
        ring_[static_cast<std::size_t>(wr_ & ringMask_)] = s;
    }
    double at(std::int64_t m) const noexcept {
        return ring_[static_cast<std::size_t>(m & ringMask_)];
    }
    double emit(std::int64_t m) noexcept {
        double y = 0;
        for (int k = 0; k < 6; ++k) y += fir_[k] * at(m - k);
        if (p_.noise > 0) {
            rng_ ^= rng_ << 13;
            rng_ ^= rng_ >> 17;
            rng_ ^= rng_ << 5;
            const double u1 = (rng_ & 0xffffff) / 16777216.0 - 0.5;
            y += p_.noise * 3.4641016 * u1; // uniform with the given rms
        }
        return y - p_.vshift;
    }
    void step(double tEv, double d) noexcept {
        if (d == 0.0) return;
        // samples n in (tEv − kHalf, tEv + kHalf] get the band-limited step; the first sample after the window gets
        // the remainder in the level ring
        const auto n0 = static_cast<std::int64_t>(std::floor(tEv - kHalf)) + 1;
        const auto n1 = static_cast<std::int64_t>(std::floor(tEv + kHalf));
        const double* __restrict bl = blep_.data();
        const double maxIdx = static_cast<double>(blep_.size() - 2);
        for (std::int64_t n = std::max(n0, now_ + 1); n <= n1; ++n) {
            const double x = std::clamp((static_cast<double>(n) - tEv + kHalf) * kPhases, 0.0, maxIdx);
            const auto xi = static_cast<std::size_t>(x);
            const double fr = x - static_cast<double>(xi);
            acc_[static_cast<std::size_t>(n & (kRing - 1))] += d * (bl[xi] + fr * (bl[xi + 1] - bl[xi]));
        }
        const std::int64_t nl = std::max(n1 + 1, now_ + 1);
        lev_[static_cast<std::size_t>(nl & (kRing - 1))] += d;
    }
    Params p_;
    double fs_ = 48000, T_ = 1.0 / 48000, fixed_ = 0;
    Law law_;
    ControlRows rows_;
    int half_ = 512;
    std::int64_t ringMask_ = 1023;
    std::vector<double> ring_, blep_, acc_, lev_;
    double fir_[6] = {1, 0, 0, 0, 0, 0};
    double hist_[6] = {};
    double phi_ = 0, fPrev_ = 1, level_ = 0, lastY_ = 0;
    std::int64_t wr_ = 0, now_ = 0, nextOut_ = 0;
    std::uint32_t rng_ = 1;
};

} // namespace rig::ce
