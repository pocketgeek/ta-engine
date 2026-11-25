// ta_shot_test: TotalA.exe's shot timing and body test for every shot kind.
//
//   ta_shot_test <retail-install-dir>
//
//   1. The fire tick: TotalA.exe's tick 0x495490 runs the units (0x4954ed,
//      which fire at 0x48adda -> 0x49e1a0) and then the projectiles
//      (0x495513 -> 0x49b720), so a cannon shell (0x49cde0), a rocket
//      (0x49c9c0, selfprop) and a bomb (0x49dd60) have taken one step when
//      their fire tick ends: the shell and the bomb moved, drifted and fell
//      (0x49bce3), the rocket moved by its velocity (0x49bb5b).
//   2. 0x49b090's body test (0x49b1c7..0x49b211): the body owning the cell is
//      struck anywhere in its cells below its y + model height, with no
//      outline: a thin tall tower and a wide low solar, each at a corner cell
//      and just under / at its top.
//   3. A moving target: every shot is tested against the bodies where the
//      units' pass of the same tick left them. An LLT laser and a rocket at
//      a walking enemy: at the end of every tick the live head is not in the
//      target's cells below its top, and the burst point is.
//
// A dev harness; not shipped in a game.

#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/retailtashot.h"
#include "sim/sim.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace ta;

namespace {
int failures = 0;
void check(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++failures;
}
constexpr int32_t F(int px) { return int32_t(uint32_t(px) << 16); }
std::array<int32_t,3> add(std::array<int32_t,3> a, const std::array<int32_t,3>& v) {
    for (size_t i = 0; i < 3; ++i) a[i] = int32_t(uint32_t(a[i]) + uint32_t(v[i]));
    return a;
}
std::string pt(const std::array<int32_t,3>& p) {
    char b[96];
    std::snprintf(b, sizeof b, "(%.2f, %.2f, %.2f)", p[0] / 65536.0, p[1] / 65536.0, p[2] / 65536.0);
    return b;
}

void setup(sim::World& w, const sim::TypeRegistry& reg, hpi::Vfs& vfs, const std::string& map) {
    sim::MatchConfig cfg;
    cfg.vfs = &vfs;
    cfg.mapPath = map;
    cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
    cfg.slots[0].team = 0;
    cfg.slots[1].team = 1;
    sim::setupMatch(w, reg, cfg);
    w.setStartResources(0, 50000, 50000);
    w.setStartResources(1, 50000, 50000);
}

bool levelRow(const sim::World& w, const sim::UnitType* t, int length, float& x, float& z) {
    const sim::NavGrid& g = w.navFor(t);
    const int cells = length / 16 + 4;
    for (int cz = 12; cz < g.height() - 12; cz += 3)
        for (int cx = 12; cx + cells < g.width() - 12; cx += 3) {
            bool ok = true;
            for (int dx = 0; dx <= cells && ok; ++dx)
                for (int dz = -4; dz <= 4 && ok; ++dz)
                    if (!g.fits(cx + dx, cz + dz, 2)) ok = false;
            if (!ok) continue;
            x = float(cx) * 16 + 8;
            z = float(cz) * 16 + 8;
            return true;
        }
    return false;
}

// The shot `from` launched in the tick just run (+0x42 is the fire tick).
const sim::Projectile* fresh(const sim::World& w, int from) {
    for (const auto& p : w.projectiles())
        if (p.fromId == from && p.start == w.tickCount() && p.life > 0 && !p.spent &&
            (p.taBallistic || p.taSelfProp || p.taLineOfSight))
            return &p;
    return nullptr;
}
const sim::Projectile* byLaunch(const sim::World& w, int from, uint32_t start) {
    for (const auto& p : w.projectiles())
        if (p.fromId == from && p.start == start && p.life > 0 && !p.spent) return &p;
    return nullptr;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ta_shot_test <retail-install-dir>\n");
        return 2;
    }
    hpi::Vfs vfs = hpi::mountRetailRoot(argv[1]);
    sim::TypeRegistry reg;
    sim::setupRegistry(reg, vfs);
    const std::string map = hpi::findMap(vfs, "Ashap Plateau");
    const sim::UnitType* ham = reg.find("armham");
    const sim::UnitType* rock = reg.find("armrock");
    const sim::UnitType* thund = reg.find("armthund");
    const sim::UnitType* llt = reg.find("armllt");
    const sim::UnitType* solar = reg.find("corsolar");
    const sim::UnitType* tower = reg.find("corllt");
    const sim::UnitType* walker = reg.find("corak");
    if (!ham || !rock || !thund || !llt || !solar || !tower || !walker || map.empty()) {
        std::printf("missing a unit type or Ashap Plateau\n");
        return 1;
    }
    float x = 0, z = 0;
    {
        sim::World probe;
        setup(probe, reg, vfs, map);
        if (!levelRow(probe, solar, 480, x, z)) { check(false, "level ground"); return 1; }
    }

    // ---- 1. The fire tick ------------------------------------------------
    std::printf("[a shot takes its first step in its fire tick (0x4954ed, then 0x495513)]\n");
    struct Shooter { const sim::UnitType* type; float gap; const char* what; };
    const Shooter shooters[] = {
        {ham, 200.0f, "an armham shell (0x49cde0, 0x49bce3)"},
        {rock, 200.0f, "an armrock rocket (0x49c9c0 selfprop, 0x49bb5b)"},
        {thund, 400.0f, "an armthund bomb (0x49dd60, 0x49bce3)"},
    };
    for (const auto& s : shooters) {
        sim::World w;
        setup(w, reg, vfs, map);
        const int shooter = w.spawn(s.type, x, z, 0.0f, 0);
        const int target = w.spawn(solar, x + s.gap, z, 0.0f, 1);
        w.attack(shooter, target, false);
        const sim::Projectile* shot = nullptr;
        for (int t = 0; t < 1800 && !shot; ++t) {
            w.tick(1.0f / 30.0f);
            shot = fresh(w, shooter);
        }
        check(shot != nullptr, std::string(s.what) + ": fired");
        if (!shot) continue;
        const auto m = shot->muzzle, p = shot->position, v = shot->velocity;
        bool exact = false;
        std::string detail = "muzzle " + pt(m) + " head " + pt(p) + " velocity " + pt(v);
        if (shot->taSelfProp) {
            // Velocity rebuilt from heading, pitch and speed, then the move.
            exact = p == add(m, v);
        } else {
            // Move, the wind (horizontal), then gravity: y is exact, x and z
            // differ from the velocity by the wind vector only.
            const int32_t g = w.ballisticGravityRaw();
            const int32_t dy = int32_t(uint32_t(p[1]) - uint32_t(m[1]));
            const double wx = (double(p[0]) - m[0] - v[0]) / 65536.0;
            const double wz = (double(p[2]) - m[2] - v[2]) / 65536.0;
            exact = dy == int32_t(uint32_t(v[1]) + uint32_t(g)) && std::abs(wx) < 0.2 && std::abs(wz) < 0.2 &&
                    p != m;
            char b[96];
            std::snprintf(b, sizeof b, ", wind %.3f %.3f", wx, wz);
            detail += b;
            if (shot->wsrc && shot->wsrc->taAim == sim::Weapon::TaAim::Dropped)
                exact = exact && p[1] == m[1] && v[1] == -g;   // a bomb leaves level (0x49de19)
        }
        check(exact && shot->age == 1, std::string(s.what) + " has moved one step when its fire tick ends", detail);
    }

    // ---- 2. 0x49b090: the height of the cell's body ----------------------
    std::printf("[0x49b090 strikes a cell's body below its top, no outline (0x49b1c7..0x49b211)]\n");
    {
        sim::World w;
        setup(w, reg, vfs, map);
        const int thin = w.spawn(tower, x, z, 0.0f, 1);
        const int wide = w.spawn(solar, x + 240, z, 0.0f, 1);
        for (int t = 0; t < 3; ++t) w.tick(1.0f / 30.0f);
        for (int id : {thin, wide}) {
            const sim::Unit& u = *w.unit(id);
            const sim::UnitType& t = *u.type;
            const int ox = sim::footprintOrigin(u.x, t.footX), oz = sim::footprintOrigin(u.z, t.footZ);
            // A corner cell of the footprint, at its far corner pixel: outside
            // any outline drawn inside the footprint.
            const int cx = ox + t.footX - 1, cz = oz + t.footZ - 1;
            const int32_t top = int32_t(uint32_t(u.groundY.v) + uint32_t(t.modelTop));
            const std::array<int32_t,3> under{F(cx * 16 + 15), top - 1, F(cz * 16 + 15)};
            const std::array<int32_t,3> at{F(cx * 16 + 15), top, F(cz * 16 + 15)};
            const std::array<int32_t,3> low{F(ox * 16), u.groundY.v + 1, F(oz * 16)};
            int32_t vy = 0;
            const auto a = w.projectileCollision(under, vy, 0, 0, {});
            const auto b = w.projectileCollision(at, vy, 0, 0, {});
            const auto c = w.projectileCollision(low, vy, 0, 0, {});
            const auto own = w.projectileCollision(under, vy, 0, 1, {});
            char d[160];
            std::snprintf(d, sizeof d, "%s %dx%d, %.1f px tall", t.id.c_str(), t.footX, t.footZ,
                          t.modelTop / 65536.0);
            check(a.code == 2 && a.unitId == id && c.code == 2 && c.unitId == id,
                  std::string(id == thin ? "a thin tall" : "a wide low") +
                      " body is struck at its corner cells, just under its top", d);
            check(b.unitId != id, std::string(id == thin ? "the tower" : "the solar") +
                                      ": a point at its top passes over (0x49b20d jge)", d);
            check(own.unitId != id, "its owner's shot passes it (0x49b1f4)");
        }
        const sim::Unit& lo = *w.unit(wide);
        const sim::Unit& hi = *w.unit(thin);
        // One height between the two tops: over the solar, into the tower.
        const int32_t mid = int32_t((int64_t(lo.groundY.v) + lo.type->modelTop + hi.groundY.v + hi.type->modelTop) / 2);
        int32_t vy = 0;
        check(lo.type->modelTop < hi.type->modelTop &&
                  w.projectileCollision({lo.x.v, mid, lo.z.v}, vy, 0, 0, {}).unitId != wide &&
                  w.projectileCollision({hi.x.v, mid, hi.z.v}, vy, 0, 0, {}).unitId == thin,
              "a shot at one height flies over the low solar and into the tall tower");
    }

    // ---- 3. A moving target ----------------------------------------------
    std::printf("[a moving target: the bodies as the units' pass left them]\n");
    for (const sim::UnitType* st : {llt, rock}) {
        sim::World w;
        setup(w, reg, vfs, map);
        const int shooter = w.spawn(st, x, z, 0.0f, 0);
        const int target = w.spawn(walker, x + 420, z + 40, 0.0f, 1);
        // The target walks past the shooter, across its line of fire.
        w.order(target, x + 60, z - 120, false);
        w.attack(shooter, target, false);
        int bursts = 0, clean = 0, live = 0, alive = 0;
        std::string why;
        for (int round = 0; round < 6; ++round) {
            const sim::Projectile* s = nullptr;
            for (int t = 0; t < 400 && !s; ++t) {
                w.tick(1.0f / 30.0f);
                s = fresh(w, shooter);
            }
            if (!s || !w.unit(target) || !w.unit(target)->alive()) break;
            const uint32_t start = s->start;
            auto head = s->position;
            const auto vel = s->velocity;
            bool flying = true;
            for (int t = 0; t < 120 && flying; ++t) {
                // At the end of the tick: the live head is clear of the target.
                int32_t vy = vel[1];
                ++live;
                if (w.projectileCollision(head, vy, 0, 0, {}).unitId == target) {
                    if (why.empty()) why = "a live head inside the target at tick " + std::to_string(w.tickCount());
                } else ++alive;
                w.tick(1.0f / 30.0f);
                if (const auto* q = byLaunch(w, shooter, start)) {
                    head = q->position;
                    continue;
                }
                flying = false;
                // Its burst point: one more straight step (no steering: these
                // two fly at a fixed velocity).
                const auto burst = add(head, vel);
                vy = vel[1];
                const auto hit = w.projectileCollision(burst, vy, 0, 0, {});
                if (hit.code == 2) {
                    ++bursts;
                    if (hit.unitId == target) ++clean;
                }
            }
        }
        const std::string name = st->id;
        check(bursts > 0 && live == alive, name + ": no live head is inside the target as it stands after the units moved",
              why.empty() ? std::to_string(live) + " live ticks" : why);
        check(bursts > 0 && clean > 0, name + ": its shots burst in the moving target where it stands that tick",
              std::to_string(clean) + " of " + std::to_string(bursts) + " bursts in the target");
    }

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
