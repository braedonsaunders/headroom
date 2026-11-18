// Headroom desktop app for Windows and Linux: the engine (RealtimeAudioServer on 127.0.0.1) plus the
// app/ui page in a native web view. macOS uses StudioApp.mm; this is the same app with the same flags:
//   --screenshot <png>   load, snapshot the page, quit (runs on port 17771 without audio, like the Mac app)
//   --debug              web inspector + console to stdout
//   HEADROOM_PORT, HEADROOM_NO_AUDIO as in StudioApp.mm (a test instance beside the main one).
//
// Layout it looks for (the engine reads <root>/app/{ui,presets,resources,samples}):
//   dev build    RIG_APP_SOURCE_DIR/..           (the source tree)
//   Linux pkg    <exe>/../share/headroom         (AppImage / .deb: usr/bin/Headroom + usr/share/headroom/app/…)
//   Windows pkg  <exe dir>                       (Program Files\Headroom\Headroom.exe + app\…)
#include "Bridge.hpp"
#include "Shell.hpp"
#include "../../audio/RealtimeAudioServer.hpp"
#include "../../platform/Socket.hpp"
#include "../../plugins/PluginEditor.hpp"

#include "webview.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;
using namespace rig;

namespace {

std::string findRoot() {
    std::vector<fs::path> c;
#ifdef RIG_APP_SOURCE_DIR
    c.push_back(fs::path(RIG_APP_SOURCE_DIR).parent_path());
#endif
    const fs::path exe = shell::executableDir();
    c.push_back(exe.parent_path() / "share" / "headroom");
    c.push_back(exe);
    c.push_back(exe.parent_path());
    std::error_code ec;
    c.push_back(fs::current_path(ec));
    for (const auto& r : c)
        if (fs::exists(r / "app" / "ui" / "index.html", ec)) return fs::weakly_canonical(r, ec).string();
    return {};
}

std::string fileUrl(const fs::path& p) {
    std::string s = p.generic_string();
#ifdef _WIN32
    s = "/" + s; // file:///C:/…
#endif
    static const char* hex = "0123456789ABCDEF";
    std::string out = "file://";
    for (unsigned char ch : s) {
        if (std::isalnum(ch) || std::strchr("/-_.~:", ch)) out += static_cast<char>(ch);
        else { out += '%'; out += hex[ch >> 4]; out += hex[ch & 15]; }
    }
    return out;
}

struct App {
    webview_t w = nullptr;
    std::unique_ptr<shell::Platform> platform;
    std::unique_ptr<shell::Bridge> bridge;
};

// Runs `fn` on the UI thread (webview_dispatch is the one thread-safe entry point).
void onMain(webview_t w, std::function<void()> fn) {
    auto* f = new std::function<void()>(std::move(fn));
    webview_dispatch(w, [](webview_t, void* arg) {
        auto* g = static_cast<std::function<void()>*>(arg);
        (*g)();
        delete g;
    }, f);
}

} // namespace

int main(int argc, char** argv) {
    std::string screenshot;
    bool debug = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
        else if (!std::strcmp(argv[i], "--debug")) debug = true;
    }
    const bool is_shot = !screenshot.empty();
    // One interactive instance: a second launch exits instead of fighting over the port and the audio device.
    if (!is_shot && !std::getenv("HEADROOM_PORT") && !shell::acquireSingleInstance()) {
        std::fprintf(stderr, "Headroom is already running.\n");
        return 0;
    }
    const std::string root = findRoot();
    if (root.empty()) {
        std::fprintf(stderr, "Headroom: could not find app/ui/index.html next to the executable.\n");
        return 1;
    }

    // WebView2 owns the UI thread's STA. Create it before WASAPI initializes COM.
    App app;
    app.w = webview_create(debug ? 1 : 0, nullptr);
    if (!app.w) {
        std::fprintf(stderr, "Headroom: could not create the web view (Linux: WebKitGTK 4.1; Windows: WebView2 runtime).\n");
        return 1;
    }
    net::ensureInit();
    plugins::installAppEditorHooks();
    auto server = std::make_unique<audio::RealtimeAudioServer>();
    const bool enable_audio = !is_shot && !std::getenv("HEADROOM_NO_AUDIO");
    uint16_t port = is_shot ? 17771 : 17770;
    if (const char* p = std::getenv("HEADROOM_PORT")) port = static_cast<uint16_t>(std::atoi(p));
    if (!server->start(root, port, enable_audio)) {
        port = (port == 17770) ? 17771 : 17772;
        server->start(root, port, enable_audio);
    }
    std::fprintf(stderr, "[shell] engine on 127.0.0.1:%u, root %s\n", unsigned(port), root.c_str());

    webview_set_title(app.w, "Headroom");
    webview_set_size(app.w, 1600, 1000, WEBVIEW_HINT_NONE);
    void* window = webview_get_window(app.w);
    void* browser = webview_get_native_handle(app.w, WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER);
    shell::configureBrowser(browser, debug);
    app.platform = shell::makePlatform(window, browser);
    webview_t w = app.w;
    app.bridge = std::make_unique<shell::Bridge>(*app.platform, [w](std::string js) {
        onMain(w, [w, js] { webview_eval(w, js.c_str()); });
    });
    shell::Bridge* bridge = app.bridge.get();
    // Messages are handled from the main loop, outside the browser's callback (file dialogs nest a loop).
    shell::installNativeMessageHandler(browser, [w, bridge](const std::string& j) { onMain(w, [bridge, j] { bridge->handle(j); }); });
    webview_bind(app.w, "__hrPost", [](const char* id, const char* req, void* arg) {
        auto* a = static_cast<App*>(arg);
        webview_return(a->w, id, 0, "null");
        const std::string j = req ? req : "";
        shell::Bridge* b = a->bridge.get();
        onMain(a->w, [b, j] { b->handle(j); });
    }, &app);
    const std::string boot = shell::bridgeInitScript(port, is_shot) +
        "if(location.protocol==='file:') window.HEADROOM_API_TOKEN='" + server->apiToken() + "';";
    webview_init(app.w, boot.c_str());
    shell::installQuitHandler([w] { onMain(w, [w] { webview_terminate(w); }); });

    const fs::path html = fs::path(root) / "app" / "ui" / "index.html";
    webview_navigate(app.w, fileUrl(html).c_str());

    if (is_shot) {
        // Like the Mac app: give the page a moment to render, snapshot, quit.
        const double delay = std::getenv("HEADROOM_SCREENSHOT_DELAY") ? std::atof(std::getenv("HEADROOM_SCREENSHOT_DELAY")) : 3.0;
        std::thread([w, browser, screenshot, delay] {
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(delay * 1000)));
            onMain(w, [w, browser, screenshot] {
                if (!shell::snapshotPng(browser, screenshot, [w](bool ok) {
                        std::fprintf(stderr, "[shell] screenshot %s\n", ok ? "written" : "failed");
                        webview_terminate(w);
                    }))
                    webview_terminate(w);
            });
        }).detach();
    }

    webview_run(app.w);
    webview_destroy(app.w);
    app.w = nullptr;
    server->stop();
    server.reset();
    return 0;
}
