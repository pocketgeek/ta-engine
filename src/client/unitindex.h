#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace tak::render {

// Broad-phase unit candidates (workstream L), the unit counterpart of FeatureIndex.
//
// A UnitGrid belongs to ONE render snapshot (Frame): captureFrame builds it from the
// positions it has just copied (UnitR::x/z), in the order of Frame::live, so it swaps
// with the snapshot and is never shared with the simulation. Entries are indices into
// Frame::live. A query returns, in ascending live order, every unit whose position MAY
// lie in a world rectangle: a superset (whole grid cells), never a subset. Callers keep
// their exact per-unit test, so the accepted units -- and their order, which decides
// equal-depth painter order -- are exactly those of a scan over the whole list.
//
// Cells are 256 world px (doubled until each side has at most kMaxSide cells), laid
// over the bounding box of the snapshot's finite positions; a unit's cell index is a
// monotone function of its position (clamped to the grid), so every point of a query
// rectangle maps into the query's cell range. Units of a cell are stored in ascending
// live order, cells row-major, so one grid row of a query is one contiguous run.
// Non-finite positions (never produced by the snapshot) are returned by every query.
class UnitGrid {
public:
    static constexpr float kCell = 256.0f;
    static constexpr int kMaxSide = 256;

    struct QueryStats { uint32_t rows = 0, cells = 0, gathered = 0; };

    // xs/zs: positions in live order. scratch: the builder's reusable cell buffer.
    void build(std::span<const float> xs, std::span<const float> zs, std::vector<uint32_t>& scratch) {
        const size_t count = std::min(xs.size(), zs.size());
        count_ = uint32_t(count);
        entries_.clear(); always_.clear(); start_.clear();
        w_ = h_ = 0;
        bool any = false;
        float minX = 0, maxX = 0, minZ = 0, maxZ = 0;
        for (size_t i = 0; i < count; ++i) {
            const float x = xs[i], z = zs[i];
            if (!std::isfinite(x) || !std::isfinite(z)) continue;
            if (!any) { minX = maxX = x; minZ = maxZ = z; any = true; continue; }
            minX = std::min(minX, x); maxX = std::max(maxX, x);
            minZ = std::min(minZ, z); maxZ = std::max(maxZ, z);
        }
        minX_ = minX; maxX_ = maxX; minZ_ = minZ; maxZ_ = maxZ; any_ = any;
        if (!any) {
            for (size_t i = 0; i < count; ++i) always_.push_back(uint32_t(i));
            return;
        }
        cell_ = kCell;
        // Spans in double: a float subtraction of far-apart positions could round down.
        while (double(maxX) - double(minX) >= double(cell_) * (kMaxSide - 1) ||
               double(maxZ) - double(minZ) >= double(cell_) * (kMaxSide - 1)) cell_ *= 2.0f;
        originX_ = std::floor(minX / cell_) * cell_;
        originZ_ = std::floor(minZ / cell_) * cell_;
        w_ = std::clamp(int((double(maxX) - double(originX_)) / double(cell_)) + 1, 1, kMaxSide);
        h_ = std::clamp(int((double(maxZ) - double(originZ_)) / double(cell_)) + 1, 1, kMaxSide);
        const size_t cells = size_t(w_) * size_t(h_);
        start_.assign(cells + 1, 0);
        scratch.resize(count);
        for (size_t i = 0; i < count; ++i) {
            const float x = xs[i], z = zs[i];
            if (!std::isfinite(x) || !std::isfinite(z)) { scratch[i] = UINT32_MAX; always_.push_back(uint32_t(i)); continue; }
            const uint32_t c = uint32_t(cellZ(z)) * uint32_t(w_) + uint32_t(cellX(x));
            scratch[i] = c;
            ++start_[c + 1];
        }
        for (size_t c = 0; c < cells; ++c) start_[c + 1] += start_[c];
        entries_.resize(start_[cells]);
        // Scatter in ascending live order: each cell's run stays sorted.
        std::vector<uint32_t>& fill = fill_;
        fill.assign(start_.begin(), start_.end() - 1);
        for (size_t i = 0; i < count; ++i)
            if (scratch[i] != UINT32_MAX) entries_[fill[scratch[i]]++] = uint32_t(i);
    }

    void clear() { count_ = 0; w_ = h_ = 0; any_ = false; entries_.clear(); always_.clear(); start_.clear(); }
    size_t size() const { return count_; }
    int width() const { return w_; }
    int height() const { return h_; }
    float cell() const { return cell_; }

    // Every unit whose position may lie in [left,right] x [top,bottom] (inclusive),
    // ascending. `bits` is the caller's scratch (all zero between calls; left zero).
    // A NaN bound returns every unit (conservative); an empty or inverted rectangle
    // returns only the non-finite entries.
    void query(float left, float top, float right, float bottom,
               std::vector<uint64_t>& bits, std::vector<uint32_t>& out, QueryStats* stats = nullptr) const {
        out.clear();
        if (covers(left, top, right, bottom)) {
            all(out);
            if (stats) stats->gathered += count_;
            return;
        }
        if (std::isnan(left) || std::isnan(top) || std::isnan(right) || std::isnan(bottom)) {
            all(out);
            if (stats) stats->gathered += count_;
            return;
        }
        const size_t words = (size_t(count_) + 63) / 64;
        if (bits.size() < words) bits.resize(words, 0);
        size_t lo = words, hi = 0;
        const auto mark = [&](uint32_t i) {
            bits[i >> 6] |= uint64_t(1) << (i & 63);
            lo = std::min(lo, size_t(i >> 6)); hi = std::max(hi, size_t(i >> 6) + 1);
        };
        for (uint32_t i : always_) mark(i);
        // The stored extremes are exact: a rectangle beyond them holds no finite unit.
        if (any_ && left <= right && top <= bottom &&
            !(right < minX_ || left > maxX_ || bottom < minZ_ || top > maxZ_)) {
            const int x0 = cellX(left), x1 = cellX(right), z0 = cellZ(top), z1 = cellZ(bottom);
            for (int z = z0; z <= z1; ++z) {
                const uint32_t b = start_[size_t(z) * size_t(w_) + size_t(x0)];
                const uint32_t e = start_[size_t(z) * size_t(w_) + size_t(x1) + 1];
                for (uint32_t k = b; k < e; ++k) mark(entries_[k]);
                if (stats) { ++stats->rows; stats->cells += uint32_t(x1 - x0 + 1); stats->gathered += e - b; }
            }
        }
        // Ascending order for free; the words are cleared as they are read.
        for (size_t wd = lo; wd < hi; ++wd) {
            uint64_t v = bits[wd];
            bits[wd] = 0;
            while (v) {
                out.push_back(uint32_t(wd * 64 + size_t(std::countr_zero(v))));
                v &= v - 1;
            }
        }
    }

    // The rectangle holds every unit: no non-finite positions and the stored (exact)
    // extremes inside it. A caller may then take the whole list without a query.
    bool covers(float left, float top, float right, float bottom) const {
        return always_.empty() && (count_ == 0 || (any_ && left <= minX_ && right >= maxX_ && top <= minZ_ && bottom >= maxZ_));
    }

    void all(std::vector<uint32_t>& out) const {
        out.resize(count_);
        for (uint32_t i = 0; i < count_; ++i) out[i] = i;
    }

    size_t capacityBytes() const {
        return (entries_.capacity() + always_.capacity() + start_.capacity() + fill_.capacity()) * sizeof(uint32_t);
    }

private:
    // Monotone in the position, clamped to the grid: the cell range of a rectangle
    // contains the cell of every point inside it.
    int cellX(float x) const {
        const float f = std::floor((x - originX_) / cell_);
        return f <= 0.0f ? 0 : f >= float(w_ - 1) ? w_ - 1 : int(f);
    }
    int cellZ(float z) const {
        const float f = std::floor((z - originZ_) / cell_);
        return f <= 0.0f ? 0 : f >= float(h_ - 1) ? h_ - 1 : int(f);
    }

    uint32_t count_ = 0;
    int w_ = 0, h_ = 0;
    bool any_ = false;
    float cell_ = kCell, originX_ = 0, originZ_ = 0;
    float minX_ = 0, maxX_ = 0, minZ_ = 0, maxZ_ = 0;
    std::vector<uint32_t> start_;    // w*h+1 prefix offsets into entries_
    std::vector<uint32_t> entries_;  // live indices, by cell (row-major), ascending within a cell
    std::vector<uint32_t> always_;   // non-finite positions
    std::vector<uint32_t> fill_;     // build scratch (per grid: builds of different frames never share it)
};

// The lift a unit's anchor may carry, in world px: screen = (pos - offset - lift) * zoom.
struct LiftBounds { float minX = 0, maxX = 0, minY = 0, maxY = 0; };

// The world rectangle a unit position must lie in for its lifted anchor
//   sx = (x - offX) * zoom - liftX * zoom,  sy = (z - offY) * zoom - liftY * zoom
// to pass `!(sx < left || sx > right || sy < top || sy > bottom)` for SOME lift in
// `lift`. Widened by one world px plus one screen px: the exact test evaluates the
// expression in float, in its own order (error far below 0.01 px at map scale).
// A zoom that is not positive and finite gives an unbounded (NaN) rectangle, which
// UnitGrid::query answers with every unit.
struct WorldRect { float left, top, right, bottom; };
inline WorldRect anchorBandToWorld(float left, float top, float right, float bottom,
                                   float offX, float offY, float zoom, const LiftBounds& lift) {
    if (!(zoom > 0.0f) || !std::isfinite(zoom)) {
        const float n = std::numeric_limits<float>::quiet_NaN();
        return {n, n, n, n};
    }
    const float slack = 1.0f + 1.0f / zoom;
    return {offX + left / zoom + lift.minX - slack, offY + top / zoom + lift.minY - slack,
            offX + right / zoom + lift.maxX + slack, offY + bottom / zoom + lift.maxY + slack};
}

// The renderer's per-unit passes that cull a unit's lifted anchor against the map
// view (GameView::draw): each pass's screen position (the renderer's expression,
// evaluated here so the renderer and unit_index_test share it), its acceptance test,
// and the band of lifted-anchor positions sx = (x - offX) * zoom - liftX * zoom,
// sy = (z - offY) * zoom - liftY * zoom it can accept, for anchorBandToWorld. A pass
// whose offset depends on the unit type's footprint depth covers [footMin, footMax].
struct ScreenBand { float left, top, right, bottom; };
struct ScreenPoint { float x, y; };
struct AnchorView { float offX, offY, zoom; int viewW, viewH; };

namespace unitpass {
// Painter list: the frustum cull. The margins are generous and asymmetric (models
// extend above their anchor); they also stand for interpolation and flyer altitude.
inline ScreenPoint painter(float x, float z, float liftX, float liftY, const AnchorView& v) {
    return {(x - v.offX) * v.zoom - liftX * v.zoom, (z - v.offY) * v.zoom - liftY * v.zoom};
}
inline bool painterInView(ScreenPoint p, const AnchorView& v) {
    return !(p.x < -160 || p.x > v.viewW + 160 || p.y < -260 || p.y > v.viewH + 120);
}
inline ScreenBand painterBand(const AnchorView& v) {
    return {-160.0f, -260.0f, float(v.viewW + 160), float(v.viewH + 120)};
}
// Health bar: its top-left corner.
inline ScreenPoint healthBar(float x, float z, float liftX, float liftY, const AnchorView& v) {
    const float zm = v.zoom, bw = 26 * zm;
    return {(x - v.offX) * zm - bw / 2 - liftX * zm, (z - v.offY) * zm - 30 * zm - liftY * zm};
}
inline bool healthBarInView(ScreenPoint p, const AnchorView& v) {
    return !(p.x < -40 || p.x > v.viewW + 40 || p.y < -40 || p.y > v.viewH + 40);
}
inline ScreenBand healthBarBand(const AnchorView& v) {
    const float zm = v.zoom;
    return {-40 + 26 * zm / 2, -40 + 30 * zm, float(v.viewW + 40) + 26 * zm / 2, float(v.viewH + 40) + 30 * zm};
}
// Self-destruct countdown: the number's origin.
inline ScreenPoint selfDestruct(float x, float z, float liftX, float liftY, const AnchorView& v) {
    const float zm = v.zoom;
    return {(x - v.offX) * zm - liftX * zm, (z - v.offY) * zm - 44 * zm - liftY * zm};
}
inline bool selfDestructInView(ScreenPoint p, const AnchorView& v) {
    return !(p.x < -20 || p.x > v.viewW + 20 || p.y < -20 || p.y > v.viewH + 20);
}
inline ScreenBand selfDestructBand(const AnchorView& v) {
    const float zm = v.zoom;
    return {-20.0f, -20 + 44 * zm, float(v.viewW + 20), float(v.viewH + 20) + 44 * zm};
}
// Control-squad marker: centred below the unit by its footprint depth.
inline ScreenPoint squadMarker(float x, float z, float liftX, float liftY, int footZ, const AnchorView& v) {
    const float zm = v.zoom;
    return {(x - v.offX) * zm - liftX * zm,
            (z - v.offY) * zm - liftY * zm + (float(std::max(footZ, 1)) * 8.0f + 7.0f) * zm};
}
inline bool squadMarkerInView(ScreenPoint p, const AnchorView& v) {
    return !(p.x < -20 || p.x > v.viewW + 20 || p.y < -30 || p.y > v.viewH + 30);
}
inline ScreenBand squadMarkerBand(const AnchorView& v, int footMin, int footMax) {
    const float zm = v.zoom;
    return {-20.0f, -30 - (float(std::max(footMax, 1)) * 8.0f + 7.0f) * zm, float(v.viewW + 20),
            float(v.viewH + 30) - (float(std::max(footMin, 1)) * 8.0f + 7.0f) * zm};
}
}  // namespace unitpass

// Cursor picking (perf follow-up C). GameView::unitUnderCursor accepts a unit when the
// pointer (mx, my) lies in a box around the unit's sprite anchor
//   A = ((ix - offX) * zoom - liftX * zoom,
//        (iz - offY) * zoom - liftY * zoom + sink * zoom - altitude * zoom)
// (ix, iz: the interpolated position; sink: the waterline sink; altitude: a flyer's):
// the type's hit box (offset from A, px at zoom 1, scaled by the zoom and widened to
// at least kPickMinHalf each side of its centre), or a dot of half side `dotHalf`
// centred on A (Tactical Dots, HIDDEN UNITS dots). PickBoxes is the union of the hit
// boxes over every unit type.
inline constexpr float kPickMinHalf = 9.0f;
struct PickBoxes {
    bool any = false;
    float minL = 0, maxR = 0, minCx = 0, maxCx = 0;   // box left, right, centre (x)
    float minT = 0, maxB = 0, minCy = 0, maxCy = 0;   // box top, bottom, centre (y)
    void add(float x, float y, float w, float h) {
        const float cx = x + w * 0.5f, cy = y + h * 0.5f;
        if (!any) {
            any = true;
            minL = x; maxR = x + w; minCx = maxCx = cx;
            minT = y; maxB = y + h; minCy = maxCy = cy;
            return;
        }
        minL = std::min(minL, x); maxR = std::max(maxR, x + w);
        minCx = std::min(minCx, cx); maxCx = std::max(maxCx, cx);
        minT = std::min(minT, y); maxB = std::max(maxB, y + h);
        minCy = std::min(minCy, cy); maxCy = std::max(maxCy, cy);
    }
};
// The band of snapshot-lifted anchors S = ((x - offX) * zoom - liftX * zoom,
// (z - offY) * zoom - liftY * zoom) -- unitpass's convention, for anchorBandToWorld --
// that can hold a unit the pointer picks, for a unit WITHOUT altitude, whose
// interpolated position lies within `interp` world px of its snapshot position on
// each axis and whose sink lies in [sinkLo, sinkHi] world px. A box of a type, at
// any zoom > 0, spans [c * zoom - max(w * zoom / 2, kPickMinHalf), c * zoom + ...]:
// inside [minL * zoom, maxR * zoom] when the half width is the box's own, else inside
// [minCx * zoom - kPickMinHalf, maxCx * zoom + kPickMinHalf]. One screen px of slack
// stands for the exact test's own float evaluation order.
inline ScreenBand pickBand(float mx, float my, float zoom, const PickBoxes& boxes, float dotHalf, float interp,
                           float sinkLo, float sinkHi) {
    float left = -dotHalf, right = dotHalf, top = -dotHalf, bottom = dotHalf;   // A + offset
    if (boxes.any) {
        left = std::min({left, boxes.minL * zoom, boxes.minCx * zoom - kPickMinHalf});
        right = std::max({right, boxes.maxR * zoom, boxes.maxCx * zoom + kPickMinHalf});
        top = std::min({top, boxes.minT * zoom, boxes.minCy * zoom - kPickMinHalf});
        bottom = std::max({bottom, boxes.maxB * zoom, boxes.maxCy * zoom + kPickMinHalf});
    }
    const float slack = 1.0f + interp * zoom;
    return {mx - right - slack, my - bottom - sinkHi * zoom - slack,
            mx - left + slack, my - top - sinkLo * zoom + slack};
}

}  // namespace tak::render
