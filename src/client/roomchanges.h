#pragma once

// What a guest is shown when the host changes the game room: which lines flash and the
// short note under the controls ("HOST CHANGED FOG OF WAR -- READY AGAIN"). Pure (no SDL),
// so tools/room_changes_test.cpp can drive it; the room screen (gameview_room.cpp) diffs
// each new LobbyState against the last one it saw and draws the result.

#include <cstdint>
#include <string>
#include <vector>

namespace tak::net { struct RoomView; }

namespace tak {

// One line of the room a change can light: the map and its MAP SCENARIO, each cell of the
// options block, the slot table, the mods line.
enum class RoomField : uint8_t { Map, Scenario, Crusades, DoubleSight, UnitCap, Monarch,
    SpeedUnlock, Fog, StartLocations, Pathfinding, PowerCodes, AllowSpectators, Players, Mods, kCount };
const char* roomFieldCaption(RoomField);   // "FOG OF WAR", ... (for the notes)

// What changed between two LobbyStates of one room.
struct RoomChanges {
    uint8_t bits = 0;                  // the server's changeBits for a new roomRev (0: none)
    // Option cells whose value changed: RoomField::Map for a new map, Scenario for MAP
    // SCENARIO, then the rules in the options block's order.
    std::vector<RoomField> options;
    std::vector<int> seats;            // seats whose record changed, READY aside (Players: rows to flash)
    int aiRemoved = 0;                 // AIs gone from seats at or past a lowered capacity
    bool unreadied = false;            // my READY went 1 -> 0 with a new roomRev
    int othersUnreadied = 0;           // other seated humans whose READY went 1 -> 0 with it
    int movedTo = -1;                  // the server moved me (mySlot changed without my MoveSeat)
    int capacity = 0;                  // the room's seats after the change ("THE MAP HAS M STARTS")
    bool scenarioOn = false;           // MAP SCENARIO after the change
};
// Empty when the two are not the same room. A capacity move is told from the player's own
// MoveSeat by the capacity: it fell, and the old seat is past it.
RoomChanges roomChanges(const net::RoomView& before, const net::RoomView& after);
// The note line's text: a guest is told what the host changed (" -- READY AGAIN" when it
// cleared the guest's READY); everyone is told of AIs removed and seats moved by a smaller
// map; the host, of the guests its change unreadied. "" for none.
std::string roomChangeNote(const RoomChanges&, bool host);

}  // namespace tak
