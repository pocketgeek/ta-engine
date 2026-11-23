// Staged unit geometry (client/geometrystages.h, workstream D): the placement and
// material stages must reproduce the former single-pass body/shadow arithmetic
// bit for bit, partial updates must equal a complete rebuild, and the stage plan
// must redo material when a conjuring unit's placement changes (its pulse reads
// the placed y). The "former" code below is copied from GameView::buildUnitGeom /
// buildUnitShadow as they were before the stages (c2c25707), so this test pins
// the new code to that arithmetic, not to itself.
#include "client/geometrystages.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

namespace {
using namespace tak::render;
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

// ---- the former single pass (verbatim arithmetic) ----
struct FormerInputs {
    float zm, ax, ay, discoBob;
    int veteran;
    bool conjuring;
    float p, animClock;
    bool disco;
    SDL_Color discoCol;
    float discoMix;
    bool ghost;
};
SDL_Vertex formerBodyVertex(SDL_Vertex v, const FormerInputs& in) {
    const float zm = in.zm, ax = in.ax, ay = in.ay, discoBob = in.discoBob, p = in.p;
    const bool conjuring = in.conjuring, disco = in.disco;
    Uint8 alpha = Uint8(p * 255.0f);
    const bool spectral = in.ghost && !conjuring;
    float vetGold = (!conjuring && in.veteran >= 4)
                        ? float(std::min(in.veteran, 10) - 3) / 7.0f * 0.5f : 0.0f;
    v.position.x = v.position.x * zm + ax;
    v.position.y = v.position.y * zm + ay - discoBob;
    if (vetGold > 0) {
        v.color.r = Uint8(v.color.r + (255 - v.color.r) * vetGold);
        v.color.g = Uint8(v.color.g + (200 - v.color.g) * vetGold * 0.85f);
        v.color.b = Uint8(v.color.b * (1.0f - vetGold * 0.7f));
    }
    if (conjuring) {
        float pulse = 0.5f + 0.5f * std::sin(in.animClock * 7.0f + v.position.y * 0.03f);
        float glow = (1.0f - p) * pulse;
        v.color.a = alpha;
        v.color.r = Uint8(v.color.r * (1.0f - 0.75f * glow));
        v.color.g = Uint8(v.color.g * (1.0f - 0.25f * glow));
    }
    if (disco) {
        const SDL_Color discoCol = in.discoCol;
        const float discoMix = in.discoMix;
        v.color.r = Uint8(int(v.color.r) + int((int(discoCol.r) - int(v.color.r)) * discoMix));
        v.color.g = Uint8(int(v.color.g) + int((int(discoCol.g) - int(v.color.g)) * discoMix));
        v.color.b = Uint8(int(v.color.b) + int((int(discoCol.b) - int(v.color.b)) * discoMix));
    }
    if (spectral) v.color.a = Uint8(int(v.color.a) * 55 / 100);
    return v;
}
constexpr float kShadowLX = 0.25f, kShadowLZ = 0.25f, kProjY = 0.5f;
SDL_FPoint formerShadowPoint(SDL_FPoint point, float ax, float ay, float alt, float zm) {
    const float sx = ax + kShadowLX * alt * zm;
    const float sy = ay + (kProjY - kShadowLZ) * alt * zm;
    return {sx + point.x * zm, sy + point.y * zm};
}

// ---- the staged equivalents, as GameView wires them ----
BodyMaterial materialOf(const FormerInputs& in) {
    BodyMaterial m;
    m.vetGold = (!in.conjuring && in.veteran >= 4) ? float(std::min(in.veteran, 10) - 3) / 7.0f * 0.5f : 0.0f;
    m.conjuring = in.conjuring; m.progress = in.p; m.clock = in.animClock;
    m.emote = in.disco; m.emoteColour = in.discoCol; m.emoteMix = in.discoMix;
    m.spectral = in.ghost && !in.conjuring;
    return m;
}
ShadowPlacement shadowOf(float ax, float ay, float alt, float zm) {
    return {zm, ax + kShadowLX * alt * zm, ay + (kProjY - kShadowLZ) * alt * zm};
}

struct Staged {
    std::vector<SDL_FPoint> local;
    std::vector<SDL_Color> base;   // empty = all white
    std::vector<SDL_Vertex> verts;
};
// Local stage output from collector vertices (positions local, colours base).
Staged localStage(const std::vector<SDL_Vertex>& collected) {
    Staged s;
    s.verts = collected;
    bool white = true;
    for (const auto& v : collected) { s.local.push_back(v.position); white = white && isWhite(v.color); }
    if (!white) for (const auto& v : collected) s.base.push_back(v.color);
    return s;
}
void runStages(Staged& s, uint8_t stages, const FormerInputs& in) {
    if (stages & kStagePlacement) placeBody(s.local, s.verts, {in.zm, in.ax, in.ay, in.discoBob});
    if (stages & kStageMaterial) applyBodyMaterial(s.verts, s.base, materialOf(in));
}
bool sameBytes(const std::vector<SDL_Vertex>& a, const std::vector<SDL_Vertex>& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof a[0]) == 0);
}
std::vector<SDL_Vertex> formerAll(const std::vector<SDL_Vertex>& collected, const FormerInputs& in) {
    std::vector<SDL_Vertex> out;
    for (const auto& v : collected) out.push_back(formerBodyVertex(v, in));
    return out;
}
MaterialKey materialKeyOf(const FormerInputs& in) {
    return {uint64_t(in.veteran), in.ghost, in.conjuring, in.disco,
            uint64_t(in.discoCol.r) | (uint64_t(in.discoCol.g) << 8) | (uint64_t(in.discoCol.b) << 16),
            floatBits(in.discoMix), floatBits(in.p), in.conjuring ? floatBits(in.animClock) : 0u};
}
PlacementKey placementKeyOf(const FormerInputs& in) {
    return {floatBits(in.zm), floatBits(in.ax), floatBits(in.ay), floatBits(in.discoBob), 0, 0};
}
}  // namespace

int main() {
    std::mt19937 rng(20261016);
    auto uniform = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };
    auto byte = [&] { return Uint8(rng() & 255); };
    auto coin = [&](unsigned oneIn) { return rng() % oneIn == 0; };
    auto randomInputs = [&] {
        FormerInputs in{};
        in.zm = coin(4) ? 1.0f : uniform(0.05f, 2.0f);
        in.ax = uniform(-9000, 9000); in.ay = uniform(-9000, 9000);
        in.discoBob = coin(3) ? uniform(-16, 11) : 0.0f;
        in.veteran = int(rng() % 13) - 1;
        in.conjuring = coin(4);
        in.p = in.conjuring ? uniform(0, 1) : 1.0f;
        in.animClock = uniform(0, 4000);
        in.disco = coin(4);
        in.discoCol = {byte(), byte(), byte(), 255};
        in.discoMix = uniform(0, 0.65f);
        in.ghost = coin(5);
        return in;
    };
    auto randomCollected = [&](size_t n, bool grey) {
        std::vector<SDL_Vertex> v(n);
        for (auto& x : v) {
            x.position = {uniform(-200, 200), uniform(-300, 100)};
            x.tex_coord = {uniform(0, 1), uniform(0, 1)};
            x.color = grey && coin(3) ? SDL_Color{170, 170, 180, 255} : SDL_Color{255, 255, 255, 255};
        }
        return v;
    };

    {   // Placement and shadow placement: exact former arithmetic, incl. awkward values.
        const float specials[] = {0.0f, -0.0f, 1e-38f, -1e-38f, 3.4e38f, 0.1f, -123.456f,
                                  std::numeric_limits<float>::denorm_min()};
        size_t bad = 0, badShadow = 0;
        for (int round = 0; round < 20000; ++round) {
            FormerInputs in = randomInputs();
            SDL_Vertex v{{uniform(-500, 500), uniform(-500, 500)}, {255, 255, 255, 255}, {0, 0}};
            if (round < 64) v.position = {specials[round % 8], specials[(round / 8) % 8]};
            std::vector<SDL_FPoint> local{v.position};
            std::vector<SDL_Vertex> out{v};
            placeBody(local, out, {in.zm, in.ax, in.ay, in.discoBob});
            FormerInputs plain = in; plain.veteran = 0; plain.conjuring = plain.disco = plain.ghost = false;
            const SDL_Vertex former = formerBodyVertex(v, plain);
            bad += std::memcmp(&former.position, &out[0].position, sizeof former.position) != 0;
            const float alt = coin(2) ? uniform(0, 300) : 0.0f;
            const SDL_FPoint a = formerShadowPoint(v.position, in.ax, in.ay, alt, in.zm);
            const SDL_FPoint b = placeShadowPoint(v.position, shadowOf(in.ax, in.ay, alt, in.zm));
            badShadow += std::memcmp(&a, &b, sizeof a) != 0;
        }
        check(bad == 0, "body placement equals the former transform");
        check(badShadow == 0, "shadow placement equals the former transform");
    }
    {   // Material over placed vertices: exact former colour code, every option mix.
        size_t bad = 0;
        for (int round = 0; round < 20000; ++round) {
            const FormerInputs in = randomInputs();
            const auto collected = randomCollected(6, coin(2));
            Staged s = localStage(collected);
            runStages(s, kStageLocal | kStagePlacement | kStageMaterial, in);
            bad += !sameBytes(s.verts, formerAll(collected, in));
        }
        check(bad == 0, "complete staged build equals the former single pass");
    }
    {   // Partial updates equal a complete rebuild; the plan decides which stages.
        size_t bad = 0, conjuringPlacement = 0, skippedWouldDiffer = 0;
        for (int round = 0; round < 20000; ++round) {
            const FormerInputs first = randomInputs();
            FormerInputs next = first;
            switch (rng() % 4) {
                case 0: next.ax += uniform(-50, 50); next.ay += uniform(-50, 50); break;   // pan
                case 1: next.zm = uniform(0.05f, 2.0f); break;                             // zoom
                case 2: next.veteran = int(rng() % 13); next.ghost = coin(3); break;       // appearance
                default: next.animClock += 1.0f / 60; next.ax += 1; break;                 // both
            }
            if (coin(5)) next.conjuring = !next.conjuring;
            const auto collected = randomCollected(9, coin(2));
            Staged s = localStage(collected);
            runStages(s, kStageLocal | kStagePlacement | kStageMaterial, first);
            const uint8_t stages = planStages(true, placementKeyOf(first) == placementKeyOf(next),
                                              materialKeyOf(first) == materialKeyOf(next), next.conjuring);
            Staged partial = s;
            runStages(partial, stages, next);
            Staged complete = localStage(collected);
            runStages(complete, kStageLocal | kStagePlacement | kStageMaterial, next);
            bad += !sameBytes(partial.verts, complete.verts);
            if (next.conjuring && (stages & kStagePlacement) && materialKeyOf(first) == materialKeyOf(next)) {
                // Material is redone only because the pulse reads the placed y. Skipping
                // it must be visible, or this test could not catch a plan that skips it.
                ++conjuringPlacement;
                Staged skipped = s;
                runStages(skipped, kStagePlacement, next);
                skippedWouldDiffer += !sameBytes(skipped.verts, complete.verts);
            }
        }
        check(bad == 0, "partial stage updates equal a complete rebuild");
        check(conjuringPlacement > 0, "conjuring re-placements exercised");
        check(skippedWouldDiffer > 0, "skipping a conjuring unit's material on a pan is detectable");
    }
    {   // Stage plan truth table.
        check(planStages(false, true, true, false) == (kStageLocal | kStagePlacement | kStageMaterial),
              "invalid local redoes everything");
        check(planStages(true, true, true, false) == 0, "nothing changed");
        check(planStages(true, false, true, false) == kStagePlacement, "pan re-places only");
        check(planStages(true, true, false, false) == kStageMaterial, "appearance re-colours only");
        check(planStages(true, false, true, true) == (kStagePlacement | kStageMaterial),
              "conjuring pan also re-colours");
        check(planStages(true, true, false, true) == kStageMaterial, "conjuring clock re-colours only");
    }
    {   // Base colours: empty means white; a white base reproduces the identity material.
        std::vector<SDL_Vertex> v(3, SDL_Vertex{{1, 2}, {9, 9, 9, 9}, {0, 0}});
        applyBodyMaterial(v, {}, BodyMaterial{});
        check(isWhite(v[0].color) && isWhite(v[2].color), "empty base colours are white");
        check(BodyMaterial{}.identity(), "default material is identity");
        BodyMaterial gold; gold.vetGold = 0.1f;
        check(!gold.identity(), "veteran gold is not identity");
    }
    {   // Shadow bounds: the former min/max loop (opaque first, then masked), incl.
        // NaN points and signed zeros, where comparison order matters.
        const float nan = std::nanf("");
        const std::vector<std::vector<SDL_FPoint>> sets = {
            {{0.0f, -0.0f}, {-0.0f, 0.0f}, {nan, 3}, {5, nan}, {-2, 7}, {1e30f, -1e30f}},
            {{0.0f, -0.0f}, {-0.0f, 0.0f}},          // signed zeros are the extremes
            {{-0.0f, 0.0f}, {0.0f, -0.0f}},
            {{1, 1}, {nan, nan}},                    // NaN last
            {{nan, nan}, {1, 1}},                    // NaN first
        };
        size_t bad = 0;
        for (const auto& pts : sets) {
            float minX = std::numeric_limits<float>::infinity(), minY = minX, maxX = -minX, maxY = -minX;
            for (const auto& p : pts) {
                minX = std::min(minX, p.x); minY = std::min(minY, p.y);
                maxX = std::max(maxX, p.x); maxY = std::max(maxY, p.y);
            }
            ShadowBoundsAccumulator acc;
            for (const auto& p : pts) acc.add(p);
            const std::array<float, 4> former{minX, minY, maxX, maxY};
            bad += std::memcmp(former.data(), acc.bounds().data(), sizeof former) != 0;
        }
        check(bad == 0, "shadow bounds equal the former loop");
        const auto empty = ShadowBoundsAccumulator{}.bounds();
        check(std::isinf(empty[0]) && empty[0] > 0 && std::isinf(empty[2]) && empty[2] < 0, "empty bounds");
    }
    {   // Workstream G: which local-key changes keep the body's vertex count (the
        // body collector culls back faces, so every pose word counts; only the
        // colour atlas, player, colour slot, shadow toggle and texture frames do not).
        constexpr size_t kPoseKeyWordsForTest = 10;              // visible..rendered, move[3], rot[3]
        std::vector<uint64_t> base(kLocalKeyFixedWords, 7);
        base.push_back(11); base.push_back(12);                  // two animated frames
        const LocalKeyLayout layout{uint32_t(base.size())};
        base.push_back(0xabc);                                   // piece table
        for (int piece = 0; piece < 3; ++piece)
            for (size_t w = 0; w < kPoseKeyWordsForTest; ++w) base.push_back(100 + piece * 10 + w);
        check(bodyVertexCountStable(base, layout, base, layout), "identical keys are stable");
        size_t stable = 0, unstable = 0;
        for (size_t i = 0; i < base.size(); ++i) {
            auto changed = base; ++changed[i];
            const bool keeps = bodyVertexCountStable(base, layout, changed, layout);
            const bool appearance = i == 3 || i == 4 || i == 5 || i == 11 ||
                                    (i >= kLocalKeyFixedWords && i < layout.poseBegin);
            check(keeps == appearance, "only appearance words keep the body vertex count");
            (keeps ? stable : unstable) += 1;
        }
        check(stable == 6 && unstable == base.size() - 6, "stable word census");
        auto shorter = base; shorter.pop_back();
        check(!bodyVertexCountStable(base, layout, shorter, layout), "a different pose count is not stable");
        check(!bodyVertexCountStable(base, layout, base, LocalKeyLayout{layout.poseBegin + 1}),
              "a different layout is not stable");
    }
    if (failures) return 1;
    std::puts("PASS: staged geometry equals the former single pass; partial updates equal rebuilds");
    return 0;
}
