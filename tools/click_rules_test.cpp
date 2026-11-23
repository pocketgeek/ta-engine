// What a click orders (client/clickrules.h), row by row against retail's
// resolver (KINGDOMS.icd 0x4de530 / 0x5214b0) in the right-click interface, the
// owner's combined rules (PLAN.md section 4), and a parity test: under an armed
// command every unit's cursor is an order cursor exactly when its click orders
// something, and the cursor names the order. Pure functions: no renderer, no data.

#include "client/clickrules.h"

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace {
using tak::CursorId;
using namespace tak::client;

int failures = 0;

const char* name(ClickOrder o) {
    switch (o) {
        case ClickOrder::None: return "None";
        case ClickOrder::Move: return "Move";
        case ClickOrder::Attack: return "Attack";
        case ClickOrder::AttackMove: return "AttackMove";
        case ClickOrder::Patrol: return "Patrol";
        case ClickOrder::Assist: return "Assist";
        case ClickOrder::Repair: return "Repair";
        case ClickOrder::Board: return "Board";
        case ClickOrder::Pickup: return "Pickup";
        case ClickOrder::Guard: return "Guard";
        case ClickOrder::Resurrect: return "Resurrect";
        case ClickOrder::Reclaim: return "Reclaim";
        case ClickOrder::ReclaimArea: return "ReclaimArea";
        case ClickOrder::Unload: return "Unload";
        case ClickOrder::AttackGround: return "AttackGround";
    }
    return "?";
}

void expect(ClickOrder got, ClickOrder want, const std::string& row) {
    if (got == want) return;
    std::fprintf(stderr, "FAIL: %s: got %s, want %s\n", row.c_str(), name(got), name(want));
    ++failures;
}
void expectCursor(CursorId got, CursorId want, const std::string& row) {
    if (got == want) return;
    std::fprintf(stderr, "FAIL: %s: cursor %s, want %s\n", row.c_str(), tak::cursorSequenceName(got),
                 tak::cursorSequenceName(want));
    ++failures;
}
void expectAction(SelectAction got, SelectAction want, const std::string& row) {
    if (got == want) return;
    std::fprintf(stderr, "FAIL: %s: select action %d, want %d\n", row.c_str(), int(got), int(want));
    ++failures;
}

ClickTarget target(HoverKind k, bool flies = false) { ClickTarget t; t.kind = k; t.flies = flies; return t; }

ClickUnit soldier() { ClickUnit u; u.mobile = true; u.attacker = true; u.canHit = true; u.pointFire = true; u.canGuard = true; u.canPatrol = true; u.friendly = FriendlyOrder::Guard; return u; }
ClickUnit builder() { ClickUnit u; u.mobile = true; u.canGuard = true; u.canPatrol = true; u.friendly = FriendlyOrder::Guard; return u; }
ClickUnit scout() { ClickUnit u; u.mobile = true; u.friendly = FriendlyOrder::Move; return u; }   // no weapon, no canguard
ClickUnit tower() { ClickUnit u; u.attacker = true; u.pointFire = true; return u; }
ClickUnit factory() { ClickUnit u; u.rallyHolder = true; return u; }
ClickUnit transportUnit() { ClickUnit u; u.mobile = true; u.transport = true; u.canGuard = true; u.friendly = FriendlyOrder::Guard; return u; }

// 2.1 right-click, nothing armed (mode 1).
void rightClickRows() {
    const auto enemy = target(HoverKind::Enemy), ally = target(HoverKind::Friendly),
               own = target(HoverKind::OwnSelectable), ground = target(HoverKind::None);
    expect(rightClick(enemy, soldier(), false), ClickOrder::Attack, "2.1 enemy: an attacker that can hit it attacks");
    ClickUnit immune = soldier(); immune.canHit = false;
    expect(rightClick(enemy, immune, false), ClickOrder::None, "2.1 enemy: an attacker that cannot hurt it does nothing (0x4df215)");
    expect(rightClick(enemy, builder(), false), ClickOrder::Move, "2.1 enemy: a unit without a weapon moves to the point");
    ClickUnit towerOut = tower();
    expect(rightClick(enemy, towerOut, false), ClickOrder::Attack, "2.1 enemy: a tower keeps its Attack out of range (ours)");
    expect(rightClick(enemy, factory(), false), ClickOrder::Attack, "2.1 enemy: a rally holder sends its output (ours)");
    ClickUnit wall; // an immobile unit with nothing to do
    expect(rightClick(enemy, wall, false), ClickOrder::None, "2.1 enemy: a structure without a weapon does nothing");

    ClickUnit site = builder(); site.assist = true;
    expect(rightClick(ally, site, false), ClickOrder::Assist, "2.1 site: a builder that can build it helps (HELPBUILD)");
    ClickUnit repair = builder(); repair.friendly = FriendlyOrder::Repair;
    expect(rightClick(ally, repair, false), ClickOrder::Repair, "2.1 damaged ally: a builder repairs it");
    expect(rightClick(ally, soldier(), false), ClickOrder::Guard, "2.1 ally: a soldier guards it (FOLLOW)");
    expect(rightClick(ally, scout(), false), ClickOrder::Move, "2.1 ally: no canguard -> Move (retail's fallback)");
    expect(rightClick(ally, factory(), false), ClickOrder::Move, "2.1 ally: a rally holder rallies to it (ours)");
    expect(rightClick(ally, wall, false), ClickOrder::None, "2.1 ally: a structure with nothing to do does nothing");
    ClickUnit pick = transportUnit(); pick.pickup = true;
    expect(rightClick(own, pick, false), ClickOrder::Pickup, "2.1 own unit: the single weaponless transport picks it up");
    ClickUnit self = soldier(); self.self = true;
    expect(rightClick(own, self, false), ClickOrder::None, "2.1 the hovered unit is left out (0x521562)");

    // C4c: passengers that fit board, the rest guard the transport.
    ClickUnit fits = soldier(); fits.board = true;
    expect(rightClick(own, fits, false), ClickOrder::Board, "C4 a passenger that fits boards the transport");
    expect(rightClick(own, soldier(), false), ClickOrder::Guard, "C4 a passenger that does not fit guards it");
    ClickUnit fitsDamaged = builder(); fitsDamaged.board = true; fitsDamaged.friendly = FriendlyOrder::Repair;
    expect(rightClick(own, fitsDamaged, false), ClickOrder::Board, "C4 a builder that fits boards rather than repairs");

    // Ground, bodies and features.
    expect(rightClick(ground, soldier(), false), ClickOrder::Move, "2.1 ground: Move");
    expect(rightClick(ground, factory(), false), ClickOrder::Move, "2.1 ground: a rally holder sets its rally");
    expect(rightClick(ground, wall, false), ClickOrder::None, "2.1 ground: a structure stays");
    ClickUnit caster = builder(); caster.raise = true; caster.reclaim = true;
    expect(rightClick(ground, caster, false), ClickOrder::Resurrect, "2.1 body: a caster raises it first");
    ClickUnit mason = builder(); mason.reclaim = true;
    expect(rightClick(ground, mason, false), ClickOrder::Reclaim, "2.1 feature: a reclaimer reclaims it");
    expect(rightClick(ground, soldier(), false), ClickOrder::Move, "2.1 body: a unit that can do neither moves there");

    // C5: a loaded transport moves; Alt unloads (ours, retail never reads Alt).
    ClickUnit loaded = transportUnit(); loaded.loaded = true;
    expect(rightClick(ground, loaded, false), ClickOrder::Move, "C5 a loaded transport on ground: Move (retail)");
    expect(rightClick(ground, loaded, true), ClickOrder::Unload, "C5 Alt+right-click: a loaded transport unloads");
    expect(rightClick(ground, transportUnit(), true), ClickOrder::Move, "C5 Alt+right-click: an empty transport moves");
    expect(rightClick(ground, soldier(), true), ClickOrder::Move, "C5 Alt+right-click: other units move");
    expect(rightClick(enemy, loaded, true), ClickOrder::Move, "C5 Alt over an enemy: the usual order");
}

UnitClick armed(char cmd, HoverKind k, const ClickUnit& u, ArmedSelection sel = {}, bool flies = false) {
    return armedClick(cmd, target(k, flies), u, sel);
}

// 2.3-2.8 armed commands.
void armedRows() {
    // 2.3 / C8: armed MOVE.
    ClickUnit repair = builder(); repair.friendly = FriendlyOrder::Repair;
    auto c = armed('m', HoverKind::Friendly, repair);
    expect(c.order, ClickOrder::Repair, "2.3 armed M over a damaged ally: a builder repairs");
    expectCursor(c.cursor, CursorId::Repair, "2.3 armed M over a damaged ally: Repair");
    c = armed('m', HoverKind::Friendly, soldier());
    expect(c.order, ClickOrder::Guard, "2.3 armed M over an ally: a soldier guards");
    expectCursor(c.cursor, CursorId::Defend, "2.3 armed M over an ally: Defend");
    ClickUnit site = builder(); site.assist = true;
    expect(armed('m', HoverKind::Friendly, site).order, ClickOrder::Assist, "2.3 armed M over a site: Assist");
    c = armed('m', HoverKind::None, soldier());
    expect(c.order, ClickOrder::Move, "2.3 / C8 armed M over a body or ground: Move");
    expectCursor(c.cursor, CursorId::Move, "C8 armed M over a body: the Move cursor (the click moves)");
    expect(armed('m', HoverKind::Enemy, soldier()).order, ClickOrder::Move, "2.3 armed M over an enemy: Move");
    ClickUnit self = soldier(); self.self = true;
    c = armed('m', HoverKind::OwnSelectable, self);
    expect(c.order, ClickOrder::None, "2.3 armed M over itself, alone: nothing");
    expectCursor(c.cursor, CursorId::Normal, "2.3 armed M over itself: Normal, stays armed");
    // Fight-move: retail's MoveFight is armed MOVE plus the 0x400 bit, which
    // turns only a resolved Move into a fight-move (0x521bf4..0x521c5b).
    expectCursor(armed('f', HoverKind::None, soldier()).cursor, CursorId::Move, "fight-move draws Move");
    expect(armed('f', HoverKind::None, soldier()).order, ClickOrder::AttackMove, "fight-move attack-moves");
    c = armed('f', HoverKind::Enemy, soldier());
    expect(c.order, ClickOrder::AttackMove, "fight-move over an enemy: a fight-move to the point");
    expectCursor(c.cursor, CursorId::Move, "fight-move over an enemy: Move");
    c = armed('f', HoverKind::Friendly, repair);
    expect(c.order, ClickOrder::Repair, "fight-move over a damaged ally: a builder repairs");
    expectCursor(c.cursor, CursorId::Repair, "fight-move over a damaged ally: Repair");
    c = armed('f', HoverKind::Friendly, soldier());
    expect(c.order, ClickOrder::Guard, "fight-move over an ally: a soldier guards (Follow)");
    expectCursor(c.cursor, CursorId::Defend, "fight-move over an ally: Defend");
    expect(armed('f', HoverKind::Friendly, site).order, ClickOrder::Assist, "fight-move over a site: Assist");
    ClickUnit lifter = transportUnit(); lifter.pickup = true;
    c = armed('f', HoverKind::OwnSelectable, lifter);
    expect(c.order, ClickOrder::Pickup, "fight-move over a passenger: the transport picks it up");
    expectCursor(c.cursor, CursorId::Load, "fight-move over a passenger: Load");
    c = armed('f', HoverKind::Friendly, scout());
    expect(c.order, ClickOrder::AttackMove, "fight-move over an ally it cannot guard: the Move fights");
    expectCursor(c.cursor, CursorId::Move, "fight-move over an ally it cannot guard: Move");
    expect(armed('f', HoverKind::None, factory()).order, ClickOrder::AttackMove,
           "fight-move: a rally holder's rally fights (QMove)");
    expect(armed('f', HoverKind::OwnSelectable, self).order, ClickOrder::None, "fight-move over itself, alone: nothing");
    for (HoverKind k : {HoverKind::None, HoverKind::OwnSelectable, HoverKind::Friendly, HoverKind::Enemy})
        for (const ClickUnit& u : {soldier(), builder(), scout(), tower(), factory(), transportUnit(), repair, site, lifter}) {
            const UnitClick m = armed('m', k, u), fm = armed('f', k, u);
            expect(fm.order, m.order == ClickOrder::Move ? ClickOrder::AttackMove : m.order,
                   "fight-move is armed M with its Move fighting");
            expectCursor(fm.cursor, m.cursor, "fight-move draws armed M's cursor");
        }
    expect(armed('p', HoverKind::None, soldier()).order, ClickOrder::Patrol, "armed P: Patrol");
    expect(armed('p', HoverKind::None, scout()).order, ClickOrder::None, "armed P: no canpatrol, nothing");

    // 2.4 / C3: armed GUARD and the flyer rule.
    ArmedSelection ok; ok.guardOk = true;
    ArmedSelection refused;
    c = armed('g', HoverKind::OwnSelectable, soldier(), refused, true);
    expect(c.order, ClickOrder::None, "C3 a ground unit over a flyer: no guard");
    expectCursor(c.cursor, CursorId::Normal, "C3 a ground unit over a flyer: Normal, stays armed");
    c = armed('g', HoverKind::OwnSelectable, soldier(), ok, true);
    expect(c.order, ClickOrder::Guard, "C3 a mixed selection with a flyer: the ground unit guards too");
    expect(armed('g', HoverKind::Friendly, soldier(), ok).order, ClickOrder::Guard, "2.4 over an allied unit: Guard");
    expect(armed('g', HoverKind::Enemy, soldier(), ok).order, ClickOrder::None, "2.4 over an enemy: nothing");
    expect(armed('g', HoverKind::None, soldier(), ok).order, ClickOrder::None, "2.4 over ground: nothing");
    expect(armed('g', HoverKind::OwnSelectable, scout(), ok).order, ClickOrder::None, "2.4 no canguard: nothing");

    // 2.5: armed CLEAR raises first; the cursor says what it does.
    ClickUnit caster = builder(); caster.raise = true; caster.reclaim = true;
    c = armed('c', HoverKind::None, caster);
    expect(c.order, ClickOrder::Resurrect, "2.5 a raisable reclaimable body: Resurrect");
    expectCursor(c.cursor, CursorId::Revive, "2.5 a raisable reclaimable body: Revive (retail shows Reclaim)");
    ClickUnit mason = builder(); mason.reclaim = true;
    c = armed('c', HoverKind::None, mason);
    expect(c.order, ClickOrder::Reclaim, "2.5 a feature: Reclaim");
    expectCursor(c.cursor, CursorId::Reclaim, "2.5 a feature: Reclaim cursor");
    ClickUnit raiser = builder(); raiser.raise = true;
    expectCursor(armed('c', HoverKind::None, raiser).cursor, CursorId::Revive, "2.5 a non-reclaimable body: Revive (ours)");
    expectCursor(armed('c', HoverKind::None, soldier()).cursor, CursorId::Normal, "2.5 nothing there: Normal");

    // 2.6 / C1: armed ATTACK over ground or a friendly fires at the point
    // (0x4de65f..0x4de7e3), the cursor Attack where the unit acts (ours) and
    // TooFar where it gets nothing.
    c = armed('a', HoverKind::None, soldier());
    expect(c.order, ClickOrder::AttackGround, "C1 armed A over ground: fire at the point");
    expectCursor(c.cursor, CursorId::Attack, "C1 armed A over ground: Attack");
    c = armed('a', HoverKind::Friendly, soldier());
    expect(c.order, ClickOrder::AttackGround, "C1 armed A over a friendly: fire at the point");
    expectCursor(c.cursor, CursorId::Attack, "C1 armed A over a friendly: Attack (retail TooFar; ours names the order)");
    c = armed('a', HoverKind::OwnSelectable, soldier());
    expect(c.order, ClickOrder::AttackGround, "C1 armed A over an own unit: fire at the point");
    expectCursor(c.cursor, CursorId::Attack, "C1 armed A over an own unit: Attack");
    ClickUnit air = soldier(); air.airstrike = true; air.flies = true;
    c = armed('a', HoverKind::None, air);
    expect(c.order, ClickOrder::AttackGround, "C1 an airstrike flyer over ground: AIRSTRIKE at the point");
    expectCursor(c.cursor, CursorId::Airstrike, "C1 an airstrike flyer over ground: Airstrike");
    c = armed('a', HoverKind::Friendly, tower());
    expect(c.order, ClickOrder::AttackGround, "C1 a tower over a friendly: fire at the point");
    expectCursor(c.cursor, CursorId::Attack, "C1 a tower over a friendly: Attack");
    c = armed('a', HoverKind::None, tower());
    expect(c.order, ClickOrder::AttackGround, "C1 a tower over ground (in reach or not): fire at the point");
    ClickUnit antiAir = soldier(); antiAir.pointFire = false;   // a toairweapon, or melee off a clearable blocker
    c = armed('a', HoverKind::None, antiAir);
    expect(c.order, ClickOrder::None, "C1 an attacker the resolver refuses: nothing");
    expectCursor(c.cursor, CursorId::TooFar, "C1 an attacker the resolver refuses: TooFar");
    c = armed('a', HoverKind::Friendly, antiAir);
    expect(c.order, ClickOrder::None, "C1 refused over a friendly: nothing");
    expectCursor(c.cursor, CursorId::TooFar, "C1 refused over a friendly: TooFar");
    {
        ClickUnit both[2] = {antiAir, soldier()};
        CursorId agg = CursorId::Normal;
        for (const auto& u : both) agg = lowerCursorSlot(agg, armed('a', HoverKind::None, u).cursor);
        expectCursor(agg, CursorId::Attack, "C1 one unit acts, one refused: Attack (lowest slot)");
    }
    c = armed('a', HoverKind::None, builder());
    expect(c.order, ClickOrder::None, "C1 a builder without a weapon: nothing");
    expectCursor(c.cursor, CursorId::Normal, "C1 a builder without a weapon: Normal");
    expect(armed('a', HoverKind::None, factory()).order, ClickOrder::AttackMove, "C1 a rally holder rallies");
    c = armed('f', HoverKind::None, soldier());
    expect(c.order, ClickOrder::AttackMove, "C1 fight-move stays our attack-move");

    // 2.7: armed ATTACK over an enemy.
    c = armed('a', HoverKind::Enemy, soldier());
    expect(c.order, ClickOrder::Attack, "2.7 an enemy it can hit: Attack");
    ClickUnit immune = soldier(); immune.canHit = false;
    c = armed('a', HoverKind::Enemy, immune);
    expect(c.order, ClickOrder::None, "2.7 an enemy it cannot hurt: nothing");
    expectCursor(c.cursor, CursorId::TooFar, "2.7 an enemy it cannot hurt: TooFar");
    c = armed('a', HoverKind::Enemy, tower());
    expect(c.order, ClickOrder::Attack, "2.7 a tower out of range keeps the order");
    expectCursor(c.cursor, CursorId::TooFar, "2.7 a tower out of range: TooFar");

    // 2.8: LOAD, REPAIR, UNLOAD.
    ArmedSelection one; one.singleTransport = true;
    ClickUnit carrier = transportUnit(); carrier.loadTarget = true;
    c = armed('l', HoverKind::OwnSelectable, carrier, one);
    expect(c.order, ClickOrder::Pickup, "2.8 armed L: the one transport picks the hovered unit up");
    expectCursor(c.cursor, CursorId::Load, "2.8 armed L: Load");
    expect(armed('l', HoverKind::OwnSelectable, carrier).order, ClickOrder::None, "2.8 armed L: two transports: nothing");
    ClickUnit damaged = builder(); damaged.friendly = FriendlyOrder::Repair;
    expect(armed('r', HoverKind::Friendly, damaged).order, ClickOrder::Repair, "2.8 armed R: a damaged ally: Repair");
    expect(armed('r', HoverKind::Friendly, site).order, ClickOrder::Assist, "2.8 armed R: a site: Assist");
    expect(armed('r', HoverKind::Friendly, builder()).order, ClickOrder::None, "2.8 armed R: a healthy ally: nothing");
    ClickUnit loaded = transportUnit(); loaded.loaded = true;
    expect(armed('u', HoverKind::None, loaded).order, ClickOrder::Unload, "2.8 armed U: a loaded transport unloads");
    expect(armed('u', HoverKind::None, transportUnit()).order, ClickOrder::None, "2.8 armed U: an empty one does not");
    ArmedSelection queued; queued.queue = true;
    expect(armed('u', HoverKind::None, transportUnit(), queued).order, ClickOrder::Unload,
           "2.8 armed U with Shift: queued behind a pickup");

    // P4: only order cursors act.
    for (CursorId cur : {CursorId::Attack, CursorId::Airstrike, CursorId::TooFar, CursorId::Defend, CursorId::Repair,
                         CursorId::Patrol, CursorId::Revive, CursorId::Reclaim, CursorId::Load, CursorId::Unload,
                         CursorId::Move, CursorId::FindSite})
        if (!isOrderCursor(cur)) { std::fprintf(stderr, "FAIL: P4 %s is an order cursor\n", tak::cursorSequenceName(cur)); ++failures; }
    for (CursorId cur : {CursorId::Select, CursorId::Red, CursorId::Green, CursorId::Normal})
        if (isOrderCursor(cur)) { std::fprintf(stderr, "FAIL: P4 %s does nothing\n", tak::cursorSequenceName(cur)); ++failures; }
}

// 2.2 / 2.9 boxes.
void boxRows() {
    const auto orders = [](BoxMode m, std::vector<BoxUnit> units) { return boxOrders(m, units); };
    BoxUnit pureBuilder; pureBuilder.reclaimer = true;
    BoxUnit armedBuilder = pureBuilder; armedBuilder.attacker = true;
    BoxUnit soldierHits; soldierHits.attacker = true; soldierHits.hits = 2;
    BoxUnit soldierNone; soldierNone.attacker = true;
    BoxUnit lone; lone.transport = true; lone.loadable = 3;

    auto o = orders(BoxMode::Right, {pureBuilder});
    expect(o[0], ClickOrder::ReclaimArea, "2.2 pure builder over empty ground: ReclaimArea");
    BoxUnit armedHits = armedBuilder; armedHits.hits = 1;
    o = orders(BoxMode::Right, {armedHits});
    expect(o[0], ClickOrder::Attack, "2.2 armed builder, enemies in the box: Attack only");
    o = orders(BoxMode::Right, {armedBuilder});
    expect(o[0], ClickOrder::ReclaimArea, "2.2 armed builder, no enemies: ReclaimArea");
    BoxUnit builderWithEnemy = pureBuilder;   // no weapon: hits stays 0, even with one enemy (C10b)
    o = orders(BoxMode::Right, {soldierHits, builderWithEnemy});
    expect(o[0], ClickOrder::Attack, "2.2 soldier + builder: the soldier attacks");
    expect(o[1], ClickOrder::ReclaimArea, "2.2 soldier + builder: the builder clears (C10b)");
    o = orders(BoxMode::Right, {lone});
    expect(o[0], ClickOrder::Pickup, "2.2 a lone weaponless transport loads the box's units");
    o = orders(BoxMode::Right, {lone, soldierNone});
    expect(o[0], ClickOrder::None, "2.2 transport + soldier: no Load");
    expect(o[1], ClickOrder::None, "2.2 transport + soldier, no enemies: nothing");
    expectCursor(boxCursor({lone, soldierNone}, o), CursorId::Normal, "2.2 nothing would happen: Normal");
    BoxUnit armedTransport = lone; armedTransport.attacker = true;
    o = orders(BoxMode::Right, {armedTransport});
    expect(o[0], ClickOrder::None, "2.2 a transport with a weapon does not load");
    BoxUnit raiser; raiser.raiser = true;
    o = orders(BoxMode::Right, {lone, raiser});
    expect(o[0], ClickOrder::None, "2.2 a raiser in the selection stops the load");

    std::vector<BoxUnit> u = {lone};
    expectCursor(boxCursor(u, orders(BoxMode::Right, u)), CursorId::Load, "2.2 cursor: Load");
    u = {soldierHits, pureBuilder};
    expectCursor(boxCursor(u, orders(BoxMode::Right, u)), CursorId::Attack, "2.2 cursor: Attack");
    BoxUnit air = soldierHits; air.airstrike = true;
    u = {air};
    expectCursor(boxCursor(u, orders(BoxMode::Right, u)), CursorId::Airstrike, "2.2 cursor: Airstrike, every attacker airstrikes");
    u = {air, soldierHits};
    expectCursor(boxCursor(u, orders(BoxMode::Right, u)), CursorId::Attack, "2.2 cursor: mixed attackers, Attack");
    u = {pureBuilder};
    expectCursor(boxCursor(u, orders(BoxMode::Right, u)), CursorId::Reclaim, "2.2 cursor: only clearing, Reclaim");
    u = {soldierNone};
    expectCursor(boxCursor(u, orders(BoxMode::Right, u)), CursorId::Normal,
                 "2.2 cursor: attackers over an empty box, Normal (retail shows Attack, its release does nothing)");

    // 2.9 armed boxes.
    o = orders(BoxMode::ArmedAttack, {soldierHits, pureBuilder});
    expect(o[0], ClickOrder::Attack, "2.9 armed A box: attackers attack");
    expect(o[1], ClickOrder::None, "2.9 armed A box: no clear fallback");
    BoxUnit armedLone = lone; armedLone.attacker = true;
    o = orders(BoxMode::ArmedLoad, {armedLone, soldierNone});
    expect(o[0], ClickOrder::Pickup, "2.9 armed L box: the one transport loads, armed or not");
    o = orders(BoxMode::ArmedLoad, {lone, lone});
    expect(o[0], ClickOrder::None, "2.9 armed L box: two transports, nothing");
    o = orders(BoxMode::ArmedClear, {pureBuilder, soldierHits});
    expect(o[0], ClickOrder::ReclaimArea, "2.9 armed C box: reclaimers clear");
    expect(o[1], ClickOrder::None, "2.9 armed C box: ignores units");
}

// 2.11 left-click select.
void selectRows() {
    const auto sel = [](HoverKind k, bool shift, bool ctrl, bool already, bool own, bool watcher = false) {
        SelectClick s; s.target = k; s.shift = shift; s.ctrl = ctrl; s.alreadySelected = already;
        s.ownSelected = own; s.watcher = watcher;
        return selectClick(s);
    };
    expectAction(sel(HoverKind::OwnSelectable, false, false, false, true), SelectAction::Replace, "2.11 own unit: select it");
    expectAction(sel(HoverKind::OwnSelectable, true, false, false, true), SelectAction::Add, "C7 Shift on an unselected unit: add");
    expectAction(sel(HoverKind::OwnSelectable, true, false, true, true), SelectAction::Remove, "C7 Shift on a selected unit: toggled off");
    expectAction(sel(HoverKind::OwnSelectable, false, true, true, true), SelectAction::Remove, "C7 Ctrl removes (ours)");
    expectAction(sel(HoverKind::Enemy, false, false, false, false), SelectAction::Replace, "C6 enemy, nothing of ours selected: inspect");
    expectAction(sel(HoverKind::Friendly, false, false, false, false), SelectAction::Replace, "C6 ally, nothing of ours selected: inspect");
    expectAction(sel(HoverKind::Enemy, false, false, false, true), SelectAction::Clear, "C6 enemy with a selection: deselect all (retail)");
    expectAction(sel(HoverKind::Enemy, true, false, false, true), SelectAction::Keep, "C6 Shift on an enemy: the selection stays");
    expectAction(sel(HoverKind::None, false, false, false, true), SelectAction::Clear, "2.11 ground: deselect all");
    expectAction(sel(HoverKind::None, true, false, false, true), SelectAction::Keep, "2.11 Shift on ground: nothing");
    expectAction(sel(HoverKind::Enemy, false, false, false, false, true), SelectAction::Replace, "watcher: selects any unit");
    expectAction(sel(HoverKind::Enemy, true, false, false, false, true), SelectAction::Add, "watcher: Shift adds");
}

// Parity: for every fact set, under every armed command, a unit's cursor is an
// order cursor exactly when its click orders something (TooFar alone may come
// with nothing: retail's per-unit "cannot hurt it"), and the cursor names the order.
bool cursorNames(CursorId c, ClickOrder o) {
    switch (o) {
        case ClickOrder::Attack: return c == CursorId::Attack || c == CursorId::Airstrike || c == CursorId::TooFar;
        case ClickOrder::AttackGround: return c == CursorId::Attack || c == CursorId::Airstrike;
        case ClickOrder::AttackMove: return c == CursorId::Attack || c == CursorId::Airstrike || c == CursorId::Move;
        case ClickOrder::Reclaim: case ClickOrder::ReclaimArea: return c == CursorId::Reclaim;
        case ClickOrder::Resurrect: return c == CursorId::Revive;
        case ClickOrder::Board: case ClickOrder::Pickup: return c == CursorId::Load;
        case ClickOrder::Assist: case ClickOrder::Repair: return c == CursorId::Repair;
        case ClickOrder::Guard: return c == CursorId::Defend;
        case ClickOrder::Move: return c == CursorId::Move;
        case ClickOrder::Patrol: return c == CursorId::Patrol;
        case ClickOrder::Unload: return c == CursorId::Unload;
        case ClickOrder::None: return !isOrderCursor(c) || c == CursorId::TooFar;
    }
    return false;
}

void parity() {
    int cases = 0;
    const FriendlyOrder friendlies[] = {FriendlyOrder::None, FriendlyOrder::Repair, FriendlyOrder::Guard, FriendlyOrder::Move};
    for (uint32_t bits = 0; bits < (1u << 16); ++bits) {
        ClickUnit u;
        u.self = false;
        u.mobile = bits & 1; u.rallyHolder = bits & 2; u.attacker = bits & 4; u.airstrike = (bits & 8) && u.attacker;
        u.canHit = bits & 16; u.canPatrol = bits & 32; u.canGuard = bits & 64; u.transport = bits & 128;
        u.loaded = u.transport && (bits & 256); u.assist = bits & 512; u.board = bits & 1024;
        u.pickup = bits & 2048; u.loadTarget = u.transport && (bits & 2048); u.raise = bits & 4096;
        u.reclaim = bits & 8192; u.friendly = friendlies[(bits >> 14) & 3];
        for (bool pointFire : {false, true}) {
        u.pointFire = pointFire;
        for (char cmd : {'a', 'm', 'f', 'p', 'g', 'r', 'l', 'u', 'c'})
            for (HoverKind k : {HoverKind::None, HoverKind::OwnSelectable, HoverKind::Friendly, HoverKind::Enemy})
                for (int s = 0; s < 8; ++s) {
                    ArmedSelection sel; sel.guardOk = s & 1; sel.singleTransport = s & 2; sel.queue = s & 4;
                    const UnitClick c = armedClick(cmd, target(k), u, sel);
                    ++cases;
                    if (!cursorNames(c.cursor, c.order)) {
                        if (failures < 20)
                            std::fprintf(stderr, "FAIL: parity %c bits=%x kind=%d sel=%d: cursor %s, order %s\n", cmd,
                                         bits, int(k), s, tak::cursorSequenceName(c.cursor), name(c.order));
                        ++failures;
                    }
                }
        }
    }
    // The box: its cursor is Normal exactly when the release orders nothing.
    for (uint32_t bits = 0; bits < (1u << 12); ++bits)
        for (BoxMode m : {BoxMode::Right, BoxMode::ArmedAttack, BoxMode::ArmedLoad, BoxMode::ArmedClear}) {
            std::vector<BoxUnit> units(2);
            for (int i = 0; i < 2; ++i) {
                const uint32_t b = bits >> (6 * i);
                units[i].attacker = b & 1; units[i].airstrike = b & 2; units[i].reclaimer = b & 4;
                units[i].transport = b & 8; units[i].hits = (b & 16) ? 1 : 0; units[i].loadable = (b & 32) ? 1 : 0;
            }
            const auto o = boxOrders(m, units);
            bool any = false;
            for (auto x : o) any |= x != ClickOrder::None;
            ++cases;
            if (any != (boxCursor(units, o) != CursorId::Normal)) {
                std::fprintf(stderr, "FAIL: box parity mode %d bits=%x\n", int(m), bits);
                ++failures;
            }
        }
    std::printf("click rules: parity over %d cases\n", cases);
}
} // namespace

// 2.12 the order modifiers (B.4, 0x5214bf): Ctrl (with or without Shift) at
// the head, Shift appended, both toggling; plain replaces. A box's follow-on
// targets go behind the first: appended untoggled, or (Ctrl) each at the head.
void modifierRows() {
    using namespace tak::net;
    const auto row = [](uint8_t got, uint8_t want, const char* what) {
        if (got == want) return;
        std::fprintf(stderr, "FAIL: %s: queue %d, want %d\n", what, int(got), int(want));
        ++failures;
    };
    row(clickQueue(false, false), kQueueReplace, "plain click replaces");
    row(clickQueue(true, false), kQueueAppend | kQueueToggle, "Shift appends, toggling");
    row(clickQueue(false, true), kQueueFront | kQueueToggle, "Ctrl goes to the front, toggling");
    row(clickQueue(true, true), kQueueFront | kQueueToggle, "Ctrl+Shift is Ctrl");
    const auto flag = [&](bool got, bool want, const char* what) {
        if (got == want) return;
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    };
    flag(queuesBehind(clickQueue(true, false)), true, "Shift queues behind (an unload may wait on a pickup)");
    flag(queuesBehind(clickQueue(false, true)), false, "Ctrl goes first: no unload on an empty transport");
    flag(queuesBehind(clickQueue(true, true)), false, "Ctrl+Shift is Ctrl");
    flag(queuesBehind(clickQueue(false, false)), false, "plain replaces");
    row(followQueue(kQueueReplace), kQueueAppend, "after a replacing first order, the rest append");
    row(followQueue(kQueueAppend | kQueueToggle), kQueueAppend | kQueueToggle,
        "after a Shift first order, each is a Shift order too (every target toggles)");
    row(followQueue(kQueueFront | kQueueToggle), kQueueFront | kQueueToggle, "after a Ctrl first order, each goes to the front");
}

int main() {
    modifierRows();
    rightClickRows();
    armedRows();
    boxRows();
    selectRows();
    parity();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("click rules: all rows pass\n");
    return 0;
}
