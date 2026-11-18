#include "Paths.hpp"
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")
#endif

namespace rig::platform {

namespace {
#ifdef _WIN32
std::string utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}
// Environment as UTF-8 (getenv returns the ANSI code page, which mangles non-ASCII user names).
std::string env(const char* name) {
    std::wstring wn(name, name + std::char_traits<char>::length(name));
    return utf8(_wgetenv(wn.c_str()));
}
#else
std::string env(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string();
}
#endif

std::string tempFallback() {
    std::error_code ec;
    const auto t = std::filesystem::temp_directory_path(ec);
    return (ec ? std::string("/tmp") : t.string()) + "/Headroom";
}

#ifdef _WIN32
std::string knownFolder(REFKNOWNFOLDERID id) {
    PWSTR w = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &w)) && w) out = utf8(w);
    if (w) CoTaskMemFree(w);
    return out;
}
#endif
} // namespace

std::string homeDir() {
#ifdef _WIN32
    if (auto h = env("USERPROFILE"); !h.empty()) return h;
    return knownFolder(FOLDERID_Profile);
#else
    return env("HOME");
#endif
}

std::string appDataDir() {
    if (auto o = env("HEADROOM_DATA_DIR"); !o.empty()) return o;
#if defined(__APPLE__)
    const std::string home = homeDir();
    return home.empty() ? "/tmp/Headroom" : home + "/Library/Application Support/Headroom";
#elif defined(_WIN32)
    std::string base = env("APPDATA");
    if (base.empty()) base = knownFolder(FOLDERID_RoamingAppData);
    return base.empty() ? tempFallback() : base + "\\Headroom";
#else
    if (auto x = env("XDG_DATA_HOME"); !x.empty()) return x + "/Headroom";
    const std::string home = homeDir();
    return home.empty() ? tempFallback() : home + "/.local/share/Headroom";
#endif
}

std::string documentsDir() {
#if defined(_WIN32)
    if (auto d = knownFolder(FOLDERID_Documents); !d.empty()) return d;
    return homeDir() + "\\Documents";
#else
#if !defined(__APPLE__)
    if (auto d = env("XDG_DOCUMENTS_DIR"); !d.empty()) return d;
#endif
    return homeDir() + "/Documents";
#endif
}

namespace {
std::mutex g_root_mtx;
std::string g_root;
} // namespace

void setAppRoot(const std::string& root) {
    std::lock_guard<std::mutex> lk(g_root_mtx);
    g_root = root;
}

std::string appRoot() {
    std::lock_guard<std::mutex> lk(g_root_mtx);
    return g_root;
}

std::vector<std::string> vst3Dirs() {
    std::vector<std::string> d;
    const std::string home = homeDir();
#if defined(__APPLE__)
    d.push_back("/Library/Audio/Plug-Ins/VST3");
    if (!home.empty()) d.push_back(home + "/Library/Audio/Plug-Ins/VST3");
#elif defined(_WIN32)
    // Steinberg's documented locations: %CommonProgramFiles%\VST3 (64-bit), per-user %LOCALAPPDATA%\Programs\Common\VST3.
    if (auto c = env("CommonProgramFiles"); !c.empty()) d.push_back(c + "\\VST3");
    if (auto l = env("LOCALAPPDATA"); !l.empty()) d.push_back(l + "\\Programs\\Common\\VST3");
#else
    d.push_back("/usr/lib/vst3");
    d.push_back("/usr/local/lib/vst3");
    if (!home.empty()) d.push_back(home + "/.vst3");
#endif
    return d;
}

} // namespace rig::platform
