// Windows side of the portable shell (WebView2 via webview/webview). Compile-tested; see docs/PORTING.md.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Shell.hpp"
#include "../../platform/Paths.hpp"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <filesystem>
#include <string>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

namespace rig::shell {

namespace {

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

class WinPlatform final : public Platform {
public:
    explicit WinPlatform(HWND w) : hwnd_(w) {}

    void openUrl(const std::string& url) override { ShellExecuteW(hwnd_, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
    void openFolder(const std::string& path) override { ShellExecuteW(hwnd_, L"explore", wide(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
    void revealFile(const std::string& path) override {
        PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(wide(path).c_str());
        if (pidl) {
            SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
            ILFree(pidl);
        } else {
            openFolder(utf8(std::filesystem::path(wide(path)).parent_path().c_str()));
        }
    }

    void pickPaths(const PickOptions& o, std::function<void(std::vector<std::string>)> done) override {
        std::vector<std::string> out;
        IFileOpenDialog* dlg = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
            DWORD flags = 0;
            dlg->GetOptions(&flags);
            flags |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
            if (o.multiple) flags |= FOS_ALLOWMULTISELECT;
            if (!o.files && o.folders) flags |= FOS_PICKFOLDERS; // the shell dialog can't mix files and folders
            dlg->SetOptions(flags);
            dlg->SetTitle(wide(o.title).c_str());
            if (!o.accept_label.empty()) dlg->SetOkButtonLabel(wide(o.accept_label).c_str());
            std::wstring spec, name = wide(o.filter_name.empty() ? std::string("Supported files") : o.filter_name);
            for (const auto& e : o.extensions) spec += (spec.empty() ? L"*." : L";*.") + wide(e);
            COMDLG_FILTERSPEC filters[2] = {{name.c_str(), spec.c_str()}, {L"All files", L"*.*"}};
            if (o.files && !spec.empty()) dlg->SetFileTypes(2, filters);
            if (!o.start_dir.empty()) {
                IShellItem* start = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(wide(o.start_dir).c_str(), nullptr, IID_PPV_ARGS(&start)))) {
                    dlg->SetFolder(start);
                    start->Release();
                }
            }
            if (SUCCEEDED(dlg->Show(hwnd_))) {
                IShellItemArray* items = nullptr;
                if (SUCCEEDED(dlg->GetResults(&items)) && items) {
                    DWORD n = 0;
                    items->GetCount(&n);
                    for (DWORD i = 0; i < n; ++i) {
                        IShellItem* it = nullptr;
                        if (FAILED(items->GetItemAt(i, &it))) continue;
                        PWSTR p = nullptr;
                        if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                            out.push_back(utf8(p));
                            CoTaskMemFree(p);
                        }
                        it->Release();
                    }
                    items->Release();
                }
            }
            dlg->Release();
        }
        done(std::move(out));
    }

private:
    HWND hwnd_;
};

} // namespace

std::unique_ptr<Platform> makePlatform(void* window, void*) { return std::make_unique<WinPlatform>(static_cast<HWND>(window)); }

bool acquireSingleInstance() {
    // Held for the life of the process (the OS releases it on exit or crash).
    HANDLE m = CreateMutexW(nullptr, TRUE, L"Local\\HeadroomStudio.SingleInstance");
    return !(m && GetLastError() == ERROR_ALREADY_EXISTS);
}

// WebView2 needs no extra settings for the file:// UI (plain scripts, CORS-open engine API). If Chromium's
// Private Network Access ever blocks file:// → 127.0.0.1, map the UI folder to a virtual host instead
// (ICoreWebView2_3::SetVirtualHostNameToFolderMapping) and load https://headroom.app/ui/index.html.
void configureBrowser(void*, bool) {}

bool installNativeMessageHandler(void*, std::function<void(const std::string&)>) { return false; }

bool snapshotPng(void*, const std::string&, std::function<void(bool)>) { return false; } // TODO: CapturePreview

std::string executableDir() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0) return ".";
    return utf8(std::filesystem::path(buf).parent_path().c_str());
}

namespace {
std::function<void()>* g_quit = nullptr;
BOOL WINAPI onConsoleCtrl(DWORD) {
    if (g_quit) (*g_quit)();
    return TRUE;
}
} // namespace

void installQuitHandler(std::function<void()> quit) {
    g_quit = new std::function<void()>(std::move(quit)); // process lifetime; the callback must be thread-safe
    SetConsoleCtrlHandler(onConsoleCtrl, TRUE);
}

} // namespace rig::shell
