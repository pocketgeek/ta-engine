// ta_beam_test: TotalA.exe's line-of-sight shots and beams (docs/retail-data.md 3.3).
//
//   ta_beam_test <retail-install-dir>
//
//   1. taLineOfSightStep (0x49bb60..0x49bc07): the head moves by the velocity
//      every tick; a beam's tail starts the tick after the counter passes the
//      launch tick + duration, a plain LOS shot's never moves.
//   2. The draw helpers (client/tabeamdraw.h): the colour map 0x4ac7d0 over
//      the shipped palettes, the beam's two lines (0x49bf8d..0x49c097) and
//      the lightning strands' step count (0x49c494..0x49c590).
//   3. In a World: an Arm LLT's laser leaves its muzzle on the fire tick, its
//      head reaches the target after distance / velocity ticks and bursts
//      there (damage on contact, never before), its tail follows a tick
//      behind the start; an enemy body in the line takes the shot, an own one
//      does not; a shot at a ground point bursts where its head meets the
//      ground (0x49b090); a Zeus's lightning keeps its tail at the muzzle
//      (duration 10 s); paired Worlds keep one state hash.
//
// A dev harness; not shipped in a game.

#include "client/tabeamdraw.h"
#include "gaf/gaf.h"
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/retailtashot.h"
#include "sim/sim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace ta;

namespace ta::sim {
// Test access to the fire path (World::fire is private).
struct RetailReplayProbe {
    static bool fire(World& w, int shooter, const World::WeaponTarget& t, int slot) {
        return w.fire(*w.unit(shooter), t, slot, false);
    }
};
}

namespace {
int failures = 0;
void check(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++failures;
}
constexpr int32_t F(int px) { return int32_t(uint32_t(px) << 16); }

std::array<int32_t,3> add(std::array<int32_t,3> a, const std::array<int32_t,3>& v, int k) {
    for (size_t i = 0; i < 3; ++i) a[i] = int32_t(uint32_t(a[i]) + uint32_t(v[i]) * uint32_t(k));
    return a;
}
std::string pt(const std::array<int32_t,3>& p) {
    char b[96];
    std::snprintf(b, sizeof b, "(%.1f, %.1f, %.1f)", p[0] / 65536.0, p[1] / 65536.0, p[2] / 65536.0);
    return b;
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

// A row of open ground `length` px long, west to east.
bool levelRow(const sim::World& w, const sim::UnitType* t, int length, float& x, float& z) {
    const sim::NavGrid& g = w.navFor(t);
    const int cells = length / 16 + 4;
    for (int cz = 12; cz < g.height() - 12; cz += 3)
        for (int cx = 12; cx + cells < g.width() - 12; cx += 3) {
            bool ok = true;
            for (int dx = 0; dx <= cells && ok; ++dx)
                for (int dz = -3; dz <= 3 && ok; ++dz)
                    if (!g.fits(cx + dx, cz + dz, 2)) ok = false;
            if (!ok) continue;
            x = float(cx) * 16 + 8;
            z = float(cz) * 16 + 8;
            return true;
        }
    return false;
}

const sim::Projectile* shotOf(const sim::World& w, int from, uint32_t launch) {
    for (const auto& p : w.projectiles())
        if (p.fromId == from && p.taLineOfSight && p.taLaunch == launch && p.life > 0) return &p;
    return nullptr;
}
const sim::Projectile* newShot(const sim::World& w, int from) {
    for (const auto& p : w.projectiles())
        if (p.fromId == from && p.taLineOfSight && p.taLaunch == w.tickCount() && p.life > 0) return &p;
    return nullptr;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ta_beam_test <retail-install-dir>\n");
        return 2;
    }
    // ---- 1. 0x49bb60 -----------------------------------------------------
    std::printf("[0x49bb60, the line-of-sight step]\n");
    {
        const std::array<int32_t,3> v{F(20), -F(1), F(5)};
        for (uint16_t duration : {uint16_t(0), uint16_t(1), uint16_t(4)}) {
            sim::TaLineOfSightShot s;
            s.velocity = v;
            s.launch = 100;
            bool ok = true;
            std::string why;
            // The pass of the launch tick is the first (World fires inside the
            // units' pass, before the projectiles').
            for (uint32_t now = 100; now < 112; ++now) {
                sim::taLineOfSightStep(s, true, duration, now);
                const int k = int(now - 100) + 1;
                // The bit is set on the first tick past launch + duration and
                // the tail moves from the next one.
                const int tailSteps = std::max(0, int(now) - int(100 + duration) - 1);
                if (s.head != add({}, v, k) || s.tail != add({}, v, tailSteps) ||
                    s.tailMoving != (now > 100u + duration)) {
                    ok = false;
                    why = "tick " + std::to_string(now) + " head " + pt(s.head) + " tail " + pt(s.tail);
                    break;
                }
            }
            check(ok, "duration " + std::to_string(duration) +
                      ": head every tick, tail from launch + duration + 2 (0x49bbe1..0x49bc03)", why);
        }
        sim::TaLineOfSightShot s;
        s.velocity = v;
        for (uint32_t now = 0; now < 20; ++now) sim::taLineOfSightStep(s, false, 0, now);
        check(s.head == add({}, v, 20) && s.tail == std::array<int32_t,3>{} && !s.tailMoving,
              "a plain LOS shot's tail never moves (0x49bba3 tests beamweapon)");
    }

    // ---- 2. The draw helpers --------------------------------------------
    hpi::Vfs vfs = hpi::mountRetailRoot(argv[1]);
    std::printf("[0x49be60, the beam and lightning draw]\n");
    {
        const auto read = [&](const char* p) { return gaf::Palette::fromBytes(vfs.read(p), p); };
        const auto gui = read("palettes/guipal.pal"), game = read("palettes/palette.pal");
        const auto map = taGuiColourMap(gui.rgba, game.rgba);
        // The shipped lasers' colours: LLT 232/234, HLT 96/98, Annihilator 144/217.
        check(map[232] == 212 && map[234] == 180 && map[96] == 250 && map[98] == 250 &&
              map[144] == 252 && map[217] == 182 && map[0] == 0,
              "guipal -> palette.pal nearest-colour map (0x4ac7d0)",
              std::to_string(map[232]) + " " + std::to_string(map[234]) + " " + std::to_string(map[96]) + " " +
              std::to_string(map[144]) + " " + std::to_string(map[217]));
        const auto& red = game.rgba[map[232]];
        check(red[0] > 200 && red[1] < 60 && red[2] < 20, "the light laser draws red (211, 43, 0)");

        const auto lines = taBeamLines({F(110), F(20), F(50)}, {F(80), F(20), F(52)}, 1, 7, 2);
        check(lines.size() == 2 && lines[0].colour == 2 && lines[1].colour == 1 &&
              lines[0].x1 == 80 && lines[0].y1 == 41 && lines[0].x2 == 110 && lines[0].y2 == 39 &&
              lines[1].x1 == 80 && lines[1].y1 == 42 && lines[1].x2 == 110 && lines[1].y2 == 40,
              "a level beam: color2 a row above, then color, ends ordered west to east (0x49c002, 0x49c04d)");
        const auto steep = taBeamLines({F(50), F(0), F(10)}, {F(52), F(0), F(60)}, 1, 7, 2);
        check(steep.size() == 2 && steep[0].x1 == 49 && steep[0].y1 == 10 && steep[0].x2 == 53 &&
              steep[0].y2 == 60 && steep[1].x1 == 50 && steep[1].x2 == 52,
              "a steep beam's color2 runs from a column left of its top to one right of its bottom (0x49c023)");
        check(taBeamLines({F(1), 0, 0}, {F(9), 0, 0}, 5, 0, 9).size() == 1,
              "no color2, one line (0x49bfd2)");

        TaCrtRand rand;
        const auto bolt = taLightningLines({F(100), F(0), F(0)}, {F(0), F(0), F(0)}, 4, rand);
        bool fromTail = !bolt.empty() && bolt[0].x1 == 0 && bolt[0].y1 == 0 && bolt[20].x1 == 0;
        check(bolt.size() == 40 && fromTail,
              "100 px of lightning: two strands of 20 jittered 5 px steps from the tail (0x49c510)",
              std::to_string(bolt.size()) + " segments");
        bool jitter = true;
        for (size_t i = 0; i < 20; ++i)
            jitter = jitter && std::abs(bolt[i].x2 - int(5 * (i + 1))) <= 5 && std::abs(bolt[i].y2) <= 5 + 2;
        check(jitter, "each point within 5 px of its step (rand * 11 / 32768 - 5)");
        TaCrtRand crt;
        check(crt() == 41 && crt() == 18467, "the CRT rand (0x4e4870)");
    }

    // ---- 3. In a World ---------------------------------------------------
    sim::TypeRegistry reg;
    sim::setupRegistry(reg, vfs);
    const std::string map = hpi::findMap(vfs, "Ashap Plateau");
    const sim::UnitType* llt = reg.find("armllt");
    const sim::UnitType* solar = reg.find("corsolar");
    const sim::UnitType* ownSolar = reg.find("armsolar");
    const sim::UnitType* zeus = reg.find("armzeus");
    if (!llt || !solar || !ownSolar || !zeus || map.empty()) {
        std::printf("missing armllt, corsolar, armsolar, armzeus or Ashap Plateau\n");
        return 1;
    }
    const sim::Weapon& laser = llt->weapons[0];
    check(laser.hasTaFlag(3) && laser.hasTaFlag(0) && laser.velocityFx > 0 && laser.durationTicks16 == 0,
          "armllt's ARM_LIGHTLASER is a line-of-sight beam, duration 0");
    float x = 0, z = 0;
    {
        sim::World probe;
        setup(probe, reg, vfs, map);
        if (!levelRow(probe, solar, 320, x, z)) { check(false, "level ground"); return 1; }
    }

    std::printf("[an LLT's laser in flight]\n");
    {
        Pair p;
        int shooter = 0, target = 0;
        for (auto* w : {&p.a, &p.b}) {
            setup(*w, reg, vfs, map);
            shooter = w->spawn(llt, x, z, 0.0f, 0);
            target = w->spawn(solar, x + 200, z, 0.0f, 1);
            w->attack(shooter, target, false);
        }
        sim::World& w = p.a;
        const sim::Projectile* shot = nullptr;
        for (int t = 0; t < 300 && !shot; ++t) {
            p.tick();
            shot = newShot(w, shooter);
        }
        check(shot != nullptr, "the LLT fires a line-of-sight shot");
        if (shot) {
            const uint32_t launch = shot->taLaunch;
            const auto muzzle = shot->muzzle, v = shot->velocity;
            const auto aim = shot->taTargetPoint;
            const int32_t damage = int32_t(laser.damageVs(solar));
            check(shot->start == launch && shot->position == add(muzzle, v, 1) && shot->taTail == muzzle &&
                      !shot->taTailMoving,
                  "it moves in its fire tick's pass: head one step out, tail at the muzzle",
                  "head " + pt(shot->position) + " muzzle " + pt(muzzle));
            const double speed = std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]) / 65536.0;
            check(std::abs(speed - 900.0 / 30.0) < 0.5, "at weaponvelocity, 900 px/s = 30 px a tick",
                  std::to_string(speed));
            // Where the head first meets the target body, flown straight.
            int contact = 0;
            for (int k = 1; k < 40 && !contact; ++k) {
                int32_t vy = v[1];
                if (w.projectileCollision(add(muzzle, v, k), vy, 0, 0, {}).unitId == target) contact = k;
            }
            const double dist = std::sqrt(std::pow((aim[0] - muzzle[0]) / 65536.0, 2) +
                                          std::pow((aim[1] - muzzle[1]) / 65536.0, 2) +
                                          std::pow((aim[2] - muzzle[2]) / 65536.0, 2));
            char d[160];
            std::snprintf(d, sizeof d, "contact after %d steps, %.0f px to the aim point = %.1f steps", contact,
                          dist, dist / speed);
            check(contact > 0 && contact <= int(std::ceil(dist / speed)) && contact * speed > dist - 64.0,
                  "the head reaches the target body after about distance / velocity ticks", d);
            bool tailOk = true, before = true;
            std::string why;
            int passes = 1, hitPass = 0;
            const sim::Unit* tu = w.unit(target);
            const float hp0 = tu ? tu->hp.toFloat() : 0.0f;
            for (; passes < 60; ) {
                p.tick();
                ++passes;
                const sim::Unit* t2 = w.unit(target);
                const float hp = t2 ? t2->hp.toFloat() : 0.0f;
                const auto* s = shotOf(w, shooter, launch);
                if (!s) {
                    hitPass = passes;
                    // A closed solar's script armours it (damage x 0.3).
                    check(hp < hp0, "the target is hurt on the contact tick",
                          std::to_string(hp0) + " -> " + std::to_string(hp) + ", damage " + std::to_string(damage));
                    break;
                }
                if (hp < hp0) before = false;
                // Pass 2 sets the tail bit (tick > launch + 0), pass 3 moves it.
                const auto wantTail = add(muzzle, v, std::max(0, passes - 2));
                if (s->position != add(muzzle, v, passes) || s->taTail != wantTail || s->taTailMoving != (passes >= 2)) {
                    tailOk = false;
                    why = "pass " + std::to_string(passes) + " head " + pt(s->position) + " tail " + pt(s->taTail);
                }
            }
            check(before, "no damage before the head arrives");
            check(tailOk, "the tail stays at the muzzle two passes, then follows the head two steps behind", why);
            check(hitPass == contact, "the shot bursts on the pass its head enters the body",
                  "burst on pass " + std::to_string(hitPass) + ", contact " + std::to_string(contact));
        }
        for (int t = 0; t < 30; ++t) p.tick();
        check(p.paired, "paired Worlds keep the same state hash");
    }

    std::printf("[what stops a beam: 0x49b090 tests the head]\n");
    // The head runs about 16 px over the ground at the middle body: an enemy
    // energy storage (37 px tall) stops it, a closed solar (13 px) lets it
    // over, and an own storage lets it through (0x49b1f4).
    struct Blocker { const char* type; int player; bool stops; const char* what; };
    static const Blocker blockers[] = {
        {"corestor", 1, true, "an enemy storage in the line takes the first laser, not the target behind it"},
        {"corsolar", 1, false, "a closed enemy solar (13 px tall) under the line lets it over (0x49b20d: below its top)"},
        {"armestor", 0, false, "an own storage in the line lets it through (0x49b1f4: the owner's bodies pass)"},
    };
    for (const auto& b : blockers) {
        sim::World w;
        setup(w, reg, vfs, map);
        const int shooter = w.spawn(llt, x, z, 0.0f, 0);
        const int body = w.spawn(reg.find(b.type), x + 110, z, 0.0f, b.player);
        const int target = w.spawn(solar, x + 240, z, 0.0f, 1);
        w.attack(shooter, target, false);
        const float bodyHp = w.unit(body)->hp.toFloat(), targetHp = w.unit(target)->hp.toFloat();
        uint32_t launch = 0;
        for (int t = 0; t < 300 && !launch; ++t) {
            w.tick(1.0f / 30.0f);
            if (const auto* s = newShot(w, shooter)) launch = s->taLaunch;
        }
        for (int t = 0; t < 40 && shotOf(w, shooter, launch); ++t) w.tick(1.0f / 30.0f);
        const float bodyNow = w.unit(body) ? w.unit(body)->hp.toFloat() : 0.0f;
        const float targetNow = w.unit(target) ? w.unit(target)->hp.toFloat() : 0.0f;
        const std::string detail = "body " + std::to_string(bodyHp) + " -> " + std::to_string(bodyNow) +
                                   ", target " + std::to_string(targetHp) + " -> " + std::to_string(targetNow);
        check(launch && (b.stops ? bodyNow < bodyHp && targetNow == targetHp
                                 : bodyNow == bodyHp && targetNow < targetHp), b.what, detail);
    }
    {
        sim::World w;
        setup(w, reg, vfs, map);
        const int shooter = w.spawn(llt, x, z, 0.0f, 0);
        for (int t = 0; t < 5; ++t) w.tick(1.0f / 30.0f);
        // A ground point 160 px east, at the terrain's height (the point's
        // own whole px: 0x49c9c0 aims the head straight at it).
        int32_t ground = 0;
        for (int h = 255; h >= 0; --h) {
            int32_t vy = 0;
            if (w.projectileEnvironment({F(int(x) + 160), F(h), F(int(z))}, vy, 0) == 2) { ground = h; break; }
        }
        const std::array<int32_t,3> point{F(int(x) + 160), F(ground), F(int(z))};
        const bool fired = sim::RetailReplayProbe::fire(w, shooter, sim::World::WeaponTarget{nullptr, point}, 0);
        const sim::Projectile* s = nullptr;
        for (const auto& q : w.projectiles()) if (q.fromId == shooter && q.taLineOfSight) s = &q;
        check(fired && s, "a shot at a ground point launches");
        if (s) {
            const uint32_t launch = s->taLaunch, end = s->end;
            const auto vel = s->velocity;
            std::array<int32_t,3> last = s->position;
            int passes = 0;
            while (const auto* q = shotOf(w, shooter, launch)) {
                last = q->position;
                w.tick(1.0f / 30.0f);
                ++passes;
                if (passes > 60) break;
            }
            // The burst point: one more step from the last head seen.
            const auto burst = add(last, vel, 1);
            int32_t vy = 0, vy2 = 0;
            const double dx = (burst[0] - point[0]) / 65536.0;
            check(w.tickCount() < end && passes < 60,
                  "it bursts on the ground before its expiry (0x49bd86 -> 0x49b090 -> 0x499eb0)",
                  std::to_string(passes) + " passes, burst at " + pt(burst) + ", aim " + pt(point));
            check(std::abs(dx) <= 32.0 && w.projectileEnvironment(last, vy, 0) == 0 &&
                      w.projectileEnvironment(burst, vy2, 0) == 2,
                  "at the first step its head is in the ground, near the aim point", "dx " + std::to_string(dx));
        }
    }

    std::printf("[a Zeus's lightning: duration 10 s]\n");
    {
        sim::World w;
        setup(w, reg, vfs, map);
        const int shooter = w.spawn(zeus, x, z, 0.0f, 0);
        const int target = w.spawn(solar, x + 150, z, 0.0f, 1);
        w.attack(shooter, target, false);
        const sim::Projectile* s = nullptr;
        for (int t = 0; t < 600 && !s; ++t) {
            w.tick(1.0f / 30.0f);
            s = newShot(w, shooter);
        }
        check(s && s->wsrc->renderType == 7 && s->wsrc->durationTicks16 == 300, "armzeus fires LIGHTNING (rendertype 7)");
        bool still = s != nullptr;
        if (s) {
            const uint32_t launch = s->taLaunch;
            const auto muzzle = s->muzzle;
            for (int t = 0; t < 30; ++t) {
                const auto* q = shotOf(w, shooter, launch);
                if (!q) break;
                still = still && q->taTail == muzzle && !q->taTailMoving;
                w.tick(1.0f / 30.0f);
            }
        }
        check(still, "its tail stays at the muzzle for the whole flight: the bolt joins the Zeus to the head");
    }

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
