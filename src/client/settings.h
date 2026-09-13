#pragma once

// User-adjustable Options, persisted to a per-user config file. These are ALL
// local display / input / audio preferences -- none of them feed the deterministic
// sim (World::stateHash) or the net GameOptions, so they may differ per client and
// change live. Keep it that way: never include this from src/sim, src/net, or
// src/server. (Net-synced, hashed choices -- game speed, unit cap, crusades, FFA --
// live in GameOptions/MatchConfig, not here.)

#include <map>
#include <set>
#include <string>
#include <vector>

namespace tak {

// Completion index for a campaign's alternate-ending branch (it has no numbered slot).
inline constexpr int kAltMission = -2;

struct Settings {
    // ---- display / window ----
    bool  fullscreen = true;       // borderless-desktop fullscreen (default on)
    bool  vsync      = true;
    int   maxFps     = 60;         // frame cap when vsync is off; clamp 30..480
    float uiScale    = 1.0f;       // in-game HUD scale; 0.75..2.0 (1.0 = 100%)
    int   antiAlias  = 0;          // scene supersampling: 0=off, 2=on (2x)
    int   buildBarAlign = 1;       // conjure/build icon row: 0=left, 1=center, 2=right
    float buildBarScale = 1.0f;    // extra scale on the build icon row, ON TOP of uiScale; 0.75..2.0
    bool  bilinear   = false;      // smooth (bilinear) terrain + feature scaling, like retail's option
    bool  treeSway   = true;       // trees sway in the wind (beyond-retail nicety; display-only)
    int   healthBars = 1;          // unit health bars: 0=off, 1=only when damaged, 2=always

    // ---- audio (0..256, matching SoundBank's internal scale) ----
    int   masterVol  = 256;        // global gain over everything
    int   bgmVol      = 128;       // background music (SoundBank music + MenuMusic); 50%
    int   sfxVol      = 256;       // unit / world / UI sound effects
    float chanGain[8] = {1, 1, 1, 1, 1, 1, 1, 1};   // per-output-speaker trim, 0..1
    std::string audioDevice;       // output device NAME; "" = system default. Falls back
                                   // to system default if the saved name is gone at startup.

    // ---- camera / input ----
    float mouseZoomSpeed  = 1.0f;  // wheel-zoom sensitivity; 0.25..4.0
    float edgeScrollSpeed = 1.0f;  // edge-scroll rate;       0.25..4.0
    bool  edgeScroll      = true;  // pan when the cursor is at a screen edge
    int   cursorScale     = 1;     // custom mouse-cursor size multiplier; 1..8 (1 = retail size)
    bool  hardwareCursor  = false; // OS-tracked cursor: stays smooth when the game hitches
    bool  smoothMotion    = true;  // interpolate unit motion between 30Hz sim ticks (glide, not step)

    // ---- misc ----
    std::string playerName;        // default name for multiplayer
    std::string accountName;       // last multiplayer account signed in with. The
                                   // PASSWORD is deliberately not here and is never
                                   // written to disk -- see src/net/auth.h.
    std::string lastMap;           // last map picked in the create/SP lobby (remembered)
    // Servers that connected successfully (most recent first, capped at 8). The
    // menu's CONNECT dropdown lists these under the default server.
    std::vector<std::string> knownServers;

    // ---- data / install location ----
    std::string dataDir;           // retail install folder (picked once; re-checked at launch)
    std::string dataManifest;      // authenticity digest of the root HPIs when last validated
                                   // (hpi::rootManifest); a mismatch re-runs validation

    // ---- hotkeys ----
    // Rebindable in-game key chords, by action id (see src/client/hotkeys). Only
    // bindings that DIFFER from the factory default are stored ("NONE" = an explicit
    // unbind); anything absent uses the default, so new defaults propagate.
    std::map<std::string, std::string> hotkeys;

    // ---- campaign progress ----
    // Which missions the player has COMPLETED, per campaign (id -> set of 0-based
    // mission indices; the alternate ending is kAltMission). Keyed by Campaign::id
    // (the lowercased camps/*.tdf stem). Missions are NEVER locked -- this only records
    // what's been beaten, for the DONE marker and the "play next" hint. See src/campaign.
    std::map<std::string, std::set<int>> campaignCompleted;
    bool missionCompleted(const std::string& id, int mission) const {
        auto it = campaignCompleted.find(id);
        return it != campaignCompleted.end() && it->second.count(mission) > 0;
    }
    int completedCount(const std::string& id) const {
        auto it = campaignCompleted.find(id);
        return it == campaignCompleted.end() ? 0 : int(it->second.size());
    }

    friend bool operator==(const Settings& a, const Settings& b) {
        for (int i = 0; i < 8; ++i) if (a.chanGain[i] != b.chanGain[i]) return false;
        return a.fullscreen == b.fullscreen && a.vsync == b.vsync && a.maxFps == b.maxFps
            && a.uiScale == b.uiScale && a.antiAlias == b.antiAlias
            && a.buildBarAlign == b.buildBarAlign && a.buildBarScale == b.buildBarScale
            && a.bilinear == b.bilinear && a.treeSway == b.treeSway
            && a.healthBars == b.healthBars
            && a.masterVol == b.masterVol && a.bgmVol == b.bgmVol && a.sfxVol == b.sfxVol
            && a.mouseZoomSpeed == b.mouseZoomSpeed && a.edgeScrollSpeed == b.edgeScrollSpeed
            && a.edgeScroll == b.edgeScroll && a.cursorScale == b.cursorScale
            && a.hardwareCursor == b.hardwareCursor && a.smoothMotion == b.smoothMotion
            && a.playerName == b.playerName && a.accountName == b.accountName
            && a.lastMap == b.lastMap
            && a.knownServers == b.knownServers
            && a.audioDevice == b.audioDevice
            && a.hotkeys == b.hotkeys
            && a.campaignCompleted == b.campaignCompleted;
    }
    friend bool operator!=(const Settings& a, const Settings& b) { return !(a == b); }
};

// The config file path (SDL_GetPrefPath based). Empty only if SDL can't provide one.
std::string settingsPath();

// Read the config file; returns defaults for anything missing/corrupt (never throws).
Settings loadSettings();

// Atomically write the config file. Returns false on I/O failure.
bool saveSettings(const Settings&);

}  // namespace tak
