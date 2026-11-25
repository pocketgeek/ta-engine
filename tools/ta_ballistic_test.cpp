// ta_ballistic_test: TotalA.exe's ballistic shells (docs/retail-weapon-scripts.md 7.8).
//
//   ta_ballistic_test <retail-install-dir>
//
//   1. ta::taBallisticPitch reproduces 0x49a890 (values from
//      tools/re/ballistic_emu.py, which runs the routine under unicorn): the
//      low arc, the pi/4 cap, minbarrelangle, and the unsigned angle a target
//      below at short range gets.
//   2. A shell launched on that pitch (0x49cde0's velocity) and flown by
//      0x49bce3's motion (move, wind, then gravity, once a tick) passes its
//      aim point within a few px, at several ranges and heights; the barrel
//      word lowers it (0x49ce6a); the wind vector drifts it (0x490d05).
//   3. In a World: ballistic cannons ordered on a target fire and their shells
//      burst near the aim point; Core's Battle Cruiser (cor_bats, 350 px/s)
//      engages at 1000 px on level ground and not at ~1100 px, where 0x49aa80's
//      arc test refuses (no arc past ~1094 px at gravity 112); paired Worlds
//      keep one state hash.
//
// A dev harness; not shipped in a game.

#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/retailaim.h"
#include "sim/retailtashot.h"
#include "sim/sim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace ta;

namespace {
int failures = 0;
void check(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++failures;
}
constexpr int32_t F(int px) { return int32_t(uint32_t(px) << 16); }
constexpr int32_t V(double pxPerSecond) { return int32_t(pxPerSecond * 65536.0 / 30.0); }
constexpr int32_t kGravity = 8155;   // 112 * 65536 / 900 (0x48383b)
constexpr float kMin25 = -0.4363323f;

// Fly a shell from the origin toward a point `dist` px along -z and `height`
// px above, launched on `pitch`; return its height above the point (px) when
// it passes the point's horizontal distance.
double passHeight(int dist, int height, int32_t speed, uint16_t pitch, int32_t barrel = 0,
                  std::array<int32_t,3> wind = {}, double* drift = nullptr) {
    std::array<int32_t,3> pos{}, vel = sim::taBallisticVelocity(0, pitch, speed, barrel, kGravity);
    for (int tick = 0; tick < 100000; ++tick) {
        const auto prev = pos;
        sim::taBallisticStep(pos, vel, wind, kGravity);
        if (-double(pos[2]) / 65536.0 >= dist) {
            const double z0 = -double(prev[2]) / 65536.0, z1 = -double(pos[2]) / 65536.0;
            const double f = (dist - z0) / (z1 - z0);
            const double y = (prev[1] + (pos[1] - prev[1]) * f) / 65536.0;
            if (drift) *drift = (prev[0] + (pos[0] - prev[0]) * f) / 65536.0;
            return y - height;
        }
    }
    return 1e9;
}

struct Pair {
    sim::World a, b;
    bool paired = true;
    void tick() {
        a.tick(1.0f / 30.0f);
        b.tick(1.0f / 30.0f);
        paired = paired && a.stateHash() == b.stateHash();
    }
};

void setup(Pair& p, const sim::TypeRegistry& reg, hpi::Vfs& vfs, const std::string& map) {
    sim::MatchConfig cfg;
    cfg.vfs = &vfs;
    cfg.mapPath = map;
    cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
    cfg.slots[0].team = 0;
    cfg.slots[1].team = 1;
    sim::setupMatch(p.a, reg, cfg);
    sim::setupMatch(p.b, reg, cfg);
    for (auto* w : {&p.a, &p.b}) {
        w->setStartResources(0, 50000, 50000);
        w->setStartResources(1, 50000, 50000);
    }
}

// A row of open ground `length` px long for type `t`, west to east.
bool levelRow(const sim::World& w, const sim::UnitType* t, int length, float& x, float& z) {
    const sim::NavGrid& g = w.navFor(t);
    const int foot = std::max(1, std::max(t->footX, t->footZ)), cells = length / 16 + 4;
    for (int cz = 12; cz < g.height() - 12; cz += 3)
        for (int cx = 12; cx + cells < g.width() - 12; cx += 3) {
            bool ok = true;
            for (int dx = 0; dx <= cells && ok; ++dx)
                for (int dz = -1; dz <= 1 && ok; ++dz)
                    if (!g.fits(cx + dx, cz + dz, foot)) ok = false;
            if (!ok) continue;
            x = float(cx) * 16 + 8;
            z = float(cz) * 16 + 8;
            return true;
        }
    return false;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ta_ballistic_test <retail-install-dir>\n");
        return 2;
    }
    // ---- 1. 0x49a890 ----------------------------------------------------
    std::printf("[0x49a890, the ballistic aim solve]\n");
    struct Case { int dx, dy, dz; double v; float minAngle; uint16_t want; };
    // d = P - T; the expected values are the routine's own under unicorn.
    static const Case cases[] = {
        {200, -60, 0, 350, kMin25, 0x0fbb}, {200, 0, 0, 350, kMin25, 0x03be},
        {200, 40, 0, 350, kMin25, 0x045d},  {500, 0, 0, 350, kMin25, 0x09ab},
        {800, -60, 0, 350, kMin25, 0x148e}, {1000, 0, 0, 350, kMin25, 0x1780},
        {1000, 40, 0, 350, kMin25, 0x14d4}, {1094, 0, 0, 350, kMin25, 0x8000},
        {1094, 40, 0, 350, kMin25, 0x19b1}, {1100, -60, 0, 350, kMin25, 0x8000},
        {1100, 0, 0, 450, kMin25, 0x0d52},  {200, 40, 0, 950, kMin25, 0x0789},
        {800, 40, 0, 950, kMin25, 0x0003},  {1100, -60, 0, 950, kMin25, 0x0505},
        {300, 200, 0, 350, kMin25, 0x12c1}, {0, 0, 500, 450, 0.0f, 0x05b5},
        {400, 0, -300, 450, 0.5f, 0x8000},
    };
    int same = 0;
    std::string bad;
    for (const auto& c : cases) {
        const uint16_t got = ta::taBallisticPitch(F(c.dx), F(c.dy), F(c.dz), V(c.v), c.minAngle, kGravity);
        if (got == c.want) ++same;
        else bad += " (" + std::to_string(c.dx) + "," + std::to_string(c.dy) + "," + std::to_string(c.dz) + ")";
    }
    check(same == int(std::size(cases)), "taBallisticPitch equals 0x49a890 on every emulated case",
          std::to_string(same) + "/" + std::to_string(std::size(cases)) + bad);

    // ---- 2. The motion lands the shell on its aim point ------------------
    std::printf("[0x49cde0 launch + 0x49bce3 motion]\n");
    double worst = 0;
    std::string where;
    for (double v : {350.0, 450.0, 950.0})
        for (int dist : {150, 400, 700, 1000})
            for (int height : {-40, 0, 60}) {
                // A target below at short range gets acos's unsigned angle
                // (retail's quirk); skip the cases where the true low arc
                // points down.
                const uint16_t pitch = ta::taBallisticPitch(0, F(-height), F(dist), V(v), kMin25, kGravity);
                if (pitch == 0x8000) continue;
                const double vx = v / 30.0, g = kGravity / 65536.0;
                const double tanLow = (vx * vx - std::sqrt(std::max(0.0, vx * vx * vx * vx -
                                       g * (g * dist * dist + 2.0 * height * vx * vx)))) / (g * dist);
                if (tanLow < 0) continue;
                const double miss = std::abs(passHeight(dist, height, V(v), pitch));
                if (miss > worst) { worst = miss; where = std::to_string(int(v)) + " px/s at " +
                                    std::to_string(dist) + " px, " + std::to_string(height) + " up"; }
            }
    char detail[160];
    std::snprintf(detail, sizeof detail, "worst %.1f px (%s)", worst, where.c_str());
    check(worst < 8.0, "a shell on its solved pitch passes its aim point within 8 px", detail);
    {
        const uint16_t pitch = ta::taBallisticPitch(0, 0, F(800), V(350), kMin25, kGravity);
        const double plain = passHeight(800, 0, V(350), pitch);
        const double barrel = passHeight(800, 0, V(350), pitch, F(30));   // 30 px: 2 ticks of gravity
        check(barrel < plain - 4.0, "the barrel word takes gravity off the launch (0x49ce6a)",
              std::to_string(plain) + " -> " + std::to_string(barrel));
        double drift = 0;
        passHeight(800, 0, V(350), pitch, 0, sim::taWindVector(1000, 0), &drift);
        // 2 * 1000 / 65536 px a tick over the ~65-tick flight: about 2 px.
        check(drift < -1.5 && drift > -3.0, "the wind vector drifts a shell (-2 r sin per tick, 0x490d05)",
              std::to_string(drift) + " px");
        std::array<int32_t,3> pos{0, 0, 0}, vel{F(1), F(2), F(3)};
        sim::taBallisticStep(pos, vel, {0, 0, 0}, 100);
        check(pos[1] == F(2) && vel[1] == F(2) - 100, "the shell moves before gravity (0x49bce3..0x49bd41)");
    }

    // ---- 3. In a World ---------------------------------------------------
    hpi::Vfs vfs = hpi::mountRetailRoot(argv[1]);
    sim::TypeRegistry reg;
    sim::setupRegistry(reg, vfs);
    const std::string map = hpi::findMap(vfs, "Ashap Plateau");
    const sim::UnitType* dummy = reg.find("corsolar");
    if (!dummy || map.empty()) {
        std::printf("missing corsolar or Ashap Plateau\n");
        return 1;
    }
    std::printf("[cannons fire and their shells land near the aim point]\n");
    for (const char* name : {"armham", "armstump", "corlevlr", "cormort"}) {
        const sim::UnitType* t = reg.find(name);
        if (!t || t->weapons.empty() || !t->weapons[0].hasTaFlag(1)) {
            check(false, std::string(name) + " has a ballistic first weapon");
            continue;
        }
        const auto& w = t->weapons[0];
        for (float frac : {0.4f, 0.8f}) {
            Pair p;
            setup(p, reg, vfs, map);
            const int gap = int(float(w.range) * frac);
            float x = 0, z = 0;
            if (!levelRow(p.a, t, gap + 64, x, z)) { check(false, std::string(name) + ": level ground"); continue; }
            int ids[2][2];
            int k = 0;
            for (auto* wd : {&p.a, &p.b}) {
                ids[k][0] = wd->spawn(t, x, z, 0.0f, 0);
                ids[k][1] = wd->spawn(dummy, x + float(gap), z, 0.0f, 1);
                wd->attack(ids[k][0], ids[k][1], false);
                ++k;
            }
            if (frac < 0.5f) {
                const int32_t barrel = p.a.unit(ids[0][0])->taBarrel[0];
                std::snprintf(detail, sizeof detail, "%.1f px", barrel / 65536.0);
                check(barrel >= 0 && barrel < F(64), std::string(name) + "'s barrel word is a short muzzle offset (0x49e11b)",
                      detail);
            }
            int shells = 0, near = 0;
            double worstMiss = 0;
            std::vector<sim::Projectile> live;
            for (int tick = 0; tick < 600; ++tick) {
                p.tick();
                // A shell that left the list burst where it last was.
                for (const auto& old : live) {
                    bool still = false;
                    for (const auto& q : p.a.projectiles())
                        if (q.fromId == old.fromId && q.start == old.start && q.taBallistic && !q.spent && q.life > 0)
                            still = true;
                    if (still) continue;
                    ++shells;
                    // The burst point is the next step along: allow one tick of travel,
                    // and half the target's footprint: 0x49b090 strikes the body
                    // anywhere in its cells below its top (0x49b1c7..0x49b211).
                    const double dx = (old.position[0] - old.taTargetPoint[0]) / 65536.0;
                    const double dz = (old.position[2] - old.taTargetPoint[2]) / 65536.0;
                    const double miss = std::sqrt(dx * dx + dz * dz);
                    worstMiss = std::max(worstMiss, miss);
                    if (miss < 48.0 + 8.0 * std::max(dummy->footX, dummy->footZ) + w.accuracy / 65536.0 * 6.283 * gap) ++near;
                }
                live.clear();
                for (const auto& q : p.a.projectiles())
                    if (q.fromId == ids[0][0] && q.taBallistic && !q.spent && q.life > 0) live.push_back(q);
                const sim::Unit* d = p.a.unit(ids[0][1]);
                if (!d || !d->alive()) break;
            }
            std::snprintf(detail, sizeof detail, "%d px: %d shells, %d near the aim point, worst %.0f px",
                          gap, shells, near, worstMiss);
            check(shells > 0 && near * 4 >= shells * 3, std::string(name) + " shells land near their aim point", detail);
            check(p.paired, std::string(name) + ": paired Worlds keep the same state hash");
        }
    }

    std::printf("[cor_bats at its arc limit]\n");
    if (const sim::UnitType* bats = reg.find("corbats")) {
        const sim::Weapon* found = nullptr;
        for (const auto& x : bats->weapons) if (x.section == "cor_bats") found = &x;
        if (!found) { check(false, "corbats carries cor_bats"); return 1; }
        const auto& w = *found;
        const auto gate = [&](int dist) {
            // The range gate on level ground: origin to a point dist px off.
            std::array<int32_t,3> target{F(dist), 0, 0};
            return ta::taBallisticPitch(-target[0], 0, 0, w.velocityFx, w.minBarrelAngle, kGravity) != 0x8000 &&
                   int64_t(dist) * dist <= int64_t(w.range) * w.range;
        };
        check(gate(1000) && gate(1090) && !gate(1094) && !gate(1110),
              "cor_bats (350 px/s) has an arc to 1090 px and none from 1094 px (0x49aa80)",
              "range " + std::to_string(w.range));
    } else check(false, "corbats loads");

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
