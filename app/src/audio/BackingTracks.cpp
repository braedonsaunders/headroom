#include "BackingTracks.hpp"

#include "../library/Library.hpp"
#include "json.hpp"
#include "../platform/Socket.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace rig::audio {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr const char* kIndexFile = ".headroom-backing.json";

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool audioExt(const fs::path& p) {
    static const char* kExt[] = {".wav", ".wave", ".aif", ".aiff", ".aifc", ".mp3", ".m4a", ".aac", ".mp4", ".m4b",
                                 ".caf", ".flac", ".alac", ".ogg", ".opus", ".mp2", ".ac3", ".snd", ".au", ".w64", ".rf64"};
    const std::string e = lower(p.extension().string());
    for (const char* k : kExt) if (e == k) return true;
    return false;
}

std::string makeId() {
    static std::atomic<uint32_t> seq{0};
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    char b[40];
    std::snprintf(b, sizeof b, "bt%llx%x", static_cast<unsigned long long>(ns / 1000), seq.fetch_add(1) & 0xfff);
    return b;
}

std::string dumps(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

double num(const std::string& s, double def) {
    if (s.empty()) return def;
    try { const double v = std::stod(s); return std::isfinite(v) ? v : def; } catch (...) { return def; }
}

std::string trimTitle(std::string s) {
    for (auto& c : s) if (static_cast<unsigned char>(c) < 0x20) c = ' ';
    const auto a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
    if (a == std::string::npos) return {};
    s = s.substr(a, b - a + 1);
    if (s.size() > 200) s.resize(200);
    return s;
}

} // namespace

BackingTracks::~BackingTracks() { stop(); }

std::string BackingTracks::defaultDir() {
    if (const char* env = std::getenv("HEADROOM_BACKING_DIR"); env && *env) return env;
    return library::appSupportDir() + "/Backing Tracks";
}

void BackingTracks::start(double sample_rate, const std::string& dir) {
    dir_ = dir;
    std::error_code ec;
    fs::create_directories(dir_, ec);
    player_.prepare(sample_rate);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        readIndex();
        scanFolderLocked();
        writeIndexLocked();
    }
    started_ = true;
}

void BackingTracks::stop() {
    joinLoader();
}

void BackingTracks::joinLoader() {
    cancel_.store(true);
    if (loader_.joinable()) loader_.join();
    cancel_.store(false);
}

void BackingTracks::setSampleRate(double sample_rate) {
    if (!started_) return;
    joinLoader();
    Entry cur;
    double pos = 0.0;
    bool was_playing = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        cur = current_;
        pos = player_.position();
        was_playing = player_.playing();
        player_.clear();
        player_.collect();
        player_.prepare(sample_rate);
        loading_.store(false);
    }
    if (!cur.id.empty()) startDecode(cur, was_playing, pos);
}

// ───────────── library ─────────────

void BackingTracks::readIndex() {
    entries_.clear();
    std::ifstream f(dir_ + "/" + kIndexFile);
    if (!f) return;
    try {
        json j = json::parse(f);
        for (const auto& t : j.value("tracks", json::array())) {
            Entry e;
            e.id = t.value("id", "");
            e.file = t.value("file", "");
            if (e.id.empty() || e.file.empty() || e.file.find('/') != std::string::npos) continue;
            e.title = t.value("title", "");
            e.artist = t.value("artist", "");
            e.format = t.value("format", "");
            e.original = t.value("original", "");
            e.duration = t.value("duration", 0.0);
            e.added = t.value("added", int64_t{0});
            e.last_used = t.value("last_used", int64_t{0});
            e.size = t.value("size", uint64_t{0});
            entries_.push_back(std::move(e));
        }
    } catch (...) {
        entries_.clear();
    }
}

void BackingTracks::writeIndexLocked() {
    json arr = json::array();
    for (const auto& e : entries_)
        arr.push_back({{"id", e.id}, {"file", e.file}, {"title", e.title}, {"artist", e.artist}, {"format", e.format},
                       {"original", e.original}, {"duration", e.duration}, {"added", e.added}, {"last_used", e.last_used}, {"size", e.size}});
    const std::string tmp = dir_ + "/" + kIndexFile + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return;
        f << dumps(json{{"version", 1}, {"tracks", arr}});
    }
    std::error_code ec;
    fs::rename(tmp, dir_ + "/" + kIndexFile, ec);
}

// Files the user put in the folder by hand join the library; entries whose file is gone leave it.
void BackingTracks::scanFolderLocked() {
    std::error_code ec;
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return !fs::is_regular_file(dir_ + "/" + e.file, ec); }),
                   entries_.end());
    for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path p = it->path();
        const std::string name = p.filename().string();
        if (name.empty() || name[0] == '.' || !it->is_regular_file(ec) || !audioExt(p)) continue;
        if (std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.file == name; })) continue;
        BackingFileInfo info;
        if (!probeBackingFile(p.string(), info)) continue;
        Entry e;
        e.id = makeId();
        e.file = name;
        e.original = name;
        e.title = info.title.empty() ? p.stem().string() : info.title;
        e.artist = info.artist;
        e.format = info.format;
        e.duration = info.seconds;
        e.size = static_cast<uint64_t>(fs::file_size(p, ec));
#ifdef _WIN32
        struct _stat64 st {};
        e.added = ::_wstat64(p.c_str(), &st) == 0 ? static_cast<int64_t>(st.st_mtime) * 1000 : nowMs();
#else
        struct stat st {};
        e.added = ::stat(p.c_str(), &st) == 0 ? static_cast<int64_t>(st.st_mtime) * 1000 : nowMs();
#endif
        entries_.push_back(std::move(e));
    }
}

std::vector<BackingTracks::Entry> BackingTracks::list() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<Entry> v = entries_;
    std::stable_sort(v.begin(), v.end(), [](const Entry& a, const Entry& b) {
        const int64_t ka = std::max(a.last_used, a.added), kb = std::max(b.last_used, b.added);
        return ka != kb ? ka > kb : a.title < b.title;
    });
    return v;
}

bool BackingTracks::importFile(const std::string& path, Entry& out, std::string& err) {
    std::error_code ec;
    const fs::path src(path);
    if (!fs::is_regular_file(src, ec)) { err = "Not a file"; return false; }
    BackingFileInfo info;
    if (!probeBackingFile(path, info, &err)) return false;
    const uint64_t size = static_cast<uint64_t>(fs::file_size(src, ec));
    const std::string fname = src.filename().string();
    std::lock_guard<std::mutex> lk(mtx_);
    // Same file again (same name and size): hand back the track that's already there.
    for (const auto& e : entries_)
        if (e.original == fname && e.size == size) { out = e; return true; }
    const bool inside = fs::equivalent(src.parent_path(), fs::path(dir_), ec);
    std::string dest_name = fname;
    if (!inside) {
        const std::string stem = src.stem().string(), ext = src.extension().string();
        for (int n = 2; fs::exists(dir_ + "/" + dest_name, ec); ++n) dest_name = stem + " (" + std::to_string(n) + ")" + ext;
        fs::copy_file(src, dir_ + "/" + dest_name, fs::copy_options::none, ec);
        if (ec) { err = "Couldn't copy the file: " + ec.message(); return false; }
    }
    Entry e;
    e.id = makeId();
    e.file = dest_name;
    e.original = fname;
    e.title = trimTitle(info.title.empty() ? src.stem().string() : info.title);
    if (e.title.empty()) e.title = src.stem().string();
    e.artist = trimTitle(info.artist);
    e.format = info.format;
    e.duration = info.seconds;
    e.size = size;
    e.added = nowMs();
    entries_.push_back(e);
    writeIndexLocked();
    out = e;
    return true;
}

bool BackingTracks::rename(const std::string& id, const std::string& title) {
    const std::string t = trimTitle(title);
    if (t.empty()) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& e : entries_)
        if (e.id == id) {
            e.title = t;
            if (current_.id == id) current_.title = t;
            writeIndexLocked();
            return true;
        }
    return false;
}

bool BackingTracks::remove(const std::string& id) {
    bool cancel_load = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        cancel_load = loading_.load() && loading_id_ == id;
    }
    if (cancel_load) {
        joinLoader();
        std::lock_guard<std::mutex> lk(mtx_);
        loading_.store(false);
        loading_id_.clear();
    }
    std::lock_guard<std::mutex> lk(mtx_);
    const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.id == id; });
    if (it == entries_.end()) return false;
    if (current_.id == id) {
        player_.clear();
        current_ = Entry{};
        peaks_.clear();
        rms_.clear();
    }
    std::error_code ec;
    fs::remove(dir_ + "/" + it->file, ec);
    entries_.erase(it);
    writeIndexLocked();
    return true;
}

// ───────────── loading ─────────────

bool BackingTracks::load(const std::string& id, bool play, double start_sec, std::string& err) {
    Entry e;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& x) { return x.id == id; });
        if (it == entries_.end()) { err = "No such track"; return false; }
        e = *it;
        if (current_.id == id && player_.hasAudio() && !loading_.load()) {
            if (start_sec >= 0.0) player_.seek(start_sec);
            if (play) player_.play();
            return true;
        }
    }
    startDecode(e, play, std::max(0.0, start_sec));
    return true;
}

void BackingTracks::startDecode(const Entry& e, bool play, double start_sec) {
    joinLoader();
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        gen = ++gen_;
        loading_.store(true);
        loading_id_ = e.id;
        error_.clear();
    }
    const std::string path = pathOf(e);
    const double sr = player_.sampleRate();
    const std::string id = e.id;
    loader_ = std::thread([this, path, sr, id, gen, play, start_sec] {
        std::string err;
        auto audio = decodeBackingFile(path, sr, &err, &cancel_);
        std::lock_guard<std::mutex> lk(mtx_);
        if (gen != gen_) return; // superseded
        loading_.store(false);
        loading_id_.clear();
        if (!audio) {
            if (err != "cancelled") error_ = err.empty() ? "Couldn't decode that track" : err;
            return;
        }
        const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& x) { return x.id == id; });
        if (it == entries_.end()) return; // deleted meanwhile
        peaks_ = audio->bin_peak;
        rms_ = audio->bin_rms;
        norm_gain_ = audio->norm_gain;
        it->duration = audio->seconds();
        it->last_used = nowMs();
        current_ = *it;
        player_.setAudio(std::move(audio), start_sec);
        writeIndexLocked();
        if (play) player_.play();
    });
}

bool BackingTracks::waitLoaded(int timeout_ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (loading_.load()) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// ───────────── JSON ─────────────

std::string BackingTracks::entryJson(const Entry& e) const {
    return dumps(json{{"id", e.id}, {"title", e.title}, {"artist", e.artist}, {"format", e.format}, {"file", e.file},
                      {"duration", std::round(e.duration * 100.0) / 100.0}, {"added", e.added}, {"last_used", e.last_used}, {"size", e.size}});
}

std::string BackingTracks::stateJson() {
    std::lock_guard<std::mutex> lk(mtx_);
    auto& p = player_;
    const bool loaded = !current_.id.empty() && p.hasAudio();
    json j = {
        {"ok", true},
        {"loaded", loaded},
        {"id", loaded ? current_.id : ""},
        {"title", loaded ? current_.title : ""},
        {"artist", loaded ? current_.artist : ""},
        {"format", loaded ? current_.format : ""},
        {"duration", p.duration()},
        {"position", p.position()},
        {"playing", p.playing()},
        {"ended", p.ended()},
        {"count_in_left", p.countInLeft()},
        {"loading", loading_.load()},
        {"loading_id", loading_id_},
        {"error", error_},
        {"volume", p.volume()},
        {"speed", p.speed()},
        {"semitones", p.semitones()},
        {"loop", {{"on", p.loopOn()}, {"a", p.loopA()}, {"b", p.loopB()}}},
        {"count_in", {{"beats", p.countInBeats()}, {"bpm", p.countInBpm()}}},
        {"duck", {{"on", p.duckOn()}, {"depth_db", p.duckDepthDb()}, {"amount", p.duckAmount()}}},
        {"normalize", p.normalize()},
        {"norm_gain_db", 20.0 * std::log10(std::max(1e-4f, norm_gain_))},
        {"level", p.level()},
        {"stretch", {{"engine", "AUNewTimePitch"}, {"pitch_preserved", true}, {"latency_ms", p.stretchLatency() * 1000.0}}},
    };
    return dumps(j);
}

std::string BackingTracks::telemetryJson() {
    player_.collect();
    std::string id;
    bool loading;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        id = player_.hasAudio() ? current_.id : "";
        loading = loading_.load();
    }
    char b[320];
    std::snprintf(b, sizeof b,
                  "{\"id\":\"%s\",\"playing\":%s,\"pos\":%.3f,\"dur\":%.3f,\"speed\":%.3f,\"count\":%d,\"loading\":%s,\"ended\":%s,\"duck\":%.2f,\"level\":%.4f}",
                  id.c_str(), player_.playing() ? "true" : "false", player_.position(), player_.duration(), player_.speed(), player_.countInLeft(),
                  loading ? "true" : "false", player_.ended() ? "true" : "false", player_.duckAmount(), player_.level());
    return b;
}

std::string BackingTracks::handleApi(const std::string& path, const Query& q, const std::string& body) {
    if (!started_) return R"({"ok":false,"error":"Backing tracks aren't available here"})";
    player_.collect();
    auto has = [&](const char* k) { return !q(k).empty(); };

    if (path == "/api/backing/list") {
        json arr = json::array();
        for (const auto& e : list()) arr.push_back(json::parse(entryJson(e)));
        std::string cur;
        { std::lock_guard<std::mutex> lk(mtx_); cur = current_.id; }
        return dumps(json{{"ok", true}, {"dir", dir_}, {"current", cur}, {"tracks", arr}});
    }
    if (path == "/api/backing/import") {
        std::vector<std::string> paths;
        if (has("path")) paths.push_back(q("path"));
        if (!body.empty()) {
            try {
                const json j = json::parse(body);
                for (const auto& p : j.value("paths", json::array())) if (p.is_string()) paths.push_back(p.get<std::string>());
            } catch (...) {}
        }
        json ok = json::array(), failed = json::array();
        for (const auto& p : paths) {
            // A folder imports the audio files directly inside it.
            std::error_code ec;
            std::vector<std::string> files;
            if (fs::is_directory(p, ec)) {
                for (fs::directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec))
                    if (it->is_regular_file(ec) && audioExt(it->path()) && it->path().filename().string()[0] != '.') files.push_back(it->path().string());
                std::sort(files.begin(), files.end());
            } else files.push_back(p);
            for (const auto& f : files) {
                Entry e;
                std::string err;
                if (importFile(f, e, err)) ok.push_back(json::parse(entryJson(e)));
                else failed.push_back({{"path", f}, {"error", err}});
            }
        }
        return dumps(json{{"ok", !ok.empty()}, {"imported", ok}, {"failed", failed}});
    }
    if (path == "/api/backing/rename") {
        const bool r = rename(q("id"), q("title"));
        return r ? R"({"ok":true})" : R"({"ok":false,"error":"Couldn't rename that track"})";
    }
    if (path == "/api/backing/delete") {
        const bool r = remove(q("id"));
        return r ? R"({"ok":true})" : R"({"ok":false,"error":"No such track"})";
    }
    if (path == "/api/backing/peaks") {
        std::lock_guard<std::mutex> lk(mtx_);
        if (current_.id.empty() || peaks_.empty()) return R"({"ok":false,"error":"No track loaded"})";
        const int nb = std::clamp(static_cast<int>(num(q("bins"), 800)), 16, static_cast<int>(BackingAudio::kBins));
        const std::size_t src = peaks_.size();
        float top = 1e-6f;
        for (float v : peaks_) top = std::max(top, v);
        std::string pk, rm;
        pk.reserve(nb * 6);
        rm.reserve(nb * 6);
        char b[16];
        for (int i = 0; i < nb; ++i) {
            const std::size_t a = i * src / nb, z = std::max(a + 1, (i + 1) * src / nb);
            float mp = 0.0f, mr = 0.0f;
            for (std::size_t k = a; k < z && k < src; ++k) { mp = std::max(mp, peaks_[k]); mr = std::max(mr, rms_[k]); }
            std::snprintf(b, sizeof b, "%s%.3f", i ? "," : "", mp / top);
            pk += b;
            std::snprintf(b, sizeof b, "%s%.3f", i ? "," : "", mr / top);
            rm += b;
        }
        char head[160];
        std::snprintf(head, sizeof head, "{\"ok\":true,\"id\":\"%s\",\"duration\":%.3f,\"bins\":%d,\"peak_db\":%.2f,", current_.id.c_str(), player_.duration(), nb,
                      20.0 * std::log10(top));
        return std::string(head) + "\"peak\":[" + pk + "],\"rms\":[" + rm + "]}";
    }
    if (path != "/api/backing") return R"({"ok":false,"error":"not_found"})";

    // /api/backing?action=…&<settings>
    const std::string action = q("action");
    std::string err;
    if (action == "load") {
        if (!load(q("id"), q("play") == "1", has("at") ? num(q("at"), 0.0) : -1.0, err))
            return dumps(json{{"ok", false}, {"error", err}});
    } else if (action == "unload") {
        joinLoader();
        std::lock_guard<std::mutex> lk(mtx_);
        loading_.store(false);
        loading_id_.clear();
        player_.clear();
        current_ = Entry{};
        peaks_.clear();
        rms_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto& p = player_;
        if (has("volume")) p.setVolume(static_cast<float>(num(q("volume"), 1.0)));
        if (has("speed")) p.setSpeed(static_cast<float>(num(q("speed"), 1.0)));
        if (has("semitones")) p.setSemitones(static_cast<float>(std::round(num(q("semitones"), 0.0))));
        if (has("loop") || has("a") || has("b"))
            p.setLoop(has("loop") ? q("loop") == "1" : p.loopOn(), has("a") ? num(q("a"), 0.0) : p.loopA(), has("b") ? num(q("b"), 0.0) : p.loopB());
        if (has("countin") || has("bpm")) p.setCountIn(has("countin") ? static_cast<int>(num(q("countin"), 0)) : p.countInBeats(), num(q("bpm"), p.countInBpm()));
        if (has("duck") || has("duck_db")) p.setDuck(has("duck") ? q("duck") == "1" : p.duckOn(), static_cast<float>(num(q("duck_db"), p.duckDepthDb())));
        if (has("normalize")) p.setNormalize(q("normalize") == "1");
        if (action == "seek" || has("sec") || has("pos")) {
            if (has("sec")) p.seek(num(q("sec"), 0.0));
            else if (has("pos")) p.seek(std::clamp(num(q("pos"), 0.0), 0.0, 1.0) * p.duration());
        }
        if (action == "play") p.play();
        else if (action == "pause") p.pause();
        else if (action == "toggle") { if (p.playing()) p.pause(); else p.play(); }
        else if (action == "stop") { p.pause(); p.seek(p.loopOn() ? p.loopA() : 0.0); }
    }
    return stateJson();
}

// ───────────── YouTube host page ─────────────

void BackingTracks::sendHtml(int fd, const std::string& html) {
    std::ostringstream o;
    o << "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-cache\r\n"
      << "Content-Length: " << html.size() << "\r\n\r\n" << html;
    const std::string s = o.str();
    std::size_t sent = 0;
    while (sent < s.size()) {
        const auto n = net::sendBytes(fd, s.data() + sent, s.size() - sent);
        if (n <= 0) break;
        sent += static_cast<std::size_t>(n);
    }
}

// The page the Practice view embeds for YouTube. It is served from the engine so the player has a real
// http origin (YouTube refuses embeds without a referrer — errors 152/153 — which a file:// page can't send).
// It only relays: the parent posts {hr:"yt", cmd:"load"|"call"|"loop", …}; the page posts back
// {hr:"yt", type:"host"|"api"|"ready"|"state"|"error", …}. Playback is YouTube's own (official IFrame API).
const std::string& BackingTracks::youtubeHostHtml() {
    static const std::string html = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="referrer" content="strict-origin-when-cross-origin">
<title>Headroom · YouTube</title>
<style>html,body{margin:0;height:100%;background:#000;overflow:hidden}#p,iframe{position:absolute;inset:0;width:100%;height:100%;border:0}</style>
</head><body><div id="p"></div>
<script>
(function () {
  "use strict";
  var P = null, ready = false, pending = null, loop = { on: false, a: 0, b: 0 };
  var ALLOWED = /^(playVideo|pauseVideo|stopVideo|seekTo|setPlaybackRate|mute|unMute|setVolume)$/;
  function post(m) { m.hr = "yt"; try { parent.postMessage(m, "*"); } catch (e) {} }
  function state() {
    if (!P || !ready || !P.getPlayerState) return;
    var d = {}; try { d = P.getVideoData() || {}; } catch (e) {}
    post({ type: "state", state: P.getPlayerState(), time: P.getCurrentTime() || 0, duration: P.getDuration() || 0,
      rate: P.getPlaybackRate() || 1, rates: P.getAvailablePlaybackRates ? P.getAvailablePlaybackRates() : [1],
      title: d.title || "", author: d.author || "", videoId: d.video_id || "", muted: P.isMuted ? P.isMuted() : false,
      volume: P.getVolume ? P.getVolume() : 100, buffered: P.getVideoLoadedFraction ? P.getVideoLoadedFraction() : 0 });
  }
  function create(m) {
    P = new YT.Player("p", {
      videoId: m.videoId, host: "https://www.youtube.com",
      playerVars: { playsinline: 1, rel: 0, iv_load_policy: 3, fs: 0, enablejsapi: 1, autoplay: m.autoplay ? 1 : 0,
        start: Math.floor(m.start || 0), origin: location.origin, widget_referrer: location.href },
      events: {
        onReady: function () { ready = true; if (m.muted) P.mute(); post({ type: "ready" }); state(); },
        onStateChange: state, onPlaybackRateChange: state,
        onError: function (e) { post({ type: "error", code: e.data }); }
      }
    });
  }
  function load(m) {
    if (!window.YT || !YT.Player) { pending = m; return; }
    if (!P) { create(m); return; }
    var o = { videoId: m.videoId, startSeconds: m.start || 0 };
    if (m.autoplay) P.loadVideoById(o); else P.cueVideoById(o);
  }
  window.onYouTubeIframeAPIReady = function () { post({ type: "api" }); if (pending) { var m = pending; pending = null; load(m); } };
  addEventListener("message", function (e) {
    if (e.source !== parent) return;
    var m = e.data; if (!m || m.hr !== "yt") return;
    if (m.cmd === "load") load(m);
    else if (m.cmd === "loop") loop = { on: !!m.on, a: +m.a || 0, b: +m.b || 0 };
    else if (m.cmd === "call" && P && ready && ALLOWED.test(m.fn) && typeof P[m.fn] === "function") { P[m.fn].apply(P, m.args || []); setTimeout(state, 60); }
  });
  setInterval(function () {
    if (!P || !ready || !P.getCurrentTime) return;
    if (loop.on && loop.b > loop.a + 0.2 && P.getPlayerState() === 1 && P.getCurrentTime() >= loop.b) P.seekTo(loop.a, true);
    state();
  }, 100);
  var s = document.createElement("script");
  s.src = "https://www.youtube.com/iframe_api";
  s.onerror = function () { post({ type: "error", code: "offline" }); };
  document.head.appendChild(s);
  post({ type: "host" });
})();
</script></body></html>
)HTML";
    return html;
}

} // namespace rig::audio
