// Shadow silhouette tile reuse (client/shadowtiles.h) and the body/shadow input
// split (tak::render::shadowInputsEqual), workstream E.
//
// The central property: a retained tile is kept exactly when everything it is
// rasterized from is bit-identical. The raster inputs are computed here with the
// pre-E expressions copied from GameView (c2c25707: buildUnitShadow's placement
// sx + x*zm, the atlas transform x*scale - pixel + slot + 1, the floor/ceil tile
// bounds), independently of the code under test, across every invalidation cause:
// body-only (material) changes, coverage changes, owner changes, relocation,
// resizing, output scale, zoom, whole-pixel and sub-pixel translations, altitude
// lean. The pre-E rule (geometry revision + screen pixel) must never keep a tile
// the new rule redraws.
#include "client/shadowtiles.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace {
using namespace tak::render;
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

// ---- the pre-E arithmetic (verbatim expressions) ----
SDL_FPoint formerShadowPoint(SDL_FPoint local, float zm, float sx, float sy) {
    return {sx + local.x * zm, sy + local.y * zm};
}
SDL_FPoint formerTilePoint(SDL_FPoint p, float scaleX, float scaleY, int pixelX, int pixelY, int slotX, int slotY) {
    return {p.x * scaleX - float(pixelX) + float(slotX) + 1.0f, p.y * scaleY - float(pixelY) + float(slotY) + 1.0f};
}

// One unit's shadow as the silhouette builder sees it.
struct Unit {
    const void* entry = nullptr;
    int owner = 7;
    uint64_t coverage = 1, revision = 1;
    std::vector<SDL_FPoint> local, maskedLocal;   // local (model-projected) points
    ShadowPlacement placement{1, 300, 200};
};
struct Tile {
    ShadowTileStamp stamp;
    std::vector<SDL_FPoint> placed, maskedPlaced;  // what the geometry stage placed
    std::vector<SDL_FPoint> raster;                // tile points, former expressions
};
Tile makeTile(const Unit& u, int slotX, int slotY, float scaleX, float scaleY) {
    Tile t;
    float minX = INFINITY, minY = INFINITY, maxX = -INFINITY, maxY = -INFINITY;
    auto place = [&](const std::vector<SDL_FPoint>& in, std::vector<SDL_FPoint>& out) {
        for (SDL_FPoint p : in) {
            const SDL_FPoint q = formerShadowPoint(p, u.placement.zoom, u.placement.sx, u.placement.sy);
            out.push_back(q);
            minX = std::min(minX, q.x); minY = std::min(minY, q.y);
            maxX = std::max(maxX, q.x); maxY = std::max(maxY, q.y);
        }
    };
    place(u.local, t.placed);
    place(u.maskedLocal, t.maskedPlaced);
    const int pixelX = int(std::floor(double(minX) * scaleX)), pixelY = int(std::floor(double(minY) * scaleY));
    const int width = std::max(1, int(std::ceil(double(maxX) * scaleX)) - pixelX);
    const int height = std::max(1, int(std::ceil(double(maxY) * scaleY)) - pixelY);
    auto& s = t.stamp;
    s.source = u.entry; s.owner = u.owner; s.coverage = u.coverage; s.revision = u.revision;
    s.placement = u.placement; s.pixelX = pixelX; s.pixelY = pixelY; s.slotX = slotX; s.slotY = slotY;
    s.width = width; s.height = height; s.scaleX = scaleX; s.scaleY = scaleY;
    for (const auto* list : {&t.placed, &t.maskedPlaced})
        for (SDL_FPoint p : *list) t.raster.push_back(formerTilePoint(p, scaleX, scaleY, pixelX, pixelY, slotX, slotY));
    return t;
}
bool sameRaster(const Tile& a, const Tile& b) {
    return a.raster.size() == b.raster.size() &&
           (a.raster.empty() || !std::memcmp(a.raster.data(), b.raster.data(), a.raster.size() * sizeof a.raster[0]));
}
// The decision exactly as GameView makes it.
TileVerdict decide(const Tile& before, const Unit& u, const Tile& now) {
    return classifyShadowTile(&before.stamp, now.stamp, [&] {
        return sameTilePoints(u.local, [&](size_t i) { return now.placed[i]; }, before.stamp, now.stamp) &&
               sameTilePoints(u.maskedLocal, [&](size_t i) { return now.maskedPlaced[i]; }, before.stamp, now.stamp);
    });
}

Unit randomUnit(std::mt19937& rng, const void* entry) {
    std::uniform_real_distribution<float> coord(-60.f, 60.f);
    std::uniform_int_distribution<int> count(3, 40);
    Unit u;
    u.entry = entry;
    for (int i = count(rng) * 3; i > 0; --i) u.local.push_back({coord(rng), coord(rng)});
    for (int i = (count(rng) % 4) * 3; i > 0; --i) u.maskedLocal.push_back({coord(rng), coord(rng)});
    static const float zooms[] = {1.f, 0.5f, 0.25f, 0.75f, 0.73f, 1.25f, 2.f, 0.9f};
    std::uniform_real_distribution<float> where(-200.f, 1800.f);
    u.placement = {zooms[rng() % 8], where(rng), where(rng)};
    if (rng() % 3 == 0) {   // whole-unit anchors: the common screen position of a zoom-1 unit
        u.placement.sx = std::round(u.placement.sx); u.placement.sy = std::round(u.placement.sy);
    }
    return u;
}

void tileTransformMatchesFormer() {
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> pos(-500.f, 9000.f);
    std::uniform_int_distribution<int> px(-400, 9000), slot(0, 2040);
    const float scales[] = {1.f, 1.5f, 2.f, 0.75f, 1.25f};
    bool ok = true;
    for (int i = 0; i < 200000 && ok; ++i) {
        const SDL_FPoint p{pos(rng), pos(rng)};
        const float sx = scales[i % 5], sy = scales[(i / 5) % 5];
        const int pxX = px(rng), pxY = px(rng), sX = slot(rng), sY = slot(rng);
        const SDL_FPoint a = tilePoint(p, sx, sy, pxX, pxY, sX, sY), b = formerTilePoint(p, sx, sy, pxX, pxY, sX, sY);
        ok = sameBits(a.x, b.x) && sameBits(a.y, b.y);
        const SDL_FPoint l{pos(rng) * 0.01f, pos(rng) * 0.01f};
        const ShadowPlacement sp{scales[i % 5] * 0.5f, pos(rng), pos(rng)};
        const SDL_FPoint c = placeShadowPoint(l, sp), d = formerShadowPoint(l, sp.zoom, sp.sx, sp.sy);
        ok = ok && sameBits(c.x, d.x) && sameBits(c.y, d.y);
    }
    check(ok, "tilePoint / placeShadowPoint equal the pre-E expressions bit for bit");
}

void namedCauses() {
    int entryA = 0, entryB = 0;
    std::mt19937 rng(5);
    Unit u = randomUnit(rng, &entryA);
    u.placement = {1.f, 700.f, 700.f};   // every placed point stays in [512, 1024) below
    const Tile t0 = makeTile(u, 10, 20, 1.f, 1.f);

    // Nothing changed.
    check(decide(t0, u, makeTile(u, 10, 20, 1.f, 1.f)) == TileVerdict::Reuse, "unchanged tile reused");
    // Body-only change (material, texture frame of an unmasked texture): revision moves.
    {
        Unit v = u; v.revision = 2;
        const Tile t1 = makeTile(v, 10, 20, 1.f, 1.f);
        check(decide(t0, v, t1) == TileVerdict::ReuseBody, "body-only change keeps the tile");
        check(!originalRuleReuses(&t0.stamp, t1.stamp), "pre-E key redrew a body-only change");
        check(sameRaster(t0, t1), "body-only change: identical raster input");
    }
    // Coverage change (pose, mask frame, shadow flag, corpse cull): redrawn.
    {
        Unit v = u; v.coverage = 2; v.revision = 2; v.local[0].x += 1.f;
        check(decide(t0, v, makeTile(v, 10, 20, 1.f, 1.f)) == TileVerdict::Content, "coverage change redraws");
        // Even an unchanged-looking coverage revision bump redraws (never inferred).
        Unit w = u; w.coverage = 3; w.revision = 3;
        check(decide(t0, w, makeTile(w, 10, 20, 1.f, 1.f)) == TileVerdict::Content, "coverage revision alone redraws");
    }
    // Another entry, another owner of the same entry.
    {
        Unit v = u; v.entry = &entryB;
        check(decide(t0, v, makeTile(v, 10, 20, 1.f, 1.f)) == TileVerdict::Owner, "other entry redraws");
        Unit w = u; w.owner = 8;
        check(decide(t0, w, makeTile(w, 10, 20, 1.f, 1.f)) == TileVerdict::Owner, "other owner redraws");
    }
    // Relocated, resized (output scale).
    check(decide(t0, u, makeTile(u, 11, 20, 1.f, 1.f)) == TileVerdict::Relocated, "relocated tile redraws");
    {
        const Tile t1 = makeTile(u, 10, 20, 2.f, 2.f);
        check(decide(t0, u, t1) == TileVerdict::Resized, "output scale change redraws");
    }
    // Whole-pixel translation at zoom 1 inside one binade: kept, inputs identical.
    {
        Unit v = u; v.placement.sx += 3.f; v.placement.sy -= 2.f; v.revision = 2;
        const Tile t1 = makeTile(v, 10, 20, 1.f, 1.f);
        check(t1.stamp.pixelX == t0.stamp.pixelX + 3 && t1.stamp.pixelY == t0.stamp.pixelY - 2,
              "whole-pixel translation moved the tile origin by whole pixels");
        check(decide(t0, v, t1) == TileVerdict::ReuseTranslated, "whole-pixel translation keeps the tile");
        check(sameRaster(t0, t1), "whole-pixel translation: identical raster input");
        check(!originalRuleReuses(&t0.stamp, t1.stamp), "pre-E key redrew a translation");
    }
    // Sub-pixel translation: the phase changes, redrawn.
    for (float d : {0.5f, 0.25f, 0.125f, 1.0f / 1024.0f}) {
        Unit v = u; v.placement.sx += d;
        const Tile t1 = makeTile(v, 10, 20, 1.f, 1.f);
        // Moved, or Resized when the shift also changes the tile's pixel extent.
        check(!tak::render::reuses(decide(t0, v, t1)), "sub-pixel translation redraws");
        check(!sameRaster(t0, t1), "sub-pixel translation changes the raster input");
    }
    // Zoom change and flyer altitude lean.
    {
        Unit v = u; v.placement.zoom = 0.9f;
        const Tile t1 = makeTile(v, 10, 20, 1.f, 1.f);
        if (t1.stamp.width == t0.stamp.width && t1.stamp.height == t0.stamp.height)
            check(decide(t0, v, t1) == TileVerdict::Moved, "zoom change redraws");
        Unit w = u; w.placement.sy += 0.37f;
        check(decide(t0, w, makeTile(w, 10, 20, 1.f, 1.f)) == TileVerdict::Moved, "altitude lean redraws");
    }
    // A cutout point that alone differs is caught (masked points are compared too).
    {
        Unit v = u;
        v.local.clear();
        v.maskedLocal = {{0.25f, 0.5f}, {10.f, 3.f}, {-4.f, 7.f}};
        v.placement = {1.f, 100.f, 100.f};
        const Tile a = makeTile(v, 0, 0, 1.f, 1.f);
        Unit w = v; w.placement.sx = 103.f;
        const Tile b = makeTile(w, 0, 0, 1.f, 1.f);
        check(decide(a, w, b) == TileVerdict::ReuseTranslated, "masked-only tile translates");
        Unit x = v; x.placement.sx = 103.5f;
        check(!tak::render::reuses(decide(a, x, makeTile(x, 0, 0, 1.f, 1.f))), "masked-only sub-pixel shift redraws");
    }
    // Binade crossing: a whole-pixel shift whose points change floating-point
    // exponent may round differently; the decision must follow the actual points.
    {
        Unit v = u; v.local = {{0.3f, 0.7f}, {1.1f, 0.2f}, {0.6f, 1.3f}};
        v.placement = {1.f, 1023.4f, 511.6f};
        const Tile a = makeTile(v, 0, 0, 1.f, 1.f);
        Unit w = v; w.placement.sx += 1.f; w.placement.sy += 1.f;
        const Tile b = makeTile(w, 0, 0, 1.f, 1.f);
        check(tak::render::reuses(decide(a, w, b)) == sameRaster(a, b), "binade crossing follows the actual points");
    }
}

// Randomized differential: for every change that leaves coverage, entry, slot,
// size and scale alone, the tile is kept IF AND ONLY IF the independently computed
// raster inputs are bit-identical; any kept tile has identical inputs; the pre-E
// key never keeps what the new key redraws.
void randomizedDifferential() {
    std::mt19937 rng(20261016);
    int entry = 0;
    size_t kept = 0, translated = 0, moved = 0, sameSizeTrials = 0;
    bool iff = true, keptIdentical = true, superset = true;
    for (int trial = 0; trial < 60000; ++trial) {
        Unit u = randomUnit(rng, &entry);
        const float scale = (rng() % 5 == 0) ? 1.5f : 1.f;
        const Tile t0 = makeTile(u, int(rng() % 512), int(rng() % 512), scale, scale);
        Unit v = u;
        switch (rng() % 7) {
            case 0: break;                                                  // nothing
            case 1: v.revision++; break;                                    // body-only
            case 2: { const float k = float(int(rng() % 11) - 5);          // whole units
                      v.placement.sx += k; v.placement.sy += float(int(rng() % 7) - 3); v.revision++; } break;
            case 3: v.placement.sx += float(rng() % 1000) / 1000.f; v.revision++; break;   // sub-pixel
            case 4: v.placement.zoom *= (rng() % 2) ? 1.0001f : 1.25f; v.revision++; break; // zoom
            case 5: v.placement.sy += 0.5f * float(rng() % 4); v.revision++; break;          // lean
            case 6: v.coverage++; v.revision++; if (!v.local.empty()) v.local[0].y += 0.5f; break;
        }
        const Tile t1 = makeTile(v, t0.stamp.slotX, t0.stamp.slotY, scale, scale);
        const TileVerdict verdict = decide(t0, v, t1);
        const bool keep = tak::render::reuses(verdict);
        kept += keep;
        translated += verdict == TileVerdict::ReuseTranslated;
        moved += verdict == TileVerdict::Moved;
        const bool comparable = v.coverage == u.coverage && t1.stamp.width == t0.stamp.width &&
                                t1.stamp.height == t0.stamp.height;
        if (comparable) { ++sameSizeTrials; iff = iff && keep == sameRaster(t0, t1); }
        if (keep) keptIdentical = keptIdentical && comparable && sameRaster(t0, t1);
        if (originalRuleReuses(&t0.stamp, t1.stamp)) superset = superset && keep;
    }
    std::printf("randomized: kept=%zu translated=%zu moved=%zu comparable=%zu\n", kept, translated, moved,
                sameSizeTrials);
    check(iff, "kept exactly when the raster inputs are bit-identical");
    check(keptIdentical, "every kept tile has identical raster inputs");
    check(superset, "the pre-E key never keeps a tile the new key redraws");
    check(translated > 1000 && moved > 1000, "both translation outcomes exercised");
}

void shadowInputs() {
    // Fixed words (13), then 3 animated-frame words, then piece table + poses.
    std::vector<uint64_t> base(kLocalKeyFixedWords);
    for (size_t i = 0; i < base.size(); ++i) base[i] = 100 + i;
    base.insert(base.end(), {5, 6, 7});              // frames: [0] unmasked, [1] masked, [2] unmasked
    base.insert(base.end(), {0xabc, 1, 2, 3, 4});    // piece table, poses
    const LocalKeyLayout layout{uint32_t(kLocalKeyFixedWords + 3)};
    const std::vector<uint8_t> masks = {0, 1, 0};
    auto same = [&](const std::vector<uint64_t>& after) {
        return shadowInputsEqual(base, layout, after, layout, masks);
    };
    check(same(base), "identical key: shadow inputs equal");
    auto changed = [&](size_t word, uint64_t delta = 1) { auto k = base; k[word] += delta; return k; };
    check(same(changed(kLocalKeyAtlasWord)), "colour atlas is body-only");
    check(same(changed(kLocalKeyFixedWords + 0)), "unmasked animated frame is body-only");
    check(same(changed(kLocalKeyFixedWords + 2)), "second unmasked frame is body-only");
    check(!same(changed(kLocalKeyFixedWords + 1)), "masked animated frame is a shadow input");
    for (size_t w = 0; w < kLocalKeyFixedWords; ++w)
        if (w != kLocalKeyAtlasWord) check(!same(changed(w)), "every other fixed word is a shadow input");
    for (size_t w = layout.poseBegin; w < base.size(); ++w)
        check(!same(changed(w)), "piece table and poses are shadow inputs");
    {   // Layout or size differences never compare equal.
        auto longer = base; longer.push_back(9);
        check(!same(longer), "longer key differs");
        const LocalKeyLayout other{uint32_t(kLocalKeyFixedWords + 2)};
        check(!shadowInputsEqual(base, layout, base, other, masks), "other layout differs");
        check(!shadowInputsEqual(std::vector<uint64_t>(5), LocalKeyLayout{}, std::vector<uint64_t>(5),
                                 LocalKeyLayout{}, masks), "malformed key differs");
    }
    {   // Unmapped frame words (TAK_TEXTURE_TICK_REFERENCE: one tick word) compare all.
        const std::vector<uint8_t> two = {0, 0};
        check(!shadowInputsEqual(base, layout, changed(kLocalKeyFixedWords), layout, two),
              "unmapped frame words are all shadow inputs");
    }
}
}  // namespace

int main() {
    tileTransformMatchesFormer();
    namedCauses();
    randomizedDifferential();
    shadowInputs();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("shadow_tiles: all checks passed\n");
    return 0;
}
