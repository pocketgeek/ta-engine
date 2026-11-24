#pragma once
#include <cstdint>

namespace ta {
// TotalA.exe's under-attack alert: unit speech event 2 ("underattack", text "Under
// Attack"; row 0x50870c of the event table 0x5086dc, stride 0x18: priority 9,
// cooldown 20 s). Every damage record runs the hit response 0x406f80, which ends by
// asking for it for the unit hit (0x4071d8 -> 0x47f850); 0x47f850 passes only a unit
// of the local player (+0xff == game +0x2a43) that is NOT in the list of units on
// screen (0x48bcb0, game +0x1435f), and the speech queue 0x47fad0 drops it while the
// game tick is below the event's next-allowed tick (0x47faf0) or one is queued
// (0x47fb0a). Playing it (0x47fd70) sets that tick to now + 20 x 30 (0x47feee..
// 0x47fefd): the cooldown is 600 GAME ticks from the last alert heard, not from the
// last hit.
struct MonarchAlert {
    static constexpr uint32_t kCooldownTicks = 20 * 30;
    uint32_t nextTick = 0;
    bool admit(int unitPlayer, int localPlayer, bool spectator, bool onScreen, uint32_t tick) {
        if (spectator || localPlayer < 0 || unitPlayer != localPlayer || onScreen || tick < nextTick) return false;
        nextTick = tick + kCooldownTicks;
        return true;
    }
};
}
