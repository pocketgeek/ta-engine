// The match standing a room tick reports (srv::TickResult decided / winningTeam /
// fielded): what the server ends a game on when no human is seated. Constructed
// referee worlds, no game data:
//   duel      two teams; one is wiped out: decided on that tick, the other team wins
//   draw      both are wiped out on one tick: decided, nobody wins
//   allies    one team only: never decided, though World::winningTeam() names it
//   late      a second team that fields its first unit later counts from then on
//   carry     the fielded players are the input's: without them a wiped-out team
//             was never there, and the survivor's game reads as a game of one team
//   monarch   with the Monarch not expendable, losing it decides the game
#include "server/roomtick.h"
#include "ai/ai.h"
#include "sim/matchsetup.h"
#include <cstdio>
#include <limits>
#include <vector>
using namespace tak;

namespace {
int failures = 0;
void check(bool ok, const char* label) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", label); ++failures; }
}

struct Room {
    sim::World world;
    sim::TypeRegistry registry;
    std::vector<ai::Controller> ai;
    uint32_t tick = 0;
    uint8_t fielded = 0;   // carried from tick to tick, as the server's Room does
    Room(int players, std::vector<int> teams) {
        world.setSerialThreads(true);
        world.setPlayerCount(players);
        for (int p = 0; p < players; ++p) world.setTeam(p, teams[size_t(p)]);
        world.setTerrain(std::vector<uint8_t>(64 * 64, 100), 64, 64, 20);
    }
    srv::TickResult step(std::vector<net::Command> commands = {}, bool carry = true) {
        srv::TickInput in;
        in.tick = tick++; in.simThreads = 1;
        in.replayBudget = std::numeric_limits<uint64_t>::max();
        in.clientHashes = in.replayTrail = false;
        in.fielded = carry ? fielded : 0;
        in.commands = std::move(commands);
        auto result = srv::simulateRoomTick(world, registry, ai, std::move(in));
        if (carry) fielded = result.fielded;
        return result;
    }
};

net::Command destroy(int player, int unit) {
    net::Command c;
    c.kind = net::Cmd::Destroy; c.player = uint8_t(player); c.unitId = unit;
    return c;
}

// Steps until the standing is decided (at most `ticks`); each tick's report must agree
// with the world it was read from. Returns the deciding result (decided false: never).
srv::TickResult settle(Room& room, int ticks, std::vector<net::Command> first = {}) {
    srv::TickResult result;
    for (int i = 0; i < ticks; ++i) {
        result = room.step(i ? std::vector<net::Command>{} : first);
        if (result.decided) check(result.winningTeam == room.world.winningTeam(), "the winner is the world's winning team");
        else check(result.winningTeam == -1, "no winner is reported before the result");
        if (result.decided) break;
    }
    return result;
}
}  // namespace

int main() {
    sim::UnitType fighter;
    fighter.id = "fighter"; fighter.maxHp = 100; fighter.canMove = true;
    fighter.maxVel = sim::Fixed::fromInt(2); fighter.footX = fighter.footZ = 2;
    fighter.turnRate = fighter.turnInPlaceRate = 1000;
    fighter.selfDestructCountdown = 0;   // a Destroy order lands on its first run
    sim::UnitType monarch = fighter;
    monarch.id = "monarch"; monarch.commander = true;

    {   // duel
        Room room(2, {0, 1});
        const int a = room.world.spawn(&fighter, 100, 100, 0, 0), b = room.world.spawn(&fighter, 600, 600, 0, 1);
        (void)a;
        for (int i = 0; i < 30; ++i) {
            const auto r = room.step();
            check(!r.decided && r.winningTeam == -1, "duel: undecided while both teams stand");
            check(r.fielded == 3, "duel: both players have fielded units");
        }
        const auto r = settle(room, 120, {destroy(1, b)});
        check(r.decided && r.winningTeam == 0, "duel: decided for the survivor's team");
        check(r.defeated[1] && !r.defeated[0], "duel: the loser is the defeated one");
        check(room.step().decided, "duel: the standing holds on later ticks");
    }
    {   // draw
        Room room(2, {0, 1});
        const int a = room.world.spawn(&fighter, 100, 100, 0, 0), b = room.world.spawn(&fighter, 600, 600, 0, 1);
        check(!room.step().decided, "draw: undecided at the start");
        const auto r = settle(room, 120, {destroy(0, a), destroy(1, b)});
        check(r.decided && r.winningTeam == -1, "draw: decided with nobody left, and no winner");
        check(r.defeated[0] && r.defeated[1], "draw: both are defeated");
    }
    {   // allies
        Room room(2, {0, 0});
        room.world.spawn(&fighter, 100, 100, 0, 0);
        const int b = room.world.spawn(&fighter, 600, 600, 0, 1);
        const auto first = room.step();
        check(room.world.winningTeam() == 0 && !first.decided, "allies: one team is no result");
        const auto r = settle(room, 120, {destroy(1, b)});
        check(!r.decided && r.defeated[1], "allies: losing an ally decides nothing");
    }
    {   // late
        Room room(2, {0, 1});
        const int a = room.world.spawn(&fighter, 100, 100, 0, 0);
        for (int i = 0; i < 10; ++i) check(!room.step().decided, "late: a team that has fielded nothing has not lost");
        check(room.fielded == 1 && room.world.winningTeam() == 0, "late: only the first team has fielded units");
        room.world.spawn(&fighter, 600, 600, 0, 1);
        const auto both = room.step();
        check(!both.decided && both.fielded == 3, "late: the second team counts once it fields a unit");
        const auto r = settle(room, 120, {destroy(0, a)});
        check(r.decided && r.winningTeam == 1, "late: decided for the team that arrived later");
    }
    {   // carry
        Room room(2, {0, 1});
        room.world.spawn(&fighter, 100, 100, 0, 0);
        const int b = room.world.spawn(&fighter, 600, 600, 0, 1);
        room.step();
        check(settle(room, 120, {destroy(1, b)}).decided, "carry: decided with the fielded players carried");
        const auto forgotten = room.step({}, /*carry=*/false);
        check(!forgotten.decided && forgotten.fielded == 1, "carry: without them the loser never fielded a unit");
        check(room.step().decided, "carry: and with them the result stands");
    }
    {   // monarch
        Room room(2, {0, 1});
        room.world.setMonarchExpendable(false);
        room.world.spawn(&monarch, 100, 100, 0, 0);
        room.world.spawn(&fighter, 140, 100, 0, 0);
        const int king = room.world.spawn(&monarch, 600, 600, 0, 1);
        room.world.spawn(&fighter, 640, 600, 0, 1);
        check(!room.step().decided, "monarch: undecided at the start");
        const auto r = settle(room, 120, {destroy(1, king)});
        check(r.decided && r.winningTeam == 0, "monarch: losing the Monarch decides the game");
    }
    std::printf("room standing: %d failures\n", failures);
    return failures ? 1 : 0;
}
