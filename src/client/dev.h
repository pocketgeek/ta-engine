#pragma once

// A release build of taclient honours NO environment variables and no command-line
// options beyond --data and --version: the game is configured through the menu and the
// Options screen, never through hidden switches. Every TA_* dev/test/diagnostic hook
// in the viewer reads its environment through devEnv(), which compiles to a constant
// nullptr in a release build (NDEBUG) -- so those hooks simply vanish from the shipped
// binary while staying available in debug builds. Use this instead of std::getenv.

#include <cstdlib>

namespace ta {

// Boolean form of devEnv. Reads the VALUE, so a harness can turn a flag OFF by setting
// it to 0 -- which matters the moment any of these becomes a default-on knob: the
// presence test these all used meant TA_MONARCH_EXPENDABLE=0 switched the option ON,
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

}  // namespace ta
