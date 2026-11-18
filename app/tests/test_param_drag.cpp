// Control-drag regression: render the engine in a paced real-time thread (48 kHz, 128-sample callbacks) while
// another thread drags controls through the real HTTP routes exactly like the UI does (one request in flight per
// control, latest value wins), and check that dragging never produces clicks, dropouts or deadline misses.
//
//   * engine: RealtimeAudioServer in hosted mode (no CoreAudio device; the test is the "host"), HTTP on an
//     ephemeral loopback port (never 17770), data dir redirected to a temp folder (no writes to the user's files);
//   * input: a sustained three-note chord, so the output envelope is steady and any dip or spike is an artifact;
//   * controls: an amp knob and a pedal knob (/api/param), macro 1 (/api/macro), the top-bar IN / OUT knobs
//     (/api/audio/set handler, which hosted mode refuses over HTTP, so it is called directly) and
//     /api/transport?action=gain;
//   * metrics per control: callback max / mean time, deadline misses (render longer than a block period),
//     requests handled per second, clicks and glitched periods (see analyse());
//   * also: knob routes answer small, and IN / OUT drags save the settings once (debounced), not per tick;
//   * with rtguard preloaded (CTest), heap calls and mutex locks on the audio thread are counted as well.
// Exit 0 = pass. HEADROOM_DRAG_REPORT=1 prints the numbers without failing (used for before/after measurements).
#include "../src/audio/RealtimeAudioServer.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <numbers>
#include <string>
#include <thread>
#include <vector>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#endif

namespace {
constexpr double kSr = 48000.0;
constexpr uint32_t kBlock = 128;

using clk = std::chrono::steady_clock;

std::string api_token;
std::string httpGet(uint16_t port, const std::string& path, const std::string& token = api_token,
                    const std::string& host = "", const std::string& origin = "", bool raw = false) {
    const int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) return {};
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { ::close(fd); return {}; }
    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + (host.empty() ? std::to_string(port) : host) + "\r\nOrigin: " + origin + "\r\nX-Headroom-Token: " + token + "\r\nConnection: close\r\n\r\n";
    ::send(fd, req.data(), req.size(), 0);
    std::string resp;
    char buf[16384];
    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    if (raw) return resp;
    const auto body = resp.find("\r\n\r\n");
    return body == std::string::npos ? std::string() : resp.substr(body + 4);
}

// Optional rtguard counters (DYLD_INSERT_LIBRARIES / LD_PRELOAD); absent = not counted.
using BeginFn = void (*)();
using EndFn = long (*)();
BeginFn g_rt_begin = nullptr;
EndFn g_rt_end = nullptr, g_rt_locks = nullptr;

void makeRealtime() {
#ifdef __APPLE__
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const double ns_to_abs = double(tb.denom) / double(tb.numer);
    const double period_ns = 1e9 * kBlock / kSr;
    thread_time_constraint_policy_data_t pol;
    pol.period = static_cast<uint32_t>(period_ns * ns_to_abs);
    pol.computation = static_cast<uint32_t>(0.5 * period_ns * ns_to_abs);
    pol.constraint = static_cast<uint32_t>(period_ns * ns_to_abs);
    pol.preemptible = 1;
    thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, reinterpret_cast<thread_policy_t>(&pol),
                      THREAD_TIME_CONSTRAINT_POLICY_COUNT);
#endif
}

struct Phase {
    std::string name;
    std::vector<float> out;          // left channel
    double cb_max_us = 0, cb_sum_us = 0;
    // misses: the render itself used more CPU than a block period; preempted: it overran in wall time only (the OS ran
    // something else on this thread's core: other processes share the machine); late: finished after it was due.
    long blocks = 0, misses = 0, preempted = 0, late = 0;
    long requests = 0;
    double seconds = 0;
    long heap = -1, locks = -1;
    double req_max_ms = 0;
};

// Shared between the audio thread and the driver.
std::atomic<bool> g_run{true};
std::atomic<int> g_phase{-1}; // index into phases being recorded, -1 = warm-up / not recording
std::vector<Phase>* g_phases = nullptr;

void audioThread(rig::audio::RealtimeAudioServer* srv) {
    makeRealtime();
    std::vector<float> inL(kBlock), inR(kBlock), outL(kBlock), outR(kBlock);
    uint64_t n = 0;
    const double f[3] = {120.0, 240.0, 360.0}; // periodic in 400 samples (see analyse)
    const float amp[3] = {0.2f, 0.1f, 0.06f};
    const auto period = std::chrono::nanoseconds(static_cast<long long>(1e9 * kBlock / kSr));
    auto next = clk::now();
    int cur = -1;
    while (g_run.load(std::memory_order_relaxed)) {
        const int ph = g_phase.load(std::memory_order_acquire);
        if (ph != cur) {
            if (cur >= 0 && g_rt_end) {
                (*g_phases)[cur].heap = g_rt_end();
                if (g_rt_locks) (*g_phases)[cur].locks = g_rt_locks();
            }
            cur = ph;
            if (cur >= 0 && g_rt_begin) g_rt_begin();
        }
        for (uint32_t i = 0; i < kBlock; ++i, ++n) {
            const double t = double(n) / kSr;
            float s = 0.0f;
            for (int h = 0; h < 3; ++h) s += amp[h] * static_cast<float>(std::sin(2.0 * std::numbers::pi * f[h] * t));
            inL[i] = inR[i] = s;
        }
        timespec c0{}, c1{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c0);
        const auto t0 = clk::now();
        srv->renderHost(inL.data(), inR.data(), outL.data(), outR.data(), kBlock);
        const auto t1 = clk::now();
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c1);
        const auto cpu = std::chrono::nanoseconds((c1.tv_sec - c0.tv_sec) * 1000000000LL + (c1.tv_nsec - c0.tv_nsec));
        if (cur >= 0) {
            auto& p = (*g_phases)[cur];
            const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            p.cb_max_us = std::max(p.cb_max_us, us);
            p.cb_sum_us += us;
            ++p.blocks;
            // Deadline: the render's own work (thread CPU time) must fit in one device period. A wall-time overrun
            // without the CPU time is the OS preempting the thread; it and late wake-ups are reported, not asserted.
            if (cpu > period) ++p.misses;
            else if (t1 - t0 > period) ++p.preempted;
            if (t1 > next + period) ++p.late;
            p.out.insert(p.out.end(), outL.begin(), outL.end());
        }
        next += period;
        const auto now = clk::now();
        if (next < now - 4 * period) next = now; // fell far behind: resync (counted as misses above)
        std::this_thread::sleep_until(next);
    }
    if (cur >= 0 && g_rt_end) {
        (*g_phases)[cur].heap = g_rt_end();
        if (g_rt_locks) (*g_phases)[cur].locks = g_rt_locks();
    }
}

struct Artifacts { long clicks = 0, glitches = 0, glitch_periods = 0; double d2_max = 0, click_max = 0, worst_db = -200; };

// The input is periodic with exactly kPeriod samples, so a steady rig's output is too: y[n] - y[n - P] ~ 0. A slow
// knob sweep changes the waveform a little per period; a click, a state reset or a crossfade from a held sample
// changes it a lot. A period is glitched when its difference (relative to its level) is above kGlitchDb
// (consecutive ones count as one event). The drags
// sweep knobs over 0.15 of their range per second-long pass: without factory cabinet IRs (a brighter, rawer output)
// a 0.4 sweep through a strongly nonlinear stretch reached -17 dB legitimately, while a reset or the old
// held-sample bridge does not depend on sweep speed (-6 to -15 dB at UI request rates). Clicks (short steps that carry little energy): the second difference of the
// period difference, y[n] - y[n - P], above the threshold (1.5x the steady-state maximum second difference of y).
// A knob sweep keeps that difference smooth (a fast high-gain sweep peaks near 0.6-0.9x, anywhere in a block); a
// discontinuity of size d shows up as d (the old per-edit output bridge measured 1.1-3.2x, at block boundaries).
constexpr std::size_t kPeriod = 400; // 120 Hz at 48 kHz
constexpr double kGlitchDb = -20.0;
Artifacts analyse(const std::vector<float>& y, double click_threshold) {
    Artifacts a;
    const std::size_t skip = 4800; // the first 100 ms after a phase switch
    long last_click = -100000;
    auto r = [&](std::size_t i) { return double(y[i]) - double(y[i - kPeriod]); };
    for (std::size_t i = skip + 2; i < y.size(); ++i) {
        a.d2_max = std::max(a.d2_max, std::abs(double(y[i]) - 2.0 * y[i - 1] + y[i - 2]));
        const double c = std::abs(r(i) - 2.0 * r(i - 1) + r(i - 2));
        a.click_max = std::max(a.click_max, c);
        if (click_threshold > 0 && c > click_threshold && long(i) - last_click > 480) { ++a.clicks; last_click = long(i); }
    }
    std::vector<double> dbs;
    for (std::size_t i = skip; i + kPeriod <= y.size(); i += kPeriod) {
        double e = 0, d = 0;
        for (std::size_t k = i; k < i + kPeriod; ++k) {
            e += double(y[k]) * y[k];
            const double r = double(y[k]) - y[k - kPeriod];
            d += r * r;
        }
        if (e < 1e-9) continue;
        dbs.push_back(10.0 * std::log10(std::max(1e-20, d / e)));
    }
    bool in_glitch = false;
    for (const double db : dbs) {
        a.worst_db = std::max(a.worst_db, db);
        const bool g = db > kGlitchDb;
        if (g) ++a.glitch_periods;
        if (g && !in_glitch) ++a.glitches;
        in_glitch = g;
    }
    return a;
}
} // namespace

int main(int argc, char** argv) {
    ::setenv("HEADROOM_NOISE_FIXED_SEED", "1", 1); // reproducible noise streams (CircuitUnit, docs §3.9)
    const bool report_only = std::getenv("HEADROOM_DRAG_REPORT") != nullptr;
    const double drag_s = std::getenv("HEADROOM_DRAG_SECONDS") ? std::atof(std::getenv("HEADROOM_DRAG_SECONDS")) : 3.0;
    const double rate_hz = std::getenv("HEADROOM_DRAG_RATE") ? std::atof(std::getenv("HEADROOM_DRAG_RATE")) : 120.0;
    std::string root = argc > 1 ? argv[1] : RIG_RESEARCH_ROOT;

    g_rt_begin = reinterpret_cast<BeginFn>(dlsym(RTLD_DEFAULT, "rtguard_begin"));
    g_rt_end = reinterpret_cast<EndFn>(dlsym(RTLD_DEFAULT, "rtguard_end"));
    g_rt_locks = reinterpret_cast<EndFn>(dlsym(RTLD_DEFAULT, "rtguard_locks"));

    const auto tmp = std::filesystem::temp_directory_path() / ("headroom-drag-" + std::to_string(::getpid()));
    std::filesystem::create_directories(tmp);
    ::setenv("HEADROOM_DATA_DIR", tmp.string().c_str(), 1);

    rig::audio::RealtimeAudioServer srv;
    std::string err;
    if (!srv.startHosted(root, kSr, &err)) {
        std::fprintf(stderr, "startHosted failed: %s\n", err.c_str());
        std::filesystem::remove_all(tmp);
        return 77;
    }
    srv.prepareHost(kSr);
    srv.startHostedEditor();
    const uint16_t port = srv.boundHttpPort();
    api_token = srv.apiToken();
    const auto status = [&](const std::string& path, const std::string& token, const std::string& host = "", const std::string& origin = "") {
        return httpGet(port, path, token, host, origin, true).substr(0, 12);
    };
    const bool http_ok =
        status("/api/rack", "") == "HTTP/1.1 403" &&
        status("/api/rack", api_token, "1.evil.test") == "HTTP/1.1 403" &&
        status("/api/rack", api_token, "", "https://evil.test") == "HTTP/1.1 403" &&
        status("/api/tempo?bpm=%ZZ&nudge=%A&unused=%", api_token, "", "null") == "HTTP/1.1 200" &&
        status("/api/tempo?token=" + api_token, "") == "HTTP/1.1 200";
    std::fprintf(stderr, "HTTP authentication / Host / Origin / malformed query: %s\n", http_ok ? "PASS" : "FAIL");
    if (!http_ok) { srv.stopHosted(); std::filesystem::remove_all(tmp); return 1; }


    // Wait for the editor services (component bank) and read the default rig.
    std::string rack;
    for (int i = 0; i < 400 && rack.find("\"slots\"") == std::string::npos; ++i) {
        rack = httpGet(port, "/api/rack");
        if (rack.find("\"slots\"") == std::string::npos) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    const auto j = nlohmann::json::parse(rack, nullptr, false);
    int amp_slot = -1;
    std::string amp_param;
    if (j.is_object() && j.contains("slots")) {
        // The amp: a unit with a Presence or Master knob; drag its gain (Gain / Preamp / Drive).
        for (const auto& s : j["slots"]) {
            if (amp_slot >= 0) break;
            bool amp = false;
            std::string gain;
            for (const auto& p : s.value("params", nlohmann::json::array())) {
                const std::string id = p.value("id", std::string()), name = p.value("name", std::string());
                if (name == "Presence" || name == "Master") amp = true;
                if (gain.empty() && (name == "Gain" || name == "Preamp" || name == "Drive")) gain = id;
            }
            if (amp && !gain.empty()) { amp_slot = s.value("slot_idx", -1); amp_param = gain; }
        }
    }
    if (amp_slot < 0) {
        std::fprintf(stderr, "no amp knob found in the default rig\n");
        if (j.is_object() && j.contains("slots"))
            for (const auto& s : j["slots"]) {
                std::fprintf(stderr, "  slot %d %d %s:", s.value("slot_idx", -1), s.value("table_id", 0), s.value("family", std::string()).c_str());
                for (const auto& p : s.value("params", nlohmann::json::array()))
                    std::fprintf(stderr, " %s(%s)", p.value("id", std::string()).c_str(), p.value("name", std::string()).c_str());
                std::fprintf(stderr, "\n");
            }
        srv.stopHosted();
        std::filesystem::remove_all(tmp);
        return 1;
    }
    // A second unit's knob: the first continuous knob of the first unit that is not the amp (a drive pedal here).
    int fx_slot = -1;
    std::string fx_param, fx_name;
    for (const auto& s : j["slots"]) {
        if (fx_slot >= 0) break;
        if (s.value("slot_idx", -1) == amp_slot) continue;
        for (const auto& p : s.value("params", nlohmann::json::array()))
            if (p.value("kind", std::string()) == "continuous") {
                fx_slot = s.value("slot_idx", -1);
                fx_param = p.value("id", std::string());
                fx_name = std::to_string(s.value("table_id", 0)) + " " + p.value("name", std::string());
                break;
            }
    }
    std::printf("rig: %s, amp slot %d param %s, unit slot %d %s, port %u, drag %.1f s at %.0f Hz\n",
                j.value("preset_name", std::string("?")).c_str(), amp_slot, amp_param.c_str(), fx_slot, fx_name.c_str(), port, drag_s, rate_hz);

    // The engine never re-levels a rig by itself (no background loudness match): the output level is the rig's, so
    // every phase below measures edit artifacts only, from the first block on.
    auto rackState = [&] { return nlohmann::json::parse(httpGet(port, "/api/rack"), nullptr, false); };
    {
        const auto r = rackState();
        if (r.is_object() && (r.contains("loudness_pending") || r.contains("loudness_rev"))) {
            std::fprintf(stderr, "/api/rack still reports a background loudness match\n");
            srv.stopHosted();
            std::filesystem::remove_all(tmp);
            return 1;
        }
    }

    // Response sizes: knob routes must answer small (no full-rack JSON / device list per tick).
    bool ok = true;
    {
        const auto r1 = httpGet(port, "/api/param?slot=" + std::to_string(amp_slot) + "&id=" + amp_param + "&norm=0.5");
        const auto r2 = httpGet(port, "/api/macro?col=0&norm=0.5");
        const auto r3 = srv.handleAudioSet("output_gain_db=-6");
        std::printf("response bytes: /api/param %zu, /api/macro %zu, /api/audio/set (gain) %zu\n", r1.size(), r2.size(), r3.size());
        if (r1.size() > 256 || r2.size() > 256 || r3.size() > 256) ok = false;
    }

    struct Ctl { std::string name; std::function<void(float)> send; };
    auto paramCtl = [&](int slot, const std::string& id, const std::string& label) {
        return Ctl{label, [&, slot, id](float v) {
            httpGet(port, "/api/param?slot=" + std::to_string(slot) + "&id=" + id + "&norm=" + std::to_string(0.425f + 0.15f * v));
        }};
    };
    // The routes the UI sends while dragging. Hosted mode refuses /api/audio/set over HTTP (the DAW owns devices),
    // so the top-bar IN / OUT knobs call its handler directly (same code the route runs).
    std::vector<Ctl> ctls = {
        paramCtl(amp_slot, amp_param, "amp knob /api/param"),
        {"macro 1 /api/macro", [&](float v) { httpGet(port, "/api/macro?col=0&norm=" + std::to_string(0.425f + 0.15f * v)); }},
        {"top-bar IN /api/audio/set", [&](float v) { srv.handleAudioSet("rig_input_db=" + std::to_string(-6.0f + 6.0f * v)); }},
        {"top-bar OUT /api/audio/set", [&](float v) { srv.handleAudioSet("output_gain_db=" + std::to_string(-9.0f + 6.0f * v)); }},
        {"IN /api/transport gain", [&](float v) { httpGet(port, "/api/transport?action=gain&val=" + std::to_string(std::pow(10.0f, (-6.0f + 6.0f * v) / 20.0f))); }},
    };
    if (fx_slot >= 0) ctls.push_back(paramCtl(fx_slot, fx_param, "unit knob " + fx_name));

    // HEADROOM_DRAG_ALL=1: also every continuous knob of every unit in the rig, one phase each.
    if (std::getenv("HEADROOM_DRAG_ALL"))
        for (const auto& s : j["slots"])
            for (const auto& p : s.value("params", nlohmann::json::array())) {
                if (p.value("kind", std::string()) != "continuous") continue;
                const int slot = s.value("slot_idx", -1);
                ctls.push_back(paramCtl(slot, p.value("id", std::string()), "s" + std::to_string(slot) + " " +
                                        std::to_string(s.value("table_id", 0)) + " " + p.value("name", std::string())));
            }

    std::vector<Phase> phases;
    phases.resize(ctls.size() + 1);
    phases[0].name = "steady (no edits)";
    for (std::size_t i = 0; i < ctls.size(); ++i) phases[i + 1].name = ctls[i].name;
    for (auto& p : phases) p.out.reserve(static_cast<std::size_t>((drag_s + 1) * kSr));
    g_phases = &phases;

    // Every control starts where its drag will begin (0.5), so no phase opens with a jump from the rig's own value.
    for (auto& c : ctls) c.send(0.5f);

    std::thread audio(audioThread, &srv);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500)); // warm-up: amp / cab state settle

    for (std::size_t pi = 0; pi < phases.size(); ++pi) {
        auto& p = phases[pi];
        g_phase.store(static_cast<int>(pi), std::memory_order_release);
        const auto start = clk::now();
        const auto end = start + std::chrono::duration_cast<clk::duration>(std::chrono::duration<double>(drag_s));
        if (pi == 0) {
            std::this_thread::sleep_until(end);
        } else {
            // UI drag: pointermove at rate_hz, one request in flight per control (queueSend), latest value wins.
            auto next = start;
            const auto period = std::chrono::duration_cast<clk::duration>(std::chrono::duration<double>(1.0 / rate_hz));
            while (clk::now() < end) {
                const double t = std::chrono::duration<double>(clk::now() - start).count();
                // 2 s back-and-forth, starting from the value the previous reset left (0.5): a drag never begins with a jump
                const float v = static_cast<float>(0.5 + 0.5 * std::sin(2.0 * std::numbers::pi * t / 2.0));
                const auto r0 = clk::now();
                ctls[pi - 1].send(v);
                p.req_max_ms = std::max(p.req_max_ms, std::chrono::duration<double, std::milli>(clk::now() - r0).count());
                ++p.requests;
                next += period;
                if (next > clk::now()) std::this_thread::sleep_until(next);
                else next = clk::now();
            }
        }
        p.seconds = std::chrono::duration<double>(clk::now() - start).count();
        // Reset the control between phases (not recorded) so each starts from the same rig state.
        g_phase.store(-1, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(20)); // the audio thread has left the phase
        if (pi > 0) ctls[pi - 1].send(0.5f);
        std::this_thread::sleep_for(std::chrono::milliseconds(pi > 0 ? 600 : 50));
    }
    g_phase.store(-1, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    g_run.store(false);
    audio.join();
    // The IN / OUT drags saved their settings once, ~500 ms after the last move (debounced), not per tick.
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    {
        std::ifstream f(tmp / "audio-settings.txt");
        std::stringstream ss;
        ss << f.rdbuf();
        const bool saved = ss.str().find("output_gain_db=-6") != std::string::npos; // the reset value sent last
        std::printf("debounced settings save: %s\n", saved ? "written" : "MISSING");
        if (!saved) ok = false;
    }
    srv.stopHosted();
    std::filesystem::remove_all(tmp);

    if (const char* dump = std::getenv("HEADROOM_DRAG_DUMP")) // raw float32 mono per phase, for offline analysis
        for (std::size_t i = 0; i < phases.size(); ++i)
            if (FILE* f = std::fopen((std::string(dump) + "/phase" + std::to_string(i) + ".f32").c_str(), "wb")) {
                std::fwrite(phases[i].out.data(), sizeof(float), phases[i].out.size(), f);
                std::fclose(f);
            }
    const auto base = analyse(phases[0].out, 0.0);
    const double thr = 1.5 * base.d2_max;
    const auto base2 = analyse(phases[0].out, thr);
    std::printf("click threshold %.4f (1.5x steady-state d2 max); steady period residual %.1f dB (glitch > %.0f dB)\n\n", thr,
                base2.worst_db, kGlitchDb);
    std::printf("%-30s %9s %9s %7s %5s %5s %8s %10s %7s %8s %9s %8s %9s %6s %6s\n", "phase", "cb max us", "cb avg us", "misses", "preem", "late", "req/s",
                "req max ms", "clicks", "glitchP", "evt/min", "worst dB", "click/thr", "heap", "locks");
    for (const auto& p : phases) {
        const auto a = analyse(p.out, thr);
        const double mins = p.seconds / 60.0;
        const double per_min = mins > 0 ? (a.clicks + a.glitch_periods) / mins : 0;
        std::printf("%-30.30s %9.0f %9.1f %7ld %5ld %5ld %8.1f %10.2f %7ld %8ld %9.0f %8.1f %9.2f %6ld %6ld\n", p.name.c_str(), p.cb_max_us,
                    p.blocks ? p.cb_sum_us / p.blocks : 0.0, p.misses, p.preempted, p.late, p.seconds > 0 ? p.requests / p.seconds : 0.0, p.req_max_ms,
                    a.clicks, a.glitch_periods, per_min, a.worst_db, a.click_max / thr, p.heap, p.locks);
        // On a shared CI runner (HEADROOM_SHARED_RUNNER) other jobs steal the CPU, so deadline misses are reported, not
        // asserted; clicks and glitches in the rendered audio still fail.
        if (a.clicks || a.glitches || (p.misses && !std::getenv("HEADROOM_SHARED_RUNNER"))) ok = false;
        if (p.heap > 0 || p.locks > 0) ok = false;
        // Request throughput is reported, and only a stall is asserted: an amp edit prepares a copy of the amp (~15 ms
        // of HTTP-thread CPU, 50-90 edits/s on an idle machine), but that thread runs at normal priority, so an
        // oversubscribed machine (load 28 on 12 cores measured 7/s) sets the rate, not the engine. The deterministic
        // server checks are the response sizes and the debounced save above.
        if (&p != &phases[0] && p.requests / std::max(1e-9, p.seconds) < 2.0) ok = false;
    }
    std::printf("\n%s\n", ok ? "PASS" : "FAIL");
    return report_only || ok ? 0 : 1;
}
