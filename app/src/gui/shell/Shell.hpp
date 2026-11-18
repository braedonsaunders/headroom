#pragma once
// Portable desktop shell for Headroom on Windows and Linux (macOS keeps the Cocoa shell in StudioApp.mm).
//
//   ShellMain.cpp   window (webview/webview: WebKitGTK on Linux, WebView2 on Windows), engine start, UI lookup
//   Bridge.cpp      the page ↔ native message protocol, identical to StudioApp.mm's (same actions, same
//                   window.headroom* reply callbacks), so app/ui runs unmodified
//   ShellLinux.cpp  GTK file chooser, xdg-open, single-instance lock, WebKit settings, snapshots
//   ShellWin.cpp    IFileOpenDialog, ShellExecute / SHOpenFolderAndSelectItems, named-mutex single instance
//
// JS side: window.HRNative.post(msg) everywhere. window.webkit.messageHandlers.headroom.postMessage(msg) keeps
// working: natively on WebKitGTK (a real script message handler), through a polyfill on WebView2.
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rig::shell {

struct PickOptions {
    std::string title;
    std::string accept_label;
    bool files = true;
    bool folders = false;
    bool multiple = true;
    std::vector<std::string> extensions; // lowercase, without the dot; empty = any
    std::string filter_name;             // e.g. "Audio files"
    std::string start_dir;
};

class Platform {
public:
    virtual ~Platform() = default;
    virtual void openUrl(const std::string& url) = 0;          // default browser
    virtual void openFolder(const std::string& path) = 0;      // file manager at a folder
    virtual void revealFile(const std::string& path) = 0;      // file manager with the file selected, if supported
    // Main thread. Calls `done` on the main thread with the chosen paths (empty when cancelled).
    virtual void pickPaths(const PickOptions& o, std::function<void(std::vector<std::string>)> done) = 0;
};

// window = the native top-level window (GtkWindow* / HWND); browser = WebKitWebView* / ICoreWebView2Controller*.
std::unique_ptr<Platform> makePlatform(void* window, void* browser);

// One interactive instance per user. Returns false when another instance already holds the lock.
bool acquireSingleInstance();

// Engine-side settings for the embedded browser (file:// → http://127.0.0.1 fetches, devtools in debug).
void configureBrowser(void* browser, bool debug);

// Native "headroom" message handler where the engine has one (WebKitGTK); `on_json` gets the message as JSON.
// Returns false when the page must use the HRNative / polyfill path only.
bool installNativeMessageHandler(void* browser, std::function<void(const std::string&)> on_json);

// Writes a PNG of the page (`--screenshot`), then calls `done` on the main thread. False if unsupported.
bool snapshotPng(void* browser, const std::string& path, std::function<void(bool ok)> done);

std::string executableDir();

// SIGINT/SIGTERM (Linux) or console Ctrl+C/close (Windows) → `quit` on the main thread.
void installQuitHandler(std::function<void()> quit);

} // namespace rig::shell
