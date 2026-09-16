// objectives_test -- mission win/lose conditions, and which side each is on.
//
// Data-independent, so CI runs it.
//
// The classification is the whole point. TA states its objectives as plain .ota
// keys and the engine keeps them in TWO lists -- a key's list IS its meaning --
// and the names do not tell you which (docs/retail-engine-ta.md, factory at
// TotalA.exe 0x48e000):
//
//   * `CommanderKilled` and `KillEnemyCommander` are DIFFERENT KEYS on OPPOSITE
//     lists. The common one, 123 of the 272 shipped .ota files, is the DEFEAT.
//   * `AllUnitsKilled` is a defeat: all of YOURS.
//   * `AnyUnitPassesX` is a defeat (an enemy reaches the line) where
//     `UnitTypePassesX` is a victory (your escort reaches it).
//
// Getting any of those backwards inverts a mission, and it would not show up as a
// crash -- the campaign would simply declare the wrong result. The other property
// worth pinning is ARMING: a "destroy all X" rule must not be satisfied at t=0
// before any X exists, or a mission ends on its first tick.

#include "sim/mission.h"
#include "sim/sim.h"
#include "tdf/tdf.h"

#include <cstdio>
#include <memory>
#include <string>

using namespace ta::sim;

static int g_fail = 0;
static void check(bool cond, const std::string& what) {
    std::printf("  %-70s %s\n", what.c_str(), cond ? "ok" : "FAIL");
    if (!cond) ++g_fail;
}
static void eqi(int got, int want, const std::string& what) {
    bool ok = got == want;
    std::printf("  %-70s %s\n", what.c_str(), ok ? "ok" : "FAIL");
    if (!ok) { std::printf("      got %d, want %d\n", got, want); ++g_fail; }
}

static UnitType soldier(const char* id) {
    UnitType t{};
    t.name = id; t.id = id;
    t.maxVel = 60; t.turnRate = 10000; t.canMove = true;
    t.maxHp = 100; t.footX = 2; t.footZ = 2;
    return t;
}
static UnitType commanderType(const char* id) {
    UnitType t = soldier(id);
    t.commander = true;
    return t;
}

// A world with two opposing players: 0 is the human, 1 the enemy.
static World* makeWorld() {
    World* w = new World();
    w->setVisPlayer(-1);
    w->setTerrain(std::vector<uint8_t>(64 * 64, 100), 64, 64, /*seaLevel=*/20);
    w->setPlayerCount(2);
    w->setTeam(0, 0);
    w->setTeam(1, 1);
    return w;
}

static void attach(World& w, const std::string& headerBody) {
    static TypeRegistry empty;   // the conditions below name no unit types
    ta::tdf::Node root = ta::tdf::parseText("[GlobalHeader]\n{\n" + headerBody + "}\n",
                                            "<test>");
    const ta::tdf::Node* gh = root.child("globalheader");
    w.setMission(std::make_unique<MissionScript>(std::vector<uint8_t>{}, *gh, empty,
                                                 /*humanPlayer=*/0, "<test>",
                                                 std::vector<int>{-1, 0, 1}));
}
static void run(World& w, float seconds) {
    for (int i = 0; i < int(seconds * 30.0f); ++i) w.tick(1.0f / 30.0f);
}

int main() {
    std::printf("objectives_test\n");
    UnitType sol = soldier("sol");
    UnitType com = commanderType("com");

    // --- AllUnitsKilled is a DEFEAT: all of YOURS ----------------------------
    {
        World* w = makeWorld();
        const int mine = w->spawn(&sol, 200, 200, 0, /*player=*/0);
        w->spawn(&sol, 900, 900, 0, /*player=*/1);
        attach(*w, "AllUnitsKilled=1;\n");
        run(*w, 0.5f);
        eqi(w->missionOutcome(), 0, "AllUnitsKilled: running while the player has units");
        if (Unit* u = w->unit(mine)) u->hp = 0;    // the player's last unit dies
        run(*w, 0.5f);
        eqi(w->missionOutcome(), -1,
            "AllUnitsKilled fires as a DEFEAT when the player's units are gone");
        delete w;
    }

    // --- ...and it does not fire on the ENEMY being wiped ---------------------
    {
        World* w = makeWorld();
        w->spawn(&sol, 200, 200, 0, 0);
        const int theirs = w->spawn(&sol, 900, 900, 0, 1);
        attach(*w, "AllUnitsKilled=1;\n");
        if (Unit* u = w->unit(theirs)) u->hp = 0;
        run(*w, 0.5f);
        eqi(w->missionOutcome(), 0,
            "wiping the ENEMY does not trigger the player's AllUnitsKilled");
        delete w;
    }

    // --- CommanderKilled (defeat) vs KillEnemyCommander (victory) -------------
    // The two keys that would invert a mission if swapped.
    {
        World* w = makeWorld();
        const int myCom = w->spawn(&com, 200, 200, 0, 0);
        w->spawn(&com, 900, 900, 0, 1);
        attach(*w, "CommanderKilled=1;\n");
        run(*w, 0.5f);
        eqi(w->missionOutcome(), 0, "CommanderKilled: running while ours lives");
        if (Unit* u = w->unit(myCom)) u->hp = 0;
        run(*w, 0.5f);
        eqi(w->missionOutcome(), -1, "CommanderKilled is a DEFEAT when OURS dies");
        delete w;
    }
    {
        World* w = makeWorld();
        w->spawn(&com, 200, 200, 0, 0);
        const int theirCom = w->spawn(&com, 900, 900, 0, 1);
        attach(*w, "KillEnemyCommander=1;\n");
        run(*w, 0.5f);
        eqi(w->missionOutcome(), 0, "KillEnemyCommander: running while theirs lives");
        if (Unit* u = w->unit(theirCom)) u->hp = 0;
        run(*w, 0.5f);
        eqi(w->missionOutcome(), 1, "KillEnemyCommander is a VICTORY when THEIRS dies");
        delete w;
    }
    // ...and neither is satisfied by the other side's commander dying.
    {
        World* w = makeWorld();
        w->spawn(&com, 200, 200, 0, 0);
        const int theirCom = w->spawn(&com, 900, 900, 0, 1);
        attach(*w, "CommanderKilled=1;\n");
        if (Unit* u = w->unit(theirCom)) u->hp = 0;
        run(*w, 0.5f);
        eqi(w->missionOutcome(), 0,
            "CommanderKilled is NOT satisfied by the enemy commander dying");
        delete w;
    }

    // --- KillEnemyCommander ARMS: no enemy commander yet is not a win --------
    // A mission whose enemy commander is spawned later must not be won at t=0.
    {
        World* w = makeWorld();
        w->spawn(&sol, 200, 200, 0, 0);
        attach(*w, "KillEnemyCommander=1;\n");
        run(*w, 1.0f);
        eqi(w->missionOutcome(), 0,
            "KillEnemyCommander does not fire before an enemy commander ever exists");
        const int late = w->spawn(&com, 900, 900, 0, 1);   // arrives later
        run(*w, 0.5f);
        eqi(w->missionOutcome(), 0, "...still running once it arrives");
        if (Unit* u = w->unit(late)) u->hp = 0;
        run(*w, 0.5f);
        eqi(w->missionOutcome(), 1, "...and fires once it is killed");
        delete w;
    }

    // --- AnyUnitPassesX is a DEFEAT: an ENEMY reaching the line -------------
    {
        World* w = makeWorld();
        w->spawn(&sol, 100, 200, 0, 0);            // ours, well past the line
        w->spawn(&sol, 100, 900, 0, 1);            // theirs, short of it
        attach(*w, "AnyUnitPassesX=20;\n");        // line at cell 20
        run(*w, 0.5f);
        eqi(w->missionOutcome(), 0,
            "AnyUnitPassesX: running while no enemy has crossed");
        w->spawn(&sol, 2000, 900, 0, 1);           // an enemy well past it
        run(*w, 0.5f);
        eqi(w->missionOutcome(), -1,
            "AnyUnitPassesX is a DEFEAT when an ENEMY crosses");
        delete w;
    }
    {
        // Our own unit sitting past the line must not lose us the mission.
        World* w = makeWorld();
        w->spawn(&sol, 2000, 200, 0, 0);
        w->spawn(&sol, 100, 900, 0, 1);
        attach(*w, "AnyUnitPassesX=20;\n");
        run(*w, 1.0f);
        eqi(w->missionOutcome(), 0, "...and OUR unit past the line does not");
        delete w;
    }

    // --- a mission declaring nothing never concludes on its own --------------
    {
        World* w = makeWorld();
        w->spawn(&sol, 200, 200, 0, 0);
        attach(*w, "missionname=nothing;\n");
        run(*w, 2.0f);
        eqi(w->missionOutcome(), 0, "a mission with no conditions stays running");
        delete w;
    }

    std::printf(g_fail ? "objectives_test: %d FAILURE(S)\n" : "objectives_test: all passed\n",
                g_fail);
    return g_fail ? 1 : 0;
}
