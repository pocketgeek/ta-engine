// sweetspot_test: a shooter aims at its target's drawn SweetSpot.
//
//   sweetspot_test <retail-install-dir>
//
// Retail's SweetSpot (4dd4f0 -> 4dd2a0) bounds the piece's vertex buffer that
// the draw path rewrites in place (4ee620 -> 4ee920/4eea20: piece chain plus
// body pitch/heading/roll), so a drawn Zhon Hunter's SweetSpot sits at its
// torso's middle, about 19 px up. The simulation once bounded the authored
// (local) torso vertices instead, which put the aim point 1.24 px below the
// feet: Hunters lobbed every spear into the ground in front of each other
// (a 0.9.0 game: 727 Hunter-on-Hunter shots, 0 hits).
//
// Checks, in both balances, on shipped zonter:
// - the rest-pose SweetSpot offset against four drawn Hunters emulated from a
//   live retail capture (4dd4f0 on next-device-state-01, tick 11134);
// - two Hunters on flat ground in that game's geometry (dx -272, dz +192,
//   333 px): the shooter's spears hit its standing target.
// - a Thirsha's lightning at a parked Creon Submersible in two setups of a
//   0.9.0 game: it hits where retail's collision does with the drawn aim
//   point (sub 328) and misses where retail misses (sub 106).
//
// A dev harness; not shipped in a game.

#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace tak;

namespace {
int failures = 0;

void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++failures;
}

void flatGround(sim::World& w) {
    w.setTerrain(std::vector<uint8_t>(64 * 64, 62), 64, 64, 0);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: sweetspot_test <retail-install-dir>\n"); return 2; }
    const hpi::Vfs vfs = hpi::mountRetailRoot(argv[1]);
    for (const bool crusades : {false, true}) {
        std::printf("[%s balance]\n", crusades ? "Crusades" : "Original");
        sim::TypeRegistry reg;
        sim::setupRegistry(reg, vfs, crusades);
        const sim::UnitType* hunter = reg.find("zonter");
        if (!hunter) { check(false, "shipped zonter loads"); continue; }

        // Retail heading -> SweetSpot offset (px) of four drawn Hunters at rest.
        struct Captured { uint16_t heading; float x, y, z; };
        const Captured captured[] = {{8192, 1.54f, 18.99f, -1.55f}, {40960, -1.54f, 18.99f, 1.55f},
                                     {57508, -1.55f, 18.99f, -1.53f}, {42005, -1.51f, 18.99f, 1.56f}};
        {
            sim::World w;
            flatGround(w);
            const int id = w.spawn(hunter, 512, 512, 0, 0);
            w.tick(1.f / 30);
            for (const auto& c : captured) {
                sim::Unit* u = w.unit(id);
                u->heading = sim::Bam(uint16_t(c.heading + 32768));   // portHeadingToRetail inverse
                const auto point = w.queryUnitScriptPoint(id, true);
                const float x = float(point[0] - u->x.v) / 65536.f, y = float(point[1] - u->groundY.v) / 65536.f,
                            z = float(point[2] - u->z.v) / 65536.f;
                char buf[160];
                std::snprintf(buf, sizeof buf, "heading %u: (%.2f, %.2f, %.2f), retail (%.2f, %.2f, %.2f)",
                              unsigned(c.heading), x, y, z, c.x, c.y, c.z);
                check(std::fabs(x - c.x) < 0.03f && std::fabs(y - c.y) < 0.03f && std::fabs(z - c.z) < 0.03f,
                      "zonter SweetSpot is the drawn torso bounds centre", buf);
            }
        }

        // The 0.9.0 pair: shooter 213 at rest, target 162 at dx -272, dz +192.
        {
            sim::World w;
            flatGround(w);
            const float sx = 700, sz = 300;
            const int shooter = w.spawn(hunter, sx, sz, 0, 0);
            const int target = w.spawn(hunter, sx - 272, sz + 192, 0, 1);
            w.setStance(target, 2);
            w.unit(target)->moveState = 0;
            w.unit(target)->fireState = 0;
            w.unit(target)->hp = sim::Fixed::fromInt(30000);
            w.unit(shooter)->moveState = 0;
            for (int i = 0; i < 30; ++i) w.tick(1.f / 30);
            w.attack(shooter, target, false);
            int shots = 0, hits = 0;
            for (int i = 0; i < 45 * 30; ++i) {
                w.tick(1.f / 30);
                for (const auto& h : w.hits()) {
                    if (h.fromPlayer != 0) continue;
                    ++shots;
                    hits += h.victimId == target;
                }
            }
            const auto* t = w.unit(target);
            const auto* s = w.unit(shooter);
            const float moved = s ? std::hypot(s->x.toFloat() - sx, s->z.toFloat() - sz) : -1.f;
            char buf[160];
            std::snprintf(buf, sizeof buf, "impacts=%d hits=%d damage=%.0f shooter moved %.0f px", shots, hits,
                          t ? 30000.f - t->hp.toFloat() : -1.f, moved);
            check(shots >= 8 && hits * 10 >= shots * 8, "a Hunter's spears hit a standing Hunter at 333 px", buf);
        }

        // A Thirsha's lightning at a parked Creon Submersible, in two setups from
        // a 0.9.0 game (47 bolts, 0 hits). The sub's SweetSpot sits 20.3 px out
        // from its centre, past its 2x2 cells, and the drawn offset turns with
        // its heading. Retail's 52a4d0 run on the game's own 47 bolts with the
        // drawn aim point hits sub 328 (heading 14493: 7 of 7) and still misses
        // sub 106 (heading 4616: 0 of 20). Ours hits both (engine deviation,
        // protocol 325): a SweetSpot outside the footprint is moved over the
        // body's centre, so the bolts reach it whichever way the bow points.
        {
            const sim::UnitType* thirsha = reg.find("zonhunt");
            const sim::UnitType* subType = reg.find("cresubm");
            if (!thirsha || !subType) { check(false, "shipped zonhunt and cresubm load"); continue; }
            auto sub = *subType;
            sub.maxHp = 10000000;
            sub.healTime = 0;
            sub.weapons.clear();
            sub.weapon.damage = 0;
            struct Setup { const char* what; float sx, sz; uint16_t heading; float mx, mz; bool hits; };
            const Setup setups[] = {
                {"lightning hits sub 328 at heading 14493, as retail", 80, 2192, 14493, 197.10f, 2156.04f, true},
                {"lightning hits sub 106 at heading 4616 (retail misses)", 400.1f, 2032.28f, 4616, 516.42f, 2063.27f, true}};
            for (const auto& s : setups) {
                sim::World w;
                w.setTerrain(std::vector<uint8_t>(64 * 192, 0), 64, 192, 58);
                w.buildNavClasses(reg);
                const int target = w.spawn(&sub, s.sx, s.sz, 0, 1);
                w.tick(1.f / 30);
                w.unit(target)->heading = sim::Bam(s.heading);
                const float dx = s.mx - s.sx, dz = s.mz - s.sz, len = std::hypot(dx, dz);
                const int shooter = w.spawn(thirsha, s.sx + 300 * dx / len, s.sz + 300 * dz / len, 0, 0);
                w.attack(shooter, target, false);
                int bolts = 0, hits = 0, firstBoltHits = -1;
                size_t live = 0;
                sim::Fixed hp = w.unit(target)->hp;
                for (int i = 0; i < 40 * 30; ++i) {
                    w.tick(1.f / 30);
                    size_t n = 0;
                    for (const auto& shot : w.projectiles()) n += shot.fromId == shooter;
                    if (bolts < 2 && bolts + int(n > live ? n - live : 0) >= 2) firstBoltHits = hits;
                    if (n > live) bolts += int(n - live);
                    live = n;
                    if (w.unit(target)->hp < hp) { ++hits; hp = w.unit(target)->hp; }
                    w.unit(target)->heading = sim::Bam(s.heading);   // parked: hold the recorded heading
                }
                char buf[96];
                std::snprintf(buf, sizeof buf, "hits %d of %d bolts, the first %s", hits, bolts,
                              firstBoltHits > 0 ? "hit" : "missed");
                check(bolts >= 8 && (s.hits ? hits * 2 >= bolts : firstBoltHits == 0), s.what, buf);
            }
        }
    }
    std::printf(failures ? "FAIL: %d checks\n" : "PASS: drawn SweetSpot aim\n", failures);
    return failures ? 1 : 0;
}
