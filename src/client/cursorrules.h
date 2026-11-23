#pragma once

// Which pointer cursor the game view shows, as pure decisions over facts the
// view gathers (GameView::desiredCursor). Viewer-only: nothing here touches the
// simulation or its hash.
//
// Retail (KINGDOMS.icd) picks the cursor once a frame in 0x525d30: build
// placement and the interface first, then the hovered unit (0x521260, one unit:
// the nearest drawn one in the main view, the nearest dot within 2 px on the
// minimap), then the aggregator 0x521cd0, which runs the per-unit selector
// 0x4dd780 for every selected unit of the local player except the hovered one
// and keeps the LOWEST cursor slot. The slot numbers are the order the loader at
// 0x4bdc0e registers the art in (retailCursorSlot below).
//
// The default action mode (nothing armed) depends on the interface. Retail's
// code default is the left-click interface (0x499940 sets options+5 = 1), but
// shipped installs run with UseLeftClickIface=0, the right-click interface our
// orders follow (friendlyorder.h, rightClickOrder). There 0x4dd780 jumps to
// 0x4de03b before any attack, repair or move test, and hover shows only four
// cursors: Select, Red, Green and Normal (cursorForHover).

#include "client/cursors.h"

#include <initializer_list>

namespace tak::client {

// Retail's cursor slot (1 = Attack .. 20 = Hourglass). The aggregator keeps the
// lowest slot over the selected units. PathIcon is no pointer; it sorts last.
inline int retailCursorSlot(CursorId c) {
    switch (c) {
        case CursorId::Attack:    return 1;
        case CursorId::Airstrike: return 2;
        case CursorId::TooFar:    return 3;
        case CursorId::Capture:   return 4;
        case CursorId::Defend:    return 5;
        case CursorId::Repair:    return 6;
        case CursorId::Patrol:    return 7;
        case CursorId::Pickup:    return 8;
        case CursorId::Teleport:  return 9;
        case CursorId::Revive:    return 10;
        case CursorId::Reclaim:   return 11;
        case CursorId::Load:      return 12;
        case CursorId::Unload:    return 13;
        case CursorId::Move:      return 14;
        case CursorId::Select:    return 15;
        case CursorId::FindSite:  return 16;
        case CursorId::Red:       return 17;
        case CursorId::Green:     return 18;
        case CursorId::Normal:    return 19;
        case CursorId::Hourglass: return 20;
        default:                  return 21;
    }
}

inline CursorId lowerCursorSlot(CursorId a, CursorId b) {
    return retailCursorSlot(b) < retailCursorSlot(a) ? b : a;
}

// Where the pointer is (retail 0x528430: main view, minimap, or neither).
enum class PointerArea {
    Unknown,     // no pointer position yet
    World,       // the main view
    Minimap,     // pointing through the minimap
    Interface,   // the command panel, the bottom bar, the build strip, an overlay
};

// What 0x4dd780 sees in the hovered unit, from the local player's side.
enum class HoverKind {
    None,           // no unit under the pointer: the ground point counts
    OwnSelectable,  // own, finished and not carried: what a left-click selects
    Friendly,       // an ally's unit, or one of ours that is not selectable
                    // (under construction: the site a builder works on included)
    Enemy,
};

// The first stage of 0x525d30 / GameView::desiredCursor: a fixed cursor, or
// which of the two evaluations below decides it.
enum class CursorStage { Fixed, Armed, Hover };

struct CursorFrame {
    bool modalUi = false;          // lobby, exit menu, give-units menu, Options, Benchmark results
    PointerArea area = PointerArea::Unknown;
    bool watcher = false;          // a spectator or replay: no orders to show
    bool watcherOverUnit = false;  // ...with a finished unit under the pointer
    bool placing = false;          // a build item armed (retail action mode 14)
    bool minimapPlaceOk = false;   // ...over the minimap, a click there would place it
    bool areaBoxDrag = false;      // a right-drag or armed box, past its area threshold
    CursorId areaBoxCursor = CursorId::Normal;   // ...what its release does (clickrules.h boxCursor)
    bool boxDrag = false;          // left-drag selection box
    bool armed = false;            // a map command armed (pendingCmd_)
};

// Retail draws the placement ghost and then sets Normal in mode 14 (0x525d30):
// an invalid site shows only on the ghost. Over the minimap our click places
// (owner C2b, retail 0x527eb0), so the cursor is FindSite where it would and
// Normal where it would not. Every interface area outside the main view and the
// minimap is Normal. Our watcher keeps Select over units (retail has no
// spectator; unchanged).
inline CursorStage cursorStage(const CursorFrame& f, CursorId& fixed) {
    fixed = CursorId::Normal;
    if (f.modalUi) return CursorStage::Fixed;
    if (f.watcher) {
        if (f.area == PointerArea::World && !f.boxDrag && f.watcherOverUnit)
            fixed = CursorId::Select;
        return CursorStage::Fixed;
    }
    if (f.area == PointerArea::Unknown || f.area == PointerArea::Interface) return CursorStage::Fixed;
    if (f.placing) {
        if (f.area == PointerArea::Minimap && f.minimapPlaceOk) fixed = CursorId::FindSite;
        return CursorStage::Fixed;
    }
    // A box past its threshold shows what its release does: Load, Attack /
    // Airstrike, Reclaim, or Normal when nothing would happen (clickrules.h).
    if (f.areaBoxDrag) { fixed = f.areaBoxCursor; return CursorStage::Fixed; }
    if (f.boxDrag) return CursorStage::Fixed;
    return f.armed ? CursorStage::Armed : CursorStage::Hover;
}

// Nothing armed (action mode 1, right-click interface; 0x521cd0 and 0x4de03b).
struct HoverFacts {
    HoverKind target = HoverKind::None;
    // Some live selected unit of ours other than the hovered one. Without one,
    // 0x521cd0 shows Select over an own selectable unit and Normal otherwise.
    bool othersSelected = false;
    // No unit hovered: an explored body at the point that a selected unit can
    // raise (canresurrect 0x497100 / cananimate 0x496fd0)...
    bool raisableAtPoint = false;
    // ...or a reclaimable feature there with a selected reclaimer (0x496ea0).
    bool reclaimableAtPoint = false;
};

inline CursorId cursorForHover(const HoverFacts& f) {
    if (!f.othersSelected)
        return f.target == HoverKind::OwnSelectable ? CursorId::Select : CursorId::Normal;
    switch (f.target) {
        case HoverKind::OwnSelectable: return CursorId::Select;
        case HoverKind::Enemy:         return CursorId::Red;
        case HoverKind::Friendly:      return CursorId::Green;
        case HoverKind::None:
            return f.raisableAtPoint || f.reclaimableAtPoint ? CursorId::Green : CursorId::Normal;
    }
    return CursorId::Normal;
}

// One selected unit under an armed ATTACK (action mode 3, 0x4dd780). Over an
// enemy, retail's selector exactly. Over ground or a friendly unit the click
// fires at the point (clickrules.h armedAttack), and retail's selector
// (0x4dd8ac..0x4ddc1b S) shows Airstrike for a `dropped` slot; for a mover,
// Attack over ground and TooFar over a friendly unit, a melee one Attack only
// over a clearable blocking feature (TooFar elsewhere); for a structure TooFar
// over a friendly unit, over ground Attack in reach and outside its minimum
// range, else TooFar (an anti-air weapon too). Ours, so that the cursor
// describes the click: a unit given the fire at the point shows Attack (its
// Airstrike for a `dropped` slot) -- the order is given out of reach and over a
// friendly unit too -- and an attacker given nothing shows TooFar, where retail
// shows Attack for an anti-air mover over ground.
struct AttackUnitFacts {
    bool rallyHolder = false;   // ours: a factory's Attack sends its output
    bool attacker = false;      // canattack, alive, finished, not carried
    bool airstrike = false;     // its active weapon carries the Airstrike bit
    bool mobile = false;        // it has a mover (unit+8)
    // The target can be hit: for an enemy, cursorCanAttackEnemy; for a friendly
    // target only an immobile unit's immunity and range tests apply.
    bool canHit = false;
    bool pointFire = false;     // ground or a friendly unit: the click fires at the point
};

inline CursorId attackUnitCursor(HoverKind target, const AttackUnitFacts& u) {
    if (u.rallyHolder) return CursorId::Attack;
    if (!u.attacker) return CursorId::Normal;
    if (target != HoverKind::Enemy) {
        if (!u.pointFire) return CursorId::TooFar;
        return u.airstrike ? CursorId::Airstrike : CursorId::Attack;
    }
    // Retail shows Airstrike over any enemy; ours only over one the click attacks
    // (one it cannot hurt gets no order, so TooFar, as for any other weapon).
    if (!u.canHit) return CursorId::TooFar;
    return u.airstrike ? CursorId::Airstrike : CursorId::Attack;
}

// Every other armed command is decided per unit with its order, in
// clickrules.h (armedClick), so the cursor and the click share one rule.

} // namespace tak::client
