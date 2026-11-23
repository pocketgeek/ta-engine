#pragma once

// Reuse of retained shadow silhouette tiles (workstream E).
//
// Every unit's shadow is rasterized into its own tile of a silhouette page
// (GameView::buildShadowSilhouetteAtlas). A tile's pixels are a function of
// exactly these raster inputs:
//
//   * the tile rectangle: page, slot, width, height (cleared with padding);
//   * the tile-relative points, one per placed shadow vertex:
//       tilePoint(placed) = placed * scale - pixel + slot + 1
//     where placed = placeShadowPoint(local, placement) (client/geometrystages.h);
//   * the cutout faces' coverage textures, UVs and colours, in run order.
//
// Before E a tile was redrawn whenever the unit's geometry `revision` moved —
// any stage, including body-only colour and material changes — or its screen
// pixel position changed. E keys the tile on what it actually depends on:
//
//   coverage   UnitGeom::shadowRevision: the exact local silhouette (local
//              points, cutout UVs/colours/runs). Advanced only when that output
//              differs, never by a body-only change.
//   placement  the exact ShadowPlacement (zoom, anchor + altitude lean) the
//              points were placed with, compared bit for bit.
//
// Same coverage and same placement => the same points => the same tile.
// A changed placement is not assumed to move the tile content: the tile is kept
// only when every tile-relative point recomputes BIT-IDENTICAL under the old and
// the new placement (sameTilePoints). That holds for whole-pixel translations
// that keep each coordinate in its floating-point binade; a fractional shift
// changes the sub-pixel phase, the points differ and the tile is redrawn. No
// translation is ever assumed to be exact.
//
// Everything here is presentation state; nothing reaches the simulation.

#include "client/geometrystages.h"

#include <SDL.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tak::render {

// What a retained tile holds and where it sits. Compared field by field.
struct ShadowTileStamp {
    const void* source = nullptr;    // the unit's stable geometry entry
    int owner = -1;                  // unit id the entry held when drawn
    uint64_t coverage = 0;           // UnitGeom::shadowRevision
    uint64_t revision = 0;           // UnitGeom::revision: diagnostics only (the pre-E key)
    ShadowPlacement placement{};     // what the points were placed with (bitwise)
    int pixelX = 0, pixelY = 0;      // screen pixel of the tile's top-left (output scale)
    int page = 0;                    // silhouette page slot (workstream F: stamps follow a cell)
    int slotX = 0, slotY = 0, width = 0, height = 0;
    float scaleX = 1, scaleY = 1;    // renderer output scale
};

// The atlas transform: a placed shadow point -> its position inside the tile.
// The silhouette builder and the reuse check both call this, so the check
// recomputes exactly the expression that was rasterized (the build uses
// -ffp-contract=off; same operations, same order).
inline SDL_FPoint tilePoint(SDL_FPoint placed, float scaleX, float scaleY,
                            int pixelX, int pixelY, int slotX, int slotY) {
    return {placed.x * scaleX - float(pixelX) + float(slotX) + 1.0f,
            placed.y * scaleY - float(pixelY) + float(slotY) + 1.0f};
}

inline bool sameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }
inline bool samePlacement(const ShadowPlacement& a, const ShadowPlacement& b) {
    return sameBits(a.zoom, b.zoom) && sameBits(a.sx, b.sx) && sameBits(a.sy, b.sy);
}

// Every point of a tile, old placement vs new: `local[i]` are the unchanged
// local points (same coverage), `placedNow(i)` the points as placed this frame
// (what the builder would rasterize). The old points are recomputed from the
// local points and the old placement with the stage's own function. The first
// point is checked alone, so a sub-pixel shift is rejected after one point; the
// rest are compared in blocks without a branch per point (vectorizable).
template <class PlacedNow>
inline bool sameTilePoints(std::span<const SDL_FPoint> local, PlacedNow&& placedNow,
                           const ShadowTileStamp& before, const ShadowTileStamp& now) {
    const auto differs = [&](size_t i) -> uint32_t {
        const SDL_FPoint was = tilePoint(placeShadowPoint(local[i], before.placement), before.scaleX,
                                         before.scaleY, before.pixelX, before.pixelY, before.slotX, before.slotY);
        const SDL_FPoint is = tilePoint(placedNow(i), now.scaleX, now.scaleY, now.pixelX, now.pixelY,
                                        now.slotX, now.slotY);
        return (std::bit_cast<uint32_t>(was.x) ^ std::bit_cast<uint32_t>(is.x)) |
               (std::bit_cast<uint32_t>(was.y) ^ std::bit_cast<uint32_t>(is.y));
    };
    if (local.empty()) return true;
    if (differs(0)) return false;
    constexpr size_t kBlock = 64;
    for (size_t begin = 1; begin < local.size(); begin += kBlock) {
        const size_t end = std::min(local.size(), begin + kBlock);
        uint32_t diff = 0;
        for (size_t i = begin; i < end; ++i) diff |= differs(i);
        if (diff) return false;
    }
    return true;
}

enum class TileVerdict : uint8_t {
    Reuse,            // same coverage, same placement, nothing else changed
    ReuseBody,        // same coverage and placement; the body changed (pre-E: redrawn)
    ReuseTranslated,  // placement changed, every tile point recomputes bit-identical
    New,              // no tile at this index before
    Owner,            // another entry / unit held the tile
    Content,          // coverage changed
    Relocated,        // same unit, other page or slot
    Resized,          // other tile size or output scale
    Moved,            // placement changed and the tile points differ (sub-pixel phase, zoom)
};
inline bool reuses(TileVerdict v) { return v <= TileVerdict::ReuseTranslated; }

// The decision for one tile. `before` is the stamp retained for this tile (null:
// none) -- since workstream F the stamp of the unit's persistent cell, carried
// along when the cell is reallocated (so a move reads Relocated, not New). `samePoints()` runs the exact point comparison; it is called
// only when coverage, slot, size and scale all match but the placement differs.
template <class SamePoints>
inline TileVerdict classifyShadowTile(const ShadowTileStamp* before, const ShadowTileStamp& now,
                                      SamePoints&& samePoints) {
    if (!before) return TileVerdict::New;
    if (before->source != now.source || before->owner != now.owner) return TileVerdict::Owner;
    if (before->coverage != now.coverage) return TileVerdict::Content;
    if (before->page != now.page || before->slotX != now.slotX || before->slotY != now.slotY)
        return TileVerdict::Relocated;
    if (before->width != now.width || before->height != now.height ||
        !sameBits(before->scaleX, now.scaleX) || !sameBits(before->scaleY, now.scaleY))
        return TileVerdict::Resized;
    if (samePlacement(before->placement, now.placement) &&
        before->pixelX == now.pixelX && before->pixelY == now.pixelY)
        return before->revision == now.revision ? TileVerdict::Reuse : TileVerdict::ReuseBody;
    return samePoints() ? TileVerdict::ReuseTranslated : TileVerdict::Moved;
}

// The pre-E rule, for the differential check: a tile was kept only when the
// entry, owner, geometry revision, screen pixel, page, slot, size and scale all
// matched (pre-F the page was implicit: stamps were stored per page).
inline bool originalRuleReuses(const ShadowTileStamp* before, const ShadowTileStamp& now) {
    return before && before->source == now.source && before->owner == now.owner &&
           before->revision == now.revision && before->pixelX == now.pixelX && before->pixelY == now.pixelY &&
           before->page == now.page && before->slotX == now.slotX && before->slotY == now.slotY &&
           before->width == now.width &&
           before->height == now.height && sameBits(before->scaleX, now.scaleX) &&
           sameBits(before->scaleY, now.scaleY);
}

}  // namespace tak::render
