#include "client/roomchanges.h"

#include "net/client.h"   // RoomView

// The room's change display (see roomchanges.h): pure, so tools/room_changes_test.cpp can
// drive it without SDL.

namespace tak {

const char* roomFieldCaption(RoomField field) {
    switch (field) {
    case RoomField::Map: return "THE MAP";
    case RoomField::Scenario: return "MAP SCENARIO";
    case RoomField::Crusades: return "CRUSADES";
    case RoomField::DoubleSight: return "DOUBLE SIGHT/RADAR";
    case RoomField::UnitCap: return "UNIT CAP";
    case RoomField::Monarch: return "MONARCH EXPENDABLE";
    case RoomField::SpeedUnlock: return "ALLOW SPEED CHANGE";
    case RoomField::Fog: return "FOG OF WAR";
    case RoomField::StartLocations: return "START LOCATIONS";
    case RoomField::Pathfinding: return "PATHFINDING";
    case RoomField::PowerCodes: return "POWER CODES";
    case RoomField::AllowSpectators: return "ALLOW SPECTATORS";
    case RoomField::Players: return "THE PLAYERS";
    case RoomField::Mods: return "THE GAME MODS";
    case RoomField::kCount: break;
    }
    return "";
}

RoomChanges roomChanges(const net::RoomView& before, const net::RoomView& after) {
    RoomChanges c;
    c.capacity = after.capacity;
    c.scenarioOn = after.opts.mapScenario != 0;
    if (before.id != after.id) return c;
    const bool newRev = after.roomRev != before.roomRev;
    if (newRev) c.bits = after.changeBits;
    const auto& a = before.opts;
    const auto& b = after.opts;
    const auto changed = [&](RoomField field, bool differs) { if (differs) c.options.push_back(field); };
    changed(RoomField::Map, before.mapId != after.mapId);
    changed(RoomField::Scenario, a.mapScenario != b.mapScenario);
    changed(RoomField::Crusades, a.crusades != b.crusades);
    changed(RoomField::DoubleSight, a.doubleSight != b.doubleSight);
    changed(RoomField::UnitCap, a.unitCap != b.unitCap);
    changed(RoomField::Monarch, a.monarchExpendable != b.monarchExpendable);
    changed(RoomField::SpeedUnlock, a.speedUnlock != b.speedUnlock);
    changed(RoomField::Fog, a.fogExplored != b.fogExplored);
    changed(RoomField::StartLocations, a.randomStarts != b.randomStarts);
    changed(RoomField::Pathfinding, a.pathfindingMode != b.pathfindingMode);
    changed(RoomField::PowerCodes, a.powerCodes != b.powerCodes);
    changed(RoomField::AllowSpectators, a.allowSpectators != b.allowSpectators);
    for (int i = 0; i < net::kMaxSlots; ++i) {
        const auto& s = before.slots[i];
        const auto& t = after.slots[i];
        // READY is left out: every host change clears the guests' READY, and a row that
        // only lost it has not changed.
        if (s.type != t.type || s.name != t.name || s.faction != t.faction || s.color != t.color ||
            s.team != t.team || (t.type == 2 && s.aiLevel != t.aiLevel))
            c.seats.push_back(i);
    }
    // A smaller map: the AIs it emptied (past the new capacity, or displaced by a player
    // moved down). Seats the host closed by hand keep the capacity, so they never count.
    const bool shrank = after.capacity < before.capacity;
    if (shrank)
        for (int i = 0; i < net::kMaxSlots; ++i)
            c.aiRemoved += before.slots[i].type == 2 && after.slots[i].type != 2;
    const int was = before.mySlot, now = after.mySlot;
    const auto readyHuman = [](const net::RoomView& r, int slot) {
        return slot >= 0 && slot < net::kMaxSlots && r.slots[slot].type == 1 && r.slots[slot].ready;
    };
    const auto human = [](const net::RoomView& r, int slot) {
        return slot >= 0 && slot < net::kMaxSlots && r.slots[slot].type == 1;
    };
    if (newRev) {
        c.unreadied = readyHuman(before, was) && human(after, now) && !after.slots[now].ready;
        for (int i = 0; i < net::kMaxSlots; ++i)
            if (i != was && i != now && readyHuman(before, i) && human(after, i) &&
                before.slots[i].name == after.slots[i].name && !after.slots[i].ready)
                ++c.othersUnreadied;
    }
    if (shrank && was >= 0 && now >= 0 && was != now && was >= after.capacity) c.movedTo = now;
    return c;
}

std::string roomChangeNote(const RoomChanges& c, bool host) {
    // Everyone: the seats a smaller map took away.
    std::string seats;
    if (c.movedTo >= 0)
        seats = "YOU MOVED TO SEAT " + std::to_string(c.movedTo + 1) + ": THE MAP HAS " + std::to_string(c.capacity) + " STARTS";
    else if (c.aiRemoved > 0)
        seats = std::to_string(c.aiRemoved) + (c.aiRemoved == 1 ? " AI PLAYER LEFT" : " AI PLAYERS LEFT") +
                ": THE MAP HAS " + std::to_string(c.capacity) + " STARTS";
    if (host) {
        if (!c.othersUnreadied) return seats;
        return seats.empty() ? "PLAYERS MUST READY AGAIN" : seats + " -- PLAYERS MUST READY AGAIN";
    }
    // A guest: what the host changed, the most telling first.
    const auto has = [&](RoomField field) {
        for (RoomField f : c.options) if (f == field) return true;
        return false;
    };
    std::vector<RoomField> rules;
    for (RoomField f : c.options) if (f != RoomField::Map && f != RoomField::Scenario) rules.push_back(f);
    std::string what;
    if ((c.bits & net::kChangeMap) || has(RoomField::Map)) what = "HOST CHANGED THE MAP";
    else if ((c.bits & net::kChangeScenario) || has(RoomField::Scenario))
        what = c.scenarioOn ? "HOST TURNED MAP SCENARIO ON" : "HOST TURNED MAP SCENARIO OFF";
    else if (rules.size() == 1) what = std::string("HOST CHANGED ") + roomFieldCaption(rules.front());
    else if (rules.size() > 1 || (c.bits & net::kChangeOptions)) what = "HOST CHANGED THE RULES";
    else if (c.bits & (net::kChangePlayers | net::kChangeCapacity)) what = "HOST CHANGED THE PLAYERS";
    else if (c.bits & net::kChangeMods) what = "HOST CHANGED THE GAME MODS";
    std::string note = seats.empty() ? what : seats;
    if (c.unreadied) note = note.empty() ? "READY AGAIN" : note + " -- READY AGAIN";
    return note;
}

}  // namespace tak
