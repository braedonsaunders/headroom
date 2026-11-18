#pragma once
// Identity of the engine, for on-disk caches of measured levels (the amps' output trims). There is no per-rig
// loudness cache any more: rigs play at their own knobs' level (core/LevelFold.hpp, docs/ENGINE_API.md "Levels").
//
// Level caches are keyed by CONTENT, never by the build (2026-11-11): a rebuild alone must not invalidate them, or
// every rebuild re-measured the whole library in the background, changing gains mid-session and taking CPU from the
// audio. A cached level stays valid while
//   - kLevelVersion is unchanged: bump it by hand with any engine change that moves output levels on purpose (a DSP,
//     tube-model or calibration change). Not bumping it means old gains keep being used, a few dB off at worst;
//   - the netlists of the circuit units concerned are unchanged (circuitNetlistHash below).
// engineBuildFingerprint() (the binary's LC_UUID and every netlist) is kept for reporting (/api/diag) only.
#include "../circuit/CircuitUnit.hpp"

#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

#ifdef __APPLE__
#include <dlfcn.h>
#include <mach-o/loader.h>
#endif

namespace rig {

// Hand-bumped level version of the engine (see above). Part of every level-cache key.
inline constexpr const char* kLevelVersion = "lvl1";

inline uint64_t fnv1a64(std::string_view s, uint64_t h = 1469598103934665603ull) noexcept {
    for (unsigned char ch : s) h = (h ^ ch) * 1099511628211ull;
    return (h ^ 0xffu) * 1099511628211ull; // field separator
}

// Hash of the library netlist (and post section) a circuit unit of `table_id` runs, or 0 when the unit is not a circuit
// unit. Computed once per id per process (the netlist text is embedded; HEADROOM_CIRCUIT_DIR edits need a restart).
inline uint64_t circuitNetlistHash(int32_t table_id) {
    if (!ce::hasCircuitFor(table_id) || !ce::circuitEngineEnabledFor(table_id)) return 0;
    static std::mutex m;
    static std::map<int32_t, uint64_t> cache;
    std::lock_guard<std::mutex> lk(m);
    if (auto it = cache.find(table_id); it != cache.end()) return it->second;
    uint64_t h = 0;
    for (const auto& b : ce::unitBindings())
        if (b.tableId == table_id) {
            h = fnv1a64(ce::libraryNetlist(b.netlistId), fnv1a64(b.post ? b.post : "", fnv1a64(b.netlistId)));
            break;
        }
    cache[table_id] = h;
    return h;
}

inline std::string engineImageUuid() {
#ifdef __APPLE__
    Dl_info info{};
    if (!dladdr(reinterpret_cast<const void*>(&engineImageUuid), &info) || !info.dli_fbase) return {};
    const auto* h = static_cast<const mach_header_64*>(info.dli_fbase);
    if (h->magic != MH_MAGIC_64) return {};
    const auto* lc = reinterpret_cast<const load_command*>(h + 1);
    for (uint32_t i = 0; i < h->ncmds; ++i) {
        if (lc->cmd == LC_UUID) {
            const auto* u = reinterpret_cast<const uuid_command*>(lc);
            return std::string(reinterpret_cast<const char*>(u->uuid), sizeof u->uuid);
        }
        lc = reinterpret_cast<const load_command*>(reinterpret_cast<const char*>(lc) + lc->cmdsize);
    }
#endif
    return {};
}

// 16 hex digits, computed once per process.
inline const std::string& engineBuildFingerprint() {
    static const std::string fp = [] {
        uint64_t h = 1469598103934665603ull; // FNV-1a 64
        const auto mix = [&h](std::string_view s) {
            for (unsigned char ch : s) h = (h ^ ch) * 1099511628211ull;
            h = (h ^ 0xffu) * 1099511628211ull; // field separator
        };
        mix(engineImageUuid());
        for (const auto& id : ce::libraryIds()) {
            mix(id);
            mix(ce::libraryNetlist(id));
        }
        char hex[17];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
        return std::string(hex);
    }();
    return fp;
}

} // namespace rig
