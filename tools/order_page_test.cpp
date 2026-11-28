// The sidebar's ORDERS page (<SIDE>GEN.GUI) against TotalA.exe's rule
// (client/orderpage.h; docs/retail-gui.md section 3, "Order buttons").
//
// 0x41b2e0 ORs the selected units' order words (UnitType +0x245, parsed at
// 0x42c8ae..0x42cac4) and 0x41a120 greys every order button no selected unit
// has, hiding only LOAD (no transport) or BLAST (a transport) of the pair that
// share a slot. REPAIR follows canreclamate (bit 9 is a copy of bit 10,
// 0x42ca3b..0x42ca4f), not builder, so a construction vehicle has it and a
// Peewee does not.
//
// The synthetic cases always run; given a retail install it checks the shipped
// units' pages, the expectations written out from their FBI keys by hand.

#include "client/orderpage.h"
#include "hpi/hpi.h"
#include "sim/sim.h"

#include <cstdio>
#include <map>
#include <set>
#include <string>

using namespace ta;
using client::OrderPageUnit;
using T = sim::UnitType;

namespace {
int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; }
}

std::map<std::string, client::OrderButton> page(const std::vector<OrderPageUnit>& sel, bool tabs = false) {
    std::map<std::string, client::OrderButton> out;
    for (const auto& b : client::orderPage(client::orderSummary(sel), tabs)) out[std::string(b.name)] = b;
    return out;
}

// enabled: the buttons shown and not greyed; hidden: the buttons not shown.
// Every other button of the page must be shown greyed.
void expect(const std::string& what, const std::vector<OrderPageUnit>& sel, std::set<std::string> enabled,
            std::set<std::string> hidden) {
    const auto p = page(sel);
    static const char* all[] = {"BUILD", "ORDERS", "CLOAK", "ONOFF", "MOVEORD", "FIREORD", "MOVE", "STOP", "ATTACK",
                                "DEFEND", "PATROL", "RECLAIM", "REPAIR", "CAPTURE", "LOAD", "UNLOAD", "BLAST"};
    check(p.size() == std::size(all), what + ": every gadget of the page is set up");
    std::string got;
    for (const char* n : all) {
        auto it = p.find(n);
        if (it == p.end()) { check(false, what + ": " + n + " missing"); continue; }
        const auto& b = it->second;
        const char* state = !b.shown ? "hidden" : b.grayed ? "grey" : "on";
        got += std::string(" ") + n + "=" + state;
        const char* want = hidden.count(n) ? "hidden" : enabled.count(n) ? "on" : "grey";
        check(std::string(state) == want, what + ": " + n + " is " + state + ", retail " + want);
    }
    std::printf("%s:%s\n", what.c_str(), got.c_str());
}

OrderPageUnit unitOf(const T& t, uint8_t move = 0, uint8_t fire = 0, bool active = false, bool cloak = false) {
    return OrderPageUnit{t.orderFlags, move, fire, active, cloak};
}
} // namespace

int main(int argc, char** argv) {
    // The summary (0x41b2e0).
    {
        const uint16_t soldier = T::kOrderFireState | T::kOrderMoveState | T::kOrderAttack;
        auto s = client::orderSummary({});
        check(s.any == 0 && s.fire == 4 && s.move == 4 && s.onOff == 3 && s.cloak == 3, "an empty selection: nothing");
        s = client::orderSummary({{soldier, 1, 2, false, false}});
        check(s.fire == 2 && s.move == 1, "one soldier: its fire and move states");
        s = client::orderSummary({{soldier, 1, 2, false, false}, {soldier, 1, 0, false, false}});
        check(s.fire == 3 && s.move == 1, "fire states that differ are mixed (3), equal move states stay");
        s = client::orderSummary({{soldier, 1, 2, false, false}, {T::kOrderAttack, 0, 0, false, false}});
        check(s.fire == 2 && s.move == 1, "a unit without the bit does not count");
        s = client::orderSummary({{T::kOrderOnOff, 0, 0, true, false}, {T::kOrderOnOff, 0, 0, true, false}});
        check(s.onOff == 1, "two powered-up units: on");
        s = client::orderSummary({{T::kOrderOnOff, 0, 0, true, false}, {T::kOrderOnOff, 0, 0, false, false}});
        check(s.onOff == 2, "one on, one off: mixed (2)");
        s = client::orderSummary({{T::kOrderCloak, 0, 0, false, true}});
        check(s.cloak == 1, "one cloaker: its cloak order");
        s = client::orderSummary({{T::kOrderCloak, 0, 0, false, true}, {T::kOrderCloak, 0, 0, false, true}});
        check(s.cloak == 2, "a second cloaker makes it 2 even when both agree (0x41b497)");
        // The page's stages and greys for the state buttons (0x41a1c3..0x41a2c1).
        auto p = page({{soldier, 1, 3, false, false}});
        check(!p["FIREORD"].grayed && p["FIREORD"].stage == 3 && p["MOVEORD"].stage == 1, "state buttons show the state");
        check(p["ONOFF"].grayed && p["CLOAK"].grayed, "no onoffable, no cloaker: both grey");
        check(page({{soldier, 0, 0, false, false}}, true)["BUILD"].grayed == false, "one builder: the tabs work");
        check(page({{soldier, 0, 0, false, false}}, false)["ORDERS"].grayed, "otherwise the tabs are grey");
        // The presses (0x41a490).
        check(client::nextStandingState(0) == 1 && client::nextStandingState(1) == 2 &&
              client::nextStandingState(2) == 0 && client::nextStandingState(3) == 0,
              "FIREORD / MOVEORD: 0 -> 1 -> 2 -> 0, mixed -> 0");
        check(client::nextOnOff(0) && !client::nextOnOff(1) && client::nextOnOff(2), "ONOFF: off/mixed -> on, on -> off");
        check(client::nextCloak(0) && !client::nextCloak(1) && !client::nextCloak(2), "CLOAK: off -> on, on/mixed -> off");
    }

    if (argc >= 2) {
        hpi::Vfs vfs = hpi::mountRetailRoot(argv[1]);
        sim::TypeRegistry reg;
        reg.loadMoveInfo(vfs, "gamedata/moveinfo.tdf");
        reg.loadDir(vfs, "units/");
        auto type = [&](const char* id) -> const T& {
            static const T none;
            const T* t = reg.find(id);
            check(t != nullptr, std::string("shipped type ") + id);
            return t ? *t : none;
        };
        // Construction units: Builder=1 CanReclamate=1, no canattack / firestandorders.
        for (const char* id : {"armcv", "armck", "armca"})
            expect(id, {unitOf(type(id))},
                   {"MOVE", "STOP", "DEFEND", "PATROL", "RECLAIM", "REPAIR", "MOVEORD"}, {"LOAD"});
        // The Commander: everything but ONOFF; candgun lights BLAST, cloakcost CLOAK.
        expect("armcom", {unitOf(type("armcom"))},
               {"MOVE", "STOP", "ATTACK", "DEFEND", "PATROL", "RECLAIM", "REPAIR", "CAPTURE", "BLAST", "FIREORD",
                "MOVEORD", "CLOAK"}, {"LOAD"});
        // Fighting units: no REPAIR or RECLAIM.
        for (const char* id : {"armpw", "armfig"})
            expect(id, {unitOf(type(id))}, {"MOVE", "STOP", "ATTACK", "DEFEND", "PATROL", "FIREORD", "MOVEORD"}, {"LOAD"});
        // The kbot lab: canmove canpatrol canstop and both standing orders, no canguard.
        // DEFEND is ours on a factory (retail greys it): its guard is a rally for its output.
        expect("armlab", {unitOf(type("armlab"))}, {"MOVE", "STOP", "DEFEND", "PATROL", "FIREORD", "MOVEORD"}, {"LOAD"});
        // Onoffable buildings: ONOFF alone.
        for (const char* id : {"armsolar", "armmakr"}) expect(id, {unitOf(type(id))}, {"ONOFF"}, {"LOAD"});
        // A transport: LOAD and UNLOAD, BLAST hidden; no standing orders (both 0).
        for (const char* id : {"armatlas", "armthovr"})
            expect(id, {unitOf(type(id))}, {"MOVE", "STOP", "DEFEND", "PATROL", "LOAD", "UNLOAD"}, {"BLAST"});
        // A cloaker: the Spy.
        expect("armspy", {unitOf(type("armspy"))}, {"MOVE", "STOP", "DEFEND", "PATROL", "MOVEORD", "CLOAK"}, {"LOAD"});
        // A mixed selection: the union.
        expect("armcv+armpw", {unitOf(type("armcv")), unitOf(type("armpw"))},
               {"MOVE", "STOP", "ATTACK", "DEFEND", "PATROL", "RECLAIM", "REPAIR", "FIREORD", "MOVEORD"}, {"LOAD"});
        expect("armpw+armatlas", {unitOf(type("armpw")), unitOf(type("armatlas"))},
               {"MOVE", "STOP", "ATTACK", "DEFEND", "PATROL", "FIREORD", "MOVEORD", "LOAD", "UNLOAD"}, {"BLAST"});
        // Every shipped type: REPAIR exactly when RECLAIM (one key, canreclamate).
        int reclaimers = 0;
        for (const auto& [id, t] : reg.types()) {
            check(bool(t.orderFlags & T::kOrderRepair) == bool(t.orderFlags & T::kOrderReclaim),
                  id + ": REPAIR follows canreclamate");
            check(bool(t.orderFlags & T::kOrderCloak) == (t.cloakCost > 0), id + ": CLOAK follows cloakcost > 0");
            reclaimers += (t.orderFlags & T::kOrderReclaim) != 0;
        }
        check(reclaimers >= 20, "shipped reclaimers found (" + std::to_string(reclaimers) + ")");
    }

    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("PASS: ORDERS page buttons follow TotalA.exe 0x41b2e0 / 0x41a120\n");
    return 0;
}
