// circuit_ab: A/B renders of circuit-engine units against the current (legacy) models, plus an ngspice render of
// the same DI through the same netlist (docs/CIRCUIT_ENGINE.md §8).
//   circuit_ab <out_dir> <di.wav> [seconds] [--spice-seconds s]
// Writes level-matched WAVs and report.md into out_dir.
#include "WavIO.hpp"
#include <libproc.h>
#include <unistd.h>
#include "circuit/CircuitUnit.hpp"
#include "circuit/Engine.hpp"
#include "dsp/ComponentModel.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace rig;
namespace fs = std::filesystem;

namespace {
std::vector<double> renderUnit(IComponent& c, const std::vector<double>& in, double sr) {
    // CE_BLOCK: the host block size (default 256; the user plays at 64)
    const std::size_t blk = std::getenv("CE_BLOCK") ? static_cast<std::size_t>(std::max(1, std::atoi(std::getenv("CE_BLOCK")))) : 256;
    c.prepare(sr, static_cast<int>(blk));
    std::vector<float> inF(in.begin(), in.end()), outL(in.size()), outR(in.size());
    // CE_BLOCKLOG=<file>: one line per block: start time (s), thread CPU time (us), and the solver counters' deltas
    // (samples, Newton iterations, LTE substeps, homotopies, half steps), to locate per-block bursts
    std::FILE* blog = std::getenv("CE_BLOCKLOG") ? std::fopen(std::getenv("CE_BLOCKLOG"), "w") : nullptr;
    UnitSolverStats st0;
    if (blog) {
        c.solverStats(st0);
        std::fprintf(blog, "t_s cpu_us kinstr samples newton lte_substeps homotopies half_steps\n");
    }
    for (std::size_t o = 0; o < in.size(); o += blk) {
        const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(blk, in.size() - o));
        timespec ts0{};
        rusage_info_v4 ru0{};
        if (blog) {
            proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&ru0));
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts0);
        }
        const float* ins[2] = {inF.data() + o, inF.data() + o};
        float* outs[2] = {outL.data() + o, outR.data() + o};
        AudioBlock b;
        b.in = ins;
        b.out = outs;
        b.num_channels = 2;
        b.num_frames = n;
        b.sample_rate = sr;
        c.processControlRate();
        c.processAudio(b);
        if (blog) {
            timespec ts1{};
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts1);
            rusage_info_v4 ru1{};
            proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&ru1));
            UnitSolverStats st;
            c.solverStats(st);
            std::fprintf(blog, "%.5f %.1f %.1f %llu %llu %llu %llu %llu\n", static_cast<double>(o) / sr,
                         1e6 * (static_cast<double>(ts1.tv_sec - ts0.tv_sec) + 1e-9 * static_cast<double>(ts1.tv_nsec - ts0.tv_nsec)),
                         1e-3 * static_cast<double>(ru1.ri_instructions - ru0.ri_instructions),
                         static_cast<unsigned long long>(st.samples - st0.samples), static_cast<unsigned long long>(st.newton_iterations - st0.newton_iterations),
                         static_cast<unsigned long long>(st.lte_substeps - st0.lte_substeps), static_cast<unsigned long long>(st.homotopies - st0.homotopies),
                         static_cast<unsigned long long>(st.half_steps - st0.half_steps));
            st0 = st;
        }
    }
    if (blog) std::fclose(blog);
    return {outL.begin(), outL.end()};
}
double rmsDb(const std::vector<double>& x) {
    double s = 0;
    for (double v : x) s += v * v;
    return 10 * std::log10(s / static_cast<double>(x.size()) + 1e-30);
}
double peakDb(const std::vector<double>& x) {
    double m = 0;
    for (double v : x) m = std::max(m, std::abs(v));
    return 20 * std::log10(m + 1e-30);
}
// Octave-band energy via a long-window DFT (Goertzel-free: direct per-band sum over a 32768-point spectrum).
std::vector<double> octaveBands(const std::vector<double>& x, double sr, const std::vector<double>& centres) {
    const std::size_t N = 32768;
    std::vector<double> e(centres.size(), 0.0);
    std::vector<std::complex<double>> buf(N);
    const std::size_t hops = std::max<std::size_t>(1, x.size() / N);
    for (std::size_t h = 0; h < hops; ++h) {
        // radix-2 FFT
        for (std::size_t i = 0; i < N; ++i) {
            const std::size_t k = h * N + i;
            const double w = 0.5 - 0.5 * std::cos(2 * M_PI * static_cast<double>(i) / N);
            buf[i] = k < x.size() ? x[k] * w : 0.0;
        }
        for (std::size_t i = 1, j = 0; i < N; ++i) {
            std::size_t bit = N >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(buf[i], buf[j]);
        }
        for (std::size_t len = 2; len <= N; len <<= 1) {
            const std::complex<double> wl = std::polar(1.0, -2 * M_PI / static_cast<double>(len));
            for (std::size_t i = 0; i < N; i += len) {
                std::complex<double> w = 1;
                for (std::size_t j = 0; j < len / 2; ++j) {
                    const auto u = buf[i + j], v = buf[i + j + len / 2] * w;
                    buf[i + j] = u + v;
                    buf[i + j + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
        for (std::size_t b = 0; b < centres.size(); ++b) {
            const double lo = centres[b] / std::sqrt(2.0), hi = centres[b] * std::sqrt(2.0);
            for (std::size_t k = 1; k < N / 2; ++k) {
                const double f = static_cast<double>(k) * sr / N;
                if (f >= lo && f < hi) e[b] += std::norm(buf[k]);
            }
        }
    }
    for (auto& v : e) v = 10 * std::log10(v + 1e-30);
    return e;
}
std::string fmt(double v, int d = 1) {
    char b[32];
    std::snprintf(b, sizeof b, "%+.*f", d, v);
    return b;
}
struct Setting {
    std::string name;
    std::map<std::string, float> params;
};
} // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::string(argv[1]) == "--dump") {
        // circuit_ab --dump <table id> <out.json> [param=value ...]: model + curves JSON of the circuit unit (UI fixtures)
        const auto* schema = catalog::findByTableId(std::atoi(argv[2]));
        auto u = schema ? ce::makeCircuitUnitAlways(*schema) : nullptr;
        if (!u) return 1;
        for (int i = 4; i < argc; ++i) {
            std::string kv = argv[i];
            const auto eq = kv.find('=');
            if (eq != std::string::npos) u->setParameter(kv.substr(0, eq), std::stof(kv.substr(eq + 1)));
        }
        u->prepare(48000, 512);
        auto* in = dynamic_cast<model::Introspectable*>(u.get());
        std::ofstream(argv[3]) << "{\"model\":" << in->modelJson() << ",\"static\":" << in->curvesJson() << "}";
        return 0;
    }
    if (argc >= 4 && std::string(argv[1]) == "--cpu") {
        // circuit_ab --cpu <table id> <di.wav> [param=value ...]: the circuit unit alone on a DI (prepare included);
        // prints % of one core and Newton diagnostics. CE_RATE=48000 resamples the DI (linear) to that host rate.
        const auto* schema = catalog::findByTableId(std::atoi(argv[2]));
        auto u = schema ? ce::makeCircuitUnitAlways(*schema) : nullptr;
        std::vector<double> di;
        double sr = 48000;
        if (!u || !ce_tools::readWav(argv[3], di, sr)) return 1;
        if (const char* r = std::getenv("CE_RATE")) {
            const double nr = std::atof(r);
            std::vector<double> y(static_cast<std::size_t>(static_cast<double>(di.size()) * nr / sr));
            for (std::size_t i = 0; i < y.size(); ++i) {
                const double x = static_cast<double>(i) * sr / nr;
                const auto k = static_cast<std::size_t>(x);
                const double f = x - static_cast<double>(k);
                y[i] = k + 1 < di.size() ? di[k] * (1 - f) + di[k + 1] * f : di.back();
            }
            di.swap(y);
            sr = nr;
        }
        for (int i = 4; i < argc; ++i) {
            std::string kv = argv[i];
            const auto eq = kv.find('=');
            if (eq != std::string::npos) u->setParameter(kv.substr(0, eq), std::stof(kv.substr(eq + 1)));
        }
        // prepare (compile, DC, mains settle) runs on a control thread; the audio cost is the render alone
        auto threadSecs = [] {
            timespec ts{};
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
            return static_cast<double>(ts.tv_sec) + 1e-9 * static_cast<double>(ts.tv_nsec);
        };
        const double cp0 = threadSecs();
        const auto tp = std::chrono::steady_clock::now();
        u->prepare(sr, std::getenv("CE_BLOCK") ? std::max(1, std::atoi(std::getenv("CE_BLOCK"))) : 256);
        const double cprep = threadSecs() - cp0;
        const double prep = std::chrono::duration<double>(std::chrono::steady_clock::now() - tp).count();
        const auto t0 = std::chrono::steady_clock::now();
        const double c0 = threadSecs();
        const auto y = renderUnit(*u, di, sr);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double cpuPct = 100.0 * (threadSecs() - c0 - cprep) / (static_cast<double>(di.size()) / sr); // thread CPU time: robust to other load
        const double audioPct = 100.0 * (s - prep) / (static_cast<double>(di.size()) / sr); // renderUnit prepares again
        auto* in = dynamic_cast<model::Introspectable*>(u.get());
        std::string diag;
        if (in) {
            const std::string m = in->modelJson();
            const auto k = m.find("\"diagnostics\"");
            if (k != std::string::npos) diag = m.substr(k, m.find('}', k) - k + 1);
        }
        std::printf("unit %s at %.0f Hz: %.1f %% of one core audio (%.1f %% incl. a %.0f ms prepare), peak %.1f dBFS; %s\n", argv[2], sr, audioPct,
                    100.0 * s / (static_cast<double>(di.size()) / sr), 1000 * prep, peakDb(y), diag.c_str());
        std::printf("thread cpu %.1f %%\n", cpuPct);
        if (const char* o = std::getenv("CE_AB_OUT")) ce_tools::writeWav(o, y, sr); // the render, for null tests
        return 0;
    }
    if (argc < 3) {
        std::fprintf(stderr, "usage: circuit_ab <out_dir> <di.wav> [seconds] [--spice-seconds s]\n");
        return 2;
    }
    const fs::path out = argv[1];
    fs::create_directories(out);
    std::vector<double> di;
    double sr = 48000;
    if (!ce_tools::readWav(argv[2], di, sr)) {
        std::fprintf(stderr, "cannot read %s\n", argv[2]);
        return 1;
    }
    double secs = argc > 3 && argv[3][0] != '-' ? std::atof(argv[3]) : 12.0;
    double spiceSecs = 3.0;
    std::vector<int32_t> only; // --only 172000,62000: just these units (the TS808 SPICE DI proof only with 15000)
    for (int i = 3; i < argc; ++i) {
        if (std::string(argv[i]) == "--spice-seconds" && i + 1 < argc) spiceSecs = std::atof(argv[i + 1]);
        if (std::string(argv[i]) == "--only" && i + 1 < argc) {
            std::stringstream ss(argv[i + 1]);
            std::string tok;
            while (std::getline(ss, tok, ','))
                if (!tok.empty()) only.push_back(std::atoi(tok.c_str()));
        }
    }
    auto wanted = [&](int32_t t) { return only.empty() || std::find(only.begin(), only.end(), t) != only.end(); };
    di.resize(std::min(di.size(), static_cast<std::size_t>(secs * sr)));
    ce_tools::writeWav((out / "00_di_input.wav").string(), di, sr);
    std::ostringstream rep;
    rep << "# Circuit engine A/B renders\n\n";
    rep << "Generated by `circuit_ab` (app/tools/circuit/circuit_ab.cpp). Input: `" << fs::path(argv[2]).filename().string() << "` (first "
        << secs << " s, " << sr << " Hz, 1.0 FS = 1 V at the input jack).\n\n";
    rep << "Each pair is the same DI through the **current model** (legacy) and the **circuit engine**, level-matched by RMS "
           "(the gain applied to the circuit render is listed). Band columns are octave-band levels of circuit − legacy after "
           "level matching (positive = the circuit has more energy there).\n\n";
    const std::vector<double> centres = {63, 125, 250, 500, 1000, 2000, 4000, 8000};
    struct Unit {
        int32_t tid;
        std::string tag, title;
        std::vector<Setting> settings;
    };
    const std::vector<Unit> units = {
        {15000, "ts808", "Tube Screamer TS808 (15000)",
         {{"default", {{"Dr", 0.5f}, {"Tn", 0.5f}, {"Vol", 0.5f}}},
          {"low-drive-bright", {{"Dr", 0.15f}, {"Tn", 0.85f}, {"Vol", 0.6f}}},
          {"max-drive-dark", {{"Dr", 1.0f}, {"Tn", 0.25f}, {"Vol", 0.5f}}}}},
        {30000, "twin", "Fender '65 Twin Reverb (30000): the complete AB763 circuit (supply, preamp, reverb, vibrato, power stage, speakers)",
         {{"clean", {{"Vol", 0.3f}, {"Tr", 0.6f}, {"Md", 0.5f}, {"Bs", 0.5f}, {"RvOn", 0.0f}, {"ViOn", 0.0f}}},
          {"edge", {{"Vol", 0.75f}, {"Tr", 0.5f}, {"Md", 0.5f}, {"Bs", 0.5f}, {"RvOn", 0.0f}, {"ViOn", 0.0f}}},
          {"bright-scooped", {{"Vol", 0.4f}, {"Tr", 0.8f}, {"Md", 0.1f}, {"Bs", 0.7f}, {"Br", 1.0f}, {"RvOn", 0.0f}, {"ViOn", 0.0f}}},
          {"reverb", {{"Vol", 0.35f}, {"Tr", 0.6f}, {"Md", 0.5f}, {"Bs", 0.5f}, {"RvOn", 1.0f}, {"RvL", 0.55f}, {"ViOn", 0.0f}}},
          {"vibrato-reverb", {{"Vol", 0.35f}, {"Tr", 0.6f}, {"Md", 0.5f}, {"Bs", 0.5f}, {"RvOn", 1.0f}, {"RvL", 0.4f}, {"ViOn", 1.0f}, {"ViInt", 0.7f}, {"ViSpd", 0.45f}}},
          {"dimed", {{"Vol", 1.0f}, {"Tr", 0.6f}, {"Md", 0.6f}, {"Bs", 0.5f}, {"RvOn", 0.0f}, {"ViOn", 0.0f}}}}},
        {172000, "deluxe", "Fender '65 Deluxe Reverb (172000): the complete AB763 Deluxe Reverb circuit",
         {{"clean", {{"volume", 0.3f}, {"treble", 0.6f}, {"bass", 0.5f}, {"reverb on", 0.0f}, {"tremolo on", 0.0f}}},
          {"edge", {{"volume", 0.65f}, {"treble", 0.5f}, {"bass", 0.5f}, {"reverb on", 0.0f}, {"tremolo on", 0.0f}}},
          {"reverb", {{"volume", 0.35f}, {"treble", 0.6f}, {"bass", 0.5f}, {"reverb on", 1.0f}, {"reverb intensity", 0.5f}, {"tremolo on", 0.0f}}},
          {"dimed", {{"volume", 1.0f}, {"treble", 0.6f}, {"bass", 0.5f}, {"reverb on", 0.0f}, {"tremolo on", 0.0f}}}}},
        {62000, "bassman", "Fender '59 Bassman (62000): the complete 5F6-A circuit",
         {{"clean", {{"VN", 0.25f}, {"VB", 0.0f}, {"T", 0.5f}, {"M", 0.5f}, {"B", 0.5f}, {"Pr", 0.5f}}},
          {"edge", {{"VN", 0.55f}, {"VB", 0.3f}, {"T", 0.6f}, {"M", 0.6f}, {"B", 0.4f}, {"Pr", 0.5f}}},
          {"dimed", {{"VN", 1.0f}, {"VB", 1.0f}, {"T", 0.6f}, {"M", 0.7f}, {"B", 0.4f}, {"Pr", 0.6f}}}}},
        {34000, "plexi", "Marshall 1959 Super Lead Plexi (34000): the complete 1959 circuit",
         {{"clean", {{"V1", 0.2f}, {"V2", 0.0f}, {"T", 0.5f}, {"M", 0.5f}, {"B", 0.5f}, {"Pr", 0.5f}}},
          {"crunch", {{"V1", 0.6f}, {"V2", 0.0f}, {"T", 0.6f}, {"M", 0.7f}, {"B", 0.4f}, {"Pr", 0.5f}}},
          {"dimed", {{"V1", 1.0f}, {"V2", 0.5f}, {"T", 0.6f}, {"M", 0.8f}, {"B", 0.4f}, {"Pr", 0.6f}}}}},
        {40000, "rangemaster", "Dallas Rangemaster (40000): the OC44 treble booster circuit", {{"half", {{"Bs", 0.5f}}}, {"full", {{"Bs", 1.0f}}}}},
        {65000, "microamp", "MXR M133 Micro Amp (65000)", {{"low", {{"B", 0.3f}}}, {"full", {{"B", 1.0f}}}}},
        {169000, "ffvintage", "Fuzz Face Vintage Ge (169000)", {{"default", {{"fuzz", 0.6f}, {"volume", 0.6f}}}, {"max", {{"fuzz", 1.0f}, {"volume", 0.6f}}}}},
        {174000, "percolator", "Interfax Harmonic Percolator (174000)", {{"default", {{"harmonics", 0.5f}, {"volume", 0.6f}}}, {"max", {{"harmonics", 1.0f}, {"volume", 0.6f}}}}},
        {167000, "ts9dx", "Ibanez TS9DX Turbo Screamer (167000)", {{"ts9", {{"overdrive", 0.5f}, {"tone", 0.5f}, {"level", 0.5f}, {"mode", 0.0f}}}, {"hot", {{"overdrive", 0.8f}, {"tone", 0.5f}, {"level", 0.5f}, {"mode", 2.0f}}}}},
        {79000, "tweed5e3", "Fender 5E3 Tweed Deluxe (79000)", {{"clean", {{"vn", 0.3f}, {"vb", 0.3f}, {"t", 0.5f}}}, {"edge", {{"vn", 0.6f}, {"vb", 0.5f}, {"t", 0.6f}}}, {"dimed", {{"vn", 1.0f}, {"vb", 1.0f}, {"t", 0.6f}}}}},
        {176000, "ac15", "Vox AC15 Handwired (176000)", {{"clean", {{"volume", 0.3f}}}, {"edge", {{"volume", 0.6f}}}, {"dimed", {{"volume", 1.0f}}}}},
        {111000, "tube-driver", "BK Butler Tube Driver (111000)", {{"low", {{"D0", 0.3f}, {"T2", 0.5f}}}, {"high", {{"D0", 0.8f}, {"T2", 0.5f}}}}},
        {152000, "supro1624t", "Supro 1624T Dual-Tone (152000)", {{"clean", {{"gain", 0.3f}, {"tone", 0.5f}}}, {"edge", {{"gain", 0.6f}, {"tone", 0.6f}}}, {"dimed", {{"gain", 1.0f}, {"tone", 0.6f}}}}},
        {92000, "plexi-variac", "Marshall 1959 Variac Plexi (92000): the 1959 circuit with its mains dropped to about 90 VAC",
         {{"clean", {{"V1", 0.2f}, {"V2", 0.0f}, {"T", 0.5f}, {"M", 0.5f}, {"B", 0.5f}, {"Pr", 0.5f}}},
          {"crunch", {{"V1", 0.6f}, {"V2", 0.0f}, {"T", 0.6f}, {"M", 0.7f}, {"B", 0.4f}, {"Pr", 0.5f}}},
          {"dimed", {{"V1", 1.0f}, {"V2", 0.5f}, {"T", 0.6f}, {"M", 0.8f}, {"B", 0.4f}, {"Pr", 0.6f}}}}},
        {101000, "jmp2203", "Marshall JMP 2203 (101000): the 2203 circuit",
         {{"clean", {{"V1", 0.2f}, {"V2", 0.5f}, {"T", 0.5f}, {"M", 0.5f}, {"B", 0.5f}, {"Pr", 0.5f}}},
          {"crunch", {{"V1", 0.6f}, {"V2", 0.5f}, {"T", 0.6f}, {"M", 0.6f}, {"B", 0.5f}, {"Pr", 0.5f}}},
          {"lead", {{"V1", 1.0f}, {"V2", 0.6f}, {"T", 0.6f}, {"M", 0.7f}, {"B", 0.5f}, {"Pr", 0.6f}}}}},
        {56000, "jcm800", "Marshall JCM800 2203 (56000): the complete 2203 circuit",
         {{"clean", {{"V1", 0.2f}, {"V2", 0.5f}, {"T", 0.5f}, {"M", 0.5f}, {"B", 0.5f}, {"Pr", 0.5f}}},
          {"crunch", {{"V1", 0.6f}, {"V2", 0.5f}, {"T", 0.6f}, {"M", 0.6f}, {"B", 0.5f}, {"Pr", 0.5f}}},
          {"lead", {{"V1", 1.0f}, {"V2", 0.6f}, {"T", 0.6f}, {"M", 0.7f}, {"B", 0.5f}, {"Pr", 0.6f}}}}},
        {17000, "ds1", "Boss DS-1 (17000): the DS1-A circuit",
         {{"default", {{"Dst", 0.5f}, {"tn", 0.5f}, {"vol", 0.5f}}}, {"max", {{"Dst", 1.0f}, {"tn", 0.6f}, {"vol", 0.5f}}}}},
        {43000, "muff", "Electro-Harmonix Big Muff Pi (43000): the 1971 triangle circuit",
         {{"default", {{"S", 0.5f}, {"T", 0.5f}, {"V", 0.5f}}}, {"max", {{"S", 1.0f}, {"T", 0.4f}, {"V", 0.5f}}}}},
        {41000, "ffsi", "Dallas Arbiter Fuzz Face, silicon (41000)",
         {{"default", {{"F", 0.5f}, {"V", 0.6f}}}, {"max", {{"F", 1.0f}, {"V", 0.6f}}}}},
        {166000, "ffge", "Dallas Arbiter Fuzz Face, germanium (166000)",
         {{"default", {{"fuzz", 0.5f}, {"volume", 0.6f}}}, {"max", {{"fuzz", 1.0f}, {"volume", 0.6f}}}}},
        {42000, "rat", "ProCo RAT 2 (42000): LM308 circuit on its filtered supply",
         {{"default", {{"D", 0.5f}, {"F", 0.5f}, {"V", 0.5f}}}, {"max", {{"D", 1.0f}, {"F", 0.3f}, {"V", 0.5f}}}}},
        {75000, "ph90", "MXR Phase 90 script (75000)", {{"slow", {{"p", 0.3f}}}, {"fast", {{"p", 0.8f}}}}},
        {71000, "gcb95", "Dunlop Cry Baby GCB-95 (71000)", {{"heel", {{"Pd", 0.1f}}}, {"mid", {{"Pd", 0.5f}}}, {"toe", {{"Pd", 0.9f}}}}},
        {72000, "v846", "Vox V846 Clyde McCoy (72000)", {{"heel", {{"Pd", 0.1f}}}, {"mid", {{"Pd", 0.5f}}}, {"toe", {{"Pd", 0.9f}}}}},
        {38000, "ac30", "Vox AC30 Top Boost (38000): the complete AC30 Top Boost circuit",
         {{"clean", {{"Br", 0.25f}, {"Vol", 0.0f}, {"Tb", 0.6f}, {"Bs", 0.5f}, {"Tc", 0.4f}}},
          {"edge", {{"Br", 0.6f}, {"Vol", 0.0f}, {"Tb", 0.6f}, {"Bs", 0.5f}, {"Tc", 0.4f}}},
          {"dimed", {{"Br", 1.0f}, {"Vol", 0.0f}, {"Tb", 0.7f}, {"Bs", 0.4f}, {"Tc", 0.5f}}}}},
    };
    setenv("HEADROOM_LEGACY_MODELS", "1", 1); // legacy side of the A/B (the circuit side uses makeCircuitUnitAlways)
    for (const auto& u : units) {
        if (!wanted(u.tid)) continue;
        const auto* schema = catalog::findByTableId(u.tid);
        rep << "## " << u.title << "\n\n| Setting | Legacy RMS | Circuit RMS (raw) | Match gain | Peak legacy / circuit |";
        for (double c : centres) rep << " " << (c >= 1000 ? std::to_string(static_cast<int>(c / 1000)) + "k" : std::to_string(static_cast<int>(c))) << " |";
        rep << "\n|---|---|---|---|---|";
        for (std::size_t i = 0; i < centres.size(); ++i) rep << "---|";
        rep << "\n";
        int idx = 1;
        for (const auto& s : u.settings) {
            auto legacy = createComponentByTableId(u.tid);
            auto circuit = ce::makeCircuitUnitAlways(*schema);
            for (const auto& [k, v] : s.params) {
                legacy->setParameter(k, v);
                circuit->setParameter(k, v);
            }
            const auto t0 = std::chrono::steady_clock::now();
            const auto a = renderUnit(*legacy, di, sr);
            const auto t1 = std::chrono::steady_clock::now();
            auto b = renderUnit(*circuit, di, sr);
            const auto t2 = std::chrono::steady_clock::now();
            const double ra = rmsDb(a), rb = rmsDb(b);
            const double g = std::pow(10.0, (ra - rb) / 20.0);
            for (auto& v : b) v *= g;
            const auto ba = octaveBands(a, sr, centres), bb = octaveBands(b, sr, centres);
            char name[256];
            std::snprintf(name, sizeof name, "%s_%d_%s", u.tag.c_str(), idx++, s.name.c_str());
            ce_tools::writeWav((out / (std::string(name) + "_legacy.wav")).string(), a, sr);
            ce_tools::writeWav((out / (std::string(name) + "_circuit.wav")).string(), b, sr);
            rep << "| `" << name << "` | " << fmt(ra) << " dB | " << fmt(rb) << " dB | " << fmt(20 * std::log10(g)) << " dB | " << fmt(peakDb(a)) << " / "
                << fmt(peakDb(b)) << " dB |";
            for (std::size_t i = 0; i < centres.size(); ++i) rep << " " << fmt(bb[i] - ba[i]) << " |";
            rep << "\n";
            const double audio = static_cast<double>(di.size()) / sr;
            std::printf("%s: legacy %.1f%% core, circuit %.1f%% core (incl. prepare)\n", name, 100 * std::chrono::duration<double>(t1 - t0).count() / audio,
                        100 * std::chrono::duration<double>(t2 - t1).count() / audio);
        }
        rep << "\n";
    }
    // SPICE vs real-time engine on the same DI (TS808 netlist, default controls), at 2x the file rate.
    if (wanted(15000)) {
        const std::string text = ce::libraryNetlist("ibanez-ts808");
        ce::Netlist n;
        std::string err;
        ce::parseNetlistText(text, n, err);
        const double isr = 2 * sr;
        const std::size_t N = static_cast<std::size_t>(spiceSecs * sr);
        std::vector<double> x(di.begin(), di.begin() + static_cast<long>(std::min(N, di.size())));
        // linear interpolation to 2x: exactly what ngspice's PWL source does between the points
        std::vector<double> x2(2 * x.size());
        for (std::size_t i = 0; i < x.size(); ++i) {
            x2[2 * i] = x[i];
            x2[2 * i + 1] = 0.5 * (x[i] + (i + 1 < x.size() ? x[i + 1] : x[i]));
        }
        ce::Circuit c;
        ce::CompileOptions o;
        o.sampleRate = isr;
        if (!c.compile(n, o, err)) {
            std::fprintf(stderr, "compile: %s\n", err.c_str());
            return 1;
        }
        std::vector<double> ye(x2.size());
        const auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < x2.size(); ++i) ye[i] = c.process(x2[i]);
        const double eSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        ce_tools::writeWav((out / "ts808_spice_vs_engine_engine.wav").string(), ye, isr);
        // ngspice deck with a PWL file source
        ce::SpiceExportOptions so;
        const std::string pwl = (out / "ts808_spice_input.pwl").string();
        {
            std::ofstream p(pwl);
            char b[64];
            for (std::size_t i = 0; i < x.size(); ++i) {
                std::snprintf(b, sizeof b, "%.10e %.9e\n", static_cast<double>(i) / sr, x[i]);
                p << b;
            }
        }
        so.inputFile = pwl;
        std::string deck = ce::exportSpice(n, so);
        const std::string raw = (fs::temp_directory_path() / "headroom_ts808_spice_raw.txt").string(); // ngspice wrdata paths can't contain spaces
        char tran[160];
        std::snprintf(tran, sizeof tran, ".options interp\n.tran %.12g %.12g 0 %.12g\n", 1.0 / isr, static_cast<double>(x.size() - 1) / sr, 1.0 / isr / 4);
        deck += tran;
        deck += ".control\nrun\nwrdata " + raw + " v(" + ce::spiceNodeName(n.io.output) + ")\n.endc\n.end\n";
        const std::string cir = (out / "ts808_spice.cir").string();
        std::ofstream(cir) << deck;
        std::printf("running ngspice on %.1f s of DI ...\n", spiceSecs);
        const auto s0 = std::chrono::steady_clock::now();
        const int rc = std::system(("ngspice -b '" + cir + "' > '" + cir + ".log' 2>&1").c_str());
        const double sSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - s0).count();
        std::vector<double> ys;
        {
            std::ifstream f(raw);
            double t, v;
            while (f >> t >> v) ys.push_back(v);
        }
        rep << "## SPICE vs real-time engine on a guitar DI\n\n";
        if (rc != 0 || ys.empty()) {
            rep << "ngspice did not run (" << rc << "); see `ts808_spice.cir.log`.\n";
        } else {
            ce_tools::writeWav((out / "ts808_spice_vs_engine_spice.wav").string(), ys, isr);
            const std::size_t m = std::min(ys.size(), ye.size());
            double err = 0, ref = 0, mean = 0;
            for (std::size_t i = 0; i < m; ++i) mean += ys[i];
            mean /= static_cast<double>(m);
            std::vector<double> diff(m);
            for (std::size_t i = 0; i < m; ++i) {
                err += (ys[i] - ye[i]) * (ys[i] - ye[i]);
                ref += (ys[i] - mean) * (ys[i] - mean);
                diff[i] = ye[i] - ys[i];
            }
            ce_tools::writeWav((out / "ts808_spice_vs_engine_difference.wav").string(), diff, isr);
            rep << "TS808 netlist at default controls, " << spiceSecs << " s of the same DI, both at " << isr
                << " Hz (the engine's internal rate for a " << sr << " Hz host). Files `ts808_spice_vs_engine_{spice,engine,difference}.wav`.\n\n";
            rep << "| | |\n|---|---|\n";
            rep << "| NRMSE (engine vs ngspice) | " << fmt(100 * std::sqrt(err / ref), 3) << " % |\n";
            rep << "| Difference level | " << fmt(10 * std::log10(err / ref), 1) << " dB below the signal |\n";
            rep << "| ngspice wall time | " << fmt(sSec, 1) << " s (" << fmt(sSec / spiceSecs, 1) << "x real time) |\n";
            rep << "| Engine wall time | " << fmt(eSec, 3) << " s (" << fmt(100 * eSec / spiceSecs, 2) << " % of real time, single core) |\n\n";
        }
    }
    std::ofstream(out / "report.md") << rep.str();
    std::printf("wrote %s\n", (out / "report.md").string().c_str());
    return 0;
}
