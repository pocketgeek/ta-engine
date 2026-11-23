#pragma once

// HIDDEN UNITS (Options > Graphics; a non-retail extra, default FADE): a unit
// that stands behind a tree, a rock or a building is still shown where it is.
//   OFF     -- as retail: whatever paints over a unit hides it.
//   DOTS    -- when the scenery covers the centre of a unit, that unit's tactical
//              dot (client/tacticaldots.h: same size, colour and selection rim)
//              is drawn on top. A unit whose centre is in plain sight gets none.
//   FADE    -- a tree, rock or building over a unit fades out whole (to the FADE
//              OPACITY setting) and back in once no unit is under it (CoverFades
//              below). The default.
// Marked are the units the frame draws anyway (own, allied, and enemies in sight:
// nothing under fog, no cloaked enemy that is not drawn), standing on the ground
// (an airborne flyer paints after every tree and is never covered). What covers is
// map scenery and buildings painted over the unit; another mobile unit never does.
// Pure display -- the sim, the lockstep hash and the network never see it.
//
// This header is the policy, kept free of GameView so it can be tested on its own:
// the option values, the painter layers (the retail order of low features), the
// exact cover tests, the screen bins that find a unit's possible covers and the
// fades.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace tak::hiddenunits {

enum Mode : int { kOff = 0, kDots = 1, kFade = 2 };
inline constexpr int kDefault = kFade;
inline constexpr int kModeCount = 3;

// Persisted as hiddenUnits = off | dots | fade. No key, a number other than 0 or 1,
// or any other value reads as the default.
inline const char* name(int mode) {
    return mode == kOff ? "off" : mode == kDots ? "dots" : "fade";
}
inline int parse(std::string_view value) {
    if (value == "off" || value == "0") return kOff;
    if (value == "dots" || value == "1") return kDots;
    return kDefault;
}

// ---- Painter layers ---------------------------------------------------------
// Retail's world pass (KINGDOMS.icd 0x4fc648..0x4fcc46) scans the map cells and
// draws every feature whose definition height (feature TDF `height`, def +0x138,
// parsed at 0x4939ef/0x493a0e) is below 10 at once, before any unit; taller
// features are drawn with the ground units in depth order, and units that are not
// on the ground go last. So a stump, a crop row, a mana deposit or a smudge never
// paints over a unit's feet, whichever side of it the unit stands on.
inline constexpr int kLowFeatureHeight = 10;
inline constexpr int kLayerLowFeature = -1;   // flat scenery: under every unit
inline constexpr int kLayerGround = 0;        // tall features + ground units, by depth
inline constexpr int kLayerAir = 1;           // airborne flyers, after everything
inline constexpr bool lowFeature(int tdfHeight) { return tdfHeight < kLowFeatureHeight; }
inline constexpr int featureLayer(int tdfHeight) {
    return lowFeature(tdfHeight) ? kLayerLowFeature : kLayerGround;
}

// ---- Screen rectangles -------------------------------------------------------
struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty() const { return !(x1 > x0 && y1 > y0); }
    bool overlaps(const Rect& o) const { return x0 < o.x1 && o.x0 < x1 && y0 < o.y1 && o.y0 < y1; }
    bool contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
    Rect clipped(const Rect& o) const {
        return {std::max(x0, o.x0), std::max(y0, o.y0), std::min(x1, o.x1), std::min(y1, o.y1)};
    }
};

// ---- Exact cover tests -------------------------------------------------------
// One bit per texel of a sprite frame: opaque or not. Feature art is palette art
// whose texels are either fully transparent or opaque; half way is the cut.
class AlphaMask {
public:
    void build(const uint8_t* rgba, int width, int height, uint8_t threshold = 128) {
        w_ = std::max(0, width); h_ = std::max(0, height);
        stride_ = (size_t(w_) + 63) / 64;
        bits_.assign(stride_ * size_t(h_), 0);
        if (!rgba) return;
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x)
                if (rgba[(size_t(y) * size_t(w_) + size_t(x)) * 4 + 3] >= threshold)
                    bits_[size_t(y) * stride_ + size_t(x) / 64] |= uint64_t(1) << (x % 64);
    }
    int width() const { return w_; }
    int height() const { return h_; }
    bool opaque(int x, int y) const {
        if (x < 0 || y < 0 || x >= w_ || y >= h_) return false;
        return (bits_[size_t(y) * stride_ + size_t(x) / 64] >> (x % 64)) & 1;
    }
    size_t bytes() const { return bits_.capacity() * sizeof(uint64_t); }

private:
    int w_ = 0, h_ = 0;
    size_t stride_ = 0;
    std::vector<uint64_t> bits_;
};

// A sprite drawn into the screen quad (x, y, w, h) whose TOP edge is shifted
// sideways by `shear` (the tree sway; 0 for a plain sprite): is the screen point
// (px, py) on one of its opaque texels?
inline bool spriteCovers(const AlphaMask& mask, float x, float y, float w, float h, float shear,
                         float px, float py) {
    if (!(w > 0) || !(h > 0) || mask.width() <= 0 || mask.height() <= 0) return false;
    const float v = (py - y) / h;
    if (!(v >= 0.0f && v < 1.0f)) return false;
    const float u = (px - x - shear * (1.0f - v)) / w;
    if (!(u >= 0.0f && u < 1.0f)) return false;
    return mask.opaque(int(u * float(mask.width())), int(v * float(mask.height())));
}

// Does one of the sprite's opaque texels lie in the screen rectangle? Sampled on a
// grid of `step` pixels (and the rectangle's far edges): a body a tree's crown
// reaches, not one merely inside the transparent corners of its quad.
inline bool spriteCoversRect(const AlphaMask& mask, float x, float y, float w, float h, float shear,
                             const Rect& r, float step = 3.0f) {
    const Rect q = r.clipped(Rect{x + std::min(0.0f, shear), y, x + w + std::max(0.0f, shear), y + h});
    if (q.empty()) return false;
    step = std::max(step, 1.0f);
    for (float py = q.y0;; py = std::min(py + step, q.y1 - 0.01f)) {
        for (float px = q.x0;; px = std::min(px + step, q.x1 - 0.01f)) {
            if (spriteCovers(mask, x, y, w, h, shear, px, py)) return true;
            if (px >= q.x1 - 0.01f) break;
        }
        if (py >= q.y1 - 0.01f) break;
    }
    return false;
}

// Is (px, py) inside the triangle (edges included)? Either winding.
inline bool pointInTriangle(float px, float py, float ax, float ay, float bx, float by,
                            float cx, float cy) {
    const float d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
    const float d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy);
    const float d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
    const bool neg = d1 < 0 || d2 < 0 || d3 < 0;
    const bool pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

// A model's screen triangles (a plain list, three vertices each; anything with
// .position.x/.y, i.e. SDL_Vertex): does one of them contain the point?
template <class Vertex>
bool trianglesCover(std::span<const Vertex> verts, float px, float py) {
    for (size_t i = 0; i + 2 < verts.size(); i += 3) {
        const auto& a = verts[i].position;
        const auto& b = verts[i + 1].position;
        const auto& c = verts[i + 2].position;
        if (px < std::min({a.x, b.x, c.x}) || px > std::max({a.x, b.x, c.x}) ||
            py < std::min({a.y, b.y, c.y}) || py > std::max({a.y, b.y, c.y})) continue;
        if (pointInTriangle(px, py, a.x, a.y, b.x, b.y, c.x, c.y)) return true;
    }
    return false;
}

// ---- Screen bins -------------------------------------------------------------
// The covers seen so far, binned by screen cell. The frame walks the painter list
// BACKWARDS, so when it reaches a unit every cover in the grid paints after it:
// a query answers "what may cover this unit" in about O(covers nearby). Each
// cover is reported once per query. Storage is kept between frames.
class CoverGrid {
public:
    void reset(float width, float height, float cell = 64.0f) {
        cell_ = std::max(cell, 1.0f);
        cols_ = std::max(1, int(std::ceil(std::max(width, 1.0f) / cell_)));
        rows_ = std::max(1, int(std::ceil(std::max(height, 1.0f) / cell_)));
        const size_t n = size_t(cols_) * size_t(rows_);
        if (bins_.size() < n) bins_.resize(n);
        for (size_t i = 0; i < n; ++i) bins_[i].clear();
        stamps_.clear();
        query_ = 0;
        count_ = 0;
    }
    size_t size() const { return count_; }
    // Ids are dense: 0, 1, 2, ... in insertion order.
    uint32_t insert(const Rect& r) {
        const uint32_t id = uint32_t(count_++);
        stamps_.push_back(0);
        int c0, r0, c1, r1;
        if (!cells(r, c0, r0, c1, r1)) return id;
        for (int y = r0; y <= r1; ++y)
            for (int x = c0; x <= c1; ++x) bins_[size_t(y) * size_t(cols_) + size_t(x)].push_back(id);
        return id;
    }
    template <class F> void query(const Rect& r, F&& visit) {
        int c0, r0, c1, r1;
        if (!cells(r, c0, r0, c1, r1)) return;
        if (++query_ == 0) { std::fill(stamps_.begin(), stamps_.end(), 0u); query_ = 1; }
        for (int y = r0; y <= r1; ++y)
            for (int x = c0; x <= c1; ++x)
                for (uint32_t id : bins_[size_t(y) * size_t(cols_) + size_t(x)]) {
                    if (stamps_[id] == query_) continue;
                    stamps_[id] = query_;
                    if (!visit(id)) return;   // the visitor returns false to stop
                }
    }
    size_t capacityBytes() const {
        size_t b = stamps_.capacity() * sizeof(uint32_t) + bins_.capacity() * sizeof(bins_[0]);
        for (const auto& v : bins_) b += v.capacity() * sizeof(uint32_t);
        return b;
    }

private:
    bool cells(const Rect& r, int& c0, int& r0, int& c1, int& r1) const {
        if (r.empty()) return false;
        c0 = std::max(0, int(std::floor(r.x0 / cell_)));
        r0 = std::max(0, int(std::floor(r.y0 / cell_)));
        c1 = std::min(cols_ - 1, int(std::floor(r.x1 / cell_)));
        r1 = std::min(rows_ - 1, int(std::floor(r.y1 / cell_)));
        return c0 <= c1 && r0 <= r1;
    }
    float cell_ = 64.0f;
    int cols_ = 1, rows_ = 1;
    std::vector<std::vector<uint32_t>> bins_;
    std::vector<uint32_t> stamps_;
    uint32_t query_ = 0;
    size_t count_ = 0;
};

// ---- FADE ---------------------------------------------------------------------
// A tree, rock or building (a cover) over a unit the frame looks at is drawn whole at
// the FADE OPACITY setting: the unit, its shadow and the ground behind show through.
// It fades out over kFadeOutMs when a unit comes under it and back in over kFadeInMs
// after the last one has left (display time; smoothstep eased): the faded cover is
// simply drawn translucent where it stands.
inline constexpr int kDefaultFadePercent = 50;   // the cover stays 50% visible
inline constexpr float kFadeOutMs = 200.0f;
inline constexpr float kFadeInMs = 350.0f;
// The setting (percent the cover stays visible: 0 = gone, 100 = no fade) as an alpha.
inline constexpr uint8_t fadeOpacity(int percent) {
    return uint8_t((std::clamp(percent, 0, 100) * 255 + 50) / 100);
}
// An alpha (or a premultiplied colour channel) scaled by `by`, rounded.
inline constexpr uint8_t scaleAlpha(uint8_t a, uint8_t by) { return uint8_t((int(a) * int(by) + 127) / 255); }

// A cover's fade key: a feature by its sim id, a building by its unit id (tagged so
// the two never meet). A new game makes a new view, and with it new fades.
inline constexpr uint64_t featureCover(uint32_t simId) { return simId; }
inline constexpr uint64_t structureCover(uint32_t unitId) { return (uint64_t(1) << 32) | unitId; }

// Do the triangle (a, b, c) and the rectangle share area? Separating axes: the
// rectangle's two and the triangle's three edges.
inline bool triangleOverlapsRect(float ax, float ay, float bx, float by, float cx, float cy, const Rect& r) {
    if (std::max({ax, bx, cx}) <= r.x0 || std::min({ax, bx, cx}) >= r.x1 ||
        std::max({ay, by, cy}) <= r.y0 || std::min({ay, by, cy}) >= r.y1) return false;
    const float px[3] = {ax, bx, cx}, py[3] = {ay, by, cy};
    const float rx[4] = {r.x0, r.x1, r.x1, r.x0}, ry[4] = {r.y0, r.y0, r.y1, r.y1};
    for (int e = 0; e < 3; ++e) {
        const int n = (e + 1) % 3, o = (e + 2) % 3;
        const float nx = py[n] - py[e], ny = px[e] - px[n];   // the edge normal
        const float side = nx * (px[o] - px[e]) + ny * (py[o] - py[e]);
        if (side == 0.0f) continue;   // degenerate
        bool apart = true;
        for (int k = 0; k < 4 && apart; ++k)
            apart = (nx * (rx[k] - px[e]) + ny * (ry[k] - py[e])) * side <= 0.0f;
        if (apart) return false;
    }
    return true;
}
// A model's screen triangles (as trianglesCover): does one of them reach the rectangle?
template <class Vertex>
bool trianglesCoverRect(std::span<const Vertex> verts, const Rect& r) {
    if (r.empty()) return false;
    for (size_t i = 0; i + 2 < verts.size(); i += 3) {
        const auto& a = verts[i].position;
        const auto& b = verts[i + 1].position;
        const auto& c = verts[i + 2].position;
        if (triangleOverlapsRect(a.x, a.y, b.x, b.y, c.x, c.y, r)) return true;
    }
    return false;
}

// Per-cover fade state, keyed by featureCover/structureCover and kept sorted. Each
// frame: begin(), cover(key) for every cover over a unit, inView(key) for every cover
// the frame draws, then advance(ms). A cover fully back, or not in view this frame,
// is dropped. Storage is kept between frames.
class CoverFades {
public:
    void begin() { hits_.clear(); ++frame_; }
    void cover(uint64_t id) { hits_.push_back(id); }
    void inView(uint64_t id) {
        if (const size_t i = find(id); i < entries_.size()) entries_[i].seen = frame_;
    }
    void advance(float ms) {
        std::sort(hits_.begin(), hits_.end());
        hits_.erase(std::unique(hits_.begin(), hits_.end()), hits_.end());
        // New covers join at level 0 (opaque), in key order.
        size_t at = 0;
        for (const uint64_t id : hits_) {
            while (at < entries_.size() && entries_[at].id < id) ++at;
            if (at < entries_.size() && entries_[at].id == id) { entries_[at].seen = frame_; continue; }
            entries_.insert(entries_.begin() + std::ptrdiff_t(at), Entry{id, 0.0f, frame_});
        }
        ms = std::max(0.0f, ms);
        size_t out = 0;
        for (size_t i = 0; i < entries_.size(); ++i) {
            Entry e = entries_[i];
            const bool hit = std::binary_search(hits_.begin(), hits_.end(), e.id);
            if (!hit && e.seen != frame_) continue;   // left the view
            e.level = hit ? std::min(1.0f, e.level + ms / kFadeOutMs) : std::max(0.0f, e.level - ms / kFadeInMs);
            if (!hit && e.level <= 0.0f) continue;     // fully back
            entries_[out++] = e;
        }
        entries_.resize(out);
    }
    // The cover's opacity now, fading towards `opacity` (fadeOpacity): 255 when it is
    // not fading.
    uint8_t alpha(uint64_t id, uint8_t opacity) const {
        const size_t i = find(id);
        if (i >= entries_.size()) return 255;
        const float level = entries_[i].level;
        const float t = level * level * (3.0f - 2.0f * level);   // smoothstep
        return uint8_t(std::lround(255.0f - t * float(255 - opacity)));
    }
    size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }

private:
    struct Entry { uint64_t id; float level; uint32_t seen; };
    size_t find(uint64_t id) const {   // its index, or entries_.size()
        const auto it = std::lower_bound(entries_.begin(), entries_.end(), id,
                                         [](const Entry& e, uint64_t v) { return e.id < v; });
        return it != entries_.end() && it->id == id ? size_t(it - entries_.begin()) : entries_.size();
    }
    std::vector<Entry> entries_;
    std::vector<uint64_t> hits_;
    uint32_t frame_ = 0;
};

// FADE's per-frame cover bookkeeping, by CoverGrid id: which covers this frame has
// already found over some unit. Such a cover fades whatever later units it reaches, so
// for a later unit its exact (sprite texel / triangle) test can only matter to that
// unit's own "covered" answer, never to the fades: it is skipped once the unit is
// known to be covered, and otherwise tested only after the covers not yet found
// (stopping at the first that reaches it). Each cover is reported to the fades once
// per frame. Storage is kept between frames.
class CoverMarks {
public:
    void clear() { found_.clear(); }
    void add() { found_.push_back(0); }   // the next CoverGrid id
    size_t size() const { return found_.size(); }
    bool found(uint32_t id) const { return found_[id] != 0; }

    // One unit. `query(visit)` hands its possible covers (CoverGrid ids, each once) to
    // visit; `reaches(id)` is the exact test of that cover against the unit; `fade(id)`
    // is called for a cover found over a unit for the first time this frame. Returns
    // whether a cover reaches the unit: exactly what testing every cover would say.
    template <class Query, class Reaches, class Fade>
    bool unit(Query&& query, Reaches&& reaches, Fade&& fade) {
        bool hit = false;
        later_.clear();
        query([&](uint32_t id) {
            if (found_[id]) {
                if (!hit) later_.push_back(id);   // only this unit's answer can depend on it
                return true;
            }
            if (reaches(id)) { found_[id] = 1; fade(id); hit = true; }
            return true;
        });
        for (size_t i = 0; !hit && i < later_.size(); ++i) hit = reaches(later_[i]);
        return hit;
    }

private:
    std::vector<uint8_t> found_;
    std::vector<uint32_t> later_;
};

}  // namespace tak::hiddenunits
