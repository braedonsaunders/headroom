#pragma once
#include <cstdlib>

namespace rig::platform {
inline int setEnv(const char* key, const char* value) {
#ifdef _WIN32
    return _putenv_s(key, value);
#else
    return ::setenv(key, value, 1);
#endif
}
inline int unsetEnv(const char* key) {
#ifdef _WIN32
    return _putenv_s(key, "");
#else
    return ::unsetenv(key);
#endif
}
} // namespace rig::platform
