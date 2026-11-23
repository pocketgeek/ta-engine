#pragma once

// Debug-only environment hooks. A release build (NDEBUG) honours no TAK_* environment
// variable: every dev/test/diagnostic hook reads its environment through devEnv(), which
// compiles to a constant nullptr under NDEBUG -- so those hooks, and their names, vanish
// from the shipped binaries while staying available in debug builds. Use this instead of
// std::getenv for any TAK_* knob. (The only exceptions are takserver's documented test
// timers -- TAK_TIMEOUT_MS, TAK_LOAD_TIMEOUT_MS, TAK_STALL_MS, TAK_DROP_WAIT_MS -- which
// work in every build type.)

#include <cstdlib>

namespace tak {

// Boolean form of devEnv. Reads the VALUE, so a harness can turn a flag OFF by setting
// it to 0 -- which matters the moment any of these becomes a default-on knob: the
// presence test these all used meant TAK_MONARCH_EXPENDABLE=0 switched the option ON,
// which is exactly what someone writing a control case would type. Unset is `def`.
inline bool devFlag(const char* name, bool def = false);

inline const char* devEnv(const char* name) {
#ifdef NDEBUG
    (void)name;
    return nullptr;
#else
    return std::getenv(name);
#endif
}

inline bool devFlag(const char* name, bool def) {
    const char* v = devEnv(name);
    if (!v || !*v) return def;
    return !(v[0] == '0' && v[1] == '\0');
}

}  // namespace tak
