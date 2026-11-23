// The information bar's status line (client/unitstatus.h). Every type
// without activatewhenbuilt is created with its activation bit clear (4c033e,
// default 0; the parser's default since protocol 312), so the archer is as
// "off" as a new gate. Only a unit that can be switched off reads INACTIVE;
// the line used to say it for every idle unit of such a type.
#include "client/unitstatus.h"
#include "sim/sim.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
using namespace tak::sim;
static void check(bool ok, const std::string& what) { if (!ok) throw std::runtime_error(what); }

static UnitR snapshot(const Unit& u) {
    UnitR r;
    r.id = u.id; r.type = u.type; r.player = u.player;
    r.active = u.active; r.cloaked = u.cloaked; r.underConstruction = u.underConstruction;
    r.selfDestructT = tak::client::selfDestructSeconds(u.selfDestructT);
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
    const std::string idle = tak::client::unitStatusLabel(snapshot(*w.unit(a)));
    check(idle == "STANDBY", "an idle archer reads " + idle + ", not STANDBY");
    w.order(a, 400, 300, false);
    const std::string moving = tak::client::unitStatusLabel(snapshot(*w.unit(a)));
    check(moving == "MOVING", "a moving archer reads " + moving + ", not MOVING");
    const std::string off = tak::client::unitStatusLabel(snapshot(*w.unit(g)));
    check(off == "INACTIVE", "a gate switched off reads " + off + ", not INACTIVE");
    UnitR on = snapshot(*w.unit(g)); on.active = true;
    const std::string onText = tak::client::unitStatusLabel(on);
    check(onText == "STANDBY", "a gate switched on reads " + onText + ", not STANDBY");
    // 4b2513..4b2551: a multiplayer game names a commander after its player.
    {
        UnitType king; king.id = king.name = "monarch"; king.commander = true;
        UnitR r; r.type = &king;
        using tak::client::panelName;
        check(panelName(r, true, "Kirenna") == "Kirenna", "a multiplayer Monarch is not shown by its player's name");
        check(panelName(r, false, "Kirenna") == "monarch", "a skirmish Monarch is shown by its player's name");
        UnitR a; a.type = &archer;
        check(panelName(a, true, "Kirenna") == "archer", "a multiplayer archer is shown by its player's name");
    }
    // 4b26c0..4b26ef: no mark below rank 4, then one frame per three ranks.
    using tak::client::experienceFrame;
    const int marks[11]={-1,-1,-1,-1,0,0,0,1,1,1,2};
    for (int rank=0;rank<=10;++rank)
        check(experienceFrame(rank)==marks[rank],"rank "+std::to_string(rank)+" shows the wrong veteran mark");
    // Ctrl+D (48e2b1..48e309): any selected unit counting down -> the press
    // removes every countdown and arms none; otherwise it arms them all.
    using tak::client::selfDestructPress;
    check(selfDestructPress({{1, false}, {2, false}}) == std::vector<int>{1, 2}, "a press did not arm them all");
    check(selfDestructPress({{1, false}, {2, true}, {3, false}}) == std::vector<int>{2},
          "a press over a counting unit armed the others");
    // The countdown notice: whole seconds left, rounded up, never below 1.
    using tak::client::selfDestructNotice;
    check(selfDestructNotice(2.0f) == "SELF-DESTRUCT IN 2...", "a fresh default countdown reads " + selfDestructNotice(2.0f));
    check(selfDestructNotice(1.2f) == "SELF-DESTRUCT IN 2...", "1.2 s left reads " + selfDestructNotice(1.2f));
    check(selfDestructNotice(0.9f) == "SELF-DESTRUCT IN 1...", "0.9 s left reads " + selfDestructNotice(0.9f));
    check(selfDestructNotice(0.0f) == "SELF-DESTRUCT IN 1...", "the last wait reads " + selfDestructNotice(0.0f));
    // The wait after the countdown's draw still reads as counting down.
    w.destroy(a);
    w.unit(a)->selfDestructT = -5;
    const std::string leaving = tak::client::unitStatusLabel(snapshot(*w.unit(a)));
    check(leaving == "LEAVING YOUR COMMAND", "a unit in the self-destruct's last wait reads " + leaving);
    std::puts("unit status: PASS");
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
