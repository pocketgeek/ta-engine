#pragma once

// Per-unit model geometry in separately reusable stages (workstream D).
//
// GameView::buildUnitGeom turns one unit into screen-space body and shadow
// vertices. The work splits by what each step reads:
//
//   local      piece transforms, triangle collection, depth sort, texture/UV
//              and shadow-mask selection, shadow silhouette collection: in
//              model-projected LOCAL coordinates (screen axes, before zoom and
//              anchor). Inputs: identity/resources (type, model, colour atlas,
//              piece table), pose (heading, body pitch/roll, piece poses incl.
//              hidden/shadow flags, corpse state), palette and texture frames,
//              construction state, the shadow toggle.
//   placement  local -> screen: x*zoom+ax, y*zoom+ay-bob for the body,
//              sx+x*zoom, sy+y*zoom for the shadow (sx/sy = anchor + altitude
//              lean). Inputs: zoom, the interpolated anchor (incl. terrain/flyer
//              lift, water sink, altitude), emote bob, flyer altitude, wall
//              occlusion line (draw-only clip state).
//   material   per-vertex body colour from the local stage's base colour:
//              veterancy gold, conjuring fade/pulse, emote/statue tint, ghost
//              translucency. Conjuring's pulse reads the PLACED screen y, so
//              for a conjuring unit material also depends on placement.
//
// Each stage keeps an exact input key (word vectors/arrays compared for
// equality, never a hash), so a stage is redone only when one of its own
// inputs changed: a camera pan or an interpolation step that only moves the
// anchor reruns placement and leaves the piece transforms, collection and
// sort alone. The arithmetic of placement and material is exactly the
// arithmetic of the former single pass (same expressions, same order; the
// build uses -ffp-contract=off), so outputs are bit-identical; the debug
// TAK_GEOMETRY_VERIFY check compares every partial update with a complete
// rebuild and with the former single-pass loop.
//
// Telemetry: which input category changed (tak::render::MissCause) is read
// from the stage keys. It never decides reuse.

#include "client/rendertelemetry.h"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace tak::render {

// ---- local stage key layout ------------------------------------------------
//   [0, kLocalKeyFixedWords)  fixed words, one cause each (kLocalKeyWordCause)
//   [kLocalKeyFixedWords, poseBegin)  animated texture frame indices (or the game
//                                     tick under TAK_TEXTURE_TICK_REFERENCE)
//   [poseBegin]               piece-name table pointer (only with a VM pose)
//   (poseBegin, end)          piece poses
inline constexpr size_t kLocalKeyFixedWords = 13;
inline constexpr MissCause kLocalKeyWordCause[kLocalKeyFixedWords] = {
    MissCause::Owner,                                               // 0 unit id
    MissCause::Resource, MissCause::Resource, MissCause::Resource,  // 1 type, 2 visual, 3 atlas
    MissCause::Material, MissCause::Material,                       // 4 player, 5 colour slot
    MissCause::Pose, MissCause::Pose,                               // 6 body pitch, 7 roll
    MissCause::Material,                                            // 8 under construction (shadow)
    MissCause::Pose, MissCause::Pose,                               // 9 corpse phase, 10 statue
    MissCause::Settings,                                            // 11 shadows on
    MissCause::Pose,                                                // 12 facing (heading, emote spin)
};

struct LocalKeyLayout {
    uint32_t poseBegin = 0;
};

// ---- shadow inputs (workstream E) -------------------------------------------
// The shadow silhouette reads every local input except two that only the body
// reads: the colour atlas (word 3: body UVs) and the frames of animated textures
// that carry no shadow mask (their frame recolours the body; a masked texture's
// frame selects the cutout, so it stays a shadow input). `frameMasks` is the
// visual's flag per animated texture, parallel to the frame words (1 = masked);
// any other frame-word count (TAK_TEXTURE_TICK_REFERENCE's single tick word)
// compares every frame word. Exact word comparison; never a hash.
inline constexpr size_t kLocalKeyAtlasWord = 3;
inline bool shadowInputsEqual(std::span<const uint64_t> before, LocalKeyLayout beforeLayout,
                              std::span<const uint64_t> after, LocalKeyLayout afterLayout,
                              std::span<const uint8_t> frameMasks) {
    if (before.size() != after.size() || beforeLayout.poseBegin != afterLayout.poseBegin ||
        after.size() < kLocalKeyFixedWords || afterLayout.poseBegin < kLocalKeyFixedWords ||
        afterLayout.poseBegin > after.size())
        return false;
    for (size_t i = 0; i < kLocalKeyFixedWords; ++i)
        if (i != kLocalKeyAtlasWord && before[i] != after[i]) return false;
    const size_t frames = afterLayout.poseBegin - kLocalKeyFixedWords;
    const bool mapped = frames == frameMasks.size();
    for (size_t f = 0; f < frames; ++f)
        if ((!mapped || frameMasks[f]) && before[kLocalKeyFixedWords + f] != after[kLocalKeyFixedWords + f])
            return false;
    return std::equal(before.begin() + afterLayout.poseBegin, before.end(), after.begin() + afterLayout.poseBegin);
}

// ---- body vertex count (workstream G) ---------------------------------------
// The body collector emits each piece's triangles minus the ground plate, hidden
// pieces and faces turned away from the view direction (a backface cull), so the
// body's vertex count depends on the model and the whole pose (piece poses,
// facing, pitch/roll, corpse state). What only colours or textures the body --
// the colour atlas (word 3), player and colour slot (4, 5: texture/frame choice),
// the shadow toggle (11) and animated texture frames -- does not change it. Two
// local keys that differ in nothing else produce bodies of equal vertex count:
// what the distant-image eligibility reads when a held image defers the body.
// Exact word comparison; any layout difference answers false.
inline bool bodyVertexCountStable(std::span<const uint64_t> before, LocalKeyLayout beforeLayout,
                                  std::span<const uint64_t> after, LocalKeyLayout afterLayout) {
    if (before.size() != after.size() || beforeLayout.poseBegin != afterLayout.poseBegin ||
        after.size() < kLocalKeyFixedWords || afterLayout.poseBegin < kLocalKeyFixedWords ||
        afterLayout.poseBegin > after.size())
        return false;
    for (size_t i = 0; i < kLocalKeyFixedWords; ++i)
        if (i != kLocalKeyAtlasWord && i != 4 && i != 5 && i != 11 && before[i] != after[i]) return false;
    return std::equal(before.begin() + afterLayout.poseBegin, before.end(), after.begin() + afterLayout.poseBegin);
}

// ---- material stage key ----------------------------------------------------
enum MaterialWord : size_t {
    kMaterialVeteran, kMaterialGhost, kMaterialConjuring, kMaterialEmote, kMaterialEmoteColour,
    kMaterialEmoteMix, kMaterialOpacity, kMaterialClock, kMaterialFade, kMaterialWords
};
using MaterialKey = std::array<uint64_t, kMaterialWords>;

// ---- placement stage key ---------------------------------------------------
enum PlacementWord : size_t {
    kPlacementZoom, kPlacementAnchorX, kPlacementAnchorY, kPlacementBob, kPlacementAltitude,
    kPlacementOcclusion, kPlacementWords
};
using PlacementKey = std::array<uint32_t, kPlacementWords>;

inline uint32_t floatBits(float value) { return std::bit_cast<uint32_t>(value); }

// ---- stage plan ------------------------------------------------------------
enum StageBit : uint8_t { kStageLocal = 1, kStagePlacement = 2, kStageMaterial = 4 };

// Which stages to redo. `localValid` = the retained local stage matches its key
// (false when cold or under a reference switch). Without a valid local stage
// everything is rebuilt. Material reads the placed y while conjuring.
inline uint8_t planStages(bool localValid, bool placementSame, bool materialSame,
                          bool materialReadsPlacement) {
    if (!localValid) return kStageLocal | kStagePlacement | kStageMaterial;
    uint8_t stages = 0;
    if (!placementSame) stages |= kStagePlacement;
    if (!materialSame || (materialReadsPlacement && !placementSame)) stages |= kStageMaterial;
    return stages;
}

// ---- placement arithmetic --------------------------------------------------
// The former single pass computed, per body vertex,
//   x = local.x * zm + ax;   y = local.y * zm + ay - bob
// and per shadow vertex sx + local.x * zm, sy + local.y * zm. Same expressions.
struct BodyPlacement {
    float zoom = 1, ax = 0, ay = 0, bob = 0;
};
inline void placeBody(std::span<const SDL_FPoint> local, std::span<SDL_Vertex> out, const BodyPlacement& p) {
    const size_t n = std::min(local.size(), out.size());
    for (size_t i = 0; i < n; ++i) {
        out[i].position.x = local[i].x * p.zoom + p.ax;
        out[i].position.y = local[i].y * p.zoom + p.ay - p.bob;
    }
}

struct ShadowPlacement {
    float zoom = 1, sx = 0, sy = 0;
};
inline SDL_FPoint placeShadowPoint(SDL_FPoint local, const ShadowPlacement& p) {
    return {p.sx + local.x * p.zoom, p.sy + local.y * p.zoom};
}

// Min x/y, max x/y over the placed shadow vertices (inf/-inf when empty), with
// the former loop's comparisons, so NaN points are skipped identically.
struct ShadowBoundsAccumulator {
    float minX = std::numeric_limits<float>::infinity(), minY = std::numeric_limits<float>::infinity();
    float maxX = -std::numeric_limits<float>::infinity(), maxY = -std::numeric_limits<float>::infinity();
    void add(SDL_FPoint p) {
        minX = std::min(minX, p.x); minY = std::min(minY, p.y);
        maxX = std::max(maxX, p.x); maxY = std::max(maxY, p.y);
    }
    std::array<float, 4> bounds() const { return {minX, minY, maxX, maxY}; }
};

// ---- material arithmetic ---------------------------------------------------
struct BodyMaterial {
    float vetGold = 0;          // > 0: veteran gold tint
    bool conjuring = false;     // materialising: fade in + pulse (reads placed y)
    float progress = 1;         // conjuring progress p
    float clock = 0;            // animation clock (conjuring pulse)
    bool emote = false;         // emote / statue tint toward emoteColour by emoteMix
    SDL_Color emoteColour{};
    float emoteMix = 0;
    bool spectral = false;      // FBI ghost: 55% alpha
    // A sinking grave's fade (4ec7b0, 4ed04a..4ed067: ftol(alpha * 255.0f) as
    // the vertex alpha of the body under blending state 6): 255 = opaque.
    Uint8 fade = 255;
    Uint8 alpha() const { return Uint8(progress * 255.0f); }
    bool identity() const { return !(vetGold > 0) && !conjuring && !emote && !spectral && fade == 255; }
};

// The collector's base colour for every textured triangle; a material over an
// all-white base needs no stored base colours.
inline constexpr SDL_Color kWhiteBase{255, 255, 255, 255};
inline bool isWhite(SDL_Color c) { return c.r == 255 && c.g == 255 && c.b == 255 && c.a == 255; }

// One vertex, `v.color` = base colour on entry, `v.position` already placed.
// Exactly the former single pass's colour code, in its order.
inline void applyMaterial(SDL_Vertex& v, const BodyMaterial& m) {
    const float vetGold = m.vetGold;
    if (vetGold > 0) {
        v.color.r = Uint8(v.color.r + (255 - v.color.r) * vetGold);
        v.color.g = Uint8(v.color.g + (200 - v.color.g) * vetGold * 0.85f);
        v.color.b = Uint8(v.color.b * (1.0f - vetGold * 0.7f));
    }
    if (m.conjuring) {
        float pulse = 0.5f + 0.5f * std::sin(m.clock * 7.0f + v.position.y * 0.03f);
        float glow = (1.0f - m.progress) * pulse;
        v.color.a = m.alpha();
        v.color.r = Uint8(v.color.r * (1.0f - 0.75f * glow));
        v.color.g = Uint8(v.color.g * (1.0f - 0.25f * glow));
    }
    if (m.emote) {
        const SDL_Color c = m.emoteColour;
        const float mix = m.emoteMix;
        v.color.r = Uint8(int(v.color.r) + int((int(c.r) - int(v.color.r)) * mix));
        v.color.g = Uint8(int(v.color.g) + int((int(c.g) - int(v.color.g)) * mix));
        v.color.b = Uint8(int(v.color.b) + int((int(c.b) - int(v.color.b)) * mix));
    }
    if (m.spectral) v.color.a = Uint8(int(v.color.a) * 55 / 100);
    if (m.fade != 255) v.color.a = Uint8(int(v.color.a) * int(m.fade) / 255);
}

// Colours of every body vertex from the base colours (`base` empty = all white).
inline void applyBodyMaterial(std::span<SDL_Vertex> verts, std::span<const SDL_Color> base, const BodyMaterial& m) {
    for (size_t i = 0; i < verts.size(); ++i) {
        verts[i].color = base.empty() ? kWhiteBase : base[i];
        applyMaterial(verts[i], m);
    }
}

// ---- miss attribution (telemetry only) -------------------------------------
inline constexpr uint32_t missBit(MissCause c) { return 1u << unsigned(c); }

// Bit set of MissCause values whose LOCAL inputs differ. An owner difference is
// reported alone (it makes every other comparison meaningless).
inline uint32_t localKeyChanges(std::span<const uint64_t> before, LocalKeyLayout beforeLayout,
                                std::span<const uint64_t> after, LocalKeyLayout afterLayout) {
    if (before.size() < kLocalKeyFixedWords || after.size() < kLocalKeyFixedWords)
        return missBit(MissCause::Changed);
    if (before[0] != after[0]) return missBit(MissCause::Owner);
    uint32_t mask = 0;
    for (size_t i = 1; i < kLocalKeyFixedWords; ++i)
        if (before[i] != after[i]) mask |= missBit(kLocalKeyWordCause[i]);
    const bool layoutOk = beforeLayout.poseBegin >= kLocalKeyFixedWords && beforeLayout.poseBegin <= before.size() &&
        afterLayout.poseBegin >= kLocalKeyFixedWords && afterLayout.poseBegin <= after.size();
    if (!layoutOk) return mask | missBit(MissCause::Changed);
    const auto texBefore = before.subspan(kLocalKeyFixedWords, beforeLayout.poseBegin - kLocalKeyFixedWords);
    const auto texAfter = after.subspan(kLocalKeyFixedWords, afterLayout.poseBegin - kLocalKeyFixedWords);
    if (!std::equal(texBefore.begin(), texBefore.end(), texAfter.begin(), texAfter.end()))
        mask |= missBit(MissCause::Material);
    const auto poseBefore = before.subspan(beforeLayout.poseBegin);
    const auto poseAfter = after.subspan(afterLayout.poseBegin);
    if (poseBefore.empty() != poseAfter.empty()) mask |= missBit(MissCause::Resource);
    else if (!poseBefore.empty()) {
        if (poseBefore[0] != poseAfter[0]) mask |= missBit(MissCause::Resource);   // piece table
        if (!std::equal(poseBefore.begin() + 1, poseBefore.end(), poseAfter.begin() + 1, poseAfter.end()))
            mask |= missBit(MissCause::Pose);
    }
    return mask;
}

inline uint32_t materialKeyChanges(const MaterialKey& before, const MaterialKey& after) {
    return before == after ? 0u : missBit(MissCause::Material);
}

inline uint32_t placementKeyChanges(const PlacementKey& before, const PlacementKey& after) {
    uint32_t mask = 0;
    if (before[kPlacementZoom] != after[kPlacementZoom]) mask |= missBit(MissCause::Zoom);
    for (size_t i = kPlacementZoom + 1; i < kPlacementWords; ++i)
        if (before[i] != after[i]) mask |= missBit(MissCause::Placement);
    return mask;
}

// The single cause a miss is filed under: the most fundamental stage that changed.
inline MissCause primaryMissCause(uint32_t mask) {
    for (MissCause c : {MissCause::Owner, MissCause::Resource, MissCause::Settings, MissCause::Pose,
                        MissCause::Material, MissCause::Zoom, MissCause::Placement})
        if (mask & missBit(c)) return c;
    return MissCause::Changed;
}

}  // namespace tak::render
