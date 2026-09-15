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
//
// "Every field" includes the CONTAINERS. The first version of this file said it covered
// every field and quietly skipped knownServers, hotkeys and campaignCompleted, which is
// how DEFAULTS-wipes-campaign-progress got past it.

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
    flips("antiAlias(4x)",   [](Settings& s) { s.antiAlias = 4; });
    flips("buildBarAlign",   [](Settings& s) { s.buildBarAlign = (s.buildBarAlign + 1) % 3; });
    flips("buildBarScale",   [](Settings& s) { s.buildBarScale += 0.25f; });
    flips("bilinear",        [](Settings& s) { s.bilinear = !s.bilinear; });
    flips("treeSway",        [](Settings& s) { s.treeSway = !s.treeSway; });
    flips("unitShadows",     [](Settings& s) { s.unitShadows = !s.unitShadows; });
    flips("smoothArt",       [](Settings& s) { s.smoothArt = !s.smoothArt; });
    flips("videoDeblock",    [](Settings& s) { s.videoDeblock = !s.videoDeblock; });
    flips("healthBars",      [](Settings& s) { s.healthBars = (s.healthBars + 1) % 3; });
    flips("statsPanel",      [](Settings& s) { s.statsPanel = !s.statsPanel; });

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

    // Container fields. The first version of this test skipped these while its header
    // claimed to cover every field -- a coverage claim is worth no more than the cases
    // behind it, and these three are exactly where DEFAULTS was destroying real user
    // data (campaign progress and the server list).
    flips("knownServers",    [](Settings& s) { s.knownServers.push_back("host:7777"); });
    flips("hotkeys",         [](Settings& s) { s.hotkeys["selectAll"] = "ctrl+a"; });
    flips("campaignCompleted", [](Settings& s) { s.campaignCompleted["aramon"].insert(3); });
    // ...and a second entry in the same container, so a comparison that only checks
    // emptiness rather than contents is caught too.
    {
        Settings a{}, b{};
        a.campaignCompleted["aramon"].insert(1);
        b.campaignCompleted["aramon"].insert(2);
        const bool noticed = !(a == b);
        std::printf("  %-22s %s\n", "campaign contents",
                    noticed ? "ok" : "FAIL (compares size/emptiness only)");
        if (!noticed) ++g_fail;
    }
    {
        Settings a{}, b{};
        a.knownServers.push_back("one:1");
        b.knownServers.push_back("two:2");
        const bool noticed = !(a == b);
        std::printf("  %-22s %s\n", "server contents",
                    noticed ? "ok" : "FAIL (compares size/emptiness only)");
        if (!noticed) ++g_fail;
    }

    // ---- what DEFAULTS does ----
    //
    // operator== coverage alone would NOT have caught the bug this section exists for:
    // DEFAULTS was erasing campaign progress and the remembered server list, and a
    // comparison test cannot see that, because the erasure happened in the reset handler.
    // preferenceDefaults() is now the single definition of the reset, so it can be tested
    // directly.
    {
        std::printf("what DEFAULTS preserves vs resets:\n");
        Settings cur{};
        // Records and configuration that must SURVIVE a reset.
        cur.campaignCompleted["aramon"].insert(4);
        cur.knownServers.push_back("friend:7777");
        cur.dataDir = "/games/kingdoms";
        cur.dataManifest = "abc123";
        cur.playerName = "curtis";
        cur.accountName = "curtis";
        cur.lastMap = "Inner Circle";
        cur.hotkeys["selectAll"] = "ctrl+a";
        // Preferences that must be RESET.
        cur.unitShadows = false;
        cur.smoothArt = true;
        cur.videoDeblock = true;
        cur.masterVol = 7;
        cur.uiScale = 1.75f;
        cur.bilinear = true;

        const Settings d = tak::preferenceDefaults(cur);
        auto keep = [&](const char* what, bool ok) {
            std::printf("  keeps %-18s %s\n", what, ok ? "ok" : "FAIL (destroyed by DEFAULTS)");
            if (!ok) ++g_fail;
        };
        auto reset = [&](const char* what, bool ok) {
            std::printf("  resets %-17s %s\n", what, ok ? "ok" : "FAIL (not reset)");
            if (!ok) ++g_fail;
        };
        keep("campaign progress", d.campaignCompleted == cur.campaignCompleted &&
                                  d.missionCompleted("aramon", 4));
        keep("known servers",     d.knownServers == cur.knownServers);
        keep("dataDir",           d.dataDir == cur.dataDir);
        keep("dataManifest",      d.dataManifest == cur.dataManifest);
        keep("playerName",        d.playerName == cur.playerName);
        keep("accountName",       d.accountName == cur.accountName);
        keep("lastMap",           d.lastMap == cur.lastMap);
        keep("hotkeys",           d.hotkeys == cur.hotkeys);

        const Settings fresh{};
        reset("unitShadows",      d.unitShadows == fresh.unitShadows);
        reset("smoothArt",        d.smoothArt == fresh.smoothArt);
        reset("videoDeblock",     d.videoDeblock == fresh.videoDeblock);
        reset("masterVol",        d.masterVol == fresh.masterVol);
        reset("uiScale",          d.uiScale == fresh.uiScale);
        reset("bilinear",         d.bilinear == fresh.bilinear);

        // And the two questions must agree: after a reset we ARE at defaults, which is
        // the invariant that broke when the preserve list and atDefaults() diverged.
        const bool agree = (d == tak::preferenceDefaults(d));
        std::printf("  %-23s %s\n", "reset => atDefaults",
                    agree ? "ok" : "FAIL (reset does not settle at defaults)");
        if (!agree) ++g_fail;
    }

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
