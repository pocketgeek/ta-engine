#pragma once
// The information bar's status line for one selected unit (GameView's
// ActionText), split out of the GameView so a test can read it.
#include "client/renderframe.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ta::client {

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

// TotalA.exe's mission status lines: each mission table row (0x4fc490..,
// stride 0x19: status text, handler, ..., name) carries the line its mission
// shows while current, and RECLAIMUNIT sets "Reclaiming" itself (0x4047ba ->
// 0x438880). Shown in capitals, as the bar draws every line.
//   SelfDestruct "SELF DESTRUCT ENGAGED"  Paralyze "Paralyzed"
//   GetBuilt "Under construction"         BeCarried "Being transported"
//   RepairUnit "Repairing"                Resurrect "Resurrecting"
//   Reclaim / ReclaimUnit "Reclaiming"    MobileBuild / HelpBuild /
//   BuildingBuild "Nanolathing"           Follow_Ground "Guarding"
//   Patrol "Patrolling"                   Ground_Pickup "Loading"
//   Ground_Unload "Unloading"             AttackSpecial "Annihilating"
//   Suppress "Suppressing fire"           Attack_Chase "Attacking"
//   Park "Parking"                        VTOL_Landing "Landing"
//   Move_Ground "Moving"                  Standby "Standby"
inline const char* unitStatusLabel(const UnitR& u) {
    if (!u.type) return "";
    if (u.selfDestructT >= 0.0f) return "SELF DESTRUCT ENGAGED";
    if (u.paralyzedFor > 0) return "PARALYZED";
    if (u.underConstruction) return "UNDER CONSTRUCTION";
    if (u.inTransport != 0) return "BEING TRANSPORTED";
    if (u.repairId != 0) return "REPAIRING";
    for (const auto& o : u.orders)
        if (o.goal) { if (o.revive) return "RESURRECTING"; break; }
    if (u.reclaimId != 0) return "RECLAIMING";
    if (!u.orders.empty() && (u.orders.front().reclaimUnit || u.orders.front().reclaimFeat ||
                              u.orders.front().reclaimArea)) return "RECLAIMING";
    if (u.buildSiteId != 0 || !u.buildQueue.empty()) return "NANOLATHING";
    if (!u.orders.empty()) {
        const auto& o = u.orders.front();
        if (o.guard) return "GUARDING";
        if (o.patrol) return "PATROLLING";
        if (o.load) return "LOADING";
        if (o.unload) return "UNLOADING";
        if (o.dgun) return "ANNIHILATING";
        if (o.suppress) return "SUPPRESSING FIRE";
        if (o.targetId != 0) return "ATTACKING";
        if (o.park) return "PARKING";
        if (o.landing) return "LANDING";
        return "MOVING";
    }
    return "STANDBY";
}

} // namespace ta::client
