#pragma once

// What a watcher (a live spectator, or anyone watching a replay) may do. A watcher can
// look at anything and change nothing, so the hotkey dispatcher asks here, by name,
// instead of each action guarding itself; GameView::watchOnly() says who is a watcher.
// Display only: no command ever leaves a watcher (GameView::issue is the backstop).

#include <cstddef>
#include <span>

#include "client/hotkeys.h"

namespace tak::client {

// The actions that only look. Everything else is an order, or feeds one, and does
// nothing for a watcher: no sound, no notice, no armed cursor.
inline bool watchAllows(Act act) {
    switch (act) {
        case Act::NextUnit: case Act::TrackSelection:
        case Act::SelectAll: case Act::SelectSameType: case Act::SelectOnScreen: case Act::SelectMonarch:
        case Act::SelectBuilders: case Act::SelectFactory: case Act::SelectMelee: case Act::SelectMagic:
        case Act::SelectBoats: case Act::SelectBallistic: case Act::SelectTroops: case Act::SelectArmed:
        case Act::SelectOnScreenType: case Act::SelectFlying:
        case Act::ToggleCounts: case Act::UnitInfo: case Act::FullScreenRadar: case Act::RetailZoom:
        case Act::PushToTalk:
        case Act::Count:   // no action bound to the key: camera keys and the like
            return true;
        default:
            return false;
    }
}

// The one thing a watcher sends: the pause and pace of a private single-player room it
// hosts (watching the AIs), where nobody else plays. Every other watcher's Pause and
// speed keys do nothing at all. Pacing changes no game state.
inline bool watchMayPace(bool singlePlayer, bool hosting) { return singlePlayer && hosting; }

// A watcher's selection shows one player's interface, so it holds one owner: a box
// keeps the owner with the most units in it, ties going to the lower slot. -1 for none.
inline int boxOwner(std::span<const int> owners) {
    int counts[16] = {};
    int best = -1;
    for (const int p : owners) if (p >= 0 && p < 16) ++counts[p];
    for (int p = 0; p < 16; ++p) if (counts[p] && (best < 0 || counts[p] > counts[best])) best = p;
    return best;
}

}  // namespace tak::client
