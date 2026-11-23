#pragma once

// The per-user folder: where the game keeps settings.ini, replays and the campaign
// replay cache, and Cartographer its preferences, recovery snapshots and trigger logs.
// Every lookup goes through prefPath(); calling SDL_GetPrefPath anywhere else fails the
// pref_guard_lint test (tools/pref_guard.cmake).
//
// SDL honours XDG_DATA_HOME on Linux only. On Windows and macOS it always answers the
// player's real profile (%APPDATA%, ~/Library/Application Support), so a test that
// isolated itself with XDG_DATA_HOME still read and wrote the real settings file there.
// Debug builds and every test executable therefore honour XDG_DATA_HOME on all three
// platforms, with SDL's Linux layout (<XDG_DATA_HOME>/TAKengine/<app>/). CTest hands
// every test its own XDG_DATA_HOME (CMakeLists.txt, "Per-user files"), so no test
// reaches the player's profile on any platform. A shipped Release program (takclient
// and cartographer define TAK_PRODUCT) reads no such variable off Linux and gets SDL's
// answer, as it always has.

#include <SDL.h>

#include <filesystem>
#include <string>
#include <system_error>
#include "util/u8path.h"

namespace tak {

inline constexpr const char* kPrefOrg = "TAKengine";

// The per-user folder for `app` ("TAKingdoms", "Cartographer"), created if missing,
// ending in a separator like SDL_GetPrefPath's. Empty if there is none.
inline std::string prefPath(const char* app) {
#if (defined(_WIN32) || defined(__APPLE__)) && (!defined(NDEBUG) || !defined(TAK_PRODUCT))
    if (const char* root = SDL_getenv("XDG_DATA_HOME"); root && *root) {
        const auto dir = tak::u8path(root) / kPrefOrg / app;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) return {};
        const auto text = (dir / "").u8string();
        return {text.begin(), text.end()};
    }
#endif
    char* base = SDL_GetPrefPath(kPrefOrg, app);
    if (!base) return {};
    std::string out = base;
    SDL_free(base);
    return out;
}

}  // namespace tak
