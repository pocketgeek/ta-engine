// sweetspot_test: a shooter aims at its target's drawn SweetSpot.
//
//   sweetspot_test <retail-install-dir>
//
// Retail's SweetSpot bounds the piece's vertex buffer that the draw path
// rewrites in place (piece chain plus body pitch/heading/roll), so a drawn
// unit's SweetSpot sits at the middle of its scripted piece, above its feet.
// The simulation once bounded the authored (local) vertices instead, which put
// the aim point below the feet, and ballistic shooters lobbed every shot into
// the ground in front of their target. (The addresses and the live capture
// this was first measured against were KINGDOMS.icd's; unverified for TA.)
//
// Checks on a shipped Hammer (armham, ballistic Plasma Cannon, range 320):
// - its SweetSpot stands well above its feet at four headings, and turns
//   with the body (its horizontal offset rotates, its height does not change);
// - two Hammers on flat ground 274 px apart (dx -224, dz +158): the
//   shooter's shells hit its standing target.
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

using namespace ta;

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
    {
        sim::TypeRegistry reg;
        sim::setupRegistry(reg,vfs);
        const sim::UnitType* hunter = reg.find("armham");
        if (!hunter) { check(false, "shipped armham loads"); return 1; }

        // The SweetSpot offset (px) of a Hammer at rest, at four headings.
        {
            sim::World w;
            flatGround(w);
            const int id = w.spawn(hunter, 512, 512, 0, 0);
            w.tick(1.f / 30);
            float firstY = -1.f, firstR = -1.f;
            for (const uint16_t heading : {uint16_t(8192), uint16_t(40960), uint16_t(57508), uint16_t(42005)}) {
                sim::Unit* u = w.unit(id);
                u->heading = sim::Bam(uint16_t(heading + 32768));
                const auto point = w.queryUnitScriptPoint(id, true);
                const float x = float(point[0] - u->x.v) / 65536.f, y = float(point[1] - u->groundY.v) / 65536.f,
                            z = float(point[2] - u->z.v) / 65536.f;
                const float r = std::hypot(x, z);
                if (firstY < 0) { firstY = y; firstR = r; }
                char buf[160];
                std::snprintf(buf, sizeof buf, "heading %u: (%.2f, %.2f, %.2f)", unsigned(heading), x, y, z);
                check(y > 4.f, "armham SweetSpot stands above its feet", buf);
                check(std::fabs(y - firstY) < 0.05f && std::fabs(r - firstR) < 0.05f,
                      "armham SweetSpot turns with the body", buf);
            }
        }

        // The pair: shooter at rest, target at dx -224, dz +158 (274 px).
        {
            sim::World w;
            flatGround(w);
            const float sx = 700, sz = 300;
            const int shooter = w.spawn(hunter, sx, sz, 0, 0);
            const int target = w.spawn(hunter, sx - 224, sz + 158, 0, 1);
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
            check(shots >= 8 && hits * 10 >= shots * 8, "a Hammer's shells hit a standing Hammer at 274 px", buf);
        }
    }
    std::printf(failures ? "FAIL: %d checks\n" : "PASS: drawn SweetSpot aim\n", failures);
    return failures ? 1 : 0;
}
