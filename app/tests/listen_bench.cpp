// Offline bench for src/analysis/ListenAnalyzer.hpp (tuner / poly tuner / chord / key).
//   clang++ -std=c++20 -O2 -I src tests/listen_bench.cpp -framework Accelerate -o build-listen/listen_bench
//   build-listen/listen_bench            synthetic accuracy suite
//   build-listen/listen_bench file.wav   timeline of key / chord / pitch over a recording
#include "analysis/ListenAnalyzer.hpp"
#include "audio/WavIO.hpp"
#include <cstdio>
#include <filesystem>
#include <map>
#include <random>

using namespace rig::listen;
using namespace rig::audio;
static std::mt19937 rng(7);
static double urand(double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); }

// Plucked-string additive model: partial k at k·f0·sqrt(1+B k²), amplitude ~ k^-tilt, faster decay up high.
static void pluck(std::vector<float>& buf, double sr, double t0, double f0, double amp, double B = 0, double tilt = 1.1, double dur = 3.0) {
    const std::size_t s0 = static_cast<std::size_t>(t0 * sr);
    const int K = std::max(1, std::min(40, static_cast<int>(9000 / f0)));
    std::vector<double> fk(K + 1), ak(K + 1), dk(K + 1), ph(K + 1);
    const double pickPos = urand(0.1, 0.25);
    for (int k = 1; k <= K; ++k) {
        fk[k] = k * f0 * std::sqrt(1 + B * k * k);
        ak[k] = amp * std::pow(k, -tilt) * (0.35 + std::fabs(std::sin(kPi * k * pickPos)));
        dk[k] = 0.6 + 0.25 * k;
        ph[k] = urand(0, 2 * kPi);
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(dur * sr) && s0 + i < buf.size(); ++i) {
        const double t = i / sr;
        double v = 0;
        for (int k = 1; k <= K; ++k) v += ak[k] * std::exp(-dk[k] * t) * std::sin(2 * kPi * fk[k] * t + ph[k]);
        buf[s0 + i] += static_cast<float>(v * std::min(1.0, t * 400));
    }
}
static void noise(std::vector<float>& buf, double db) { std::normal_distribution<double> n(0, std::pow(10, db / 20)); for (auto& x : buf) x += static_cast<float>(n(rng)); }

static std::string modelPath() { const char* m = getenv("LB_MODEL"); return m ? m : "resources/models/guitar_midi.hgnn"; }
static void run(ListenCore& c, const std::vector<float>& b, std::size_t from, std::size_t to) {
    to = std::min(to, b.size());
    if (to > from) c.feed(b.data() + from, b.data() + from, to - from);
}

static const char* NAMES[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

int monoSuite(double sr) {
    std::printf("== mono tuner (sr %.0f) ==\n", sr);
    std::vector<double> errs;
    int fails = 0;
    for (double midi : {23.0, 28.0, 33.0, 35.0, 40.0, 45.0, 50.0, 55.0, 59.0, 64.0, 69.0, 76.0, 84.0, 88.0, 96.0}) {
        for (int rep = 0; rep < 3; ++rep) {
            const double det = urand(-35, 35);
            const double f0 = midiToHz(midi + det / 100);
            std::vector<float> b(static_cast<std::size_t>(sr * 2.0), 0.0f);
            pluck(b, sr, 0.3, f0, 0.3, midi < 45 ? 3e-5 : 1e-5, midi < 45 ? 0.7 : 1.1, 1.7);
            noise(b, -72);
            ListenCore c; c.prepare(sr);
            run(c, b, 0, static_cast<std::size_t>(sr * 1.1));
            const double est = c.voiced() ? c.pitchMidi() : NAN;
            // truth is the fundamental (inharmonic partials are sharp; a strobe-style tuner reads the fundamental)
            const double err = (est - (midi + det / 100)) * 100;
            errs.push_back(std::fabs(err));
            if (!std::isfinite(err) || std::fabs(err) > (getenv("LB_V") ? 0.3 : 1.0)) { ++fails; std::printf("  midi %.0f det %+.1f -> est %.3f err %+.2f c\n", midi, det, est, err); }
        }
    }
    std::sort(errs.begin(), errs.end());
    std::printf("  median |err| %.3f c, p90 %.3f c, max %.3f c, fails(>1c) %d/%zu\n", errs[errs.size() / 2], errs[errs.size() * 9 / 10], errs.back(), fails, errs.size());
    return fails;
}

int polySuite(const char* name, std::vector<double> strings, double sr, double B) {
    std::printf("== poly %s (B=%.0e) ==\n", name, B);
    std::vector<double> all;
    int miss = 0;
    for (int rep = 0; rep < 8; ++rep) {
        std::vector<double> det(strings.size());
        std::vector<float> b(static_cast<std::size_t>(sr * 2.4), 0.0f);
        for (std::size_t s = 0; s < strings.size(); ++s) {
            det[s] = urand(-20, 20);
            pluck(b, sr, 0.3 + 0.012 * s, midiToHz(strings[s] + det[s] / 100), 0.12, B * (strings[s] < 52 ? 0.5 : 1.0), strings[s] < 40 ? 0.8 : 1.1, 2.0);
        }
        noise(b, -75);
        ListenCore c; c.prepare(sr); c.setStrings(strings);
        run(c, b, 0, static_cast<std::size_t>(sr * 1.9));
        std::string line;
        for (std::size_t s = 0; s < strings.size(); ++s) {
            const auto& p = c.poly()[s];
            char t[64];
            if (!p.present) { ++miss; std::snprintf(t, sizeof t, " [%s miss]", NAMES[static_cast<int>(strings[s]) % 12]); line += t; continue; }
            const double err = p.cents - det[s];
            all.push_back(std::fabs(err));
            std::snprintf(t, sizeof t, " %s%+.1f", NAMES[static_cast<int>(strings[s]) % 12], err); line += t;
        }
        if (rep < 3) std::printf("  err/string:%s\n", line.c_str());
    }
    std::sort(all.begin(), all.end());
    if (all.empty()) { std::printf("  nothing detected\n"); return 1; }
    std::printf("  median |err| %.2f c, p90 %.2f c, max %.2f c, missing %d\n", all[all.size() / 2], all[all.size() * 9 / 10], all.back(), miss);
    return 0;
}

struct ChordSpec { const char* name; std::vector<int> notes; int root; const char* q; };
int chordSuite(double sr) {
    std::printf("== chords ==\n");
    std::vector<ChordSpec> cs = {
        {"Em", {40, 47, 52, 55, 59, 64}, 4, "min"}, {"C", {48, 52, 55, 60, 64}, 0, "maj"}, {"G", {43, 47, 50, 55, 59, 67}, 7, "maj"},
        {"D", {50, 57, 62, 66}, 2, "maj"}, {"Am", {45, 52, 57, 60, 64}, 9, "min"}, {"F", {41, 48, 53, 57, 60, 65}, 5, "maj"},
        {"E7", {40, 47, 50, 56, 59, 64}, 4, "7"}, {"Cmaj7", {48, 52, 55, 59, 64}, 0, "maj7"}, {"A5", {45, 52, 57}, 9, "5"},
        {"E5", {40, 47, 52}, 4, "5"}, {"Am7", {45, 52, 55, 60, 64}, 9, "m7"}, {"Dsus4", {50, 57, 62, 67}, 2, "sus4"},
        {"Bm", {47, 54, 59, 62, 66}, 11, "min"}, {"Bb", {46, 53, 58, 62, 65}, 10, "maj"},
    };
    int ok = 0;
    for (auto& ch : cs) {
        std::vector<float> b(static_cast<std::size_t>(sr * 2.0), 0.0f);
        for (std::size_t i = 0; i < ch.notes.size(); ++i) pluck(b, sr, 0.2 + 0.015 * i, midiToHz(ch.notes[i] + urand(-6, 6) / 100), 0.1, 2e-5, 1.0, 1.8);
        noise(b, -75);
        ListenCore c; c.prepare(sr);
        run(c, b, 0, static_cast<std::size_t>(sr * 1.0));
        const auto& d = c.chord();
        if (getenv("LB_CHROMA")) { const std::string j = c.json(); const auto p = j.find("\"chroma\""); std::printf("    %s\n", j.substr(p, 90).c_str()); }
        const bool good = d.root == ch.root && std::string(ListenCore::qualityName(d.quality)) == ch.q;
        ok += good;
        std::printf("  %-6s -> %s%s (bass %s) %s\n", ch.name, d.root >= 0 ? NAMES[d.root] : "-", ListenCore::qualityName(d.quality), d.bass >= 0 ? NAMES[d.bass] : "-", good ? "" : "  <-- WRONG");
    }
    std::printf("  %d/%zu correct\n", ok, cs.size());
    return static_cast<int>(cs.size()) - ok;
}

int keySuite(double sr) {
    std::printf("== key ==\n");
    struct K { const char* name; int root; bool minor; std::vector<std::vector<int>> chords; std::vector<int> melody; };
    std::vector<K> ks = {
        {"E minor riff (pent)", 4, true, {{40, 47, 52}}, {52, 55, 57, 59, 62, 64, 62, 59, 57, 55, 52, 50, 52, 55, 52}},
        {"A minor i-iv-v", 9, true, {{45, 52, 57, 60, 64}, {50, 57, 62, 65}, {40, 47, 52, 56, 59, 64}, {45, 52, 57, 60, 64}}, {}},
        {"C major I-IV-V-I", 0, false, {{48, 52, 55, 60, 64}, {41, 48, 53, 57, 60, 65}, {43, 47, 50, 55, 59, 67}, {48, 52, 55, 60, 64}}, {}},
        {"D major I-V-vi-IV", 2, false, {{50, 57, 62, 66}, {45, 52, 57, 61, 64}, {47, 54, 59, 62, 66}, {43, 47, 50, 55, 59, 67}}, {}},
        {"G major melody", 7, false, {{43, 47, 50, 55, 59, 67}}, {67, 71, 74, 72, 71, 69, 67, 66, 67, 62, 64, 66, 67}},
    };
    int ok = 0;
    for (auto& k : ks) {
        const double len = 14.0;
        std::vector<float> b(static_cast<std::size_t>(sr * len), 0.0f);
        double t = 0.1;
        int i = 0;
        while (t < len - 1) {
            if (!k.melody.empty()) {
                if (i % 8 == 0) for (std::size_t j = 0; j < k.chords[0].size(); ++j) pluck(b, sr, t + 0.01 * j, midiToHz(k.chords[0][j]), 0.06, 2e-5);
                pluck(b, sr, t, midiToHz(k.melody[static_cast<std::size_t>(i) % k.melody.size()]), 0.15, 2e-5, 1.0, 0.6);
                t += 0.3;
            } else {
                const auto& ch = k.chords[static_cast<std::size_t>(i) % k.chords.size()];
                for (std::size_t j = 0; j < ch.size(); ++j) pluck(b, sr, t + 0.012 * j, midiToHz(ch[j]), 0.1, 2e-5, 1.0, 1.4);
                t += 1.2;
            }
            ++i;
        }
        noise(b, -75);
        ListenCore c; c.prepare(sr);
        if (!getenv("LB_NONN")) c.setModelPath(modelPath());
        run(c, b, 0, b.size());
        const int kk = c.key();
        const bool good = kk >= 0 && kk % 12 == k.root && (kk >= 12) == k.minor;
        ok += good;
        std::printf("  %-22s -> %s %s %s\n", k.name, kk >= 0 ? NAMES[kk % 12] : "-", kk >= 12 ? "minor" : "major", good ? "" : "  <-- WRONG");
    }
    std::printf("  %d/%zu correct\n", ok, ks.size());
    return static_cast<int>(ks.size()) - ok;
}

int bendSuite(double sr) {
    std::printf("== bend ==\n");
    // note at B3 (59), bend a whole step to C#4 landing +6 c
    std::vector<float> b(static_cast<std::size_t>(sr * 2.0), 0.0f);
    const double f0 = midiToHz(59), f1 = midiToHz(61.06);
    double ph = 0;
    for (std::size_t i = static_cast<std::size_t>(0.2 * sr); i < b.size(); ++i) {
        const double t = i / sr - 0.2;
        const double k = std::clamp((t - 0.4) / 0.25, 0.0, 1.0), s = k * k * (3 - 2 * k);
        const double f = f0 * std::pow(f1 / f0, s);
        ph += 2 * kPi * f / sr;
        b[i] = static_cast<float>(0.2 * std::exp(-0.8 * t) * (std::sin(ph) + 0.5 * std::sin(2 * ph) + 0.25 * std::sin(3 * ph)));
    }
    ListenCore c; c.prepare(sr);
    run(c, b, 0, static_cast<std::size_t>(1.4 * sr));
    const std::string j = c.json();
    const auto p = j.find("\"bend\":");
    std::printf("  %s\n", j.substr(p, j.find('}', p) - p + 1).c_str());
    return 0;
}

int wavTimeline(const char* path) {
    WavData w;
    std::string err;
    if (!readWavFile(path, w, &err)) { std::fprintf(stderr, "read failed: %s\n", err.c_str()); return 1; }
    const double sr = w.sample_rate;
    ListenCore c; c.prepare(sr); c.setStrings({40, 45, 50, 55, 59, 64});
    if (!getenv("LB_NONN")) c.setModelPath(modelPath());
    const std::size_t step = static_cast<std::size_t>(sr * 0.5);
    std::string lastChord;
    for (std::size_t pos = 0; pos < w.left.size(); pos += step) {
        run(c, w.left, pos, pos + step);
        const auto k = c.key();
        const auto& ch = c.chord();
        char line[200];
        std::snprintf(line, sizeof line, "%6.1fs key %-3s%-6s chord %s%-5s pitch %s", (pos + step) / sr, k >= 0 ? NAMES[k % 12] : "-", k >= 12 ? "minor" : k >= 0 ? "major" : "", ch.root >= 0 ? NAMES[ch.root] : "-", ListenCore::qualityName(ch.quality),
                      c.voiced() ? (std::string(NAMES[static_cast<int>(std::lround(c.pitchMidi())) % 12]) + std::to_string(static_cast<int>(std::lround(c.pitchMidi())) / 12 - 1)).c_str() : "-");
        std::printf("%s\n", line);
    }
    const auto t0 = std::chrono::steady_clock::now();
    ListenCore c2; c2.prepare(sr); c2.setStrings({40, 45, 50, 55, 59, 64});
    if (!getenv("LB_NONN")) c2.setModelPath(modelPath());
    run(c2, w.left, 0, w.left.size());
    const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("cpu: %.1f%% of one core (%.2fs for %.1fs audio)\n", 100 * el / (w.left.size() / sr), el, w.left.size() / sr);
    return 0;
}

// Demo take for UI work: single strings (some detuned), open strums, an E-minor progression, a bend, silence.
int makeDemo(const char* path) {
    const double sr = 44100;
    std::vector<float> b(static_cast<std::size_t>(sr * 34), 0.0f);
    const double det[6] = {-14, 3, 0.4, 9, -2, 1};
    const int open[6] = {40, 45, 50, 55, 59, 64};
    double t = 0.3;
    for (int s = 0; s < 6; ++s) { pluck(b, sr, t, midiToHz(open[s] + det[s] / 100), 0.25, 3e-5, 0.9, 1.6); t += 1.2; }
    for (int k = 0; k < 3; ++k) { for (int s = 0; s < 6; ++s) pluck(b, sr, t + 0.012 * s, midiToHz(open[s] + det[s] / 100 * (k == 2 ? 0.1 : 1.0)), 0.1, 5e-5, 1.0, 2.2); t += 2.2; }
    const std::vector<std::vector<int>> prog = {{40, 47, 52, 55, 59, 64}, {48, 52, 55, 59, 64}, {43, 47, 50, 55, 59, 67}, {50, 57, 62, 66}};
    for (int r = 0; r < 2; ++r) for (const auto& ch : prog) { for (std::size_t j = 0; j < ch.size(); ++j) pluck(b, sr, t + 0.014 * j, midiToHz(ch[j]), 0.09, 2e-5, 1.0, 1.6); t += 1.3; }
    // melody + bend (B → C♯, lands +5 c)
    for (int m : {64, 67, 69, 71}) { pluck(b, sr, t, midiToHz(m), 0.2, 1e-5, 1.0, 0.5); t += 0.35; }
    { const double f0 = midiToHz(71), f1 = midiToHz(73.05); double ph = 0; const std::size_t s0 = static_cast<std::size_t>(t * sr);
      for (std::size_t i = 0; i < static_cast<std::size_t>(1.6 * sr); ++i) { const double tt = i / sr, k = std::clamp((tt - 0.35) / 0.22, 0.0, 1.0), sm = k * k * (3 - 2 * k);
        ph += 2 * kPi * f0 * std::pow(f1 / f0, sm) / sr; b[s0 + i] += static_cast<float>(0.25 * std::exp(-0.9 * tt) * (std::sin(ph) + 0.4 * std::sin(2 * ph) + 0.2 * std::sin(3 * ph)) * std::min(1.0, tt * 300)); } }
    noise(b, -78);
    return rig::audio::writeWavFile16(path, static_cast<uint32_t>(sr), b, b) ? 0 : 1;
}

// Key stability/accuracy over GuitarSet excerpts (label = the " · X · " root in the file name).
// Reports: share of sampled time the shown tonic matches the label (either mode, or the label's relative minor),
// number of key changes per file, and time to the first key.
int keyEval(const char* dir) {
    static const std::map<std::string, int> R = {{"C", 0}, {"C#", 1}, {"Db", 1}, {"D", 2}, {"D#", 3}, {"Eb", 3}, {"E", 4}, {"F", 5}, {"F#", 6}, {"Gb", 6}, {"G", 7}, {"G#", 8}, {"Ab", 8}, {"A", 9}, {"A#", 10}, {"Bb", 10}, {"B", 11}};
    double hit = 0, rel = 0, tot = 0, firstSum = 0; int changes = 0, files = 0, maxCh = 0, nofirst = 0;
    for (auto& e : std::filesystem::directory_iterator(dir)) {
        const std::string name = e.path().filename().string();
        if (e.path().extension() != ".wav") continue;
        const auto a = name.find(" · ", name.find("BPM")); if (a == std::string::npos) continue;
        const auto b = name.find(" · ", a + 4);
        const std::string lab = name.substr(a + 4, b - a - 4);
        if (!R.count(lab)) continue;
        const int root = R.at(lab);
        WavData w; if (!readWavFile(e.path().string(), w)) continue;
        ListenCore c; c.prepare(w.sample_rate); c.setStrings({40, 45, 50, 55, 59, 64});
        const std::size_t step = static_cast<std::size_t>(w.sample_rate * 0.25);
        int last = -1, ch = 0; double first = -1;
        for (std::size_t pos = 0; pos < w.left.size(); pos += step) {
            run(c, w.left, pos, pos + step);
            const int k = c.key();
            const double t = (pos + step) / double(w.sample_rate);
            if (k >= 0 && first < 0) first = t;
            if (k >= 0 && last >= 0 && k != last) ++ch;
            if (k >= 0) last = k;
            if (t >= 8) { tot += 1; if (k >= 0) { const int tonic = k % 12; if (tonic == root) hit += 1; else if (tonic == (root + (k >= 12 ? 9 : 3)) % 12) rel += 1; } }
        }
        if (getenv("KE_V")) std::printf("  %-55s label %-2s -> %s%s, changes %d, first %.1fs\n", name.substr(0, 55).c_str(), lab.c_str(), last >= 0 ? NAMES[last % 12] : "-", last >= 12 ? "m" : "", ch, first);
        changes += ch; maxCh = std::max(maxCh, ch); ++files;
        if (first < 0) ++nofirst; else firstSum += first;
    }
    std::printf("key eval: %d files, tonic right %.1f%% / relative swap %.1f%% of time (after 8 s), changes/file %.2f (max %d), first key after %.1f s avg, %d never\n",
                files, 100 * hit / std::max(1.0, tot), 100 * rel / std::max(1.0, tot), double(changes) / std::max(1, files), maxCh, firstSum / std::max(1, files - nofirst), nofirst);
    return 0;
}

// Full GuitarSet key benchmark: <dir>/NN_<Style><n>-<bpm>-<Key>_<comp|solo>_mix.wav, mode from the progression
// (GuitarSet key_mode annotations: progression 2 of every style is minor, 1 and 3 major).
// Scores the key shown at the end of each take (exact / MIREX-weighted) and the share of time it was exact after
// 8 s, plus key changes. "--shard i/n" evaluates every n-th file (run shards in parallel, then sum with --merge).
int keyEval2(const char* dir, int shard, int nshard) {
    static const std::map<std::string, int> R = {{"C", 0}, {"C#", 1}, {"Db", 1}, {"D", 2}, {"D#", 3}, {"Eb", 3}, {"E", 4}, {"F", 5}, {"F#", 6}, {"Gb", 6}, {"G", 7}, {"G#", 8}, {"Ab", 8}, {"A", 9}, {"A#", 10}, {"Bb", 10}, {"B", 11}};
    std::vector<std::filesystem::path> files;
    for (auto& e : std::filesystem::directory_iterator(dir)) if (e.path().string().ends_with("_mix.wav")) files.push_back(e.path());
    std::sort(files.begin(), files.end());
    struct Acc { double exact = 0, weighted = 0, timeExact = 0, timeN = 0, changes = 0, n = 0; } acc[2];  // comp, solo
    for (std::size_t fi = 0; fi < files.size(); ++fi) {
        if (static_cast<int>(fi % nshard) != shard) continue;
        const std::string name = files[fi].filename().string();
        const auto d1 = name.find('-'), d2 = name.find('-', d1 + 1), us = name.find('_', d2);
        const std::string prog = name.substr(name.find('_') + 1, d1 - name.find('_') - 1), lab = name.substr(d2 + 1, us - d2 - 1);
        if (!R.count(lab)) continue;
        const int root = R.at(lab), minor = prog.back() == '2' ? 1 : 0, gt = root + 12 * minor;
        const int kind = name.find("_solo") != std::string::npos ? 1 : 0;
        WavData w; if (!readWavFile(files[fi].string(), w)) continue;
        ListenCore c; c.prepare(w.sample_rate); c.setStrings({40, 45, 50, 55, 59, 64});
        if (!getenv("LB_NONN")) c.setModelPath(modelPath());
        const std::size_t step = static_cast<std::size_t>(w.sample_rate * 0.25);
        int last = -1, ch = 0;
        for (std::size_t pos = 0; pos < w.left.size(); pos += step) {
            run(c, w.left, pos, pos + step);
            const int k = c.key();
            if (k >= 0 && last >= 0 && k != last) ++ch;
            if (k >= 0) last = k;
            if ((pos + step) / double(w.sample_rate) >= 8) { acc[kind].timeN += 1; if (k == gt) acc[kind].timeExact += 1; }
        }
        auto weight = [&](int k) {
            if (k < 0) return 0.0;
            const int kr = k % 12, km = k / 12;
            if (k == gt) return 1.0;
            if (km == minor && ((kr - root + 12) % 12 == 5 || (kr - root + 12) % 12 == 7)) return 0.5;
            if (km != minor && kr == (minor ? (root + 3) % 12 : (root + 9) % 12)) return 0.3;
            if (kr == root) return 0.2;
            return 0.0;
        };
        acc[kind].exact += last == gt; acc[kind].weighted += weight(last); acc[kind].changes += ch; acc[kind].n += 1;
        if (getenv("KE_V")) std::printf("  %-34s gt %s%s -> %s%s ch %d\n", name.c_str(), NAMES[root], minor ? "m" : "", last >= 0 ? NAMES[last % 12] : "-", last >= 12 ? "m" : "", ch);
    }
    for (int k = 0; k < 2; ++k)
        std::printf("RAW %s %.0f %.2f %.2f %.0f %.0f %.0f\n", k ? "solo" : "comp", acc[k].exact, acc[k].weighted, acc[k].timeExact, acc[k].timeN, acc[k].changes, acc[k].n);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--key-eval2") {
        int sh = 0, ns = 1;
        if (argc >= 4) std::sscanf(argv[3], "%d/%d", &sh, &ns);
        return keyEval2(argv[2], sh, ns);
    }
    if (argc >= 3 && std::string(argv[1]) == "--key-eval") return keyEval(argv[2]);
    if (argc >= 3 && std::string(argv[1]) == "--make-demo") return makeDemo(argv[2]);
    if (argc >= 2) return wavTimeline(argv[1]);
    int f = 0;
    f += monoSuite(48000);
    f += monoSuite(44100);
    polySuite("guitar standard", {40, 45, 50, 55, 59, 64}, 48000, 0);
    polySuite("guitar standard", {40, 45, 50, 55, 59, 64}, 48000, 8e-5);
    polySuite("guitar drop D", {38, 45, 50, 55, 59, 64}, 48000, 8e-5);
    polySuite("bass 5", {23, 28, 33, 38, 43}, 48000, 4e-5);
    polySuite("bass 4", {28, 33, 38, 43}, 44100, 4e-5);
    polySuite("7-string", {35, 40, 45, 50, 55, 59, 64}, 48000, 8e-5);
    chordSuite(48000);
    keySuite(48000);
    bendSuite(48000);
    return f ? 1 : 0;
}
