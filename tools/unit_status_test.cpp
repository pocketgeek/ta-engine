// The information bar's status line (client/unitstatus.h): TotalA.exe's
// mission status texts (the mission table 0x4fc490..).
#include "client/unitstatus.h"
#include "sim/sim.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
using namespace ta::sim;
static void check(bool ok, const std::string& what) { if (!ok) throw std::runtime_error(what); }

static UnitR snapshot(const Unit& u) {
    UnitR r;
    r.id = u.id; r.type = u.type; r.player = u.player;
    r.active = u.active; r.cloaked = u.cloaked; r.underConstruction = u.underConstruction;
    r.selfDestructT = ta::client::selfDestructSeconds(u.selfDestructT);
    r.repairId = u.repairId; r.reclaimId = u.reclaimId; r.buildSiteId = u.buildSiteId;
    r.captureOrders(u);
    return r;
}

int main() try {
    World w;
    w.setPlayerCount(2);
    w.setTerrain(std::vector<uint8_t>(64 * 64, 50), 64, 64, 0);
    // As the parser makes them: no activatewhenbuilt key, so not activated.
    UnitType archer; archer.id = archer.name = "archer"; archer.maxHp = 100;
    archer.maxVel = Fixed::fromInt(2); archer.activateWhenBuilt = false;
    UnitType gate; gate.id = gate.name = "gate"; gate.maxHp = 500;
    gate.onOffable = true; gate.activateWhenBuilt = false;
    const int a = w.spawn(&archer, 300, 300, 0, 0), g = w.spawn(&gate, 600, 600, 0, 0);
    check(!w.unit(a)->active && !w.unit(g)->active, "a type without activatewhenbuilt starts active");
    const std::string idle = ta::client::unitStatusLabel(snapshot(*w.unit(a)));
    check(idle == "STANDBY", "an idle archer reads " + idle + ", not STANDBY");
    w.order(a, 400, 300, false);
    const std::string moving = ta::client::unitStatusLabel(snapshot(*w.unit(a)));
    check(moving == "MOVING", "a moving archer reads " + moving + ", not MOVING");
    const std::string off = ta::client::unitStatusLabel(snapshot(*w.unit(g)));
    check(off == "STANDBY", "a gate switched off reads " + off + ", not STANDBY (TA has no off line)");
    // RECLAIMUNIT's own line (0x4047ba -> 0x438880): "Reclaiming".
    {
        UnitType reclaimer = archer; reclaimer.id = reclaimer.name = "reclaimer";
        reclaimer.isBuilder = true; reclaimer.canReclaim = true; reclaimer.canMove = true; reclaimer.buildDist = 60;
        const int r = w.spawn(&reclaimer, 300, 340, 0, 0);
        w.reclaimUnit(r, g, false);
        const std::string reclaiming = ta::client::unitStatusLabel(snapshot(*w.unit(r)));
        check(reclaiming == "RECLAIMING", "a RECLAIMUNIT reads " + reclaiming + ", not RECLAIMING");
        UnitR site = snapshot(*w.unit(g)); site.underConstruction = true;
        check(std::string(ta::client::unitStatusLabel(site)) == "UNDER CONSTRUCTION", "GetBuilt's line");
    }
    // 4b2513..4b2551: a multiplayer game names a commander after its player.
    {
        UnitType king; king.id = king.name = "monarch"; king.commander = true;
        UnitR r; r.type = &king;
        using ta::client::panelName;
        check(panelName(r, true, "Kirenna") == "Kirenna", "a multiplayer Monarch is not shown by its player's name");
        check(panelName(r, false, "Kirenna") == "monarch", "a skirmish Monarch is shown by its player's name");
        UnitR a; a.type = &archer;
        check(panelName(a, true, "Kirenna") == "archer", "a multiplayer archer is shown by its player's name");
    }
    // Ctrl+D (48e2b1..48e309): any selected unit counting down -> the press
    // removes every countdown and arms none; otherwise it arms them all.
    using ta::client::selfDestructPress;
    check(selfDestructPress({{1, false}, {2, false}}) == std::vector<int>{1, 2}, "a press did not arm them all");
    check(selfDestructPress({{1, false}, {2, true}, {3, false}}) == std::vector<int>{2},
          "a press over a counting unit armed the others");
    // The countdown notice: whole seconds left, rounded up, never below 1.
    using ta::client::selfDestructNotice;
    check(selfDestructNotice(2.0f) == "SELF-DESTRUCT IN 2...", "a fresh default countdown reads " + selfDestructNotice(2.0f));
    check(selfDestructNotice(1.2f) == "SELF-DESTRUCT IN 2...", "1.2 s left reads " + selfDestructNotice(1.2f));
    check(selfDestructNotice(0.9f) == "SELF-DESTRUCT IN 1...", "0.9 s left reads " + selfDestructNotice(0.9f));
    check(selfDestructNotice(0.0f) == "SELF-DESTRUCT IN 1...", "the last wait reads " + selfDestructNotice(0.0f));
    // The wait after the countdown's draw still reads as counting down.
    w.destroy(a);
    w.unit(a)->selfDestructT = -5;
    const std::string leaving = ta::client::unitStatusLabel(snapshot(*w.unit(a)));
    check(leaving == "SELF DESTRUCT ENGAGED", "a unit in the self-destruct's last wait reads " + leaving);
    std::puts("unit status: PASS");
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
