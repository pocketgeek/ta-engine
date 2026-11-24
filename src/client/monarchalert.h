#pragma once
#include <cstdint>
#include <string>

namespace ta {
// TotalA.exe's under-attack alert: unit speech event 2 ("underattack", text "Under
// Attack"; row 0x50870c of the event table 0x5086dc, stride 0x18: priority 9,
// cooldown 20 s). Every damage record runs the hit response 0x406f80, which ends by
// asking for it for the unit hit (0x4071d8 -> 0x47f850) unless its current mission
// is a ground attack or the previous hit was its own player's non-weapon damage
// (World::applyHit decides that; only the hits it lists reach here). 0x47f850 passes
// only a unit of the local player (+0xff == game +0x2a43) that is NOT in the list of
// units on screen (0x48bcb0, game +0x1435f), and the speech queue 0x47fad0 drops it
// while the game tick is below the event's next-allowed tick (0x47faf0) or one is
// queued (0x47fb0a).
//
// Playing the entry (0x47fd70): the speech when the event's priority beats 10 - the
// SPEECH level (game +0x37f17, 0/5/10; 0x47fdf9) and the unit's sound class has the
// slot -- and only then is the next-allowed tick set, to now + 20 x 30 (0x47feee..
// 0x47fefd), so the cooldown is 600 GAME ticks from the last alert HEARD; then the
// text line "<unit type name>: Under Attack" ("%s: %s", 0x47ff69) into the message
// area (0x463ca0) when the priority beats 10 - the UNITCHAT level (game +0x37f18,
// 0x47ff26). With speech off nothing starts the cooldown.
struct MonarchAlert {
    static constexpr uint32_t kCooldownTicks = 20 * 30;
    static constexpr int kPriority = 9;
    static constexpr const char* kText = "Under Attack";   // event row +0xc (0x508a64)
    uint32_t nextTick = 0;
    // 0x47f850 and the queue's tick test: the request is taken.
    bool admit(int unitPlayer, int localPlayer, bool spectator, bool onScreen, uint32_t tick) const {
        return !spectator && localPlayer >= 0 && unitPlayer == localPlayer && !onScreen && tick >= nextTick;
    }
    // 0x47fdf9 / 0x47ff26: a level of 0, 5 or 10.
    static bool speaks(int speechLevel) { return kPriority > 10 - speechLevel; }
    static bool shows(int textLevel) { return kPriority > 10 - textLevel; }
    // 0x47feee: the alert was heard.
    void spoken(uint32_t tick) { nextTick = tick + kCooldownTicks; }
    static std::string line(const std::string& unitName) { return unitName + ": " + kText; }
};
}
