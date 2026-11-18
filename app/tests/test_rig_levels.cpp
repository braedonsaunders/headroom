// Rig levels without automatic levelling (no loudness match, no glide, no cache), through the real engine:
//  1. a rig loads at the level its own knobs give (the same as an offline render of the file) and stays there:
//     no change over 5 s of real-time playing;
//  2. an edit changes the level by exactly what the edit does (a knob: its own dB; removing a unity EQ: nothing),
//     and nothing moves afterwards; /api/rack reports no background loudness state;
//  3. the level round-trips through save / load (the knob value and the rendered level);
//  4. one-time migration of a user rig the old engine levelled: the gain the old loudness cache last gave it is
//     folded into its own level knob and written into the file once (backup, ledger, /api/diag); loading it again
//     changes nothing; the cache is backed up and never changed; a rig with no level knob is left as it was;
//  5. the migration of a rig storing a non-default SSL Output (a knob that did nothing when the cache was measured,
//     and now adds +7 dB): the folded correction is the cached gain minus that, so it plays exactly where the old
//     engine had it (no jump).
//
//   * engine: RealtimeAudioServer in hosted mode (no CoreAudio device; this test is the host), HTTP on an ephemeral
//     loopback port, HEADROOM_DATA_DIR = a temp folder (the user's files are never read or written);
//   * rigs: cheap units only (studio EQ, Amp-Matched Cab), so the paced real-time render keeps up on a busy machine,
//     and no reverb in the rigs whose level is checked for steadiness (a tail building up under a held chord is the
//     rig's own sound, not a level change, but it would read as one here).
#include "../src/audio/RealtimeAudioServer.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr double kSr = 48000.0;
constexpr uint32_t kBlock = 128;
int g_failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}
namespace fs = std::filesystem;
namespace lf = rig::levelfold;

std::string api_token;
std::string httpGet(uint16_t port, const std::string& path) {
    const int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) return {};
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { ::close(fd); return {}; }
    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nX-Headroom-Token: " + api_token + "\r\nConnection: close\r\n\r\n";
    ::send(fd, req.data(), req.size(), 0);
    std::string resp;
    char buf[16384];
    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    const auto body = resp.find("\r\n\r\n");
    return body == std::string::npos ? std::string() : resp.substr(body + 4);
}
nlohmann::json getJson(uint16_t port, const std::string& path) { return nlohmann::json::parse(httpGet(port, path), nullptr, false); }

std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}
void writeFile(const std::string& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; }

std::string rigXml(const std::string& name, const std::vector<std::pair<int, std::string>>& units) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<headroom-preset version=\"1\">\n<headroom-info><soundinfo><properties><name>" + name +
                    "</name><author>test</author></properties></soundinfo></headroom-info>\n<headroom-rig>\n<metadata preset-name=\"" + name +
                    "\" preset-author=\"test\"/>\n<rack>\n<non-fix-components>\n";
    for (const auto& [t, params] : units)
        s += "  <component id=\"" + std::to_string(t) + "\">\n    <component-audio version=\"1\">\n      <parameters num-parameters=\"2\">\n"
             "        <parameter id=\"Pwr\" value=\"1\"/>\n" + params + "      </parameters>\n    </component-audio>\n  </component>\n";
    return s + "</non-fix-components>\n</rack>\n<realtimecontroldb>\n<remotecontroldb>\n<component-event-listeners>\n</component-event-listeners>\n"
               "</remotecontroldb>\n</realtimecontroldb>\n</headroom-rig>\n</headroom-preset>\n";
}
std::string par(const char* id, float v) {
    char b[96];
    std::snprintf(b, sizeof b, "        <parameter id=\"%s\" value=\"%g\"/>\n", id, static_cast<double>(v));
    return b;
}

// The input: a steady three-note chord (the level of a sustained chord is flat, so any glide shows). Its common
// period (60 Hz: 800 samples) fits a 0.25 s window exactly 15 times, so every window reads the same level.
float chord(uint64_t i) {
    const double t = static_cast<double>(i) / kSr;
    return static_cast<float>(0.03 * (std::sin(2 * M_PI * 120 * t) + std::sin(2 * M_PI * 180 * t) + std::sin(2 * M_PI * 240 * t)));
}

// Plays the engine in real time (paced by the wall clock, like a host) for `secs`; RMS (dB) per 0.25 s window.
std::vector<double> play(rig::audio::RealtimeAudioServer& srv, double secs, uint64_t& clock) {
    std::vector<float> in(kBlock), l(kBlock), r(kBlock);
    const std::size_t win = static_cast<std::size_t>(kSr / 4);
    std::vector<double> rms;
    double e = 0;
    std::size_t n = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t blocks = static_cast<uint64_t>(secs * kSr / kBlock);
    for (uint64_t b = 0; b < blocks; ++b) {
        for (uint32_t i = 0; i < kBlock; ++i) in[i] = chord(clock++);
        srv.renderHost(in.data(), in.data(), l.data(), r.data(), kBlock);
        for (uint32_t i = 0; i < kBlock; ++i) {
            e += 0.5 * (double(l[i]) * l[i] + double(r[i]) * r[i]);
            if (++n == win) { rms.push_back(10.0 * std::log10(std::max(1e-30, e / double(win)))); e = 0; n = 0; }
        }
        const auto due = t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>((b + 1) * kBlock / kSr));
        if (due > std::chrono::steady_clock::now()) std::this_thread::sleep_until(due);
    }
    return rms;
}
double meanOf(const std::vector<double>& v, std::size_t from) {
    double s = 0;
    for (std::size_t i = from; i < v.size(); ++i) s += v[i];
    return s / double(std::max<std::size_t>(1, v.size() - from));
}
double spreadOf(const std::vector<double>& v, std::size_t from) {
    if (v.size() <= from) return 1e9;
    const auto [lo, hi] = std::minmax_element(v.begin() + static_cast<std::ptrdiff_t>(from), v.end());
    if (*hi - *lo >= 0.05) { // show the windows when a check is about to fail
        std::printf("    windows (dB):");
        for (std::size_t i = from; i < v.size(); ++i) std::printf(" %.2f", v[i]);
        std::printf("\n");
    }
    return *hi - *lo;
}

// The same rig rendered offline (RackGraph alone, no engine around it), same input: its level in dB.
double offlineDb(const std::string& path) {
    rig::preset::RackPreset p;
    if (!rig::preset::loadPreset(path, p, nullptr)) return -999;
    rig::RackGraph g;
    g.buildFromPreset(p, kSr, 512);
    std::vector<float> in(kBlock), l(kBlock), r(kBlock);
    double e = 0;
    std::size_t n = 0;
    for (uint64_t c = 0, b = 0; b < static_cast<uint64_t>(2.0 * kSr / kBlock); ++b) {
        for (uint32_t i = 0; i < kBlock; ++i) in[i] = chord(c++);
        const float* ins[2] = {in.data(), in.data()};
        float* outs[2] = {l.data(), r.data()};
        rig::AudioBlock blk{};
        blk.in = ins; blk.out = outs; blk.num_channels = 2; blk.num_frames = kBlock; blk.sample_rate = kSr;
        g.process(blk);
        if (b * kBlock >= kSr)
            for (uint32_t i = 0; i < kBlock; ++i) { e += 0.5 * (double(l[i]) * l[i] + double(r[i]) * r[i]); ++n; }
    }
    return 10.0 * std::log10(std::max(1e-30, e / double(n)));
}

int libraryIdx(uint16_t port, const std::string& name) {
    for (int i = 0; i < 200; ++i) {
        const auto j = getJson(port, "/api/library");
        if (j.is_object() && j.contains("presets") && !j.value("loading", false))
            for (const auto& e : j["presets"])
                if (e.value("name", std::string()) == name) return e.value("idx", -1);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return -1;
}
bool loadRig(uint16_t port, const std::string& name) {
    const int idx = libraryIdx(port, name);
    if (idx < 0) return false;
    const auto r = getJson(port, "/api/preset/load?idx=" + std::to_string(idx));
    return r.is_object() && r.value("preset_name", std::string()) == name;
}
float slotVal(const nlohmann::json& rack, int table_id, const std::string& pid) {
    for (const auto& s : rack.value("slots", nlohmann::json::array()))
        if (s.value("table_id", 0) == table_id)
            for (const auto& p : s.value("params", nlohmann::json::array()))
                if (p.value("id", std::string()) == pid) return p.value("val", -1.0f);
    return -1.0f;
}
int slotOf(const nlohmann::json& rack, int table_id) {
    for (const auto& s : rack.value("slots", nlohmann::json::array()))
        if (s.value("table_id", 0) == table_id) return s.value("slot_idx", -1);
    return -1;
}
} // namespace

int main(int argc, char** argv) {
    ::setenv("HEADROOM_NOISE_FIXED_SEED", "1", 1);
    const std::string root = argc > 1 ? argv[1] : RIG_RESEARCH_ROOT;
    const fs::path tmp = fs::temp_directory_path() / ("headroom-levels-" + std::to_string(::getpid()));
    fs::create_directories(tmp / "User Presets");
    ::setenv("HEADROOM_DATA_DIR", tmp.string().c_str(), 1);
    const std::string users = (tmp / "User Presets").string();

    // user rigs: a plain rig, one the old engine levelled (cache entries below), one with no level knob
    const std::string plain = users + "/Level Plain.rig", migrate = users + "/Migrate Me.rig", noknob = users + "/No Knob.rig";
    writeFile(plain, rigXml("Level Plain", {{25000, par("Vol", 0.5f)}, {156000, par("MV", 0.4f) + par("Cab", 9.0f)}}));
    writeFile(migrate, rigXml("Migrate Me", {{156000, par("MV", 0.5f) + par("Cab", 9.0f)}}));
    writeFile(noknob, rigXml("No Knob", {{33000, ""}}));
    const std::string sslrig = users + "/SSL Hot.rig", sslref = (tmp / "SSL Ref.rig").string();
    writeFile(sslrig, rigXml("SSL Hot", {{156000, par("MV", 0.5f) + par("Cab", 9.0f)}, {121000, par("O14", 0.695f)}}));
    writeFile(sslref, rigXml("SSL Ref", {{156000, par("MV", 0.5f) + par("Cab", 9.0f)}, {121000, par("O14", 0.5f)}})); // as the old engine heard it
    const std::string migrate_orig = readFile(migrate), noknob_orig = readFile(noknob);
    // old cache: two lines for Migrate Me (the newest wins: 0.5 = -6.02 dB), one for No Knob, one for an unrelated rig
    const std::string cache = (tmp / "loudness-cache-exact.txt").string();
    writeFile(cache, migrate + "|lc1|0123456789abcdef|0000000000000000|48000|lvl|dsp|loud2\t0.25\n" +
                     "/elsewhere/Other.rig|lc1|0000000000000000|0000000000000000|48000|lvl|dsp|loud2\t3.0\n" +
                     migrate + "|lc1|fedcba9876543210|0000000000000000|48000|lvl|dsp|loud2\t0.5\n" +
                     noknob + "|1794157240394976795|48000|dsp8-circuit1|loud2|0b766d3f53dcd62b\t2.0\n" +
                     sslrig + "|lc1|0123456789abcdef|0000000000000000|48000|lvl|dsp|loud2\t0.5\n");
    const std::string cache_orig = readFile(cache);

    rig::audio::RealtimeAudioServer srv;
    std::string err;
    if (!srv.startHosted(root, kSr, &err)) {
        std::fprintf(stderr, "startHosted failed: %s\n", err.c_str());
        fs::remove_all(tmp);
        return 77;
    }
    srv.prepareHost(kSr);
    srv.startHostedEditor();
    srv.startHostedLibrary();
    const uint16_t port = srv.boundHttpPort();
    api_token = srv.apiToken();
    uint64_t clock = 0;

    // ── 1. load: the rig's own level, steady for 5 s ──
    check(loadRig(port, "Level Plain"), "a user rig loads");
    {
        const auto rack = getJson(port, "/api/rack");
        check(rack.is_object() && !rack.contains("loudness_pending") && !rack.contains("loudness_rev"), "/api/rack has no background loudness state");
        const auto w = play(srv, 6.0, clock);
        const double sp = spreadOf(w, 2), lvl = meanOf(w, 2), off = offlineDb(plain);
        char m[200];
        std::snprintf(m, sizeof m, "after a load the level holds for 5 s (spread %.3f dB over 0.5..6 s)", sp);
        check(sp < 0.05, m);
        std::snprintf(m, sizeof m, "the loaded rig plays at its own level: engine %.2f dB, offline render of the file %.2f dB", lvl, off);
        check(std::abs(lvl - off) < 0.25, m);
        const auto diag = getJson(port, "/api/diag");
        check(diag.is_object() && diag["level"].value("automatic_leveling", true) == false && diag["level"]["migrations"].empty(),
              "/api/diag: no automatic levelling, no migration for a rig the old cache never saw");
    }

    // ── 2. edits change exactly what they change, nothing afterwards ──
    {
        auto rack = getJson(port, "/api/rack");
        const int cab = slotOf(rack, 156000), eq = slotOf(rack, 25000);
        const double before = meanOf(play(srv, 1.5, clock), 2);
        httpGet(port, "/api/param?slot=" + std::to_string(cab) + "&id=MV&norm=0.6");
        const auto w = play(srv, 6.0, clock);
        const double want = lf::detail::cab156(0.6) - lf::detail::cab156(0.4), got = meanOf(w, 2) - before;
        char m[200];
        std::snprintf(m, sizeof m, "a cab Volume move changes the level by its own amount: %+.2f dB (knob map %+.2f)", got, want);
        check(std::abs(got - want) < 0.1, m);
        std::snprintf(m, sizeof m, "...and nothing moves for 5 s after it (spread %.3f dB)", spreadOf(w, 2));
        check(spreadOf(w, 2) < 0.05, m);
        const double before2 = meanOf(w, 2);
        httpGet(port, "/api/rack/remove?slot=" + std::to_string(eq)); // structural edit: a unity EQ out of the chain
        const auto w2 = play(srv, 6.0, clock);
        std::snprintf(m, sizeof m, "removing a unity EQ leaves the level where it was: %+.2f dB, spread %.3f dB over 5 s",
                      meanOf(w2, 2) - before2, spreadOf(w2, 2));
        check(std::abs(meanOf(w2, 2) - before2) < 0.1 && spreadOf(w2, 2) < 0.05, m);
        rack = getJson(port, "/api/rack");
        check(std::abs(slotVal(rack, 156000, "MV") - 0.6f) < 1e-4f, "the engine left the cab Volume where it was set");
    }

    // ── 3. save / load ──
    {
        const double lvl = meanOf(play(srv, 1.5, clock), 2);
        const auto s = getJson(port, "/api/preset/save?name=Level%20Saved");
        check(s.is_object() && s.value("ok", false), "the edited rig saves");
        check(loadRig(port, "No Knob") && loadRig(port, "Level Saved"), "another rig, then the saved one, load");
        const auto rack = getJson(port, "/api/rack");
        check(std::abs(slotVal(rack, 156000, "MV") - 0.6f) < 1e-4f, "the cab Volume round-trips (0.6)");
        const double back = meanOf(play(srv, 2.0, clock), 2);
        char m[160];
        std::snprintf(m, sizeof m, "the level round-trips: %.2f dB saved, %.2f dB loaded", lvl, back);
        check(std::abs(back - lvl) < 0.05, m);
    }

    // ── 4. one-time migration from the old loudness cache ──
    {
        const std::string backup = (tmp / "Level Migration Backups" / "Migrate Me.rig").string();
        check(loadRig(port, "Migrate Me"), "a rig the old engine levelled loads");
        rig::preset::RackPreset p;
        rig::preset::loadPreset(migrate, p, nullptr);
        const float mv = lf::paramValue(p.root_rack.children.at(0), "MV");
        const double want = 20.0 * std::log10(0.5);
        char m[220];
        std::snprintf(m, sizeof m, "the cached gain (newest line, %.2f dB) went into the cab Volume: MV 0.5 -> %.4f (%+.2f dB)", want, mv,
                      lf::detail::cab156(mv) - lf::detail::cab156(0.5));
        check(std::abs(lf::detail::cab156(mv) - lf::detail::cab156(0.5) - want) < 0.01, m);
        check(readFile(backup) == migrate_orig, "the original rig file was backed up first");
        check(readFile(cache) == cache_orig && readFile(cache + ".pre-level-migration.bak") == cache_orig, "the cache is backed up and unchanged");
        const std::string ledger = readFile((tmp / "level-migration.txt").string());
        check(ledger.find(migrate + "\tmigrated") != std::string::npos, "the ledger records the migration");
        const auto diag = getJson(port, "/api/diag");
        bool logged = false;
        for (const auto& e : diag["level"]["migrations"])
            if (e.value("path", std::string()) == migrate && e.value("status", std::string()) == "migrated" &&
                std::abs(e.value("applied_db", 0.0) - want) < 0.01 && e["moves"].size() == 1)
                logged = true;
        check(logged, "/api/diag level.migrations logs it (status, applied dB, the knob moved)");
        // it plays as loud as the old engine played it: the original file's own level + the cached gain
        const double now = meanOf(play(srv, 2.0, clock), 2), orig = offlineDb(backup);
        std::snprintf(m, sizeof m, "it plays where the old engine had it: %.2f dB = original %.2f %+.2f dB", now, orig, want);
        check(std::abs(now - (orig + want)) < 0.25, m);

        const std::string after = readFile(migrate);
        check(loadRig(port, "No Knob") && loadRig(port, "Migrate Me"), "the rig loads again");
        check(readFile(migrate) == after, "a second load does not fold the cached gain again");
        check(readFile(noknob) == noknob_orig && readFile((tmp / "level-migration.txt").string()).find(noknob + "\tno_level_knob") != std::string::npos,
              "a rig with no level knob is left as it was (recorded as no_level_knob)");
    }

    // ── 5. migration of a rig whose SSL Output was dead when the cache was measured ──
    {
        check(loadRig(port, "SSL Hot"), "a rig with SSL Output 0.695 (dead then, +7 dB now) loads");
        rig::preset::RackPreset p;
        rig::preset::loadPreset(sslrig, p, nullptr);
        const float o14 = lf::paramValue(p.root_rack.children.at(1), "O14");
        const double cache_db = 20.0 * std::log10(0.5), live = rig::dsp::trimlaw::ssl(0.695);
        char m[260];
        std::snprintf(m, sizeof m, "the fold took the cached %.2f dB minus the knob's now-live %+.2f dB: SSL Output 0.695 -> %.4f (%+.2f dB)",
                      cache_db, live, o14, rig::dsp::trimlaw::ssl(o14));
        check(std::abs(rig::dsp::trimlaw::ssl(o14) - cache_db) < 0.01, m);
        const auto diag = getJson(port, "/api/diag");
        bool logged = false;
        for (const auto& e : diag["level"]["migrations"])
            if (e.value("path", std::string()) == sslrig && e.value("status", std::string()) == "migrated" &&
                std::abs(e.value("live_knob_db", 0.0) - live) < 0.01 && std::abs(e.value("change_db", 9.0)) < 0.01)
                logged = true;
        check(logged, "/api/diag logs the knob's live dB and a level change of 0");
        const double now = offlineDb(sslrig), was = offlineDb(sslref) + cache_db;
        std::snprintf(m, sizeof m, "it plays where the old engine had it (Output dead, cached gain): %.2f dB vs %.2f dB", now, was);
        check(std::abs(now - was) < 0.05, m);
    }

    srv.stopHosted();
    fs::remove_all(tmp);
    std::printf("%s\n", g_failures ? "FAILED" : "all rig level checks passed");
    return g_failures ? 1 : 0;
}
