#pragma once

// The display VM's GET / GET_UNIT_VALUE answers (GameView::registerUnit's
// onGet), read from the pinned render snapshot. TotalA.exe's GET (0x480770)
// answers ids 1..20 only: the jump table takes id - 1 up to 0x13 (0x480781)
// and everything else -- 0, a negative id, any id past 20 -- returns 0
// (0x480abb). The simulation's host (World::ScriptHost::get) keeps that
// range; so does this one, so a script reading Kingdoms' 27..46 (heading,
// terrain, animation speeds, veteran level, standing order) sees 0 in both.

#include "client/renderframe.h"

#include <cstdint>

namespace ta::client {

inline int32_t displayScriptGet(const UnitR& su, int32_t id) {
    switch (id) {
        case 1:  return su.buildQueue.empty() ? 0 : 1;   // ACTIVATION
        case 4:  return su.scriptHealthPercent;          // HEALTH %
        case 6:  return su.moving() ? 1 : 0;             // BUSY
        case 9: {                                        // UNIT_XZ
            const int32_t x = int32_t(su.x) & 0xFFFF;
            const int32_t z = int32_t(su.z) & 0xFFFF;
            return (x << 16) | z;
        }
        case 17: return su.constructionPercentLeft;      // BUILD_PERCENT_LEFT
        // The authoritative yard handshake, a blocked transition and a
        // successful close (zero) included.
        case 18: return su.yardOpen;                     // YARD_OPEN
        default: return 0;
    }
}

} // namespace ta::client
