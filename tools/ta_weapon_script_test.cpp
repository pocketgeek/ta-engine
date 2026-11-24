// ta_weapon_script_test: TA units fire through TotalA.exe's weapon call-ins.
//
//   ta_weapon_script_test <retail-install-dir>
//
// Real units on Ashap Plateau (docs/retail-weapon-scripts.md):
//   1. armpw, armstump, armllt, armcom, armham and armwar, ordered to attack an
//      enemy in range, fire and hurt it (AimX -> return -> fire gate -> FireX).
//   2. The sign check: the heading a turret's AimPrimary gets, added to the
//      body heading, points from the turret at its target in the engine's
//      own movement convention (a body with heading h steps along (-sin, -cos));
//      the target is put straight ahead, behind and to either side.
//   3. A unit on a plain Move past an enemy keeps moving, fires on the move,
//      and arrives (Move never stops for an enemy; the fire-at-will sweep).
//   4. A burst weapon (armpw's EMG, burst 3, burstrate 0.1 s) releases its
//      burst burstrate apart, starting burstrate after the shot.
//   5. The reload is (ticks*(100-6k)/100)*(120-h)/100 (0x49e468): full health,
//      half health and a 10-kill veteran.
//   6. Paired Worlds stay hash-identical through all of it.
//
// A dev harness; not shipped in a game.

#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/retailmotion.h"
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

// The top-left cell of a size x size block of cells where `t` fits everywhere.
bool openApron(const sim::World& w, const sim::UnitType* t, int size, float& x, float& z,
               int fromX = 8, int fromZ = 8) {
    const sim::NavGrid& g = w.navFor(t);
    const int foot = std::max(1, std::max(t->footX, t->footZ));
    for (int cz = fromZ; cz + size < g.height() - 8; cz += 2)
        for (int cx = fromX; cx + size < g.width() - 8; cx += 2) {
            bool clear = true;
            for (int dz = 0; dz <= size && clear; ++dz)
                for (int dx = 0; dx <= size && clear; ++dx)
                    if (!g.fits(cx + dx, cz + dz, foot)) clear = false;
            if (!clear) continue;
            x = float(cx) * 16 + 8;
            z = float(cz) * 16 + 8;
            return true;
        }
    return false;
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

constexpr double kTau = 6.283185307179586;
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ta_weapon_script_test <retail-install-dir>\n");
        return 2;
    }
    hpi::Vfs vfs = hpi::mountRetailRoot(argv[1]);
    sim::TypeRegistry reg;
    sim::setupRegistry(reg, vfs);
    const std::string map = hpi::findMap(vfs, "Ashap Plateau");
    const sim::UnitType* dummy = reg.find("corsolar");   // a target that never shoots back
    if (!dummy || map.empty()) {
        std::printf("missing corsolar or Ashap Plateau\n");
        return 1;
    }

    // ---- 1. Ordered attacks fire ---------------------------------------
    std::printf("[ordered attacks fire]\n");
    static const bool trace = std::getenv("TA_WEAPON_TRACE") != nullptr;
    // TA_WEAPON_ONLY=<unit>: only that unit's ordered attack (debugging).
    const char* only = std::getenv("TA_WEAPON_ONLY");
    for (const char* name : {"armpw", "armstump", "armllt", "armcom", "armham", "armwar"}) {
        if (only && std::string(only) != name) continue;
        const sim::UnitType* t = reg.find(name);
        if (!t || !t->script() || t->weapons.empty()) {
            check(false, std::string(name) + " loads with a script and a weapon");
            continue;
        }
        Pair p;
        setup(p, reg, vfs, map);
        float x = 0, z = 0;
        if (!openApron(p.a, t, 24, x, z, 20, 20)) { check(false, std::string(name) + ": open ground"); continue; }
        const float range = float(t->weapons[0].range);
        const float gap = std::min(range * 0.6f, 180.0f);
        int ids[2][2];
        int k = 0;
        for (auto* w : {&p.a, &p.b}) {
            ids[k][0] = w->spawn(t, x, z, 0.0f, 0);
            ids[k][1] = w->spawn(dummy, x + gap, z + 40, 0.0f, 1);
            w->attack(ids[k][0], ids[k][1], false);
            ++k;
        }
        const float hp0 = p.a.unit(ids[0][1])->hp.toFloat();
        // Every slot an attack order works (a command-fire weapon, the
        // commander's D-gun, fires only on its own order) must fire.
        const size_t slots = std::min<size_t>(3, t->weapons.size());
        int firstShot = -1, shots = 0;
        int slotShots[3] = {}, last[3] = {};
        for (int tick = 1; tick <= 450; ++tick) {
            p.tick();
            const sim::Unit* u = p.a.unit(ids[0][0]);
            if (!u) break;
            for (size_t s = 0; s < slots; ++s) {
                const int now = u->reloads[s];
                if (now > last[s]) {
                    ++shots;
                    ++slotShots[s];
                    if (firstShot < 0) firstShot = tick;
                }
                last[s] = now;
                if (trace && tick < 200)
                    std::printf("    t%d s%zu flags %u h %u p %d reload %d\n", tick, s, unsigned(u->weaponAim[s].flags),
                                unsigned(u->weaponAim[s].heading), int(int16_t(u->weaponAim[s].pitch)), now);
            }
        }
        // Per slot, for the record. A slot can legitimately stay silent: the
        // D-gun (command fire) killed the target first, or another slot's
        // AimX signalled its aim thread away (armwar's AimSecondary sends
        // AimPrimary's signal 4; a killed aim never reports, and retail then
        // waits for the target to go, docs/retail-weapon-scripts.md 2.4).
        std::string perSlot;
        for (size_t s = 0; s < slots; ++s)
            perSlot += t->weapons[s].section + "=" + std::to_string(slotShots[s]) + " ";
        std::printf("    %s shots per slot: %s\n", name, perSlot.c_str());
        const sim::Unit* target = p.a.unit(ids[0][1]);
        const float hp1 = target && target->alive() ? target->hp.toFloat() : 0.0f;
        char detail[160];
        std::snprintf(detail, sizeof detail, "first shot tick %d, %d shots in 15 s, target hp %.0f -> %.0f",
                      firstShot, shots, hp0, hp1);
        check(firstShot > 0 && firstShot < 150 && (shots >= 2 || hp1 <= 0), std::string(name) + " fires at an ordered target",
              detail);
        check(hp1 < hp0, std::string(name) + "'s shots hurt the target", detail);
        check(p.paired, std::string(name) + ": paired Worlds keep the same state hash");
    }

    // The commander's D-gun (command fire) wins an ordered attack outright;
    // its laser is the one the fire-at-will sweep uses for a human owner.
    if (!only || std::string(only) == "armcom") {
        const sim::UnitType* com = reg.find("armcom");
        const sim::UnitType* radar = reg.find("corrad");
        Pair p;
        setup(p, reg, vfs, map);
        float x = 0, z = 0;
        openApron(p.a, com, 24, x, z, 20, 20);
        int ids[2][2];
        int k = 0;
        for (auto* w : {&p.a, &p.b}) {
            ids[k][0] = w->spawn(com, x, z, 0.0f, 0);
            ids[k][1] = w->spawn(radar, x + 120, z + 60, 0.0f, 1);
            ++k;
        }
        int laser = 0, dgun = 0, last[2] = {};
        for (int tick = 1; tick <= 300; ++tick) {
            p.tick();
            const sim::Unit* u = p.a.unit(ids[0][0]);
            if (u->reloads[0] > last[0]) ++laser;
            if (u->reloads[1] > last[1]) ++dgun;
            last[0] = u->reloads[0];
            last[1] = u->reloads[1];
        }
        check(laser > 0 && dgun == 0,
              "an idle armcom's laser (AimPrimary/FirePrimary) fires at will, its D-gun does not",
              std::to_string(laser) + " laser, " + std::to_string(dgun) + " D-gun shots");
        check(p.paired, "paired Worlds keep the same state hash");
    }
    if (only && std::string(only) != "move") return failures ? 1 : 0;

    // ---- 2. The sign check ---------------------------------------------
    // armllt (a turret on a structure, body heading fixed) and armstump (a
    // turret on a tank). First anchor the engine's heading convention to its
    // own mover: a tank with retail heading h steps along (-sin h, -cos h).
    std::printf("[AimPrimary heading points at the target]\n");
    {
        const sim::UnitType* tank = reg.find("armstump");
        Pair p;
        setup(p, reg, vfs, map);
        float x = 0, z = 0;
        openApron(p.a, tank, 30, x, z, 20, 20);
        const int id = p.a.spawn(tank, x + 120, z + 120, 0.0f, 0);
        p.b.spawn(tank, x + 120, z + 120, 0.0f, 0);
        const auto* u0 = p.a.unit(id);
        const float sx = u0->x.toFloat(), sz = u0->z.toFloat();
        p.a.order(id, x + 120 + 200, z + 120 - 150, false);
        p.b.order(id, x + 120 + 200, z + 120 - 150, false);
        for (int i = 0; i < 90; ++i) p.tick();
        const auto* u = p.a.unit(id);
        const double h = double(sim::portHeadingToRetail(u->heading)) * kTau / 65536.0;
        const double mx = u->x.toFloat() - sx, mz = u->z.toFloat() - sz;
        const double len = std::hypot(mx, mz);
        const double dot = len > 1 ? (-std::sin(h) * mx - std::cos(h) * mz) / len : 0;
        char detail[120];
        std::snprintf(detail, sizeof detail, "moved %.0f px, cos(angle to (-sin h, -cos h)) = %.3f", len, dot);
        check(dot > 0.9, "a body with retail heading h moves along (-sin h, -cos h)", detail);
    }
    for (const char* name : {"armllt", "armstump"}) {
        const sim::UnitType* t = reg.find(name);
        for (int side = 0; side < 4; ++side) {
            Pair p;
            setup(p, reg, vfs, map);
            float x = 0, z = 0;
            openApron(p.a, t, 30, x, z, 20, 20);
            x += 240; z += 240;
            // Body heading 0 (port): retail heading 0x8000, which faces +z.
            // Targets ahead, to the +x side, behind and to the -x side.
            const float d = 150;
            const float ox[4] = {0, d, 0, -d}, oz[4] = {d, 0, -d, 0};
            int ids[2][2];
            int k = 0;
            for (auto* w : {&p.a, &p.b}) {
                ids[k][0] = w->spawn(t, x, z, 0.0f, 0);
                ids[k][1] = w->spawn(dummy, x + ox[side], z + oz[side], 0.0f, 1);
                if (auto* u = w->unit(ids[k][0])) u->moveState = 0;   // hold position: no chase
                w->attack(ids[k][0], ids[k][1], false);
                ++k;
            }
            bool aimed = false;
            double best = -2;
            uint16_t heading = 0, pitch = 0;
            for (int tick = 0; tick < 60 && !aimed; ++tick) {
                p.tick();
                const sim::Unit* u = p.a.unit(ids[0][0]);
                if (u->weaponAim[0].requested()) {
                    aimed = true;
                    heading = u->weaponAim[0].heading;
                    pitch = u->weaponAim[0].pitch;
                    const sim::Unit* tg = p.a.unit(ids[0][1]);
                    const double h = double(uint16_t(heading + sim::portHeadingToRetail(u->heading))) * kTau / 65536.0;
                    const double vx = tg->x.toFloat() - u->x.toFloat(), vz = tg->z.toFloat() - u->z.toFloat();
                    best = (-std::sin(h) * vx - std::cos(h) * vz) / std::hypot(vx, vz);
                }
            }
            static const char* kSide[4] = {"ahead", "to the +x side", "behind", "to the -x side"};
            char detail[160];
            std::snprintf(detail, sizeof detail, "heading %u pitch %d, cos(aim, target) = %.3f", unsigned(heading),
                          int(int16_t(pitch)), best);
            check(aimed && best > 0.97, std::string(name) + ": AimPrimary heading points at a target " + kSide[side],
                  detail);
            check(p.paired, std::string(name) + ": paired Worlds keep the same state hash");
        }
    }

    // ---- 3. Fire on the move ---------------------------------------------
    std::printf("[Move past an enemy: keeps moving, fires on the move]\n");
    {
        const sim::UnitType* tank = reg.find("armstump");
        const sim::UnitType* radar = reg.find("corrad");
        check(radar && radar->shootMe, "corrad loads with ShootMe");
        Pair p;
        setup(p, reg, vfs, map);
        float x = 0, z = 0;
        const bool open = openApron(p.a, tank, 32, x, z, 20, 20);
        check(open, "open ground for the drive");
        const float sx = x + 24, sz = z + 24, gx = x + 488, gz = z + 24;
        int ids[2][2];
        int k = 0;
        for (auto* w : {&p.a, &p.b}) {
            ids[k][0] = w->spawn(tank, sx, sz, 0.0f, 0);
            // 160 px off the path. A radar tower: unarmed, and ShootMe=1, which
            // a human owner's fire-at-will picker requires (retailSelectTarget).
            ids[k][1] = w->spawn(radar, x + 256, z + 184, 0.0f, 1);
            w->order(ids[k][0], gx, gz, false);
            ++k;
        }
        const float hp0 = p.a.unit(ids[0][1])->hp.toFloat();
        int shots = 0, stoppedWhileMoving = 0, last = 0, arrived = -1;
        bool movingShot = false;
        for (int tick = 1; tick <= 900 && arrived < 0; ++tick) {
            p.tick();
            const sim::Unit* u = p.a.unit(ids[0][0]);
            const int now = u->reloads[0];
            const bool moving = u->speed > sim::Fixed();
            if (now > last) {
                ++shots;
                movingShot = movingShot || moving;
            }
            last = now;
            if (tick > 30 && !u->orders.empty() && !moving) ++stoppedWhileMoving;
            if (trace && tick % 30 == 0)
                std::printf("    t%d pos %.0f,%.0f speed %.2f orders %zu slotTarget %d flags %u reload %d\n", tick,
                            u->x.toFloat(), u->z.toFloat(), u->speed.toFloat(), u->orders.size(), u->slotTarget[0],
                            unsigned(u->weaponAim[0].flags), u->reloads[0]);
            if (u->orders.empty()) arrived = tick;
            if (!u->orders.empty() && u->orders.front().targetId) {
                check(false, "the Move gains no attack order ahead of it");
                break;
            }
        }
        const sim::Unit* u = p.a.unit(ids[0][0]);
        const sim::Unit* target = p.a.unit(ids[0][1]);
        const float hp1 = target && target->alive() ? target->hp.toFloat() : 0.0f;
        const double off = std::hypot(u->x.toFloat() - gx, u->z.toFloat() - gz);
        char detail[200];
        std::snprintf(detail, sizeof detail, "%d shots, %s, hp %.0f -> %.0f, stopped %d ticks, arrived tick %d %.0f px off",
                      shots, movingShot ? "fired while moving" : "never fired while moving", hp0, hp1,
                      stoppedWhileMoving, arrived, off);
        check(shots > 0 && movingShot, "the tank fires while it drives past", detail);
        check(hp1 < hp0, "and its shots hurt the enemy", detail);
        check(stoppedWhileMoving < 10, "it never stops to fight", detail);
        check(arrived > 0 && off < 64, "it arrives", detail);
        check(p.paired, "paired Worlds keep the same state hash");
    }

    // ---- 4. Burst ----------------------------------------------------------
    std::printf("[burst]\n");
    {
        const sim::UnitType* pw = reg.find("armpw");
        const auto& emg = pw->weapons[0];
        check(emg.burst == 3 && emg.burstRateTicks == 3, "armpw's EMG has burst 3, burstrate 3 ticks",
              std::to_string(emg.burst) + " / " + std::to_string(emg.burstRateTicks));
        Pair p;
        setup(p, reg, vfs, map);
        float x = 0, z = 0;
        openApron(p.a, pw, 20, x, z, 20, 20);
        int ids[2][2];
        int k = 0;
        for (auto* w : {&p.a, &p.b}) {
            ids[k][0] = w->spawn(pw, x, z, 0.0f, 0);
            ids[k][1] = w->spawn(dummy, x + 100, z + 30, 0.0f, 1);
            w->attack(ids[k][0], ids[k][1], false);
            ++k;
        }
        int fireTick = -1, last = 0;
        uint32_t seen = 0;
        std::vector<int> shotTicks;
        for (int tick = 1; tick <= 200; ++tick) {
            p.tick();
            const sim::Unit* u = p.a.unit(ids[0][0]);
            if (fireTick < 0 && u->reloads[0] > last) fireTick = tick;
            last = u->reloads[0];
            uint32_t newest = seen;
            for (const auto& shot : p.a.projectiles())
                if (shot.fromId == ids[0][0] && shot.start > seen) {
                    shotTicks.push_back(tick);
                    newest = std::max(newest, shot.start);
                }
            seen = newest;
            if (fireTick > 0 && tick > fireTick + 11) break;   // within one 12-tick reload
        }
        std::string list;
        for (int t : shotTicks) list += std::to_string(t - fireTick) + " ";
        check(fireTick > 0 && shotTicks.size() == 3, "one shot releases exactly 3 rounds before the next shot",
              "rounds at fire+ " + list);
        check(shotTicks.size() == 3 && shotTicks[0] - fireTick == 3 && shotTicks[1] - fireTick == 6 &&
                  shotTicks[2] - fireTick == 9,
              "the rounds leave burstrate apart, the first burstrate after the shot", "rounds at fire+ " + list);
        check(p.paired, "paired Worlds keep the same state hash");
    }

    // ---- 5. Reload ---------------------------------------------------------
    std::printf("[reload formula]\n");
    {
        const sim::UnitType* tank = reg.find("armstump");
        const int ticks = tank->weapons[0].reloadTicks;
        struct Case { const char* what; float hpFrac; int kills; };
        for (const Case c : {Case{"full health", 1.0f, 0}, Case{"half health", 0.5f, 0}, Case{"10 kills", 1.0f, 10}}) {
            Pair p;
            setup(p, reg, vfs, map);
            float x = 0, z = 0;
            openApron(p.a, tank, 24, x, z, 20, 20);
            int ids[2][2];
            int k = 0;
            for (auto* w : {&p.a, &p.b}) {
                ids[k][0] = w->spawn(tank, x, z, 0.0f, 0);
                ids[k][1] = w->spawn(dummy, x + 150, z + 30, 0.0f, 1);
                auto* u = w->unit(ids[k][0]);
                u->hp = sim::Fixed::fromFloat(float(u->maximumHp()) * c.hpFrac);
                u->kills = uint16_t(c.kills);
                w->attack(ids[k][0], ids[k][1], false);
                ++k;
            }
            int got = -1, last = 0;
            for (int tick = 1; tick <= 300 && got < 0; ++tick) {
                p.tick();
                const sim::Unit* u = p.a.unit(ids[0][0]);
                if (u->reloads[0] > last) got = u->reloads[0];
                last = u->reloads[0];
            }
            const sim::Unit* u = p.a.unit(ids[0][0]);
            const int kk = std::min(c.kills / 5, 5);
            const int h = int(int64_t(std::max(0, u->hp.floorInt())) * 20 / std::max(1, u->maximumHp()));
            const int want = (ticks * (100 - 6 * kk) / 100) * (120 - h) / 100;
            check(got == want, std::string("reload at ") + c.what,
                  "got " + std::to_string(got) + ", want " + std::to_string(want));
            check(p.paired, "paired Worlds keep the same state hash");
        }
    }

    if (failures) {
        std::printf("FAILED (%d failures)\n", failures);
        return 1;
    }
    std::printf("PASSED\n");
    return 0;
}
