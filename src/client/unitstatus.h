#pragma once
// The information bar's status line for one selected unit (GameView's
// ActionText), split out of the GameView so a test can read it.
#include "client/renderframe.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tak::client {

// The HUD's self-destruct countdown in seconds from the sim's ticks
// (Unit::selfDestructT): -1 when not armed. The wait after the countdown's
// last draw (-2 and below) reads as 0.
inline float selfDestructSeconds(int32_t ticks) {
    if (ticks == -1) return -1.0f;
    return ticks < 0 ? 0.0f : float(ticks) / 30.0f;
}

// The panel's name line (UnitText, 4b2513..4b2551): in a multiplayer game
// (map mode 3) a commander (UnitDef +0x264 & 0x40000; `showplayername`,
// 0x20000, which no shipped unit sets, is not parsed here) shows its owner's
// player name (owner record +0x54) instead of its own name.
inline const std::string& panelName(const UnitR& u, bool multiplayer, const std::string& ownerName) {
    if (multiplayer && u.type && u.type->commander && !ownerName.empty()) return ownerName;
    return u.displayName();
}

// The panel's veteran mark (the .gui's Experience, UnitLevels frames) for a
// rank (getLevel 519310): 4b26c0..4b26ef shows frame _ftol(rank / 10.0 * 3) - 1
// and hides the gadget when that is 0, so ranks 0-3 show nothing, 4-6 frame 0,
// 7-9 frame 1 and 10 frame 2. -1 for none. (rank * 3) / 10 chops alike for
// every rank 0..10.
inline int experienceFrame(int rank) {
    return rank > 0 ? (rank * 3) / 10 - 1 : -1;
}

// Ctrl+D on a selection (48e2b1..48e309): when any selected unit already
// counts down, the press removes every countdown and arms none; otherwise
// it arms them all. Each Cmd::Destroy toggles one unit, so this answers the
// units to send it to, from (id, armed) for each of the player's selected
// units.
inline std::vector<int> selfDestructPress(const std::vector<std::pair<int, bool>>& units) {
    bool anyArmed = false;
    for (const auto& [id, armed] : units) anyArmed |= armed;
    std::vector<int> out;
    for (const auto& [id, armed] : units)
        if (armed == anyArmed) out.push_back(id);
    return out;
}

// The HUD notice while a self-destruct counts down: the whole seconds left on
// the longest countdown among the units (selfDestructSeconds, or the type's
// selfdestructcountdown before the order reaches the sim), never below 1 (the
// last wait reads as its final second).
inline std::string selfDestructNotice(float seconds) {
    int whole = int(seconds);
    if (float(whole) < seconds) ++whole;
    return "SELF-DESTRUCT IN " + std::to_string(whole < 1 ? 1 : whole) + "...";
}

// Retail's own mission labels, from translate/unitmissions.tdf -- the table
// keyed by the UNITMISSIONCODE_* strings the engine's command table carries.
// Several of these used to be reasonable-sounding paraphrases that retail
// simply does not use: it says "Clearing" and not RECLAIMING, "Loading" and
// not BOARDING, and a half-built unit is an "Intangible Mass".
inline const char* unitStatusLabel(const UnitR& u) {
    if (!u.type) return "";
    // UNITMISSIONCODE_SELFDESTRUCT. First, because it is what the unit is
    // doing -- it has stopped taking orders and is walking out on you.
    if (u.selfDestructT >= 0.0f) return "LEAVING YOUR COMMAND";
    if (u.stonedFor > 0) return "PETRIFIED";
    if (u.frozenFor > 0) return "FROZEN";
    if (u.paralyzedFor > 0) return "PARALYZED";
    if (u.underConstruction) return "INTANGIBLE MASS";
    // Only a unit that can be switched off is ever "off": every type without
    // activatewhenbuilt is created with its activation bit clear (4c033e,
    // default 0), the archer as much as the gate.
    if (u.type->onOffable && !u.active) return "INACTIVE";
    if (u.repairId != 0) return "REPAIRING";
    // UNITMISSIONCODE_RESURRECT: the walk to the body (its route's
    // waypoints lead the leg) and the channel.
    for (const auto& o : u.orders)
        if (o.goal) { if (o.revive) return "RAISING DEAD"; break; }
    if (u.reclaimId != 0) return "CLEARING";
    if (u.buildSiteId != 0 || !u.buildQueue.empty()) return "CONJURING";
    if (!u.orders.empty()) {
        const auto& o = u.orders.front();
        if (o.guard) return "GUARDING";
        if (o.patrol) return "PATROLLING";
        if (o.load) return "LOADING";
        if (o.unload) return "UNLOADING";
        if (o.reclaimFeat || o.reclaimArea) return "CLEARING AREA";
        if (o.targetId != 0) return "ATTACKING";
        if (o.attackMove) return "SEEKING TO ATTACK";
        return "MOVING";
    }
    if (u.cloaked) return "CLOAKING";
    return "STANDBY";   // retail's idle label
}

} // namespace tak::client
