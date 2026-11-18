// Page → native message protocol for the portable shell. Mirrors StudioApp.mm action by action so app/ui
// needs no platform checks: same action names, same payload fields, same window.headroom* replies.
#include "Bridge.hpp"
#include "../../audio/BackingTracks.hpp"
#include "../../dsp/NamComponent.hpp"
#include "../../dsp/ToneMatchAndCalibration.hpp"
#include "../../library/Library.hpp"
#include "../../platform/Paths.hpp"
#include "../../platform/Secrets.hpp"
#include "../../plugins/PluginEditor.hpp"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace rig::shell {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string call(const char* fn, const json& arg) {
    return std::string("window.") + fn + " && window." + fn + "(" + arg.dump() + ")";
}

bool validAccount(const json& a) {
    if (!a.is_string()) return false;
    const std::string s = a.get<std::string>();
    if (s.empty() || s.size() > 64) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '_' || c == ':' || c == '-'; });
}

std::string lowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

// Copies every `ext` file from the picked files/folders into `dest` (a picked folder becomes a sub-folder).
int copyInto(const std::vector<std::string>& picked, const std::string& dest, const char* ext) {
    int copied = 0;
    std::error_code ec;
    for (const auto& src : picked) {
        const fs::path sp(src);
        const bool dir = fs::is_directory(sp, ec);
        const fs::path dd = dir ? fs::path(dest) / sp.filename() : fs::path(dest);
        fs::create_directories(dd, ec);
        std::vector<fs::path> files;
        if (dir) {
            for (auto it = fs::recursive_directory_iterator(sp, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (it->is_regular_file(ec)) files.push_back(it->path());
            }
        } else {
            files.push_back(sp);
        }
        for (const auto& f : files) {
            if (lowerExt(f) != ext) continue;
            const fs::path dst = dd / f.filename();
            fs::remove(dst, ec);
            if (fs::copy_file(f, dst, ec) && !ec) ++copied;
        }
    }
    return copied;
}

const std::vector<std::string> kAudioExts = {"wav", "aif", "aiff", "aifc", "caf", "mp3", "m4a", "aac", "mp4", "flac", "ogg", "oga", "opus", "alac", "wma"};

} // namespace

std::string bridgeInitScript(unsigned port, bool screenshot) {
    std::string js = "window.RIG_PORT = " + std::to_string(port) + ";";
    if (screenshot) js += " window.RIG_NO_SPLASH = true;";
    js += R"JS(
(() => {
  const post = (m) => {
    try {
      const h = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.headroom;
      if (h && !h.__hrPolyfill) { h.postMessage(m); return; }
      if (window.__hrPost) window.__hrPost(m);
    } catch (_) {}
  };
  window.HRNative = { post, platform: )JS";
#ifdef _WIN32
    js += "'windows'";
#ifdef RIG_WITH_ASIO
    js += ", asio: true";
#endif
#else
    js += "'linux'";
#endif
    js += R"JS( };
  // The UI talks to window.webkit.messageHandlers.headroom (WKWebView). WebKitGTK provides it natively; elsewhere
  // route it through HRNative.
  try {
    const has = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.headroom;
    if (!has) {
      const shim = { postMessage: (m) => { if (window.__hrPost) window.__hrPost(m); }, __hrPolyfill: true };
      if (!window.webkit) window.webkit = {};
      if (!window.webkit.messageHandlers) window.webkit.messageHandlers = {};
      try { window.webkit.messageHandlers.headroom = shim; } catch (_) {}
      if (!window.webkit.messageHandlers.headroom)
        window.webkit = Object.assign({}, window.webkit, { messageHandlers: Object.assign({}, window.webkit.messageHandlers, { headroom: shim }) });
    }
  } catch (_) {}
  // Page errors → app log (as StudioApp.mm does).
  const log = (m) => post({ action: 'jslog', msg: String(m).slice(0, 2000) });
  addEventListener('error', (e) => log('error: ' + e.message + ' @ ' + (e.filename || '').split('/').pop() + ':' + e.lineno + ':' + e.colno));
  addEventListener('unhandledrejection', (e) => log('unhandled rejection: ' + ((e.reason && (e.reason.stack || e.reason.message)) || e.reason)));
  const ce = console.error.bind(console);
  console.error = (...a) => { log('console.error: ' + a.map(String).join(' ')); ce(...a); };
})();
)JS";
    return js;
}

void Bridge::handle(const std::string& text) {
    json m = json::parse(text, nullptr, false);
    if (m.is_array() && !m.empty()) m = m[0]; // webview bindings pass the argument list
    if (!m.is_object() || !m.contains("action") || !m["action"].is_string()) return;
    const std::string action = m["action"].get<std::string>();
    auto str = [&](const char* k) { return m.contains(k) && m[k].is_string() ? m[k].get<std::string>() : std::string(); };

    if (action == "jslog") {
        std::fprintf(stderr, "[ui] %s\n", str("msg").c_str());
        return;
    }
    if (action == "getSecret" || action == "setSecret" || action == "deleteSecret") {
        const json rid = m.contains("id") ? m["id"] : json(nullptr);
        const std::string account = validAccount(m.value("account", json())) ? m["account"].get<std::string>() : std::string();
        const std::string value = str("value");
        EvalFn eval = eval_;
        // The secret store can block (an unlock prompt), so it runs off the main thread.
        std::thread([=] {
            json reply = {{"id", rid}};
            platform::SecretResult r;
            if (account.empty()) r.error = "Invalid account";
            else if (action == "getSecret") r = platform::secretGet(account);
            else if (action == "setSecret") r = value.empty() ? platform::SecretResult{false, false, {}, "Empty value"} : platform::secretSet(account, value);
            else r = platform::secretDelete(account);
            if (action == "getSecret") {
                reply["found"] = r.found;
                if (r.found) reply["value"] = r.value;
            }
            reply["ok"] = r.ok;
            if (!r.ok) reply["error"] = r.error.empty() ? std::string("Secret store error") : r.error;
            eval(call("headroomSecretReply", reply));
        }).detach();
        return;
    }
    if (action == "aiFetch") {
        // TODO(port): native streaming proxy (libcurl on Linux, WinHTTP on Windows) as HRAiFetchProxy does on macOS.
        const std::string rid = str("id");
        if (!rid.empty())
            eval_(call("headroomAiFetch", json{{"id", rid}, {"type", "end"},
                                               {"error", "Native AI fetch proxy is not available on this platform yet"}}));
        return;
    }
    if (action == "aiFetchCancel") return;
    if (action == "openURL") {
        const std::string u = str("url");
        std::string lower = u.substr(0, 8);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool web = lower.rfind("https://", 0) == 0 || lower.rfind("http://", 0) == 0;
        if (web && u.find_first_of(" \"'\\\n\r\t`$") == std::string::npos) platform_.openUrl(u);
        return;
    }
    if (action == "importBackingTracks") {
        PickOptions o;
        o.title = "Choose backing tracks to practise with";
        o.accept_label = "Add";
        o.extensions = kAudioExts;
        o.filter_name = "Audio files";
        EvalFn eval = eval_;
        platform_.pickPaths(o, [eval](std::vector<std::string> paths) {
            if (!paths.empty()) eval(call("headroomBackingImport", paths));
        });
        return;
    }
    if (action == "revealBackingFolder") {
        const std::string dir = audio::BackingTracks::defaultDir();
        std::error_code ec;
        fs::create_directories(dir, ec);
        platform_.openFolder(dir);
        return;
    }
    if (action == "openPluginEditor") {
        if (m.contains("instance") && m["instance"].is_number()) plugins::showEditorOnMainThread(m["instance"].get<int>());
        return;
    }
    const std::string userRigs = library::appSupportDir() + "/User Presets";
    if (action == "revealRigsFolder" || action == "revealUserPresetsFolder") {
        std::error_code ec;
        fs::create_directories(userRigs, ec);
        platform_.openFolder(userRigs);
        return;
    }
    if (action == "revealRig" || action == "revealPreset") {
        std::string rigPath = str("path");
        if (rigPath.empty() && !str("name").empty()) rigPath = userRigs + "/" + library::sanitizeFileName(str("name")) + ".rig";
        std::error_code ec;
        if (!rigPath.empty() && fs::exists(rigPath, ec)) platform_.revealFile(rigPath);
        else {
            fs::create_directories(userRigs, ec);
            platform_.openFolder(userRigs);
        }
        return;
    }
    const std::string namFolder = namCaptureFolder();
    if (action == "revealNamFolder") {
        platform_.openFolder(namFolder);
        return;
    }
    if (action == "importNam") {
        PickOptions o;
        o.title = "Choose Neural Amp Modeler captures (.nam files or folders of them)";
        o.accept_label = "Import";
        o.folders = true;
        o.extensions = {"nam"};
        o.filter_name = "NAM captures";
        EvalFn eval = eval_;
        platform_.pickPaths(o, [eval, namFolder](std::vector<std::string> paths) {
            if (paths.empty()) return;
            const int copied = copyInto(paths, namFolder, "nam");
            rescanNamCaptures();
            eval("window.headroomNamImported && window.headroomNamImported(" + std::to_string(copied) + ")");
        });
        return;
    }
    const std::string irFolder = userIrFolder();
    if (action == "revealIrFolder") {
        platform_.openFolder(irFolder);
        return;
    }
    if (action == "importIr") {
        PickOptions o;
        o.title = "Choose speaker cabinet Impulse Responses (.wav files or folders)";
        o.accept_label = "Import IRs";
        o.folders = true;
        o.extensions = {"wav"};
        o.filter_name = "Impulse responses (.wav)";
        EvalFn eval = eval_;
        platform_.pickPaths(o, [eval, irFolder](std::vector<std::string> paths) {
            if (paths.empty()) return;
            const int copied = copyInto(paths, irFolder, "wav");
            rescanUserIrs();
            eval("window.headroomIrImported && window.headroomIrImported(" + std::to_string(copied) + ")");
        });
        return;
    }
}

} // namespace rig::shell
