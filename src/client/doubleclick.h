// doubleclick -- the rule for a double-click on a unit: select every unit of the same
// type that is yours and drawn inside the map view. Pure data in, ids out, so a test
// can drive it without a window. Selection is client-local: nothing here is hashed,
// sent, or part of the protocol.
//
// Retail's input handler has no double-click select (its Ctrl+Z / Ctrl+X hotkeys do
// the work); this follows the user's request. The timing is SDL's own click counter
// (the platform double-click time), and the second press must land on the SAME unit
// the first click selected, so two quick clicks on neighbours stay two single clicks.
#pragma once

#include <vector>

namespace tak::client::doubleclick {

// One candidate, as the client sees it: where it is DRAWN (terrain lift, flyer
// altitude, or its tactical dot), already projected to the screen.
struct Candidate {
    int id = 0;
    const void* type = nullptr;   // UnitType identity
    bool mine = false;            // owned by the local player
    bool selectable = true;       // alive and not under construction
    float sx = 0, sy = 0;         // drawn position, screen pixels
};

// The map view's rectangle in screen pixels: left of the command panel, above the bar.
struct View {
    float w = 0, h = 0;
};

inline bool inView(const Candidate& c, const View& v) {
    return c.sx >= 0 && c.sy >= 0 && c.sx <= v.w && c.sy <= v.h;
}

// A double-click is a press SDL counts as >= 2 whose previous press in SDL's chain
// was the plain select-click on the SAME unit (lastClicks == sdlClicks - 1). A triple
// click therefore expands again rather than collapsing to the one unit, and a press
// in between that something else consumed (an order click, a HUD button) breaks the
// chain instead of pairing with an older select-click.
inline bool isDouble(int sdlClicks, int hitId, int lastHitId, int lastClicks) {
    return sdlClicks >= 2 && lastClicks == sdlClicks - 1 && hitId >= 0 && hitId == lastHitId;
}

// Ids to select for a double-click on `hitId`. Only the player's own units expand; an
// enemy or allied unit (or nothing) yields just the hit, as a single click does.
// The hit itself is always included (even if it sits at the view edge), first.
inline std::vector<int> expand(int hitId, const std::vector<Candidate>& all, const View& v) {
    std::vector<int> out;
    if (hitId < 0) return out;
    out.push_back(hitId);
    const Candidate* hit = nullptr;
    for (const auto& c : all) if (c.id == hitId) { hit = &c; break; }
    if (!hit || !hit->mine || !hit->type) return out;
    for (const auto& c : all)
        if (c.id != hitId && c.mine && c.selectable && c.type == hit->type && inView(c, v))
            out.push_back(c.id);
    return out;
}

}  // namespace tak::client::doubleclick
