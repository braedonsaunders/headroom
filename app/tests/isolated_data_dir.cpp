// Linked into every test executable (CMakeLists.txt, rig_isolate_test_data): before main(), points HEADROOM_DATA_DIR
// (platform::appDataDir) at a fresh temp folder, removed again at exit. Without it a test that builds a legacy amp,
// imports a rack or saves a rig wrote into the user's real data folder (the amp-trims cache, 2026-11-11), even
// from a plain `./test_x` run. A HEADROOM_DATA_DIR already in the environment is respected (a deliberate choice), and
// tests that set their own (test_nam_bridge, test_param_drag) still override this one. Folders left by aborted runs
// are swept at the next start.
#include "test_compat.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#ifndef _WIN32
#include <cerrno>
#include <csignal>
#endif

namespace {

struct IsolatedDataDir {
    std::filesystem::path dir;
    IsolatedDataDir() {
        const char* given = std::getenv("HEADROOM_DATA_DIR");
        if (given && *given) return;
        std::error_code ec;
        const auto base = std::filesystem::temp_directory_path(ec);
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        if (!ec) dir = base / ("headroom-test-data-" + std::to_string(rig_test::processId()) + "-" + std::to_string(stamp));
        if (!ec) sweepStale(base);
        if (ec || !std::filesystem::create_directories(dir, ec) || ec) { // never fall back to the real data folder
            std::fprintf(stderr, "cannot create an isolated data folder for the test (%s); set HEADROOM_DATA_DIR\n", ec.message().c_str());
            std::exit(1);
        }
        rig_test::setEnv("HEADROOM_DATA_DIR", dir.string().c_str());
    }
    // A test that aborts (a failed assert) skips the destructor: its folder is removed by the next test run instead,
    // once its process is gone.
    static void sweepStale(const std::filesystem::path& base) {
#ifndef _WIN32
        std::error_code ec;
        const std::string prefix = "headroom-test-data-";
        for (std::filesystem::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (name.rfind(prefix, 0) != 0) continue;
            const long pid = std::strtol(name.c_str() + prefix.size(), nullptr, 10);
            if (pid <= 0 || (::kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH)) {
                std::error_code rm;
                std::filesystem::remove_all(it->path(), rm);
            }
        }
#else
        (void)base;
#endif
    }
    ~IsolatedDataDir() {
        if (dir.empty()) return;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

const IsolatedDataDir gIsolatedDataDir;

} // namespace
