// settings_test -- every Settings field must be visible to operator==.
//
// atDefaults() IS that comparison (options.h builds a default Settings and compares),
// so a field left out of it makes the DEFAULTS button go dead for anyone who changed
// only that field: the UI believes they are already at defaults. That is not
// hypothetical -- it shipped when `unitShadows` was added and the comparison was not
// updated, and `dataDir`/`dataManifest` were missing from it too.
//
// The failure mode is silent and the fix is one line, so the only real defence is a
// test that fails when a new field is added without touching the comparison. Each case
// below flips exactly one field away from its default and asserts the comparison
// notices. Adding a field to Settings without adding it here leaves it unguarded, so
// keep the list in step with the struct -- that is the point of the test.

#include "client/settings.h"

#include <cstdio>
#include <string>

using tak::Settings;

static int g_fail = 0;

// Flip one field via a lambda, then assert the comparison sees a difference.
template <typename F>
static void flips(const char* field, F mutate) {
    Settings a{}, b{};
    mutate(b);
    const bool noticed = !(a == b);
    std::printf("  %-22s %s\n", field, noticed ? "ok" : "FAIL (invisible to operator==)");
    if (!noticed) ++g_fail;
    // And the reverse, so a comparison that is accidentally one-directional is caught.
    if (noticed && (b == a)) {
        std::printf("  %-22s FAIL (asymmetric)\n", field);
        ++g_fail;
    }
}

int main() {
    std::printf("settings_test: every field visible to operator==\n");

    // display / window
    flips("fullscreen",      [](Settings& s) { s.fullscreen = !s.fullscreen; });
    flips("vsync",           [](Settings& s) { s.vsync = !s.vsync; });
    flips("maxFps",          [](Settings& s) { s.maxFps += 17; });
    flips("uiScale",         [](Settings& s) { s.uiScale += 0.25f; });
    flips("antiAlias",       [](Settings& s) { s.antiAlias = s.antiAlias ? 0 : 2; });
    flips("buildBarAlign",   [](Settings& s) { s.buildBarAlign = (s.buildBarAlign + 1) % 3; });
    flips("buildBarScale",   [](Settings& s) { s.buildBarScale += 0.25f; });
    flips("bilinear",        [](Settings& s) { s.bilinear = !s.bilinear; });
    flips("treeSway",        [](Settings& s) { s.treeSway = !s.treeSway; });
    flips("unitShadows",     [](Settings& s) { s.unitShadows = !s.unitShadows; });
    flips("healthBars",      [](Settings& s) { s.healthBars = (s.healthBars + 1) % 3; });

    // audio
    flips("masterVol",       [](Settings& s) { s.masterVol -= 11; });
    flips("bgmVol",          [](Settings& s) { s.bgmVol -= 11; });
    flips("sfxVol",          [](Settings& s) { s.sfxVol -= 11; });
    flips("chanGain[0]",     [](Settings& s) { s.chanGain[0] = 0.5f; });
    flips("chanGain[7]",     [](Settings& s) { s.chanGain[7] = 0.5f; });
    flips("audioDevice",     [](Settings& s) { s.audioDevice = "some-device"; });

    // camera / input
    flips("mouseZoomSpeed",  [](Settings& s) { s.mouseZoomSpeed += 0.5f; });
    flips("edgeScrollSpeed", [](Settings& s) { s.edgeScrollSpeed += 0.5f; });
    flips("edgeScroll",      [](Settings& s) { s.edgeScroll = !s.edgeScroll; });
    flips("cursorScale",     [](Settings& s) { s.cursorScale += 1; });
    flips("hardwareCursor",  [](Settings& s) { s.hardwareCursor = !s.hardwareCursor; });
    flips("smoothMotion",    [](Settings& s) { s.smoothMotion = !s.smoothMotion; });

    // identity / session
    flips("playerName",      [](Settings& s) { s.playerName = "someone"; });
    flips("accountName",     [](Settings& s) { s.accountName = "someone"; });
    flips("lastMap",         [](Settings& s) { s.lastMap = "Inner Circle"; });
    flips("dataDir",         [](Settings& s) { s.dataDir = "/somewhere/else"; });
    flips("dataManifest",    [](Settings& s) { s.dataManifest = "deadbeef"; });

    // CONTROL: two untouched defaults must compare EQUAL. Without this the whole file
    // would still pass if operator== were simply `return false`.
    {
        Settings a{}, b{};
        const bool same = (a == b);
        std::printf("  %-22s %s\n", "(control) equal", same ? "ok" : "FAIL");
        if (!same) ++g_fail;
    }

    std::printf(g_fail ? "settings_test: %d FAILURE(S)\n" : "settings_test: all passed\n",
                g_fail);
    return g_fail ? 1 : 0;
}
