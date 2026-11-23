// room_changes_test -- client/roomchanges.h: what the game room shows of the host's changes.
// The room screen diffs each new LobbyState against the last one it saw; this drives the
// pure diff and the note line's text.
//
//  - each kind of change: the map, MAP SCENARIO, one rule, several rules, the players, the
//    game mods; the name and the password are set when the game is made, and show nothing
//  - " -- READY AGAIN" only when the guest's READY was cleared by a new revision
//  - AIs removed by a smaller map, told apart from seats the host closed by hand
//  - the player the server moved down, told apart from a MoveSeat of its own
//  - the host's own changes: no rule notes, only the seats a smaller map took and the
//    guests it must wait for again
#include "client/roomchanges.h"
#include "net/client.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace tak;
using net::RoomView;

static int g_fail = 0, g_checks = 0;
static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_fail; std::printf("  FAIL %s\n", what); }
}
static void checkNote(const std::string& got, const std::string& want, const char* what) {
    ++g_checks;
    if (got != want) { ++g_fail; std::printf("  FAIL %s: got \"%s\", want \"%s\"\n", what, got.c_str(), want.c_str()); }
}

// A room as a guest sees it: the host in seat 0, this guest in seat 1 (READY), the rest
// OPEN up to the capacity and CLOSED past it.
static RoomView room(int capacity = 8) {
    RoomView r;
    r.id = 7; r.name = "alice's Game"; r.mapId = "athri cay"; r.hostId = 1; r.mapsReady = true;
    r.capacity = uint8_t(capacity); r.roomRev = 3; r.changeBits = 0; r.mySlot = 1;
    for (int i = 0; i < net::kMaxSlots; ++i) {
        r.slots[i] = net::SlotInfo{};
        r.slots[i].type = i < capacity ? 0 : 3;
        r.slots[i].color = uint8_t(i); r.slots[i].team = uint8_t(i);
    }
    r.slots[0].type = 1; r.slots[0].name = "alice"; r.slots[0].ready = 1;
    r.slots[1].type = 1; r.slots[1].name = "bob"; r.slots[1].ready = 1;
    return r;
}
// The same room after a host change the server counted: the next revision, its bits, and
// every guest's READY cleared.
static RoomView bumped(RoomView r, uint8_t bits) {
    ++r.roomRev; r.changeBits = bits;
    for (int i = 1; i < net::kMaxSlots; ++i) if (r.slots[i].type == 1) r.slots[i].ready = 0;
    return r;
}
static bool has(const std::vector<RoomField>& v, RoomField f) { return std::find(v.begin(), v.end(), f) != v.end(); }

int main() {
    // ---- each kind of change, as a guest sees it ----
    {
        const RoomView a = room();
        RoomView b = bumped(a, net::kChangeMap); b.mapId = "inner circle";
        const auto c = roomChanges(a, b);
        check(c.bits == net::kChangeMap && has(c.options, RoomField::Map), "a new map is the map's change");
        check(c.unreadied && c.othersUnreadied == 0, "the guest's READY went with it");
        checkNote(roomChangeNote(c, false), "HOST CHANGED THE MAP -- READY AGAIN", "map note");
    }
    {
        const RoomView a = room();
        RoomView b = bumped(a, net::kChangeScenario); b.opts.mapScenario = 1;
        auto c = roomChanges(a, b);
        check(has(c.options, RoomField::Scenario) && c.scenarioOn, "MAP SCENARIO turned on");
        checkNote(roomChangeNote(c, false), "HOST TURNED MAP SCENARIO ON -- READY AGAIN", "scenario on note");
        RoomView d = bumped(b, net::kChangeScenario); d.opts.mapScenario = 0;
        c = roomChanges(b, d);
        checkNote(roomChangeNote(c, false), "HOST TURNED MAP SCENARIO OFF", "scenario off note (nobody was ready)");
    }
    {
        const RoomView a = room();
        RoomView b = bumped(a, net::kChangeOptions); b.opts.fogExplored = 2;
        const auto c = roomChanges(a, b);
        check(c.options == std::vector<RoomField>{RoomField::Fog}, "one rule: its cell alone");
        check(c.seats.empty(), "a READY cleared is no change of seat");
        checkNote(roomChangeNote(c, false), "HOST CHANGED FOG OF WAR -- READY AGAIN", "one rule names it");
    }
    {
        const RoomView a = room();
        RoomView b = bumped(a, net::kChangeOptions); b.opts.crusades = 1; b.opts.unitCap = 500;
        const auto c = roomChanges(a, b);
        check(has(c.options, RoomField::Crusades) && has(c.options, RoomField::UnitCap) && c.options.size() == 2,
              "two rules: both cells");
        checkNote(roomChangeNote(c, false), "HOST CHANGED THE RULES -- READY AGAIN", "several rules");
    }
    {
        const RoomView a = room();
        RoomView b = bumped(a, net::kChangePlayers); b.slots[3].type = 2;
        const auto c = roomChanges(a, b);
        check(c.seats == std::vector<int>{3} && c.options.empty(), "an AI seated: its row");
        check(c.aiRemoved == 0, "no AI removed");
        checkNote(roomChangeNote(c, false), "HOST CHANGED THE PLAYERS -- READY AGAIN", "players note");
    }
    {
        const RoomView a = room();
        const auto c = roomChanges(a, bumped(a, net::kChangeMods));
        check(c.bits == net::kChangeMods && c.options.empty() && c.seats.empty(), "the game mods: bits alone");
        checkNote(roomChangeNote(c, false), "HOST CHANGED THE GAME MODS -- READY AGAIN", "mods note");
    }
    {
        // The name and the password are set when the game is made: a room that differs
        // only there has nothing to light and no note.
        const RoomView a = room();
        RoomView b = a; b.name = "Friday"; b.passworded = true;
        const auto c = roomChanges(a, b);
        check(!c.bits && c.options.empty() && c.seats.empty() && !c.unreadied, "the name and the password: no change");
        checkNote(roomChangeNote(c, false), "", "... and no note");
    }
    {
        // The map outranks the rules it came with (MAP SCENARIO off, START LOCATIONS back).
        const RoomView a = room();
        RoomView b = bumped(a, net::kChangeMap | net::kChangeCapacity); b.mapId = "inner circle"; b.opts.randomStarts = 1;
        checkNote(roomChangeNote(roomChanges(a, b), false), "HOST CHANGED THE MAP -- READY AGAIN", "the map first");
    }

    // ---- READY AGAIN only with a new revision ----
    {
        // The guest clicked UNREADY itself: no revision, nothing to say.
        const RoomView a = room();
        RoomView b = a; b.slots[1].ready = 0;
        const auto c = roomChanges(a, b);
        check(!c.unreadied && !c.bits && c.seats.empty(), "the guest's own UNREADY is no change");
        checkNote(roomChangeNote(c, false), "", "no note for the guest's own UNREADY");
        // A guest not ready before is not unreadied.
        RoomView d = bumped(b, net::kChangeOptions); d.opts.crusades = 1;
        checkNote(roomChangeNote(roomChanges(b, d), false), "HOST CHANGED CRUSADES", "not ready before: no READY AGAIN");
        // Another guest changing its side moves no revision: nothing lights.
        RoomView e = a; e.slots[1].faction = 3;
        const auto g = roomChanges(a, e);
        check(!g.bits && g.seats == std::vector<int>{1}, "a guest's own edit: its row changed, no revision");
        checkNote(roomChangeNote(g, false), "", "no note for a guest's own edit");
    }

    // ---- a smaller map: AIs removed, versus seats the host closes ----
    {
        RoomView a = room(6);
        for (int i = 2; i < 6; ++i) a.slots[i].type = 2;
        RoomView b = bumped(a, net::kChangeMap | net::kChangeCapacity);
        b.mapId = "two starts"; b.capacity = 2;
        for (int i = 2; i < net::kMaxSlots; ++i) b.slots[i].type = 3;
        const auto c = roomChanges(a, b);
        check(c.aiRemoved == 4 && c.capacity == 2, "four AIs past the new capacity");
        checkNote(roomChangeNote(c, false), "4 AI PLAYERS LEFT: THE MAP HAS 2 STARTS -- READY AGAIN", "removal note (guest)");
        RoomView ha = a, hb = b; ha.mySlot = hb.mySlot = 0;   // the host's view of it
        checkNote(roomChangeNote(roomChanges(ha, hb), true), "4 AI PLAYERS LEFT: THE MAP HAS 2 STARTS -- PLAYERS MUST READY AGAIN",
                  "removal note (host)");
        RoomView one = a; one.slots[3].type = one.slots[4].type = one.slots[5].type = 0;
        RoomView oneAfter = bumped(one, net::kChangeMap | net::kChangeCapacity);
        oneAfter.capacity = 2; for (int i = 2; i < net::kMaxSlots; ++i) oneAfter.slots[i].type = 3;
        one.slots[1].ready = 0;   // a guest not ready to begin with: no READY AGAIN
        checkNote(roomChangeNote(roomChanges(one, oneAfter), false), "1 AI PLAYER LEFT: THE MAP HAS 2 STARTS", "one AI");
    }
    {
        // The host closes an AI's seat by hand: the capacity stays, so no AI "left".
        RoomView a = room();
        a.slots[4].type = 2;
        RoomView b = bumped(a, net::kChangePlayers); b.slots[4].type = 3;
        const auto c = roomChanges(a, b);
        check(c.aiRemoved == 0 && c.seats == std::vector<int>{4}, "a seat closed by hand removes no AI");
        checkNote(roomChangeNote(c, false), "HOST CHANGED THE PLAYERS -- READY AGAIN", "closed by hand");
        RoomView ha = a, hb = b; ha.mySlot = hb.mySlot = 0;
        checkNote(roomChangeNote(roomChanges(ha, hb), true), "PLAYERS MUST READY AGAIN", "the host waits for the guest");
    }

    // ---- the player moved by a smaller map, versus its own MoveSeat ----
    {
        RoomView a = room();
        a.slots[1] = net::SlotInfo{}; a.slots[1].color = 1; a.slots[1].team = 1;
        a.slots[5].type = 1; a.slots[5].name = "bob"; a.slots[5].ready = 1; a.mySlot = 5;
        RoomView b = bumped(a, net::kChangeMap | net::kChangeCapacity);
        b.capacity = 4; b.mapId = "four starts";
        std::swap(b.slots[1], b.slots[5]);
        for (int i = 4; i < net::kMaxSlots; ++i) b.slots[i] = net::SlotInfo{}, b.slots[i].type = 3;
        b.mySlot = 1;
        const auto c = roomChanges(a, b);
        check(c.movedTo == 1, "moved down to seat 2");
        check(c.unreadied, "a moved player's READY is cleared");
        checkNote(roomChangeNote(c, false), "YOU MOVED TO SEAT 2: THE MAP HAS 4 STARTS -- READY AGAIN", "moved note");
        // Its own MoveSeat: the capacity stays.
        RoomView m = room(); m.slots[1].ready = 0;
        RoomView n = m; std::swap(n.slots[1], n.slots[3]); n.slots[1].type = 0; n.slots[1].name.clear(); n.mySlot = 3;
        check(roomChanges(m, n).movedTo == -1, "a MoveSeat of its own is no move by the server");
    }

    // ---- the host's own changes ----
    {
        RoomView a = room(); a.mySlot = 0;   // the host's view
        RoomView b = bumped(a, net::kChangeOptions); b.opts.fogExplored = 0;
        auto c = roomChanges(a, b);
        check(!c.unreadied && c.othersUnreadied == 1, "the host's READY stays; the guest's went");
        checkNote(roomChangeNote(c, true), "PLAYERS MUST READY AGAIN", "the host is told to wait");
        RoomView d = bumped(b, net::kChangeOptions); d.opts.crusades = 1;   // nobody ready now
        checkNote(roomChangeNote(roomChanges(b, d), true), "", "no rule notes for the host's own change");
    }

    // ---- another room: nothing to compare ----
    {
        const RoomView a = room();
        RoomView b = bumped(a, net::kChangeMap); b.id = 8; b.mapId = "inner circle";
        const auto c = roomChanges(a, b);
        check(!c.bits && c.options.empty() && c.seats.empty() && !c.unreadied, "another room is no change");
        checkNote(roomChangeNote(c, false), "", "no note across rooms");
    }

    check(std::string(roomFieldCaption(RoomField::Fog)) == "FOG OF WAR" &&
          std::string(roomFieldCaption(RoomField::Pathfinding)) == "PATHFINDING", "captions");

    std::printf("room_changes: %d/%d checks passed\n", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}
