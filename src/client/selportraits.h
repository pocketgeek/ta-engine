// The info bar's multi-selection portrait strip: one portrait per unit TYPE in the
// selection, each carrying how many of that type are selected. The first portrait
// anchors the strip at the single-unit portrait's right edge; the rest extend to its
// LEFT, every one -- the first included -- the same size.
// Clicking a portrait narrows the selection to that type, Shift+click drops it.
//
// Pure (no SDL, no GameView) so tools/selportraits_test.cpp can check the grouping,
// the click rule, the fit/overflow layout, and the bar's group totals and squad label. Selection is local client state --
// nothing here touches the simulation.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace tak::selportraits {

struct Group {
    std::string type;        // UnitType::id
    std::vector<int> ids;    // those selected units, in selection order
};

// `units` is the selection in order as (unit id, type id), LIVE units only (the
// caller drops dead/typeless ids). The front unit's type leads -- it anchors the
// strip at the single-unit portrait's spot -- then the rest by count, most first,
// ties by type id.
inline std::vector<Group> group(const std::vector<std::pair<int, std::string>>& units) {
    std::vector<Group> out;
    for (const auto& [id, type] : units) {
        auto it = std::find_if(out.begin(), out.end(), [&](const Group& g) { return g.type == type; });
        if (it == out.end()) out.push_back({type, {id}});
        else it->ids.push_back(id);
    }
    if (out.size() > 2)
        std::sort(out.begin() + 1, out.end(), [](const Group& a, const Group& b) {
            return a.ids.size() != b.ids.size() ? a.ids.size() > b.ids.size() : a.type < b.type;
        });
    return out;
}

// A portrait click. Plain: keep only `ids` (the clicked type's selected units).
// remove (Shift): drop them. The surviving units keep their selection order.
inline std::vector<int> click(const std::vector<int>& selection, const std::vector<int>& ids,
                              bool remove) {
    std::vector<int> out;
    for (int id : selection)
        if ((std::find(ids.begin(), ids.end(), id) != ids.end()) != remove) out.push_back(id);
    return out;
}

// Fit `n` portraits (the primary included) into a strip that runs LEFT from the
// primary's full-size spot: that spot plus `room` to its left. Every portrait --
// the primary too -- is the same size, so the strip reads as one row: full size
// while it fits (the primary then exactly where a single unit's sits), else one row
// shrunk to fit but not below minScale. Past that the strip folds into TWO rows of
// half-height portraits (column by column, top first). When even that cannot hold
// them all, the last cell becomes an overflow tile ("+K") and `shown` portraits
// (primary included) are drawn -- the strip never leaves its span.
struct Layout {
    int shown = 1;           // type portraits drawn, primary included
    int rows = 1;            // 1, or 2 for the folded strip
    float scale = 1.0f;      // size of EVERY slot, the primary's included, vs full size
    bool overflow = false;   // a "+K" tile follows the last portrait (K = n - shown)
    float width = 0;         // room used left of the primary's full-size spot
};

inline Layout layout(int n, float fullW, float fullH, float gap, float room, float minScale) {
    Layout L;
    if (n <= 1 || fullW <= 0 || fullH <= 0) return L;
    const float span = std::max(room, 0.0f) + fullW;   // the primary's spot + the room
    const float s1 = std::min(1.0f, (span - float(n - 1) * gap) / (float(n) * fullW));
    int cols = n;
    if (s1 >= minScale) {
        L.shown = n;
        L.scale = s1;
    } else {
        L.rows = 2;
        L.scale = std::max(0.0f, (fullH - gap) / (2 * fullH));   // two rows + a gap = full height
        cols = int(std::floor((span + gap) / (gap + fullW * L.scale)));
        const int cells = 2 * cols;
        if (cells >= n) {
            L.shown = n;
            cols = (n + 1) / 2;
        } else if (cells >= 2) {   // the last cell holds the "+K" tile
            L.shown = cells - 1;
            L.overflow = true;
        } else {                   // not even one column (a degenerate gap): the primary alone
            L = Layout{};
            return L;
        }
    }
    L.width = std::max(0.0f, float(cols) * (fullW * L.scale + gap) - gap - fullW);
    return L;
}

// The room a strip may fill: the span from `lo` to `hi` between the primary's near
// edge and the bar's REAL limit -- the screen's left edge on the retail bar, the mana
// panel on the stone one -- less `margin`. Nothing nearer caps it (the retail bar's
// EndCap scroll art is drawn over), so the portraits stay full size until a
// full-size strip would pass that limit. A primary already past it: no room.
inline float room(float lo, float hi, float margin) {
    return std::max(0.0f, hi - lo - margin);
}

// Slot i's rectangle (slot 0 is the primary; slot `shown` is the overflow tile),
// given the primary's full-size one. Every slot is the same size, the strip's right
// edge stays on the primary's: a single row is centred on the primary's height, a
// folded strip fills it, top row first -- the primary top-right.
struct Rect { float x, y, w, h; };
inline Rect slot(const Layout& L, int i, Rect primary, float gap) {
    const float w = primary.w * L.scale, h = primary.h * L.scale;
    const int col = std::max(i, 0) / L.rows, row = std::max(i, 0) % L.rows;
    const float y = L.rows == 1 ? primary.y + (primary.h - h) * 0.5f
                                : primary.y + float(row) * (h + gap);
    return {primary.x + (primary.w - w) - float(col) * (gap + w), y, w, h};
}

// The bar's HP / mana for a multi-selection: totals over the selection, not the
// front unit's. HP is sum(hp) / sum(maxHp) over the live selected units; mana is the
// same over only the units that HAVE a pool (maxMana > 0), so a squad of knights with
// two priests shows the priests' mana rather than a sliver of it. No pool anywhere:
// hasMana is false and the bar is hidden, as it is for a single unit without one.
struct Vitals { float hp = 0, maxHp = 0, mana = 0, maxMana = 0; };
struct Totals {
    float hp = 0, maxHp = 0, mana = 0, maxMana = 0;
    bool hasMana = false;
    float hpFrac() const { return hp / std::max(1.0f, maxHp); }   // the single unit's guard
    float manaFrac() const { return hasMana ? mana / maxMana : 0.0f; }
};
inline Totals total(const std::vector<Vitals>& units) {
    Totals t;
    for (const Vitals& v : units) {
        t.hp += v.hp;
        t.maxHp += v.maxHp;
        if (v.maxMana > 0) {
            t.mana += v.mana;
            t.maxMana += v.maxMana;
            t.hasMana = true;
        }
    }
    return t;
}

// Is a multi-selection exactly one control squad (the Ctrl+digit group or formation
// whose number shows in green under its units)? `sel` is (squad, is a builder) for
// every LIVE selected unit (squad 0: none, or not the local player's); `unselected`
// is the same for every live, finished unit of the local player that has a squad but
// is NOT selected. The selection IS squad s when it is all in s and is either ALL of
// s, or exactly what recalling s selects -- recall skips builders, so its members
// minus every builder. A partial squad, or any extra unit, is not the squad: 0 -- and
// so is some-but-not-all of its builders (a part of an all-builder squad included).
// One unit is never a squad here (the bar shows its own name). Returns the squad
// value: +N group N, -N formation N.
inline int selectionSquad(const std::vector<std::pair<int, bool>>& sel,
                          const std::vector<std::pair<int, bool>>& unselected) {
    if (sel.size() < 2 || sel.front().first == 0) return 0;
    const int s = sel.front().first;
    bool selBuilder = false;
    for (const auto& [q, builder] : sel) {
        if (q != s) return 0;
        selBuilder |= builder;
    }
    for (const auto& [q, builder] : unselected)
        if (std::abs(q) == std::abs(s) && (!builder || selBuilder)) return 0;
    return s;
}

// The name slot's label for squad `s`, in the marker's terms: the recall key, so
// squad 10 is "0". "GROUP 3", "FORMATION 2"; empty for no squad.
inline std::string squadLabel(int s) {
    if (s == 0) return {};
    const int num = std::abs(s);
    const char key = num == 10 ? '0' : char('0' + num);
    return std::string(s < 0 ? "FORMATION " : "GROUP ") + key;
}

}  // namespace tak::selportraits
