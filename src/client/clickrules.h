#pragma once

// What a click orders, as pure decisions over facts the game view gathers per
// selected unit (GameView::clickUnit). Viewer-only: the commands these choose are
// ordinary ones the sim already takes, so nothing here touches its hash.
//
// Retail (KINGDOMS.icd, right-click interface) issues every order through
// 0x5214b0, which runs the per-unit resolver 0x4de530 for each selected unit
// except the hovered one (0x521562, 0x521625) in the current action mode. The
// cursor (cursorrules.h) comes from the per-unit selector 0x4dd780 over the same
// list. The armed functions below return both for one unit -- the order and the
// cursor -- so the cursor always describes the click: an armed left-click acts
// only when the aggregated cursor is an order cursor (retail 0x527dd0 /
// 0x525ce0, isOrderCursor), and the parity test in click_rules_test proves the
// two agree.
//
// S = read in the code, I = inferred, as in PLAN.md / docs/retail-engine.md.

#include "client/cursorrules.h"
#include "client/friendlyorder.h"
#include "net/lockstep.h"

#include <cstdint>
#include <vector>

namespace tak::client {

// One unit's share of a click. Load is the passenger boarding (Cmd::Load with
// the passenger as the unit); Pickup is the same command sent for the transport
// picking the target up, unitId the passenger.
enum class ClickOrder : uint8_t {
    None, Move, Attack, AttackMove, Patrol, Assist, Repair, Board, Pickup, Guard,
    Resurrect, Reclaim, ReclaimArea, Unload, AttackGround,
};

// The hovered unit (P1, GameView::hoveredUnit), from the local player's side.
struct ClickTarget {
    HoverKind kind = HoverKind::None;
    bool flies = false;   // a flyer (guard's flyer rule, 0x4dec78)
};

// One selected unit of ours that is alive, finished and not carried. The game
// view gathers these against the hovered unit and the ground point; a unit that
// is none of that gets no order at all.
struct ClickUnit {
    bool self = false;          // it IS the hovered unit: left out, as retail does
    bool mobile = false;        // it has a mover (not a structure)
    bool rallyHolder = false;   // ours: a producer's Move/Attack set its output's rally
    bool attacker = false;      // canattack (isCursorAttacker)
    bool airstrike = false;     // its active weapon carries the Airstrike cursor
    bool canHit = false;        // cursorCanAttackEnemy against the hovered unit
    // Armed ATTACK on ground or a friendly unit: the resolver gives it a fire at
    // the point (World::suppressRule by its selected slot, a melee one only over
    // a clearable blocking feature there; Cmd::AttackGround).
    bool pointFire = false;
    bool canPatrol = false;
    bool canGuard = false;      // the type's canguard bit
    bool flies = false;
    bool transport = false;     // canTransport
    bool loaded = false;        // a transport with cargo aboard
    bool assist = false;        // canAssistSite(this, hovered): HelpBuild
    bool board = false;         // canLoadPassenger(this, hovered): it may board the hovered transport
    bool pickup = false;        // the selection's one weaponless transport, and the hovered unit may board it
    bool loadTarget = false;    // a transport the hovered unit may board (armed LOAD)
    FriendlyOrder friendly = FriendlyOrder::None;   // friendlyUnitOrder on the hovered unit
    bool raise = false;         // it can raise the body at the point (no unit hovered)
    bool reclaim = false;       // a reclaimer, with a reclaimable target at the point
};

// The resolver's friendly branch (0x4de530 mode 1/2 over an own or allied unit,
// S): HelpBuild, else (ours) board an own transport it fits in, else Repair,
// else pickup by the single weaponless transport, else FOLLOW (Guard), else
// Move. A rally holder's Move sets its output's rally (ours); a repeating
// builder's Guard and Repair are refused by the sim, so it rallies instead.
// Board sits ahead of Repair: a passenger that fits boards the transport (C4c),
// the rest guard it, which is retail's action for them.
inline ClickOrder friendlyClick(const ClickUnit& u) {
    if (u.self) return ClickOrder::None;
    if (u.assist) return ClickOrder::Assist;
    if (u.board) return ClickOrder::Board;
    if (u.friendly == FriendlyOrder::Repair) return ClickOrder::Repair;
    if (u.pickup) return ClickOrder::Pickup;
    if (u.friendly == FriendlyOrder::Guard) return ClickOrder::Guard;
    if (u.friendly == FriendlyOrder::Move || u.rallyHolder) return ClickOrder::Move;
    return ClickOrder::None;
}

// A right-click with nothing armed (action mode 1, 0x4de530 S), per unit:
//  - an enemy: Attack when the unit can attack and the target resolves
//    (cursorCanAttackEnemy); a unit that can attack but cannot hurt it does
//    nothing (0x4df215); a unit without a weapon moves to the point. Ours: a
//    rally holder's Attack sends its output, and an immobile tower keeps an
//    Attack on a target out of range (the sim fires once it comes in).
//  - an own or allied unit: friendlyClick.
//  - ground: RESURRECT a body it can raise, else RECLAIM what is there, else
//    Move. A loaded transport just moves (retail: unloading needs UNLOAD
//    armed); ours: with Alt held (retail never reads Alt) it unloads there.
inline ClickOrder rightClick(const ClickTarget& t, const ClickUnit& u, bool alt) {
    if (u.self) return ClickOrder::None;
    switch (t.kind) {
        case HoverKind::Enemy:
            if (u.rallyHolder) return ClickOrder::Attack;
            if (u.attacker) return u.canHit || !u.mobile ? ClickOrder::Attack : ClickOrder::None;
            return u.mobile ? ClickOrder::Move : ClickOrder::None;
        case HoverKind::OwnSelectable:
        case HoverKind::Friendly:
            return friendlyClick(u);
        case HoverKind::None:
            if (alt && u.transport && u.loaded) return ClickOrder::Unload;
            if (u.raise) return ClickOrder::Resurrect;
            if (u.reclaim) return ClickOrder::Reclaim;
            return u.mobile || u.rallyHolder ? ClickOrder::Move : ClickOrder::None;
    }
    return ClickOrder::None;
}

// One unit under an armed command: the order its click gives and the cursor its
// selector shows. The view keeps the lowest cursor slot over the units.
struct UnitClick {
    ClickOrder order = ClickOrder::None;
    CursorId cursor = CursorId::Normal;
};

// What the armed decision needs from the selection as a whole.
struct ArmedSelection {
    // 'g': some selected unit may guard the target -- canguard, and it flies or
    // the target does not (0x4dd780 S). With one, EVERY guard-capable unit guards
    // (the resolver 0x4dec78 has no flyer rule): a mixed selection with a flyer
    // guards a flyer with its ground units too.
    bool guardOk = false;
    bool singleTransport = false;   // 'l': exactly one transport selected (0x520b60)
    bool queue = false;             // Shift (append, not Ctrl): an unload may be queued on an empty transport
};

inline CursorId friendlyCursor(ClickOrder o) {
    switch (o) {
        case ClickOrder::Assist: case ClickOrder::Repair: return CursorId::Repair;
        case ClickOrder::Board: case ClickOrder::Pickup:  return CursorId::Load;
        case ClickOrder::Guard:                           return CursorId::Defend;
        case ClickOrder::Move:                            return CursorId::Move;
        default:                                          return CursorId::Normal;
    }
}

// Armed ATTACK, one unit (mode 3; the cursor is attackUnitCursor).
//  - an enemy: Attack when it can hit it; a rally holder rallies; an immobile
//    unit keeps the order out of range (cursor TooFar, the sim fires once the
//    target comes in). A mobile unit that cannot hurt the target gets nothing
//    -- the per-unit TooFar retail shows for it, the one order cursor that may
//    come with no command.
//  - ground or a friendly unit: a fire at the click point, no target unit
//    (0x4de65f..0x4de7e3 S): SUPPRESS for a ground unit or a tower, AIRSTRIKE /
//    AIRTOGROUNDHOVER / AIRTOGROUND for a flyer (World::suppress), when
//    pointFire; nothing otherwise (an anti-air weapon, a melee one off a
//    clearable blocker). The cursor is attackUnitCursor's. Ours: a rally holder
//    sets an attack-move rally there.
inline UnitClick armedAttack(const ClickTarget& t, const ClickUnit& u) {
    if (u.self) return {};
    AttackUnitFacts f;
    f.rallyHolder = u.rallyHolder; f.attacker = u.attacker; f.airstrike = u.airstrike;
    f.mobile = u.mobile; f.canHit = u.canHit; f.pointFire = u.pointFire;
    const CursorId c = attackUnitCursor(t.kind, f);
    if (t.kind == HoverKind::Enemy)
        return {u.rallyHolder || (u.attacker && (u.canHit || !u.mobile)) ? ClickOrder::Attack : ClickOrder::None, c};
    if (u.rallyHolder) return {ClickOrder::AttackMove, c};
    return {u.attacker && u.pointFire ? ClickOrder::AttackGround : ClickOrder::None, c};
}

inline UnitClick armedClick(char cmd, const ClickTarget& t, const ClickUnit& u, const ArmedSelection& sel) {
    if (u.self) return {};
    const bool friendly = t.kind == HoverKind::OwnSelectable || t.kind == HoverKind::Friendly;
    switch (cmd) {
        case 'a': return armedAttack(t, u);
        case 'm': {
            // Mode 2 (0x4df0b9..0x4df1d2 S): over an own or allied unit the
            // friendly resolver; over an enemy, a body, a feature or ground a
            // Move. Retail's cursor shows Revive over a body while its click
            // moves; ours shows Move, what the click does (C8a).
            const ClickOrder o = friendly ? friendlyClick(u)
                               : u.mobile || u.rallyHolder ? ClickOrder::Move : ClickOrder::None;
            return {o, friendlyCursor(o)};
        }
        case 'f': {
            // Fight-move: retail's `UnitCommand MoveFight` (0x4b0e40 S) presses
            // MOVE and sets game+0x17633 |= 0x400. The resolver and the selector
            // never read that bit, so this is armed MOVE target for target (the
            // friendly resolver, else Move; the same cursors); the order code
            // (0x521bf4..0x521c5b) then gives only a resolved Move_Ground /
            // VTOL_Move / QMove response mode -1 -- our AttackMove. HelpBuild,
            // Repair, a pickup and Follow go as they are.
            ClickOrder o = friendly ? friendlyClick(u)
                         : u.mobile || u.rallyHolder ? ClickOrder::Move : ClickOrder::None;
            const CursorId c = friendlyCursor(o);
            if (o == ClickOrder::Move) o = ClickOrder::AttackMove;
            return {o, c};
        }
        case 'p':
            return u.canPatrol || u.rallyHolder ? UnitClick{ClickOrder::Patrol, CursorId::Patrol} : UnitClick{};
        case 'g':
            return friendly && u.canGuard && sel.guardOk ? UnitClick{ClickOrder::Guard, CursorId::Defend}
                                                       : UnitClick{};
        case 'r': {
            if (!friendly) return {};
            if (u.assist) return {ClickOrder::Assist, CursorId::Repair};
            if (u.friendly == FriendlyOrder::Repair) return {ClickOrder::Repair, CursorId::Repair};
            return {};
        }
        case 'l':
            return sel.singleTransport && u.loadTarget ? UnitClick{ClickOrder::Pickup, CursorId::Load} : UnitClick{};
        case 'u':
            return u.transport && (u.loaded || sel.queue) ? UnitClick{ClickOrder::Unload, CursorId::Unload}
                                                          : UnitClick{};
        case 'c':
            // Mode 12 (0x4decd6..0x4deef6 S): RESURRECT first, else RECLAIM.
            // Retail's cursor says Reclaim over a body that is also a reclaimable
            // feature; ours says Revive there, the order the click gives.
            if (u.raise) return {ClickOrder::Resurrect, CursorId::Revive};
            if (u.reclaim) return {ClickOrder::Reclaim, CursorId::Reclaim};
            return {};
        default: return {};
    }
}

// The cursors an armed left-click acts under (0x527dd0 S): an order cursor
// issues the order; Select selects; Red, Green and Normal do nothing and the
// mode stays armed. FindSite is placement's.
inline bool isOrderCursor(CursorId c) {
    return retailCursorSlot(c) < retailCursorSlot(CursorId::Select) || c == CursorId::FindSite;
}

// The queue byte a player's order click sends (net/lockstep.h; retail B.4,
// read off the event's +8 flags at 0x5214bf S): Ctrl, with or without Shift,
// puts the order at the head of the queue without clearing it (0x4d7a09 ->
// 0x4d7750 S); Shift appends (0x4d77f0 S); both carry the toggle, which takes
// back a queued order of the same kind on the same target or within 16 px
// (0x4d7a5b..0x4d7ab2 S). Alt is never read. Plain replaces.
inline uint8_t clickQueue(bool shift, bool ctrl) {
    if (ctrl) return tak::net::kQueueFront | tak::net::kQueueToggle;
    if (shift) return tak::net::kQueueAppend | tak::net::kQueueToggle;
    return tak::net::kQueueReplace;
}
// Whether the order goes behind the unit's queue (Shift), so that something
// queued ahead of it -- a pickup ahead of an unload -- runs first. Ctrl's goes
// first and plain's alone.
inline bool queuesBehind(uint8_t queue) {
    return (queue & tak::net::kQueuePlace) == tak::net::kQueueAppend;
}
// A click's follow-on orders for one unit (a box's second and later targets):
// behind a replacing first order they append; a Shift or Ctrl click's go as
// the first went, each a Shift or Ctrl order of its own that may toggle.
inline uint8_t followQueue(uint8_t first) {
    return (first & tak::net::kQueuePlace) == tak::net::kQueueReplace ? tak::net::kQueueAppend : first;
}

// ---- Boxes: the right-drag box (2.2) and the armed left-drag box (2.9) ----

// One selected unit against the box (retail 0x520e30 S): the visible enemies
// whose drawn position lies in the screen box, and the own units there.
struct BoxUnit {
    bool attacker = false;
    bool airstrike = false;
    bool reclaimer = false;     // canreclaim (World::reclaim's caster test)
    bool raiser = false;        // canresurrect
    bool transport = false;
    int hits = 0;               // box enemies it can hurt (cursorCanAttackEnemy)
    int loadable = 0;           // own units in the box that can board it
};

enum class BoxMode : uint8_t { Right, ArmedAttack, ArmedLoad, ArmedClear };

// The box's order per selected unit, in selection order:
//  - Right: step L when the selection's one transport has no weapon and no
//    selected unit can attack, reclaim or raise -- it picks up every own unit
//    in the box that fits (selected or not). Otherwise step A, one Attack per
//    box enemy each attacker can hurt; then step C, ReclaimArea over the box
//    for a reclaimer with no attack order (owner C10b: a unit without a weapon
//    always clears; retail's one-enemy quirk is dropped).
//  - ArmedAttack: step A only. ArmedLoad: step L with exactly one transport.
//    ArmedClear: every reclaimer clears the box (0x527d00 S).
// Pickup and Attack stand for the per-passenger / per-enemy command lists the
// view builds.
inline std::vector<ClickOrder> boxOrders(BoxMode mode, const std::vector<BoxUnit>& units) {
    std::vector<ClickOrder> out(units.size(), ClickOrder::None);
    int transports = 0, blockers = 0;
    for (const auto& u : units) {
        transports += u.transport;
        blockers += u.attacker || u.reclaimer || u.raiser;
    }
    const bool loadStep = mode == BoxMode::ArmedLoad ? transports == 1
                        : mode == BoxMode::Right && transports == 1 && blockers == 0;
    for (size_t i = 0; i < units.size(); ++i) {
        const BoxUnit& u = units[i];
        if (loadStep) {
            if (u.transport && u.loadable > 0) out[i] = ClickOrder::Pickup;
            continue;
        }
        if (mode == BoxMode::ArmedLoad) continue;
        if (mode == BoxMode::ArmedClear) { if (u.reclaimer) out[i] = ClickOrder::ReclaimArea; continue; }
        if (u.attacker && u.hits > 0) out[i] = ClickOrder::Attack;
        else if (mode == BoxMode::Right && u.reclaimer) out[i] = ClickOrder::ReclaimArea;
    }
    return out;
}

// The cursor while such a box is past its threshold: what the release does.
// Retail shows Attack over an empty box when attackers are selected, though its
// release does nothing there; ours shows Normal, as the click does nothing.
inline CursorId boxCursor(const std::vector<BoxUnit>& units, const std::vector<ClickOrder>& orders) {
    bool load = false, attack = false, allAir = true, clear = false;
    for (size_t i = 0; i < units.size() && i < orders.size(); ++i) {
        switch (orders[i]) {
            case ClickOrder::Pickup: load = true; break;
            case ClickOrder::Attack: attack = true; allAir &= units[i].airstrike; break;
            case ClickOrder::ReclaimArea: clear = true; break;
            default: break;
        }
    }
    if (load) return CursorId::Load;
    if (attack) return allAir ? CursorId::Airstrike : CursorId::Attack;
    if (clear) return CursorId::Reclaim;
    return CursorId::Normal;
}

// ---- Left-click select with nothing armed (2.11) ----

enum class SelectAction : uint8_t { Keep, Replace, Add, Remove, Clear };

// A plain left-click (no box), retail 0x51fb50 S with the owner's choices:
//  - an own finished unit: select it; Shift toggles it (retail; C7a: a
//    selected unit is dropped), Ctrl removes it (ours).
//  - an enemy, an ally's unit or an own unfinished one: retail deselects all.
//    Ours (C6c): with nothing of ours selected it is selected for inspection;
//    with a selection, a plain click deselects all as retail; Shift/Ctrl keep it.
//  - ground: a plain click deselects all; Shift/Ctrl keep it.
// A watcher selects any unit it clicks (retail has no spectator).
struct SelectClick {
    HoverKind target = HoverKind::None;
    bool shift = false, ctrl = false;
    bool alreadySelected = false;   // the clicked unit is in the selection
    bool ownSelected = false;       // the selection holds a unit of ours
    bool watcher = false;
};

inline SelectAction selectClick(const SelectClick& s) {
    if (s.watcher) {
        if (s.target == HoverKind::None) return s.shift || s.ctrl ? SelectAction::Keep : SelectAction::Clear;
        if (s.ctrl) return SelectAction::Remove;
        return s.shift ? SelectAction::Add : SelectAction::Replace;
    }
    switch (s.target) {
        case HoverKind::OwnSelectable:
            if (s.ctrl) return SelectAction::Remove;
            if (s.shift) return s.alreadySelected ? SelectAction::Remove : SelectAction::Add;
            return SelectAction::Replace;
        case HoverKind::Friendly:
        case HoverKind::Enemy:
            if (s.shift || s.ctrl) return SelectAction::Keep;
            return s.ownSelected ? SelectAction::Clear : SelectAction::Replace;
        case HoverKind::None:
            return s.shift || s.ctrl ? SelectAction::Keep : SelectAction::Clear;
    }
    return SelectAction::Keep;
}

} // namespace tak::client
