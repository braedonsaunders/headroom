// Unit fidelity audit harness (docs/UNIT_AUDIT.md).
//
// For every catalog unit: instantiate it through the real factory (createComponentByTableId), record
// which C++ class processes it, then sweep each parameter to min / mid / max (everything else at its
// schema default, power on) and render a fixed battery of test signals through the single unit:
//
//   di     guitar DI clip (app/samples/di, resampled to 48 kHz)
//   pink   pink noise, -20 dBFS RMS
//   sweep  exponential sine sweep 30 Hz .. 16 kHz
//   sine   375 Hz steady sine, -20 dBFS (period = 128 samples: exact AM / FM demodulation)
//   imp    single impulse + 2.5 s of silence (delay / predelay / decay via the impulse response)
//   dyn    1 kHz level staircase -40,-10,-40,-30,-20,-10,-3 dBFS (static curve, attack, release)
//
// It writes raw features + pairwise null differences as JSON. Classification (live / dead / wrong /
// suspicious) and grading happen in tools/unit_audit/report.py so the rules can be iterated without
// re-rendering.
//
// Usage: unit_audit --di <wav> --out <json> [--units 1000,2000] [--shard i/n] [--quick]
#include "../../src/audio/WavIO.hpp"
#include "../../src/core/AudioBlock.hpp"
#include "../../src/core/IComponent.hpp"
#include "../../src/generated/ComponentCatalog.hpp"
#if __has_include(<cxxabi.h>)
#include <cxxabi.h>
#define RIG_HAVE_CXXABI 1
#endif
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <fstream>
#include <memory>
#include <numbers>
#include <random>
#include <sstream>
#include <string>
#include <typeinfo>
#include <vector>

namespace {

constexpr double kSr = 48000.0;
constexpr uint32_t kBlock = 256;
constexpr double kPi = std::numbers::pi;

enum Sig { S_DI, S_PINK, S_SWEEP, S_SINE, S_IMP, S_DYN, N_SIG };
const char* kSigName[N_SIG] = {"di", "pink", "sweep", "sine", "imp", "dyn"};
constexpr double kSineHz = 375.0;
constexpr double kDynLevels[7] = {-40, -10, -40, -30, -20, -10, -3};
constexpr double kDynStep = 0.5;

struct Stereo {
    std::vector<float> l, r;
};

std::vector<float> g_sig[N_SIG];
bool g_quick = false;

// ---------------------------------------------------------------- test signals
void buildSignals(const std::string& di_path) {
    rig::audio::WavData w;
    std::string err;
    std::vector<float> di;
    if (rig::audio::readWavFile(di_path, w, &err) && !w.left.empty()) {
        // linear resample to 48 kHz, take 3 s from 1 s in
        const double ratio = static_cast<double>(w.sample_rate) / kSr;
        const std::size_t n = static_cast<std::size_t>(3.0 * kSr);
        const double start = 1.0 * w.sample_rate;
        di.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            const double pos = start + i * ratio;
            const std::size_t i0 = std::min<std::size_t>(static_cast<std::size_t>(pos), w.left.size() - 2);
            const double fr = pos - static_cast<double>(i0);
            const float a = w.right.empty() ? w.left[i0] : 0.5f * (w.left[i0] + w.right[i0]);
            const float b = w.right.empty() ? w.left[i0 + 1] : 0.5f * (w.left[i0 + 1] + w.right[i0 + 1]);
            di[i] = static_cast<float>(a + fr * (b - a));
        }
    } else {
        std::fprintf(stderr, "warning: DI clip unavailable (%s); using a plucked-string stand-in\n", err.c_str());
        di.assign(static_cast<std::size_t>(3.0 * kSr), 0.0f);
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> u(-1, 1);
        for (int note = 0; note < 6; ++note) {
            const std::size_t p = static_cast<std::size_t>(kSr * 0.06 * (80 + note * 37) / 80.0);
            std::vector<float> line(p);
            for (auto& v : line) v = 0.3f * u(rng);
            const std::size_t s0 = static_cast<std::size_t>(note * 0.5 * kSr);
            for (std::size_t i = 0; i < static_cast<std::size_t>(0.5 * kSr) && s0 + i < di.size(); ++i) {
                const float y = 0.5f * (line[i % p] + line[(i + 1) % p]) * 0.996f;
                line[i % p] = y;
                di[s0 + i] += y;
            }
        }
    }
    g_sig[S_DI] = di;

    std::mt19937 rng(1234);
    std::normal_distribution<float> g(0.0f, 1.0f);
    const std::size_t npink = static_cast<std::size_t>(2.0 * kSr);
    std::vector<float> pink(npink);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    for (auto& v : pink) {
        const double wn = g(rng);
        b0 = 0.99886 * b0 + wn * 0.0555179; b1 = 0.99332 * b1 + wn * 0.0750759;
        b2 = 0.96900 * b2 + wn * 0.1538520; b3 = 0.86650 * b3 + wn * 0.3104856;
        b4 = 0.55000 * b4 + wn * 0.5329522; b5 = -0.7616 * b5 - wn * 0.0168980;
        v = static_cast<float>(b0 + b1 + b2 + b3 + b4 + b5 + b6 + wn * 0.5362);
        b6 = wn * 0.115926;
    }
    double e = 0;
    for (float v : pink) e += v * v;
    const float k = static_cast<float>(0.1 / std::sqrt(e / npink));
    for (auto& v : pink) v *= k;
    g_sig[S_PINK] = pink;

    const std::size_t nsw = static_cast<std::size_t>(1.5 * kSr);
    std::vector<float> sw(nsw);
    const double f0 = 30.0, f1 = 16000.0, T = 1.5, L = T / std::log(f1 / f0);
    for (std::size_t i = 0; i < nsw; ++i) {
        const double t = i / kSr;
        sw[i] = static_cast<float>(0.125 * std::sin(2 * kPi * f0 * L * (std::exp(t / L) - 1.0)));
    }
    g_sig[S_SWEEP] = sw;

    const std::size_t nsine = static_cast<std::size_t>(2.5 * kSr);
    std::vector<float> s(nsine);
    for (std::size_t i = 0; i < nsine; ++i) s[i] = static_cast<float>(0.1 * std::sin(2 * kPi * kSineHz * i / kSr));
    g_sig[S_SINE] = s;

    std::vector<float> imp(static_cast<std::size_t>(2.5 * kSr), 0.0f);
    imp[0] = 0.25f;
    g_sig[S_IMP] = imp;

    const std::size_t nstep = static_cast<std::size_t>(kDynStep * kSr);
    std::vector<float> dyn(nstep * 7);
    for (int st = 0; st < 7; ++st) {
        const double a = std::pow(10.0, kDynLevels[st] / 20.0) * std::sqrt(2.0);
        for (std::size_t i = 0; i < nstep; ++i) {
            const std::size_t n = st * nstep + i;
            dyn[n] = static_cast<float>(a * std::sin(2 * kPi * 1000.0 * n / kSr));
        }
    }
    g_sig[S_DYN] = dyn;
}

// ---------------------------------------------------------------- rendering
Stereo render(rig::IComponent& c, const std::vector<float>& x) {
    const std::size_t pre = static_cast<std::size_t>(0.25 * kSr) / kBlock * kBlock;
    const std::size_t n = x.size();
    const std::size_t total = pre + (n + kBlock - 1) / kBlock * kBlock;
    std::vector<float> in(total, 0.0f), ol(total, 0.0f), orr(total, 0.0f);
    std::copy(x.begin(), x.end(), in.begin() + static_cast<std::ptrdiff_t>(pre));
    c.reset();
    for (std::size_t p = 0; p < total; p += kBlock) {
        c.processControlRate();
        const float* ins[2] = {in.data() + p, in.data() + p};
        float* outs[2] = {ol.data() + p, orr.data() + p};
        rig::AudioBlock b{};
        b.in = ins;
        b.out = outs;
        b.num_channels = 2;
        b.num_frames = kBlock;
        b.sample_rate = kSr;
        b.bpm = 120.0;
        c.processAudio(b);
    }
    Stereo s;
    s.l.assign(ol.begin() + static_cast<std::ptrdiff_t>(pre), ol.begin() + static_cast<std::ptrdiff_t>(pre + n));
    s.r.assign(orr.begin() + static_cast<std::ptrdiff_t>(pre), orr.begin() + static_cast<std::ptrdiff_t>(pre + n));
    for (auto* v : {&s.l, &s.r})
        for (auto& f : *v)
            if (!std::isfinite(f)) f = 0.0f;
    return s;
}

// ---------------------------------------------------------------- analysis
double db(double p) { return 10.0 * std::log10(std::max(p, 1e-20)); }

double meanSq(const std::vector<float>& v, std::size_t a = 0, std::size_t b = SIZE_MAX) {
    b = std::min(b, v.size());
    if (b <= a) return 0;
    double e = 0;
    for (std::size_t i = a; i < b; ++i) e += static_cast<double>(v[i]) * v[i];
    return e / static_cast<double>(b - a);
}

void fft(std::vector<std::complex<double>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * kPi / static_cast<double>(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (std::size_t j = 0; j < len / 2; ++j) {
                const auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// 1/3-octave band centres 31.5 Hz .. 16 kHz
const std::vector<double>& bandCentres() {
    static std::vector<double> c = [] {
        std::vector<double> v;
        for (int k = -15; k <= 12; ++k) v.push_back(1000.0 * std::pow(2.0, k / 3.0));
        return v;
    }();
    return c;
}

std::vector<double> bandLevels(const std::vector<float>& x) {
    constexpr std::size_t N = 4096;
    std::vector<double> psd(N / 2 + 1, 0.0);
    std::vector<std::complex<double>> buf(N);
    int frames = 0;
    for (std::size_t s = 0; s + N <= x.size(); s += N / 2) {
        for (std::size_t i = 0; i < N; ++i) {
            const double w = 0.5 - 0.5 * std::cos(2 * kPi * i / (N - 1));
            buf[i] = x[s + i] * w;
        }
        fft(buf);
        for (std::size_t i = 0; i <= N / 2; ++i) psd[i] += std::norm(buf[i]);
        ++frames;
    }
    std::vector<double> out;
    for (double fc : bandCentres()) {
        const double lo = fc / std::pow(2.0, 1.0 / 6.0), hi = fc * std::pow(2.0, 1.0 / 6.0);
        double e = 0;
        for (std::size_t i = 1; i <= N / 2; ++i) {
            const double f = i * kSr / N;
            if (f >= lo && f < hi) e += psd[i];
        }
        out.push_back(db(e / std::max(1, frames)));
    }
    return out;
}

// dominant frequency of a (mean-removed) control-rate series in [fmin, fmax]; returns {freq, amplitude}
std::pair<double, double> dominant(const std::vector<double>& v, double rate, double fmin, double fmax) {
    if (v.size() < 8) return {0, 0};
    double m = 0;
    for (double x : v) m += x;
    m /= static_cast<double>(v.size());
    double best = 0, bf = 0;
    for (double f = fmin; f <= fmax; f *= 1.02) {
        std::complex<double> acc{};
        for (std::size_t i = 0; i < v.size(); ++i) {
            const double w = 0.5 - 0.5 * std::cos(2 * kPi * i / (v.size() - 1));
            acc += (v[i] - m) * w * std::polar(1.0, -2 * kPi * f * i / rate);
        }
        const double a = std::abs(acc) * 4.0 / static_cast<double>(v.size());
        if (a > best) { best = a; bf = f; }
    }
    return {bf, best};
}

double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const std::size_t i = std::min(v.size() - 1, static_cast<std::size_t>(p * (v.size() - 1)));
    return v[i];
}

struct Features {
    double rms_db[N_SIG]{}, side_db[N_SIG]{};
    std::vector<double> bands_pink, bands_di, bands_sweep, bands_imp;
    double am_depth_db = 0, am_rate_hz = 0, fm_cents = 0, fm_rate_hz = 0, sine_thd_db = 0;
    double t60 = 0, onset_ms = 0, tail_db = -200, ir_peak_ms = 0;
    double dyn_gain_db[7]{};
    double attack_ms = 0, release_ms = 0;
};

Features analyse(const Stereo* out) {
    Features f;
    for (int s = 0; s < N_SIG; ++s) {
        f.rms_db[s] = db(meanSq(out[s].l));
        std::vector<float> side(out[s].l.size());
        for (std::size_t i = 0; i < side.size(); ++i) side[i] = 0.5f * (out[s].l[i] - out[s].r[i]);
        f.side_db[s] = db(meanSq(side));
    }
    f.bands_pink = bandLevels(out[S_PINK].l);
    f.bands_di = bandLevels(out[S_DI].l);
    f.bands_sweep = bandLevels(out[S_SWEEP].l);

    // --- sine: AM / FM demodulation over [0.5 s, end), frames of exactly 5 periods (640 samples)
    {
        const auto& y = out[S_SINE].l;
        const std::size_t P = 128, start = static_cast<std::size_t>(0.5 * kSr);
        std::vector<double> env, ph;
        std::complex<double> fund_acc{};
        double tot = 0;
        for (std::size_t s = start; s + 640 <= y.size(); s += 640) {
            env.push_back(std::sqrt(meanSq(y, s, s + 640)));
        }
        for (std::size_t s = start; s + P <= y.size(); s += P) {
            std::complex<double> acc{};
            for (std::size_t i = 0; i < P; ++i)
                acc += static_cast<double>(y[s + i]) * std::polar(1.0, -2 * kPi * kSineHz * (s + i) / kSr);
            ph.push_back(std::arg(acc));
            fund_acc += acc;
        }
        for (std::size_t i = start; i < y.size(); ++i) tot += static_cast<double>(y[i]) * y[i];
        double lo = pct(env, 0.03), hi = pct(env, 0.97);
        f.am_depth_db = (lo > 1e-7) ? 20.0 * std::log10(hi / lo) : (hi > 1e-6 ? 60.0 : 0.0);
        const double env_rate = kSr / 640.0;
        std::vector<double> envdb;
        for (double e : env) envdb.push_back(20.0 * std::log10(std::max(e, 1e-7)));
        f.am_rate_hz = f.am_depth_db > 0.5 ? dominant(envdb, env_rate, 0.15, 30.0).first : 0.0;
        // FM: unwrap phase, derivative → instantaneous frequency deviation
        for (std::size_t i = 1; i < ph.size(); ++i) {
            double d = ph[i] - ph[i - 1];
            while (d > kPi) d -= 2 * kPi;
            while (d < -kPi) d += 2 * kPi;
            ph[i] = ph[i - 1] + d;
        }
        std::vector<double> dev;
        const double fr = kSr / P;
        for (std::size_t i = 2; i < ph.size(); ++i) dev.push_back((ph[i] - ph[i - 2]) * fr / (4 * kPi)); // Hz
        // only trust FM where the fundamental dominates
        const double fund_pow = std::norm(fund_acc) * 2.0 / std::pow(static_cast<double>(ph.size() * P), 2);
        const double mean_pow = tot / std::max<std::size_t>(1, y.size() - start);
        f.sine_thd_db = db(std::max(1e-20, mean_pow - fund_pow)) - db(std::max(1e-20, fund_pow));
        if (!dev.empty() && fund_pow > 1e-10) {
            const double m = pct(dev, 0.5);
            for (auto& d : dev) d -= m;
            const double half = 0.5 * (pct(dev, 0.97) - pct(dev, 0.03));
            f.fm_cents = 1200.0 * std::log2((kSineHz + half) / kSineHz);
            f.fm_rate_hz = f.fm_cents > 1.0 ? dominant(dev, fr / 1.0, 0.15, 30.0).first : 0.0;
        }
    }

    // --- impulse response
    {
        const auto& y = out[S_IMP].l;
        const std::size_t F = 48; // 1 ms frames
        std::vector<double> fe;
        for (std::size_t s = 0; s + F <= y.size(); s += F) fe.push_back(meanSq(y, s, s + F));
        std::size_t pk = 0;
        for (std::size_t i = 0; i < fe.size(); ++i) if (fe[i] > fe[pk]) pk = i;
        f.ir_peak_ms = static_cast<double>(pk);
        // wet part: drop the first 3 ms (direct sound / short filter ringing)
        double wmax = 0;
        for (std::size_t i = 3; i < fe.size(); ++i) wmax = std::max(wmax, fe[i]);
        double total = 0, late = 0;
        for (std::size_t i = 0; i < fe.size(); ++i) { total += fe[i]; if (i >= 50) late += fe[i]; }
        f.tail_db = total > 0 ? db(late / total) : -200;
        f.onset_ms = -1;
        if (wmax > 1e-14) {
            for (std::size_t i = 3; i < fe.size(); ++i)
                if (fe[i] >= 0.1 * wmax) { f.onset_ms = static_cast<double>(i); break; }
            // Schroeder EDC of the wet part; T60 from the -5 → -25 dB slope
            std::vector<double> edc(fe.size(), 0.0);
            double acc = 0;
            for (std::size_t i = fe.size(); i-- > 3;) { acc += fe[i]; edc[i] = acc; }
            const double e0 = edc[3];
            double t5 = -1, t25 = -1;
            for (std::size_t i = 3; i < fe.size(); ++i) {
                const double l = db(edc[i] / e0);
                if (t5 < 0 && l <= -5) t5 = static_cast<double>(i);
                if (t25 < 0 && l <= -25) { t25 = static_cast<double>(i); break; }
            }
            f.t60 = (t5 >= 0 && t25 > t5) ? 3.0 * (t25 - t5) / 1000.0 : (t5 >= 0 ? 3.0 * (fe.size() - t5) / 1000.0 : 0.0);
        }
        f.bands_imp = bandLevels(y);
    }

    // --- dynamics staircase
    {
        const auto& y = out[S_DYN].l;
        const std::size_t nstep = static_cast<std::size_t>(kDynStep * kSr);
        for (int st = 0; st < 7; ++st) {
            const std::size_t a = st * nstep + nstep - static_cast<std::size_t>(0.15 * kSr), b = (st + 1) * nstep;
            f.dyn_gain_db[st] = db(meanSq(y, a, b)) - (kDynLevels[st]);
        }
        auto tc = [&](int step, double g_from, double g_to) {
            // gain trajectory in 1 ms frames (48 samples = 48 periods of 1 kHz... exactly one period each)
            if (std::abs(g_to - g_from) < 0.5) return 0.0;
            const std::size_t s0 = step * nstep;
            const double lvl = kDynLevels[step];
            for (std::size_t k = 0; k < nstep / 48; ++k) {
                const double g = db(meanSq(y, s0 + k * 48, s0 + (k + 1) * 48)) - lvl;
                const double frac = (g - g_from) / (g_to - g_from);
                if (frac >= 0.632) return static_cast<double>(k);
            }
            return kDynStep * 1000.0;
        };
        f.attack_ms = tc(1, f.dyn_gain_db[0], f.dyn_gain_db[1]);
        f.release_ms = tc(2, f.dyn_gain_db[1], f.dyn_gain_db[2]);
    }
    return f;
}

double nullDb(const Stereo& a, const Stereo& b) {
    double e = 0, ea = 0, eb = 0;
    for (std::size_t i = 0; i < a.l.size() && i < b.l.size(); ++i) {
        const double dl = a.l[i] - b.l[i], dr = a.r[i] - b.r[i];
        e += dl * dl + dr * dr;
        ea += static_cast<double>(a.l[i]) * a.l[i] + static_cast<double>(a.r[i]) * a.r[i];
        eb += static_cast<double>(b.l[i]) * b.l[i] + static_cast<double>(b.r[i]) * b.r[i];
    }
    const double ref = std::max(ea, eb);
    if (ref < 1e-18) return e < 1e-18 ? -200.0 : 0.0;
    return db(e / ref);
}

// ---------------------------------------------------------------- JSON helpers
std::string jstr(std::string_view s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
        else o += c;
    }
    return o + "\"";
}
std::string jnum(double v) {
    if (!std::isfinite(v)) return "null";
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}
std::string jarr(const std::vector<double>& v) {
    std::string o = "[";
    for (std::size_t i = 0; i < v.size(); ++i) o += (i ? "," : "") + jnum(v[i]);
    return o + "]";
}
std::string jfeat(const Features& f) {
    std::ostringstream o;
    o << "{\"rms\":{";
    for (int s = 0; s < N_SIG; ++s) o << (s ? "," : "") << "\"" << kSigName[s] << "\":" << jnum(f.rms_db[s]);
    o << "},\"side\":{";
    for (int s = 0; s < N_SIG; ++s) o << (s ? "," : "") << "\"" << kSigName[s] << "\":" << jnum(f.side_db[s]);
    o << "},\"bands_pink\":" << jarr(f.bands_pink) << ",\"bands_di\":" << jarr(f.bands_di)
      << ",\"bands_sweep\":" << jarr(f.bands_sweep) << ",\"bands_imp\":" << jarr(f.bands_imp)
      << ",\"am_depth_db\":" << jnum(f.am_depth_db) << ",\"am_rate_hz\":" << jnum(f.am_rate_hz)
      << ",\"fm_cents\":" << jnum(f.fm_cents) << ",\"fm_rate_hz\":" << jnum(f.fm_rate_hz)
      << ",\"sine_thd_db\":" << jnum(f.sine_thd_db)
      << ",\"t60\":" << jnum(f.t60) << ",\"onset_ms\":" << jnum(f.onset_ms) << ",\"tail_db\":" << jnum(f.tail_db)
      << ",\"ir_peak_ms\":" << jnum(f.ir_peak_ms)
      << ",\"dyn_gain_db\":" << jarr(std::vector<double>(f.dyn_gain_db, f.dyn_gain_db + 7))
      << ",\"attack_ms\":" << jnum(f.attack_ms) << ",\"release_ms\":" << jnum(f.release_ms) << "}";
    return o.str();
}

std::string demangle(const char* n) {
#ifdef RIG_HAVE_CXXABI
    int st = 0;
    char* d = abi::__cxa_demangle(n, nullptr, nullptr, &st);
    std::string s = (st == 0 && d) ? d : n;
    std::free(d);
    return s;
#else
    return n; // MSVC: typeid().name() is already human-readable
#endif
}

struct Config {
    std::string label;
    std::vector<std::pair<std::string, float>> set;
};

struct Rendered {
    Stereo out[N_SIG];
    Features f;
};

std::unique_ptr<rig::IComponent> makeUnit(int32_t t, const Config& cfg) {
    auto c = rig::createComponentByTableId(t);
    if (!c) return nullptr;
    c->prepare(kSr, kBlock);
    for (const auto& p : c->schema().params)
        if (p.kind != rig::catalog::ParamKind::String) c->setParameter(p.id, p.default_value);
    c->setParameter("Pwr", 1.0f);
    for (const auto& p : c->schema().params) {
        if (p.id == "on_off") c->setParameter("on_off", 1.0f);
        if (p.id == "bypass") c->setParameter("bypass", 0.0f);
    }
    for (const auto& [k, v] : cfg.set) c->setParameter(k, v);
    c->processControlRate();
    return c;
}

Rendered run(int32_t t, const Config& cfg) {
    Rendered r;
    auto c = makeUnit(t, cfg);
    for (int s = 0; s < N_SIG; ++s) r.out[s] = c ? render(*c, g_sig[s]) : Stereo{g_sig[s], g_sig[s]};
    r.f = analyse(r.out);
    return r;
}

std::string nullsJson(const Rendered& a, const Rendered& b) {
    std::ostringstream o;
    o << "{";
    for (int s = 0; s < N_SIG; ++s) o << (s ? "," : "") << "\"" << kSigName[s] << "\":" << jnum(nullDb(a.out[s], b.out[s]));
    o << "}";
    return o.str();
}

bool isMeterOrUi(std::string_view id, std::string_view name) {
    auto has = [&](std::string_view k) { return name.find(k) != std::string_view::npos; };
    return id == "Pwr" || id == "on_off" || id == "bypass" || id == "CASSt" || has("Lamp") || has("Learn") ||
           has("VU") || has("Meter") || has("Level Left") || has("Level Right") || has("Peak Left") ||
           has("Peak Right") || has("Display") || has("VST3") || has("Silent Mode");
}

// Switch-like controls (effect on / mode / channel selectors). A control that is dead at the defaults is
// re-measured with these at their maximum ("context"), so e.g. a reverb-time knob behind a reverb-on switch
// is not reported dead.
bool isSwitchLike(std::string_view name) {
    std::string n(name);
    for (auto& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (const char* bad : {"mute", "freeze", "kill", "bypass", "learn", "sidechain", "reverse", "invert", "lamp", "led",
                            "tap", "solo", "ext", "meter", "sync", "stereo", "safety", "dither"})
        if (n.find(bad) != std::string::npos) return false;
    auto word = [&](const char* w) {
        const std::size_t L = std::strlen(w);
        for (std::size_t i = n.find(w); i != std::string::npos; i = n.find(w, i + 1)) {
            const bool a = i == 0 || !std::isalpha(static_cast<unsigned char>(n[i - 1]));
            const bool b = i + L >= n.size() || !std::isalpha(static_cast<unsigned char>(n[i + L]));
            if (a && b) return true;
        }
        return false;
    };
    for (const char* w : {"on", "enable", "active", "switch", "mode", "type", "select", "selector", "effect", "fx",
                          "channel", "style", "algorithm", "always on"})
        if (word(w)) return true;
    return false;
}

// Gain-like controls (EQ band gains, channel gains). Frequency / Q / shape knobs only act once their band is
// boosted, so the context run also sets these to 85 % of their range.
bool isGainLike(std::string_view name) {
    std::string n(name);
    for (auto& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (n.find("range") != std::string::npos) return false;
    return n.find("gain") != std::string::npos || (n.rfind("band", 0) == 0 && n.size() <= 7) ||
           n.find("shelf") != std::string::npos;
}

std::string auditUnit(const rig::catalog::ComponentSchema& s) {
    std::ostringstream o;
    const int32_t t = s.table_id;
    auto probe = rig::createComponentByTableId(t);
    const std::string cls = probe ? demangle(typeid(*probe).name()) : "none";
    o << "{\"table_id\":" << t << ",\"reference_name\":" << jstr(s.reference_name)
      << ",\"display_name\":" << jstr(s.display_name) << ",\"schema_dsp_class\":" << jstr(s.dsp_class)
      << ",\"family\":" << static_cast<int>(s.family)
      << ",\"impl_kind\":" << jstr(rig::implementationKind(t)) << ",\"cpp_class\":" << jstr(cls);
    if (!probe) { o << "}"; return o.str(); }
    probe.reset();

    std::fprintf(stderr, "[%d] %s (%s)\n", t, std::string(s.reference_name).c_str(), cls.c_str());
    // dry reference: the raw input signals
    Rendered dry;
    for (int sg = 0; sg < N_SIG; ++sg) dry.out[sg] = Stereo{g_sig[sg], g_sig[sg]};
    dry.f = analyse(dry.out);
    const Rendered def = run(t, {"default", {}});
    const Rendered def2 = run(t, {"default", {}});
    o << ",\"dry\":" << jfeat(dry.f) << ",\"default\":" << jfeat(def.f)
      << ",\"null_default_vs_dry\":" << nullsJson(def, dry) << ",\"null_repeat\":" << nullsJson(def, def2);

    // params
    o << ",\"params\":[";
    bool first = true;
    for (const auto& p : s.params) {
        if (p.kind == rig::catalog::ParamKind::String) continue;
        if (p.id == "Pwr" || p.id == "on_off" || p.id == "bypass") continue;
        float mn = 0, mx = 1;
        rig::getParamRangeFor(t, p, mn, mx);
        const bool discrete = (mx - mn > 1.5f && mx - mn <= 64.0f && std::floor(mn) == mn && std::floor(mx) == mx) ||
                              p.kind == rig::catalog::ParamKind::Int;
        float md = 0.5f * (mn + mx);
        if (discrete) md = std::round(md);
        const bool ui = isMeterOrUi(p.id, p.display_name);
        o << (first ? "" : ",") << "{\"id\":" << jstr(p.id) << ",\"name\":" << jstr(p.display_name)
          << ",\"default\":" << jnum(p.default_value) << ",\"min\":" << jnum(mn) << ",\"max\":" << jnum(mx)
          << ",\"discrete\":" << (discrete ? "true" : "false") << ",\"ui\":" << (ui ? "true" : "false");
        first = false;
        if (g_quick && ui) { o << "}"; continue; }
        const Rendered rmin = run(t, {"min", {{std::string(p.id), mn}}});
        const Rendered rmax = run(t, {"max", {{std::string(p.id), mx}}});
        const Rendered rmid = run(t, {"mid", {{std::string(p.id), md}}});
        o << ",\"f_min\":" << jfeat(rmin.f) << ",\"f_mid\":" << jfeat(rmid.f) << ",\"f_max\":" << jfeat(rmax.f)
          << ",\"null_min_max\":" << nullsJson(rmin, rmax) << ",\"null_min_mid\":" << nullsJson(rmin, rmid)
          << ",\"null_mid_max\":" << nullsJson(rmid, rmax);
        // dead at defaults? retry with the unit's switches / mode selectors at max
        double change = -200;
        for (int sg = 0; sg < N_SIG; ++sg) change = std::max({change, nullDb(rmin.out[sg], rmax.out[sg]), nullDb(rmin.out[sg], rmid.out[sg])});
        if (change < -50.0 && !ui && !g_quick) {
            Config base{"ctx", {}};
            std::string names;
            for (const auto& q : s.params) {
                if (q.id == p.id || q.kind == rig::catalog::ParamKind::String || isMeterOrUi(q.id, q.display_name)) continue;
                const bool sw = isSwitchLike(q.display_name), gn = isGainLike(q.display_name) && !isGainLike(p.display_name);
                if (!sw && !gn) continue;
                float qn = 0, qx = 1;
                rig::getParamRangeFor(t, q, qn, qx);
                base.set.push_back({std::string(q.id), sw ? qx : qn + 0.85f * (qx - qn)});
                names += (names.empty() ? "" : ",") + std::string(q.id);
            }
            if (!base.set.empty()) {
                Config cmin = base, cmax = base;
                cmin.set.push_back({std::string(p.id), mn});
                cmax.set.push_back({std::string(p.id), mx});
                const Rendered xmin = run(t, cmin), xmax = run(t, cmax);
                o << ",\"ctx\":{\"set\":" << jstr(names) << ",\"f_min\":" << jfeat(xmin.f) << ",\"f_max\":" << jfeat(xmax.f)
                  << ",\"null_min_max\":" << nullsJson(xmin, xmax) << "}";
            }
        }
        o << "}";
    }
    o << "]}";
    return o.str();
}

} // namespace

int main(int argc, char** argv) {
    std::string di = "samples/di/Rock DI.wav", out = "unit_audit.json";
    std::vector<int32_t> only;
    int shard = 0, nshards = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--di") di = next();
        else if (a == "--out") out = next();
        else if (a == "--quick") g_quick = true;
        else if (a == "--units") {
            std::stringstream ss(next());
            std::string tok;
            while (std::getline(ss, tok, ',')) only.push_back(std::stoi(tok));
        } else if (a == "--shard") {
            const std::string v = next();
            shard = std::stoi(v.substr(0, v.find('/')));
            nshards = std::stoi(v.substr(v.find('/') + 1));
        }
    }
    buildSignals(di);
    std::vector<const rig::catalog::ComponentSchema*> units;
    for (const auto& s : rig::catalog::kAllComponents) units.push_back(&s);
    for (const auto& s : rig::catalog::studioComponents()) units.push_back(&s);
    std::ofstream f(out);
    f << "{\"sample_rate\":48000,\"di\":" << jstr(di) << ",\"bands_hz\":" << jarr(bandCentres())
      << ",\"dyn_levels\":" << jarr(std::vector<double>(kDynLevels, kDynLevels + 7)) << ",\"units\":[";
    bool first = true;
    int idx = 0;
    for (const auto* s : units) {
        if (!only.empty() && std::find(only.begin(), only.end(), s->table_id) == only.end()) continue;
        if ((idx++ % nshards) != shard) continue;
        f << (first ? "" : ",\n") << auditUnit(*s);
        f.flush();
        first = false;
    }
    f << "]}\n";
    return 0;
}
