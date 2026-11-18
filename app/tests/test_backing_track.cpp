// Practice backing tracks (src/audio/BackingTrackPlayer + BackingTracks):
//  * decode: WAV (16-bit, mono → stereo), AIFF and AAC/M4A through ExtAudioFile (macOS) — or AIFF, FLAC and MP3
//    through miniaudio elsewhere (tests/fixtures/backing: tiny synthetic tones) — resampled to the engine rate
//  * transport: seek (position + content), loop A–B (stays inside, crossfaded splice), end of track
//  * mix: volume, loudness normalisation, duck on input, added on top of the rig signal, soft ceiling
//  * time-pitch: speed keeps the pitch, transpose keeps the tempo
//  * count-in clicks before the track starts
//  * real time: no malloc/free on the audio thread (counted by tests/rtguard.cpp via DYLD_INSERT_LIBRARIES on macOS,
//    LD_PRELOAD on Linux; skipped on Windows),
//    including concurrent hand-over of a new track and clear()
//  * library: import (copy), duplicates, rename, delete, list order, unreadable files, the /api/backing routes
#undef NDEBUG
#include "../src/audio/BackingTrackPlayer.hpp"
#include "../src/audio/BackingTracks.hpp"

#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#endif
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <process.h>
#define RIG_GETPID _getpid
#else
#include <dlfcn.h>
#include <unistd.h>
#define RIG_GETPID ::getpid
#endif
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace rig::audio;

static int g_fail = 0;
#define CHECK(cond, ...)                                                                \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            ++g_fail;                                                                   \
            std::printf("  FAIL %s:%d  %s  — ", __FILE__, __LINE__, #cond);             \
            std::printf(__VA_ARGS__);                                                   \
            std::printf("\n");                                                          \
        }                                                                               \
    } while (0)

static constexpr double kSr = 48000.0;
static void (*g_guard_begin)() = nullptr;
static long (*g_guard_end)() = nullptr;

// ---------- files ----------
static void writeWav16(const std::string& path, int sr, int ch, const std::function<float(int64_t, int)>& f, int64_t frames) {
    std::ofstream o(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { o.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { o.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t data = static_cast<uint32_t>(frames * ch * 2);
    o.write("RIFF", 4); u32(36 + data); o.write("WAVE", 4);
    o.write("fmt ", 4); u32(16); u16(1); u16(static_cast<uint16_t>(ch)); u32(sr); u32(sr * ch * 2); u16(static_cast<uint16_t>(ch * 2)); u16(16);
    o.write("data", 4); u32(data);
    for (int64_t i = 0; i < frames; ++i)
        for (int c = 0; c < ch; ++c) {
            const int16_t s = static_cast<int16_t>(std::lrint(std::clamp(f(i, c), -1.0f, 1.0f) * 32767.0f));
            o.write(reinterpret_cast<const char*>(&s), 2);
        }
}

#ifdef __APPLE__
// AIFF / M4A through ExtAudioFile (the same framework that decodes them).
static bool writeExt(const std::string& path, AudioFileTypeID type, bool aac, int sr, const std::function<float(int64_t, int)>& f, int64_t frames) {
    AudioStreamBasicDescription file{};
    file.mSampleRate = sr;
    file.mChannelsPerFrame = 2;
    if (aac) {
        file.mFormatID = kAudioFormatMPEG4AAC;
    } else {
        file.mFormatID = kAudioFormatLinearPCM;
        file.mFormatFlags = kAudioFormatFlagIsBigEndian | kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        file.mBitsPerChannel = 16;
        file.mFramesPerPacket = 1;
        file.mBytesPerFrame = file.mBytesPerPacket = 4;
    }
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.data()), static_cast<CFIndex>(path.size()), false);
    ExtAudioFileRef ef = nullptr;
    OSStatus st = ExtAudioFileCreateWithURL(url, type, &file, nullptr, kAudioFileFlags_EraseFile, &ef);
    CFRelease(url);
    if (st != noErr) return false;
    AudioStreamBasicDescription cf{};
    cf.mSampleRate = sr;
    cf.mFormatID = kAudioFormatLinearPCM;
    cf.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    cf.mChannelsPerFrame = 2;
    cf.mBitsPerChannel = 32;
    cf.mFramesPerPacket = 1;
    cf.mBytesPerFrame = cf.mBytesPerPacket = 8;
    ExtAudioFileSetProperty(ef, kExtAudioFileProperty_ClientDataFormat, sizeof cf, &cf);
    std::vector<float> buf(4096 * 2);
    for (int64_t off = 0; off < frames; off += 4096) {
        const UInt32 n = static_cast<UInt32>(std::min<int64_t>(4096, frames - off));
        for (UInt32 i = 0; i < n; ++i) { buf[2 * i] = f(off + i, 0); buf[2 * i + 1] = f(off + i, 1); }
        AudioBufferList abl{};
        abl.mNumberBuffers = 1;
        abl.mBuffers[0] = {2, n * 8, buf.data()};
        if (ExtAudioFileWrite(ef, n, &abl) != noErr) { ExtAudioFileDispose(ef); return false; }
    }
    ExtAudioFileDispose(ef);
    return true;
}
#else
// 16-bit stereo AIFF written by hand (big-endian; 80-bit extended sample rate).
static bool writeAiff16(const std::string& path, int sr, const std::function<float(int64_t, int)>& f, int64_t frames) {
    std::ofstream o(path, std::ios::binary);
    auto b32 = [&](uint32_t v) { const char c[4] = {char(v >> 24), char(v >> 16), char(v >> 8), char(v)}; o.write(c, 4); };
    auto b16 = [&](uint16_t v) { const char c[2] = {char(v >> 8), char(v)}; o.write(c, 2); };
    const uint32_t data = static_cast<uint32_t>(frames * 4);
    o.write("FORM", 4); b32(4 + 26 + 16 + data); o.write("AIFF", 4);
    o.write("COMM", 4); b32(18); b16(2); b32(static_cast<uint32_t>(frames)); b16(16);
    int e = 0;
    double m = std::frexp(double(sr), &e); // sr = m·2^e, 0.5 ≤ m < 1
    const uint64_t mant = static_cast<uint64_t>(std::ldexp(m, 64));
    b16(static_cast<uint16_t>(16383 + e - 1));
    b32(static_cast<uint32_t>(mant >> 32)); b32(static_cast<uint32_t>(mant));
    o.write("SSND", 4); b32(8 + data); b32(0); b32(0);
    for (int64_t i = 0; i < frames; ++i)
        for (int c = 0; c < 2; ++c) b16(static_cast<uint16_t>(static_cast<int16_t>(std::lrint(std::clamp(f(i, c), -1.0f, 1.0f) * 32767.0f))));
    return static_cast<bool>(o);
}
static const fs::path kFixtures = fs::path(RIG_TEST_FIXTURES) / "backing";
#endif

// ---------- helpers ----------
// A synthetic track at kSr. L = ramp that encodes the frame index (frame % 16000 → 0..0.8),
// R = a level that steps every second (0.1, 0.2, …): enough to read back "where am I" from the output.
static std::unique_ptr<BackingAudio> rampTrack(double seconds) {
    auto a = std::make_unique<BackingAudio>();
    a->sample_rate = kSr;
    a->frames = static_cast<std::size_t>(seconds * kSr);
    a->scale = 1.0f / 32767.0f;
    a->norm_gain = 1.0f;
    a->pcm.resize(a->frames * 2);
    for (std::size_t i = 0; i < a->frames; ++i) {
        a->pcm[2 * i] = static_cast<int16_t>(std::lrint(double(i % 16000) / 16000.0 * 0.8 * 32767.0));
        a->pcm[2 * i + 1] = static_cast<int16_t>(std::lrint(0.1 * double(1 + i / static_cast<std::size_t>(kSr)) * 32767.0 * 0.5));
    }
    return a;
}
static std::unique_ptr<BackingAudio> constTrack(double seconds, float v) {
    auto a = std::make_unique<BackingAudio>();
    a->sample_rate = kSr;
    a->frames = static_cast<std::size_t>(seconds * kSr);
    a->pcm.assign(a->frames * 2, static_cast<int16_t>(std::lrint(v * 32767.0f)));
    return a;
}
static std::unique_ptr<BackingAudio> sineTrack(double seconds, double hz, float amp) {
    auto a = std::make_unique<BackingAudio>();
    a->sample_rate = kSr;
    a->frames = static_cast<std::size_t>(seconds * kSr);
    a->pcm.resize(a->frames * 2);
    for (std::size_t i = 0; i < a->frames; ++i)
        a->pcm[2 * i] = a->pcm[2 * i + 1] = static_cast<int16_t>(std::lrint(amp * std::sin(2.0 * std::numbers::pi * hz * double(i) / kSr) * 32767.0));
    return a;
}

struct Out {
    std::vector<float> l, r;
};
// Runs the player for `seconds` in `block`-sized calls over a zero (or `bed`) rig signal.
static Out run(BackingTrackPlayer& p, double seconds, uint32_t block = 256, float bed = 0.0f, const float* side = nullptr) {
    Out o;
    const std::size_t n = static_cast<std::size_t>(seconds * kSr);
    o.l.assign(n, bed);
    o.r.assign(n, bed);
    for (std::size_t off = 0; off < n; off += block) {
        const uint32_t m = static_cast<uint32_t>(std::min<std::size_t>(block, n - off));
        p.process(o.l.data() + off, o.r.data() + off, m, side ? side + off : nullptr, side ? side + off : nullptr);
    }
    return o;
}
static double freqOf(const std::vector<float>& x, std::size_t a, std::size_t b) {
    int zc = 0;
    for (std::size_t i = a + 1; i < b; ++i) if ((x[i - 1] <= 0.0f) != (x[i] <= 0.0f)) ++zc;
    return zc / 2.0 / (double(b - a) / kSr);
}
static double rms(const std::vector<float>& x, std::size_t a, std::size_t b) {
    double s = 0.0;
    for (std::size_t i = a; i < b; ++i) s += double(x[i]) * x[i];
    return std::sqrt(s / double(std::max<std::size_t>(1, b - a)));
}

// ---------- tests ----------
static void testDecode(const fs::path& dir) {
    std::printf("decode\n");
    const auto sine = [](double hz, float amp, int sr) { return [=](int64_t i, int) { return amp * static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * double(i) / sr)); }; };
    // 16-bit WAV at 44.1 kHz, stereo with different content per side
    const std::string wav = (dir / "stereo 44k.wav").string();
    writeWav16(wav, 44100, 2, [](int64_t i, int c) { return (c ? 0.25f : 0.5f) * static_cast<float>(std::sin(2.0 * std::numbers::pi * (c ? 660.0 : 440.0) * double(i) / 44100.0)); }, 44100 * 3);
    BackingFileInfo info;
    std::string err;
    CHECK(probeBackingFile(wav, info, &err), "%s", err.c_str());
    CHECK(std::fabs(info.seconds - 3.0) < 1e-3 && info.channels == 2 && info.format == "WAV", "%.3f s %d ch %s", info.seconds, info.channels, info.format.c_str());
    auto a = decodeBackingFile(wav, kSr, &err);
    CHECK(a != nullptr, "%s", err.c_str());
    if (a) {
        CHECK(std::llabs(static_cast<long long>(a->frames) - 144000) < 64, "frames %zu", a->frames);
        std::vector<float> l(a->frames), r(a->frames);
        for (std::size_t i = 0; i < a->frames; ++i) { l[i] = a->pcm[2 * i] * a->scale; r[i] = a->pcm[2 * i + 1] * a->scale; }
        CHECK(std::fabs(freqOf(l, 4800, 140000) - 440.0) < 2.0, "L %.1f Hz", freqOf(l, 4800, 140000));
        CHECK(std::fabs(freqOf(r, 4800, 140000) - 660.0) < 2.0, "R %.1f Hz", freqOf(r, 4800, 140000));
        CHECK(std::fabs(a->peak - 0.5f) < 0.01f, "peak %.3f", a->peak);
        CHECK(std::fabs(rms(l, 4800, 140000) - 0.5 / std::sqrt(2.0)) < 0.01, "L rms %.4f", rms(l, 4800, 140000));
        CHECK(a->bin_peak.size() == BackingAudio::kBins && a->bin_peak[BackingAudio::kBins / 2] > 0.45f, "bins");
        // gated RMS ≈ mean of both sides' power → normalisation brings it to the target
        CHECK(a->norm_gain > 0.2f && a->norm_gain < 1.0f, "norm %.3f", a->norm_gain);
    }
    // mono → both sides
    const std::string mono = (dir / "mono.wav").string();
    writeWav16(mono, 48000, 1, sine(220.0, 0.4f, 48000), 48000);
    auto m = decodeBackingFile(mono, kSr, &err);
    CHECK(m && m->frames == 48000, "mono frames");
    if (m) CHECK(m->pcm[2 * 1000] == m->pcm[2 * 1000 + 1] && m->pcm[2 * 1000] != 0, "mono duplicated");
#ifdef __APPLE__
    // AIFF and AAC/M4A
    const std::string aif = (dir / "tone.aiff").string(), m4a = (dir / "tone.m4a").string();
    CHECK(writeExt(aif, kAudioFileAIFFType, false, 48000, sine(330.0, 0.3f, 48000), 96000), "write aiff");
    CHECK(writeExt(m4a, kAudioFileM4AType, true, 44100, sine(330.0, 0.3f, 44100), 88200), "write m4a");
    for (const auto& p : {aif, m4a}) {
        auto d = decodeBackingFile(p, kSr, &err);
        CHECK(d != nullptr, "%s: %s", p.c_str(), err.c_str());
        if (!d) continue;
        std::vector<float> l(d->frames);
        for (std::size_t i = 0; i < d->frames; ++i) l[i] = d->pcm[2 * i] * d->scale;
        CHECK(std::fabs(d->seconds() - 2.0) < 0.06, "%s %.3f s", p.c_str(), d->seconds());
        CHECK(std::fabs(freqOf(l, 9600, 86000) - 330.0) < 2.0, "%s %.1f Hz", p.c_str(), freqOf(l, 9600, 86000));
        CHECK(std::fabs(rms(l, 9600, 86000) - 0.3 / std::sqrt(2.0)) < 0.02, "%s rms %.4f", p.c_str(), rms(l, 9600, 86000));
    }
    BackingFileInfo i2;
    probeBackingFile(m4a, i2);
    CHECK(i2.format == "AAC", "format %s", i2.format.c_str());
#else
    // AIFF (written here), FLAC and MP3 (checked-in 1 s mono 22.05 kHz 330 Hz tones with title/artist tags)
    const std::string aif = (dir / "tone.aiff").string();
    CHECK(writeAiff16(aif, 48000, sine(330.0, 0.3f, 48000), 96000), "write aiff");
    struct Want { std::string path, format; double secs, tol; };
    const Want wants[] = {{aif, "AIFF", 2.0, 0.01},
                          {(kFixtures / "tone330.flac").string(), "FLAC", 1.0, 0.01},
                          {(kFixtures / "tone330.mp3").string(), "MP3", 1.0, 0.08}}; // MP3: encoder delay/padding
    for (const auto& w : wants) {
        BackingFileInfo pi;
        CHECK(probeBackingFile(w.path, pi, &err), "%s: %s", w.path.c_str(), err.c_str());
        CHECK(pi.format == w.format, "%s format %s", w.path.c_str(), pi.format.c_str());
        if (w.format != "AIFF") CHECK(pi.title == "Test Tone" && pi.artist == "Headroom", "%s tags '%s' / '%s'", w.path.c_str(), pi.title.c_str(), pi.artist.c_str());
        auto d = decodeBackingFile(w.path, kSr, &err);
        CHECK(d != nullptr, "%s: %s", w.path.c_str(), err.c_str());
        if (!d) continue;
        std::vector<float> l(d->frames), r(d->frames);
        for (std::size_t i = 0; i < d->frames; ++i) { l[i] = d->pcm[2 * i] * d->scale; r[i] = d->pcm[2 * i + 1] * d->scale; }
        const std::size_t a0 = 4800, a1 = static_cast<std::size_t>((w.secs - 0.1) * kSr);
        CHECK(std::fabs(d->seconds() - w.secs) < w.tol, "%s %.3f s", w.path.c_str(), d->seconds());
        CHECK(std::fabs(freqOf(l, a0, a1) - 330.0) < 2.0, "%s %.1f Hz", w.path.c_str(), freqOf(l, a0, a1));
        CHECK(std::fabs(rms(l, a0, a1) - 0.3 / std::sqrt(2.0)) < 0.02, "%s rms %.4f", w.path.c_str(), rms(l, a0, a1));
        CHECK(std::fabs(rms(r, a0, a1) - rms(l, a0, a1)) < 1e-3, "%s stereo", w.path.c_str());
    }
#endif
    // not audio
    const std::string junk = (dir / "not audio.mp3").string();
    { std::ofstream(junk) << "definitely not an mp3 file, just text\n"; }
    CHECK(!probeBackingFile(junk, info, &err) && !err.empty(), "junk accepted");
    CHECK(decodeBackingFile(junk, kSr, &err) == nullptr, "junk decoded");
}

static void testTransport() {
    std::printf("seek / loop / end\n");
    BackingTrackPlayer p;
    p.prepare(kSr);
    p.setNormalize(false);
    p.setAudio(rampTrack(6.0));
    CHECK(std::fabs(p.duration() - 6.0) < 1e-9, "duration");
    run(p, 0.01);                                 // hand-over happens on the audio thread
    CHECK(!p.playing() && p.position() == 0.0, "starts paused at 0");
    Out silent = run(p, 0.1, 256, 0.3f);
    CHECK(silent.l[1000] == 0.3f, "paused player leaves the rig untouched (%f)", silent.l[1000]);

    p.play();
    Out o = run(p, 0.5);
    // continuity: once the fades settle the ramp advances by exactly one step per frame
    const float step = 0.8f / 16000.0f;
    bool cont = true;
    for (std::size_t i = 4000; i < 20000; ++i) {
        const float d = o.l[i] - o.l[i - 1];
        if (std::fabs(d - step) > 2e-4f && d > -0.5f) { cont = false; break; }
    }
    CHECK(cont, "ramp continuity");
    CHECK(std::fabs(p.position() - 0.5) < 0.006, "position %.4f after 0.5 s", p.position());

    // seek: content and position follow
    p.seek(3.25);
    CHECK(std::fabs(p.position() - 3.25) < 1e-9, "reported at once");
    o = run(p, 0.1);
    CHECK(std::fabs(p.position() - 3.35) < 0.006, "position %.4f", p.position());
    // R holds 0.1·(1+floor(t))·0.5 → 0.2 in second 3
    CHECK(std::fabs(o.r[2000] - 0.2f) < 0.003f, "after seek R=%.4f", o.r[2000]);
    const std::size_t at = static_cast<std::size_t>(3.25 * kSr) + 2000;
    CHECK(std::fabs(o.l[2000] - static_cast<float>(at % 16000) / 16000.0f * 0.8f) < 0.002f, "after seek L=%.4f", o.l[2000]);
    // the splice is crossfaded: no sample-to-sample jump larger than the content allows
    float maxjump = 0.0f;
    for (std::size_t i = 1; i < 600; ++i) maxjump = std::max(maxjump, std::fabs(o.r[i] - o.r[i - 1]));
    CHECK(maxjump < 0.01f, "seek splice jump %.4f", maxjump);

    // loop A–B: playback stays inside, the position wraps
    p.setLoop(true, 1.0, 1.5);
    p.seek(1.2);
    double lo = 9, hi = -9;
    Out lp;
    for (int k = 0; k < 40; ++k) {
        lp = run(p, 0.05);
        lo = std::min(lo, p.position());
        hi = std::max(hi, p.position());
    }
    CHECK(lo >= 0.99 && hi <= 1.51, "loop range %.3f..%.3f", lo, hi);
    CHECK(hi - lo > 0.4, "loop actually moves %.3f..%.3f", lo, hi);
    CHECK(std::fabs(lp.r[1000] - 0.1f) < 0.003f, "loop content R=%.4f (second 1)", lp.r[1000]);
    // a seek past B lands back at A
    p.seek(4.0);
    run(p, 0.02);
    CHECK(p.position() >= 1.0 && p.position() < 1.1, "seek past B → A (%.3f)", p.position());
    p.setLoop(false, 1.0, 1.5);

    // end of track: stops, flags ended; play again restarts at 0
    p.seek(5.9);
    run(p, 0.3);
    CHECK(!p.playing() && p.ended(), "stops at the end");
    CHECK(std::fabs(p.position() - 6.0) < 0.01, "parks at the end %.3f", p.position());
    p.play();
    run(p, 0.1);
    CHECK(p.playing() && p.position() < 0.2, "replay from the top %.3f", p.position());
    p.pause();
    Out tail = run(p, 0.1);
    CHECK(tail.l[2000] == 0.0f && !p.playing(), "pause fades out");
}

static void testMix() {
    std::printf("mix level / normalisation / duck\n");
    BackingTrackPlayer p;
    p.prepare(kSr);
    p.setNormalize(false);
    p.setAudio(constTrack(4.0, 0.25f));
    p.setVolume(0.5f);
    p.play();
    Out o = run(p, 0.2, 512, 0.1f);
    CHECK(std::fabs(o.l[8000] - (0.1f + 0.125f)) < 1e-3f, "rig 0.1 + track 0.25×0.5 = %.4f", o.l[8000]);
    p.setVolume(1.0f);
    o = run(p, 0.2, 512, 0.1f);
    CHECK(std::fabs(o.l[8000] - 0.35f) < 1e-3f, "unity %.4f", o.l[8000]);
    // the sum is soft-limited, never past full scale
    p.setVolume(2.0f);
    o = run(p, 0.2, 512, 0.8f);
    CHECK(o.l[8000] < 1.0f && o.l[8000] > 0.95f, "ceiling %.4f", o.l[8000]);
    // normalisation: the gain recorded at decode applies on top of the volume
    p.setVolume(1.0f);
    p.setNormalize(true);
    o = run(p, 0.2);
    CHECK(std::fabs(o.l[8000] - 0.25f) < 1e-3f, "norm_gain 1 on a hand-built track");
    // duck: input above the threshold pulls the track down by the depth, then it recovers
    p.setNormalize(false);
    p.setDuck(true, -12.0f);
    std::vector<float> side(static_cast<std::size_t>(2.6 * kSr), 0.0f);
    for (std::size_t i = 0; i < static_cast<std::size_t>(0.6 * kSr); ++i) side[i] = 0.3f * static_cast<float>(std::sin(i * 0.05));
    o = run(p, 2.6, 256, 0.0f, side.data());
    const float ducked = o.l[static_cast<std::size_t>(0.5 * kSr)], back = o.l[static_cast<std::size_t>(2.55 * kSr)];
    CHECK(std::fabs(ducked - 0.25f * std::pow(10.0f, -12.0f / 20.0f)) < 0.004f, "ducked %.4f", ducked);
    CHECK(std::fabs(back - 0.25f) < 0.01f, "recovered %.4f", back);
    p.setDuck(false);
}

static void testTimePitch() {
    std::printf("speed / transpose\n");
    BackingTrackPlayer p;
    p.prepare(kSr);
    p.setNormalize(false);
    p.setAudio(sineTrack(8.0, 440.0, 0.4f));
    std::printf("  stretch latency %.1f ms\n", p.stretchLatency() * 1000.0);
    p.play();
    run(p, 0.05);
    p.setSpeed(0.75f);
    const double t0 = p.position();
    Out o = run(p, 2.0);
    const double adv = p.position() - t0;
    CHECK(std::fabs(adv - 1.5) < 0.12, "75%%: 2 s of output covers %.3f s of track", adv);
    CHECK(std::fabs(freqOf(o.l, 24000, 96000) - 440.0) < 4.0, "75%%: pitch kept %.1f Hz", freqOf(o.l, 24000, 96000));
    CHECK(rms(o.l, 24000, 96000) > 0.2, "75%%: level %.3f", rms(o.l, 24000, 96000));
    p.setSpeed(1.0f);
    p.setSemitones(12.0f);
    const double t1 = p.position();
    o = run(p, 1.5);
    CHECK(std::fabs(p.position() - t1 - 1.5) < 0.12, "+12 st: tempo kept (%.3f s)", p.position() - t1);
    CHECK(std::fabs(freqOf(o.l, 24000, 72000) - 880.0) < 8.0, "+12 st: %.1f Hz", freqOf(o.l, 24000, 72000));
    p.setSemitones(0.0f);
    o = run(p, 0.5);
    CHECK(std::fabs(freqOf(o.l, 4800, 24000) - 440.0) < 3.0, "back to direct %.1f Hz", freqOf(o.l, 4800, 24000));
}

static void testCountIn() {
    std::printf("count-in\n");
    BackingTrackPlayer p;
    p.prepare(kSr);
    p.setNormalize(false);
    p.setAudio(constTrack(4.0, 0.2f));
    p.setCountIn(2, 120.0); // 2 beats = 1 s
    p.play();
    run(p, 0.01);
    CHECK(p.countInLeft() == 2, "beats left %d", p.countInLeft());
    Out o = run(p, 0.9);
    CHECK(p.position() == 0.0, "track waits during the count-in");
    const double click1 = rms(o.l, 0, 1200), gap = rms(o.l, 6000, 20000), click2 = rms(o.l, 24000 - 480, 25200 - 480);
    CHECK(click1 > 0.02 && click2 > 0.02 && gap < 1e-4, "clicks %.3f %.3f gap %.5f", click1, click2, gap);
    o = run(p, 0.5);
    CHECK(p.countInLeft() == 0 && p.position() > 0.35, "then it plays (%.3f)", p.position());
    CHECK(std::fabs(o.l[20000] - 0.2f) < 1e-3f, "track level %.4f", o.l[20000]);
}

static void testNoHeapOnAudioThread() {
    std::printf("audio thread: no heap\n");
    if (!g_guard_begin) {
#ifdef _WIN32
        std::printf("  (no rtguard on Windows — heap check skipped; it runs on macOS and Linux)\n");
#else
#ifdef __APPLE__
        const char* dyld = std::getenv("DYLD_INSERT_LIBRARIES");
#else
        const char* dyld = std::getenv("LD_PRELOAD");
#endif
        CHECK(!(dyld && std::strstr(dyld, "rtguard")), "rtguard requested but not loaded");
        std::printf("  (rtguard not loaded — heap check skipped; ctest preloads it)\n");
#endif
        return;
    }
    {   // the guard itself must see a heap call, or a zero below would prove nothing
        g_guard_begin();
        void* volatile probe = std::malloc(64);
        const long seen = g_guard_end();
        std::free(probe);
        CHECK(seen >= 1, "rtguard saw %ld calls for a malloc", seen);
    }
    BackingTrackPlayer p;
    p.prepare(kSr);
    p.setAudio(sineTrack(5.0, 220.0, 0.3f));
    std::atomic<bool> stop{false};
    std::atomic<long> allocs{-1};
    std::atomic<long> blocks{0};
    std::thread audio([&] {
        std::vector<float> l(1024, 0.0f), r(1024, 0.0f), side(1024, 0.0f);
        // warm-up outside the guard (first touch of the unit's own lazily-made state is a prepare-time cost)
        for (int i = 0; i < 8; ++i) p.process(l.data(), r.data(), 512, side.data(), side.data());
        g_guard_begin();
        uint32_t k = 0;
        while (!stop.load()) {
            const uint32_t n = (k % 3 == 0) ? 1024 : (k % 3 == 1 ? 256 : 97);
            for (uint32_t i = 0; i < n; ++i) side[i] = (k / 40) % 2 ? 0.2f * std::sin(0.03f * float(i)) : 0.0f;
            p.process(l.data(), r.data(), n, side.data(), side.data());
            ++k;
            blocks.fetch_add(1);
            std::this_thread::yield();
        }
        allocs.store(g_guard_end());
    });
    auto waitBlocks = [&](long n) { const long t = blocks.load() + n; while (blocks.load() < t) std::this_thread::yield(); };
    p.play();                    waitBlocks(60);
    p.seek(2.0);                 waitBlocks(30);
    p.setLoop(true, 2.0, 2.4);   waitBlocks(120);
    p.setSpeed(0.6f);            waitBlocks(80);
    p.setSemitones(-3.0f);       waitBlocks(80);
    p.seek(2.1);                 waitBlocks(40);
    p.setDuck(true);             waitBlocks(120);
    p.pause();                   waitBlocks(20);
    p.setCountIn(1, 240.0);
    p.play();                    waitBlocks(80);
    p.setSpeed(1.0f); p.setSemitones(0.0f); waitBlocks(40);
    p.setAudio(sineTrack(3.0, 330.0, 0.3f)); // concurrent hand-over: the old buffer comes back via the ring
    p.play();                    waitBlocks(80);
    p.collect();
    p.setAudio(constTrack(2.0, 0.1f), 0.5);
    p.play();                    waitBlocks(60);
    p.clear();                   waitBlocks(30);
    stop.store(true);
    audio.join();
    p.collect();
    std::printf("  %ld blocks processed\n", blocks.load());
    CHECK(allocs.load() == 0, "%ld heap calls on the audio thread", allocs.load());
}

static void testLibrary(const fs::path& tmp) {
    std::printf("library + API\n");
    const fs::path lib = tmp / "Backing Tracks", src = tmp / "src";
    fs::create_directories(src);
#ifdef __APPLE__
    const std::string funk = "Funk Jam.m4a";
#else
    const std::string funk = "Funk Jam.flac";
#endif
    const std::string a = (src / "Slow Blues in A.wav").string(), b = (src / funk).string();
    writeWav16(a, 44100, 2, [](int64_t i, int) { return 0.3f * static_cast<float>(std::sin(i * 0.02)); }, 44100 * 4);
#ifdef __APPLE__
    writeExt(b, kAudioFileM4AType, true, 44100, [](int64_t i, int) { return 0.2f * static_cast<float>(std::sin(i * 0.03)); }, 44100 * 2);
#else
    fs::copy_file(kFixtures / "tone330.flac", b, fs::copy_options::overwrite_existing);
#endif
    // a file dropped straight into the folder by the user joins on start
    fs::create_directories(lib);
    writeWav16((lib / "Already Here.wav").string(), 48000, 1, [](int64_t i, int) { return 0.1f * static_cast<float>(std::sin(i * 0.01)); }, 48000);

    BackingTracks bt;
    bt.start(kSr, lib.string());
    CHECK(bt.list().size() == 1 && bt.list()[0].title == "Already Here", "scan");
    std::map<std::string, std::string> qs;
    auto api = [&](const std::string& path, std::map<std::string, std::string> q, const std::string& body = {}) {
        return bt.handleApi(path, [&](const char* k) { auto it = q.find(k); return it == q.end() ? std::string() : it->second; }, body);
    };
    auto js = [](std::string p) { std::string o; for (char c : p) { if (c == '\\' || c == '"') o += '\\'; o += c; } return o; }; // Windows paths
    std::string r = api("/api/backing/import", {}, "{\"paths\":[\"" + js(a) + "\",\"" + js(b) + "\",\"" + js((src / "nope.txt").string()) + "\"]}");
    CHECK(r.find("\"ok\":true") != std::string::npos && r.find("Slow Blues in A") != std::string::npos && (r.find("Funk Jam") != std::string::npos || r.find("Test Tone") != std::string::npos), "%s", r.c_str());
    CHECK(r.find("nope.txt") != std::string::npos, "failure reported");
    CHECK(fs::exists(lib / "Slow Blues in A.wav") && fs::exists(lib / funk), "copied into the library");
    CHECK(fs::exists(a), "the original stays where it was");
    // same file again → no duplicate
    BackingTracks::Entry e;
    std::string err;
    CHECK(bt.importFile(a, e, err) && bt.list().size() == 3, "duplicate import (%zu)", bt.list().size());
    // a different file with the same name gets its own copy
    writeWav16((src / "x.wav").string(), 44100, 2, [](int64_t, int) { return 0.0f; }, 4410);
    fs::create_directories(src / "other");
    writeWav16((src / "other" / "Slow Blues in A.wav").string(), 44100, 2, [](int64_t i, int) { return 0.2f * static_cast<float>(std::sin(i * 0.05)); }, 44100);
    CHECK(bt.importFile((src / "other" / "Slow Blues in A.wav").string(), e, err) && fs::exists(lib / "Slow Blues in A (2).wav"), "name clash");
    CHECK(bt.remove(e.id) && !fs::exists(lib / "Slow Blues in A (2).wav"), "delete removes the copy");
    // a text file that only looks like audio
    { std::ofstream(src / "fake.mp3") << "nope"; }
    CHECK(!bt.importFile((src / "fake.mp3").string(), e, err) && !err.empty(), "fake mp3 refused");

    std::string blues;
    for (const auto& x : bt.list()) if (x.title == "Slow Blues in A") blues = x.id;
    CHECK(!blues.empty(), "found");
    r = api("/api/backing", {{"action", "load"}, {"id", blues}});
    CHECK(bt.waitLoaded(5000), "decode finished");
    r = api("/api/backing", {});
    CHECK(r.find("\"loaded\":true") != std::string::npos && r.find("Slow Blues in A") != std::string::npos, "%s", r.c_str());
    CHECK(std::fabs(bt.player().duration() - 4.0) < 0.01, "duration %.3f", bt.player().duration());
    CHECK(bt.list()[0].id == blues, "last used first");
    r = api("/api/backing/peaks", {{"bins", "100"}});
    CHECK(r.find("\"bins\":100") != std::string::npos && r.find("\"peak\":[") != std::string::npos, "peaks");
    r = api("/api/backing", {{"action", "seek"}, {"sec", "1.5"}, {"volume", "0.7"}, {"speed", "0.8"}, {"semitones", "-2"}, {"loop", "1"}, {"a", "1"}, {"b", "2.5"}, {"countin", "4"}, {"bpm", "96"}, {"duck", "1"}});
    CHECK(r.find("\"position\":1.5") != std::string::npos, "seek in state: %s", r.c_str());
    CHECK(std::fabs(bt.player().volume() - 0.7f) < 1e-6f && std::fabs(bt.player().speed() - 0.8f) < 1e-6f && bt.player().semitones() == -2.0f, "settings");
    CHECK(bt.player().loopOn() && bt.player().loopA() == 1.0 && bt.player().loopB() == 2.5, "loop");
    CHECK(bt.player().countInBeats() == 4 && bt.player().countInBpm() == 96.0 && bt.player().duckOn(), "count-in / duck");
    r = api("/api/backing", {{"action", "play"}});
    CHECK(bt.player().playing(), "play");
    CHECK(bt.telemetryJson().find("\"playing\":true") != std::string::npos, "telemetry");
    CHECK(api("/api/backing/rename", {{"id", blues}, {"title", "  12-Bar Blues  "}}).find("\"ok\":true") != std::string::npos, "rename");
    CHECK(bt.list()[0].title == "12-Bar Blues", "renamed '%s'", bt.list()[0].title.c_str());
    // the index survives a restart
    bt.stop();
    {
        BackingTracks again;
        again.start(kSr, lib.string());
        const auto l2 = again.list();
        CHECK(l2.size() == 3 && l2[0].title == "12-Bar Blues", "persisted (%zu)", l2.size());
        // a rate change re-decodes the loaded track at the new rate
        std::string e2;
        again.load(l2[0].id, false, 2.0, e2);
        again.waitLoaded(5000);
        again.setSampleRate(44100.0);
        again.waitLoaded(5000);
        CHECK(std::fabs(again.player().duration() - 4.0) < 0.01 && again.player().sampleRate() == 44100.0, "re-prepared at 44.1 kHz");
        CHECK(std::fabs(again.player().position() - 2.0) < 0.01, "kept its place %.3f", again.player().position());
    }
    BackingTracks bt2;
    bt2.start(kSr, lib.string());
    std::string id0 = bt2.list()[0].id;
    CHECK(bt2.handleApi("/api/backing/delete", [&](const char* k) { return std::string(k) == "id" ? id0 : std::string(); }, "").find("\"ok\":true") != std::string::npos, "delete via API");
    CHECK(bt2.list().size() == 2 && !fs::exists(lib / "Slow Blues in A.wav"), "deleted");
    // YouTube host page is a real page with the IFrame API
    CHECK(BackingTracks::youtubeHostHtml().find("https://www.youtube.com/iframe_api") != std::string::npos, "yt host");
}

int main() {
#ifndef _WIN32
    g_guard_begin = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "rtguard_begin"));
    g_guard_end = reinterpret_cast<long (*)()>(dlsym(RTLD_DEFAULT, "rtguard_end"));
#endif
    const fs::path tmp = fs::temp_directory_path() / ("hr-backing-test-" + std::to_string(RIG_GETPID()));
    fs::create_directories(tmp);
    testDecode(tmp);
    testTransport();
    testMix();
    testTimePitch();
    testCountIn();
    testNoHeapOnAudioThread();
    testLibrary(tmp);
    std::error_code ec;
    fs::remove_all(tmp, ec);
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("all backing-track checks passed\n");
    return 0;
}
