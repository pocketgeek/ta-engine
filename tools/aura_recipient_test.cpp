// aura_recipient_test -- the stat auras of a big world, folded per recipient tile
// by tile (World::tickAuras from kPostMinParallel units, inline or on the
// SimPool), give the serial emitter walk's buffs, bit for bit.
//
// The serial walk takes each emitter in unit order and each of its auras in type
// order, and folds a max (buff) or a min (debuff) into every unit the aura's
// forEachNear square reaches. The order matters: a unit under a buff and a debuff
// of the same stat ends on whichever comes last. Checked here, with no workers
// and with 1, 2, 3, 7, 23 and 63 (the tiles are the one pass whose chunk size
// follows the width, so the widths here give it several: World::applyAuraTiles):
//   * order: one unit under a buff and a debuff of the same stat, from emitters
//     in both unit orders -- the result follows the emitter order exactly;
//   * eligibility: dead, embarked, unbuilt, frozen and stoned emitters emit
//     nothing, an inactive or paralysed one does (51d3e0); a unit that dies or embarks after the grid is built takes nothing;
//   * same-tick spawn: a unit spawned after rebuildGrid (as tickAbilities spawns
//     its revives) emits on that pass but is no recipient until the next grid;
//   * random scenes: clustered bodies, emitters of every kind and state, bodies
//     dying, boarding and moving between passes -- every unit's buffs against
//     the serial walk written out here from its rules (RetailReplayProbe);
//   * corners: a tile under 200 auras (a four-word source list), squares clamped
//     on every grid edge or missing the grid, owners the 64-bit masks cannot hold,
//     and an ownership change after the grid -- against the same written-out walk;
//   * crowd: ~2700 bodies of four players in two alliances; two armies with
//     emitters of every kind march into each other and fight (the buffs scale
//     the damage, so they reach hp and the state hash), and two priests raise an
//     emitter's body, which takes up its aura on the very tick it rises: every
//     unit's atkBuff/armBuff bits and the state hash agree every tick.
// A debug build also sets TAK_VERIFY_POOL: every tiled pass is then redone with
// the serial walk and compared, and any difference aborts.
#include "net/lockstep.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace tak::sim {
struct RetailReplayProbe {
    static void grid(World& w) { w.rebuildGrid(); }
    static void auras(World& w) { w.tickAuras(1.0f / 30); }
    // An ownership change after the grid turns the owner masks off (captureUnit).
    static void masksOff(World& w) { w.gPlayersValid_ = false; }
    static bool masksValid(const World& w) { return w.gPlayersValid_; }
    static float gridX(const World& w, int gx) { return w.gOx_ + w.gCell_ * float(gx); }
    static float gridZ(const World& w, int gz) { return w.gOz_ + w.gCell_ * float(gz); }
    static int gridW(const World& w) { return w.gW_; }
    static int gridH(const World& w) { return w.gH_; }
    // The serial pass's fold, written out here from its rules alone: every emitter in
    // unit order, each Armor/Attack aura in type order, the units of forEachNear's
    // square in the grid as rebuildGrid left it, max for a buff and min for a debuff,
    // into a copy of every unit's (atkBuff, armBuff) after the relax.
    static std::vector<std::pair<float, float>> reference(const World& w, float dt) {
        const auto& units = w.units();
        const uint32_t stride = units.size() > 8000 ? 3u : 1u;
        const float relax = std::min(1.0f, 2.0f * dt * float(stride));
        std::vector<std::pair<float, float>> out;
        for (const Unit& u : units) {
            float atk = u.atkBuff, arm = u.armBuff;
            if (u.alive()) { atk += (1.0f - atk) * relax; arm += (1.0f - arm) * relax; }
            out.emplace_back(atk, arm);
        }
        if (w.gW_ <= 0) return out;
        for (const Unit& s : units) {
            if (!s.type || s.type->auras.empty()) continue;
            if (!World::retailAuraEmits(s)) continue;
            for (const Aura& a : s.type->auras) {
                if (a.kind == Aura::Kind::Joy || a.radius <= 0) continue;
                const float x = s.x.toFloat(), z = s.z.toFloat(), radius = float(a.radius);
                const float r2 = float(a.radius * a.radius);
                const int r = int(radius / w.gCell_) + 1;
                const int cx = int((x - w.gOx_) / w.gCell_), cz = int((z - w.gOz_) / w.gCell_);
                for (int gz = cz - r; gz <= cz + r; ++gz) {
                    if (gz < 0 || gz >= w.gH_) continue;
                    for (int gx = cx - r; gx <= cx + r; ++gx) {
                        if (gx < 0 || gx >= w.gW_) continue;
                        for (int i = w.gHead_[size_t(gz) * size_t(w.gW_) + size_t(gx)]; i >= 0; i = w.gNext_[size_t(i)]) {
                            const Unit& e = units[size_t(i)];
                            if (!e.alive() || e.embarked() || !e.type) continue;
                            if (!w.allied(e.player, s.player) != a.affectsEnemy) continue;
                            const float dx = (e.x - s.x).toFloat(), dz = (e.z - s.z).toFloat();
                            const float d2 = dx * dx + dz * dz;
                            if (d2 > r2) continue;
                            const float amt = 1.0f + (a.amount - 1.0f) * a.falloff(std::sqrt(d2));
                            float& buff = a.kind == Aura::Kind::Armor ? out[size_t(i)].second : out[size_t(i)].first;
                            buff = a.affectsEnemy ? std::min(buff, amt) : std::max(buff, amt);
                        }
                    }
                }
            }
        }
        return out;
    }
};
}  // namespace tak::sim

using namespace tak;
using namespace tak::sim;

namespace {
int g_fail = 0;
void check(bool ok, const char* what, const std::string& detail = {}) {
    std::printf("  %-70s %s%s%s\n", what, ok ? "ok" : "FAIL", detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++g_fail;
}

uint64_t buffDigest(const World& w) {
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](uint32_t v) { h = (h ^ v) * 1099511628211ULL; };
    for (const Unit& u : w.units()) { mix(std::bit_cast<uint32_t>(u.atkBuff)); mix(std::bit_cast<uint32_t>(u.armBuff)); }
    return h;
}
bool same(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }
// What one aura folds in at `dist`, as the pass computes it.
float strength(const Aura& a, float dx, float dz) {
    const float d2 = dx * dx + dz * dz;
    return 1.0f + (a.amount - 1.0f) * a.falloff(std::sqrt(d2));
}

struct Types {
    UnitType soldier, filler, smith, shaman, hexer, warlock, priest, flatBuff, flatDebuff, beacon;
    FeatType body;
    Types() {
        auto mobile = [](UnitType& t, const char* id) {
            t.name = t.id = id; t.maxHp = 300; t.maxVel = Fixed::fromInt(2); t.canMove = true;
            t.turnRate = t.turnInPlaceRate = 10000; t.footX = t.footZ = 1; t.sight = 200; t.buildTime = 30;
            t.upright = true;
        };
        auto arm = [](UnitType& t) {
            Weapon w;
            w.name = "blade"; w.range = 60; w.damage = 9; w.reload = 0.5f; w.aimTol = 32767;
            t.weapon = w; t.weapons.push_back(w);
        };
        mobile(soldier, "soldier"); arm(soldier);
        mobile(filler, "filler");
        // The retail emitters (Aramon Smith; Zhon Shaman) and two mod-style ones with debuffs:
        // a hexer debuffs foes' armour and attack, a warlock buffs friends' armour close in
        // and debuffs foes' armour far out.
        mobile(smith, "smith"); arm(smith);
        smith.auras = {Aura{Aura::Kind::Armor, 1.4f, false, 200, 0.6f}};
        mobile(shaman, "shaman"); arm(shaman);
        shaman.auras = {Aura{Aura::Kind::Armor, 1.2f, false, 200, 0.1f}, Aura{Aura::Kind::Attack, 1.2f, false, 200, 0.1f}};
        mobile(hexer, "hexer"); arm(hexer);
        hexer.auras = {Aura{Aura::Kind::Attack, 1.15f, false, 160, 0.5f}, Aura{Aura::Kind::Armor, 0.8f, true, 160, 0.5f},
                       Aura{Aura::Kind::Attack, 0.85f, true, 120, 1.0f}};
        mobile(warlock, "warlock"); arm(warlock);
        warlock.auras = {Aura{Aura::Kind::Armor, 0.7f, true, 300, 0.2f}, Aura{Aura::Kind::Armor, 1.3f, false, 90, 1.0f}};
        mobile(priest, "priest");
        priest.workerTime = 10; priest.isBuilder = true; priest.canReclaim = true; priest.canResurrect = true;
        mobile(flatBuff, "flatbuff");
        flatBuff.auras = {Aura{Aura::Kind::Armor, 1.5f, false, 300, 1.0f}, Aura{Aura::Kind::Attack, 1.25f, false, 100, 1.0f}};
        mobile(flatDebuff, "flatdebuff");
        flatDebuff.auras = {Aura{Aura::Kind::Armor, 0.7f, true, 300, 1.0f}};
        // Far-off emitters that only make up the pooled pass's emitter count.
        mobile(beacon, "beacon");
        beacon.auras = {Aura{Aura::Kind::Attack, 1.1f, false, 64, 1.0f}};
        body.name = "shaman_dead"; body.resurrectable = true; body.reclaimable = true;
        body.decomposeTicks = 100000; body.energy = 10;
    }
};

// Serial (the emitter-order pass), then 1, 2, 3, 7, 23 and 63 (SimPool::kMaxWorkers)
// workers (the recipient pass).
constexpr int kWidths[] = {-1, 1, 2, 3, 7, 23, 63};

std::unique_ptr<World> makeWorld(int width, int cells) {
    auto w = std::make_unique<World>();
    w->setPlayerCount(4);
    w->setTerrain(std::vector<uint8_t>(size_t(cells) * size_t(cells), 100), cells, cells, 20);
    w->setMapPlacementFeatures(std::vector<uint16_t>(size_t(cells) * size_t(cells), 0xffff), {});
    w->setPathService(true);
    w->setTeam(1, 0); w->setTeam(3, 2);   // alliances {0,1} and {2,3}
    if (width < 0) w->setSerialThreads(true); else w->setSimWorkers(width);
    return w;
}

// Two armies with emitters of every kind march into each other; two priests raise a
// shaman's body on the way.
void crowd(const Types& t) {
    std::printf("crowd: serial vs 1/2/3/7/23/63 workers, every tick of a fight with a revive:\n");
    std::vector<std::unique_ptr<World>> worlds;
    std::vector<int> bodies, priests;
    for (int width : kWidths) {
        auto w = makeWorld(width, 176);   // 2816 px square
        w->setFeatureTypes({t.body});
        w->mapCorpse(&t.shaman, 0);
        const UnitType* emitters[] = {&t.smith, &t.shaman, &t.hexer, &t.warlock};
        for (int i = 0; i < 2700; ++i) {
            const int player = i % 4, side = player < 2 ? 0 : 1;
            const UnitType* type = i % 13 == 5 ? emitters[size_t(i / 13) % 4] : &t.soldier;
            const float x = float(side ? 1500 + (i * 37) % 1000 : 300 + (i * 37) % 1000);
            const float z = float(200 + (i * 53) % 2400);
            w->spawn(type, x, z, 0.0f, player);
        }
        bodies.clear(); priests.clear();
        for (int k = 0; k < 2; ++k) {
            const int b = w->spawn(&t.shaman, 260.0f, float(1000 + 300 * k), 0.0f, 0);
            w->unit(b)->hp = {}; w->unit(b)->deathType = 9;
            bodies.push_back(b);
            priests.push_back(w->spawn(&t.priest, 200.0f, float(1000 + 300 * k), 0.0f, 0));
        }
        w->player(0).mana = 100;
        TypeRegistry reg;
        for (int k = 0; k < 2; ++k) {
            net::Command c;
            c.kind = net::Cmd::Resurrect; c.player = 0; c.unitId = priests[size_t(k)]; c.targetId = bodies[size_t(k)];
            applyCommand(*w, reg, c);
        }
        // March: every soldier and emitter to the far side.
        for (const Unit& u : w->units())
            if (u.alive() && u.type && u.type != &t.priest && u.hp > Fixed())
                w->order(u.id, u.player < 2 ? 2200.0f : 600.0f, u.z.toFloat(), false);
        worlds.push_back(std::move(w));
    }
    const size_t shamans0 = [&] {
        size_t n = 0;
        for (const Unit& u : worlds[0]->units()) n += u.type == &t.shaman && u.player == 0 && u.hp > Fixed();
        return n;
    }();
    int firstDiff = -1, revivedAt = -1;
    for (int step = 0; step < 160 && firstDiff < 0; ++step) {
        for (auto& w : worlds) w->tick(1.0f / 30);
        const uint64_t d0 = buffDigest(*worlds[0]), h0 = worlds[0]->stateHash();
        for (size_t k = 1; k < worlds.size(); ++k)
            if (buffDigest(*worlds[k]) != d0 || worlds[k]->stateHash() != h0) firstDiff = step;
        if (revivedAt < 0) {
            size_t n = 0;
            for (const Unit& u : worlds[0]->units()) n += u.alive() && u.type == &t.shaman && u.player == 0;
            if (n > shamans0) revivedAt = step;
        }
    }
    const World& w = *worlds[0];
    int emitterCount = 0, atkUp = 0, atkDown = 0, armUp = 0, armDown = 0, hurt = 0;
    for (const Unit& u : w.units()) {
        emitterCount += u.type && !u.type->auras.empty();
        if (!u.alive()) continue;
        atkUp += u.atkBuff > 1; atkDown += u.atkBuff < 1; armUp += u.armBuff > 1; armDown += u.armBuff < 1;
        hurt += u.hp < Fixed::fromInt(u.type->maxHp);
    }
    char detail[200];
    std::snprintf(detail, sizeof detail, "units %zu, emitters %d; first difference at step %d; final hash %016llx buffs %016llx",
                  w.units().size(), emitterCount, firstDiff, (unsigned long long)w.stateHash(),
                  (unsigned long long)buffDigest(w));
    check(w.units().size() >= 2048 && emitterCount >= 8, "the world is big enough for the pooled pass", detail);
    check(firstDiff < 0, "buff bits and state hash equal at every width, every tick", detail);
    std::snprintf(detail, sizeof detail, "atk up %d down %d, arm up %d down %d, hurt %d", atkUp, atkDown, armUp, armDown, hurt);
    check(atkUp && atkDown && armUp && armDown && hurt, "buffs and debuffs of both stats held, and the armies fought", detail);
    std::snprintf(detail, sizeof detail, "a shaman rose at step %d", revivedAt);
    check(revivedAt >= 0, "a priest raised an emitter's body during the run", detail);
}

// Random scenes against the fold written out in the probe: clusters of bodies of four
// players, emitters of every kind (some dead, unbuilt, inactive, frozen or embarked),
// several passes with the bodies moved between them, at every width; plus a scene with
// three emitters (the tiles inline although there are workers).
void randomScenes(const Types& t) {
    std::printf("random scenes against the written-out serial fold:\n");
    const UnitType* kinds[] = {&t.smith, &t.shaman, &t.hexer, &t.warlock, &t.flatBuff, &t.flatDebuff};
    int bad = 0, compared = 0;
    std::string detail;
    for (int scene = 0; scene < 4; ++scene)
        for (int width : kWidths) {
            auto w = makeWorld(width, 224);   // 3584 px square
            uint32_t rng = 0x9e3779b9u * uint32_t(scene + 1);
            auto rnd = [&](int n) { rng = rng * 1664525u + 1013904223u; return int((rng >> 8) % uint32_t(n)); };
            const int emitters = scene == 3 ? 3 : 60 + 40 * scene, every = 2300 / emitters;
            for (int i = 0, placed = 0; i < 2300; ++i) {
                // Five clusters of bodies, so auras overlap heavily in places and not at all in others.
                const int c = rnd(5);
                const float x = float(400 + 650 * c + rnd(500)), z = float(300 + 500 * ((c * 3) % 5) + rnd(700));
                const bool emits = i % every == 0 && placed < emitters;
                placed += emits;
                const UnitType* type = emits ? kinds[size_t(rnd(6))] : &t.filler;
                const int id = w->spawn(type, x, z, 0.0f, rnd(4));
                Unit& u = *w->unit(id);
                if (emits)
                    switch (rnd(12)) {
                    case 0: u.deadFor = 5; break;
                    case 1: u.underConstruction = true; break;
                    case 2: u.active = false; break;
                    case 3: u.frozenFor = 20; break;
                    default: break;
                    }
            }
            for (int pass = 0; pass < 3; ++pass) {
                RetailReplayProbe::grid(*w);
                // Liveness and boarding changed after the grid: the pass must still skip them.
                for (size_t i = size_t(pass); i < w->units().size(); i += 97) {
                    Unit& u = w->units()[i];
                    if (i % 2) u.deadFor = 0; else u.inTransport = w->units()[0].id;
                }
                const auto expect = RetailReplayProbe::reference(*w, 1.0f / 30);
                RetailReplayProbe::auras(*w);
                for (size_t i = 0; i < expect.size(); ++i) {
                    const Unit& u = w->units()[i];
                    ++compared;
                    if (!same(u.atkBuff, expect[i].first) || !same(u.armBuff, expect[i].second)) {
                        if (!bad++) {
                            char b[200];
                            std::snprintf(b, sizeof b, "scene %d width %d pass %d unit %zu: atk %.7f/%.7f arm %.7f/%.7f",
                                          scene, width, pass, i, u.atkBuff, expect[i].first, u.armBuff, expect[i].second);
                            detail = b;
                        }
                    }
                }
                // Move every live body a little for the next pass, and wake the boarded ones.
                for (Unit& u : w->units()) {
                    u.inTransport = 0;
                    if (!u.alive()) continue;
                    u.x = u.x + Fixed::fromInt(rnd(41) - 20);
                    u.z = u.z + Fixed::fromInt(rnd(41) - 20);
                }
            }
        }
    char b[120];
    std::snprintf(b, sizeof b, "%d of %d buff pairs differ%s", bad, compared, detail.empty() ? "" : "; first: ");
    check(bad == 0, "every unit's buffs equal the written-out fold, bit for bit", b + detail);
}

// The corners of the recipient pass, against the written-out fold at every width:
//   * a stack of 200 emitters on one spot, so a tile's source list runs to four
//     64-bit words, buffs and debuffs of one stat interleaved;
//   * a wide aura whose square is clamped on all four grid edges;
//   * emitters spawned after the grid outside its box on every side: some squares
//     only graze the grid, some miss it, one sits within a cell left of the origin
//     (int() truncates toward zero there, in forEachNear too);
//   * owners the 64-bit masks cannot hold (70, -1) on emitters and recipients, so
//     the grid's masks are off and the side test falls back to allied();
//   * with representable owners, an ownership change after the grid (captureUnit
//     turns the masks off; the side test reads the new owner).
void corners(const Types& t) {
    std::printf("corners: multi-word tile lists, edge clamps, off-grid emitters, odd owners:\n");
    UnitType wide;
    wide.name = wide.id = "wide"; wide.maxHp = 300; wide.maxVel = Fixed::fromInt(2); wide.canMove = true;
    wide.turnRate = wide.turnInPlaceRate = 10000; wide.footX = wide.footZ = 1; wide.sight = 200; wide.upright = true;
    wide.auras = {Aura{Aura::Kind::Attack, 0.9f, true, 3000, 0.3f}, Aura{Aura::Kind::Attack, 1.1f, false, 2500, 0.5f}};
    const UnitType* stack[] = {&t.flatBuff, &t.flatDebuff, &t.hexer, &t.warlock, &t.smith, &t.shaman};
    int bad = 0, compared = 0;
    bool masksOffSeen[2] = {false, false};
    std::string detail;
    for (int variant = 0; variant < 2; ++variant) {
        std::vector<uint64_t> digests;
        for (int width : kWidths) {
            auto w = makeWorld(width, 256);   // 4096 px square
            uint32_t rng = 0x51ed27u + uint32_t(variant);
            auto rnd = [&](int n) { rng = rng * 1664525u + 1013904223u; return int((rng >> 8) % uint32_t(n)); };
            for (int i = 0; i < 2100; ++i)
                w->spawn(&t.filler, float(1000 + rnd(2000)), float(1000 + rnd(2000)), 0.0f, rnd(4));
            for (int i = 0; i < 200; ++i)
                w->spawn(stack[size_t(i) % 6], float(1500 + rnd(40)), float(1500 + rnd(40)), 0.0f, rnd(4));
            w->spawn(&wide, 2000.0f, 2000.0f, 0.0f, 2);
            if (variant == 0) {
                // Owners outside the masks, on recipients and on emitters.
                for (size_t i = 0; i < w->units().size(); i += 7) w->units()[i].player = i % 14 ? 70 : -1;
            }
            RetailReplayProbe::grid(*w);
            const int gW = RetailReplayProbe::gridW(*w), gH = RetailReplayProbe::gridH(*w);
            const float left = RetailReplayProbe::gridX(*w, 0), right = RetailReplayProbe::gridX(*w, gW);
            const float top = RetailReplayProbe::gridZ(*w, 0), bottom = RetailReplayProbe::gridZ(*w, gH);
            // After the grid: emitters outside its box (grazing and missing), and one a
            // fraction of a cell left of the origin.
            const float mid = 0.5f * (top + bottom), midX = 0.5f * (left + right);
            w->spawn(&t.flatBuff, left - 250.0f, mid, 0.0f, 0);
            w->spawn(&t.flatDebuff, right + 250.0f, mid, 0.0f, 3);
            w->spawn(&t.warlock, midX, top - 280.0f, 0.0f, 1);
            w->spawn(&t.hexer, midX, bottom + 150.0f, 0.0f, 2);
            w->spawn(&t.flatBuff, left - 2000.0f, top - 2000.0f, 0.0f, 0);
            w->spawn(&t.flatDebuff, right + 2000.0f, bottom + 2000.0f, 0.0f, 1);
            w->spawn(&t.shaman, left - 12.0f, mid + 40.0f, 0.0f, 1);
            if (variant == 1) {
                // A capture after the grid: new owners, masks off.
                for (size_t i = 3; i < 2100; i += 11) w->units()[i].player = (w->units()[i].player + 1) % 4;
                RetailReplayProbe::masksOff(*w);
            }
            masksOffSeen[variant] = !RetailReplayProbe::masksValid(*w);
            const auto expect = RetailReplayProbe::reference(*w, 1.0f / 30);
            RetailReplayProbe::auras(*w);
            for (size_t i = 0; i < expect.size(); ++i) {
                const Unit& u = w->units()[i];
                ++compared;
                if (!same(u.atkBuff, expect[i].first) || !same(u.armBuff, expect[i].second))
                    if (!bad++) {
                        char b[200];
                        std::snprintf(b, sizeof b, "variant %d width %d unit %zu: atk %.7f/%.7f arm %.7f/%.7f", variant,
                                      width, i, u.atkBuff, expect[i].first, u.armBuff, expect[i].second);
                        detail = b;
                    }
            }
            digests.push_back(buffDigest(*w));
        }
        for (uint64_t d : digests)
            if (d != digests[0] && !bad++) detail = "digests differ between widths";
    }
    char b[120];
    std::snprintf(b, sizeof b, "%d of %d buff pairs differ%s", bad, compared, detail.empty() ? "" : "; first: ");
    check(bad == 0, "every unit's buffs equal the written-out fold, bit for bit", b + detail);
    check(masksOffSeen[0] && masksOffSeen[1], "both variants ran with the owner masks off");
}

// A world big enough for the pooled pass: 2100 still bodies and eight beacons out of the way.
std::unique_ptr<World> sceneWorld(const Types& t, int width) {
    auto w = makeWorld(width, 256);   // 4096 px square
    for (int i = 0; i < 2100; ++i)
        w->spawn(&t.filler, float(2600 + (i * 37) % 1300), float(200 + (i * 53) % 3700), 0.0f, i % 4);
    for (int i = 0; i < 8; ++i) w->spawn(&t.beacon, float(3000 + 100 * i), 3800.0f, 0.0f, i % 4);
    return w;
}

// A buff and a debuff of the same stat on one unit: the later emitter wins.
void order(const Types& t) {
    std::printf("order: a buff and a debuff of one stat fold in emitter order:\n");
    const Aura& up = t.flatBuff.auras[0];
    const Aura& down = t.flatDebuff.auras[0];
    for (int debuffFirst = 0; debuffFirst < 2; ++debuffFirst) {
        std::vector<float> got;
        for (int width : kWidths) {
            auto w = sceneWorld(t, width);
            // The recipient sits on a tile corner (grid cell origin depends on the bodies, so
            // the auras' squares straddle several tiles whatever the alignment).
            const int r = w->spawn(&t.filler, 1000.0f, 1000.0f, 0.0f, 0);
            int e1, e2;
            if (debuffFirst) {
                e1 = w->spawn(&t.flatDebuff, 1150.0f, 1080.0f, 0.0f, 2);
                e2 = w->spawn(&t.flatBuff, 880.0f, 930.0f, 0.0f, 1);
            } else {
                e1 = w->spawn(&t.flatBuff, 880.0f, 930.0f, 0.0f, 1);
                e2 = w->spawn(&t.flatDebuff, 1150.0f, 1080.0f, 0.0f, 2);
            }
            (void)e1; (void)e2;
            RetailReplayProbe::grid(*w);
            RetailReplayProbe::auras(*w);
            got.push_back(w->unit(r)->armBuff);
        }
        const float expect = debuffFirst ? strength(up, 0, 0) : strength(down, 0, 0);
        bool all = true;
        for (float v : got) all &= same(v, expect);
        char detail[160];
        std::snprintf(detail, sizeof detail, "armBuff serial %.6f, workers %.6f/%.6f/%.6f, expected %.6f",
                      got[0], got[1], got[2], got[3], expect);
        check(all, debuffFirst ? "debuff then buff: the buff stands" : "buff then debuff: the debuff stands", detail);
    }
}

// Ineligible emitters emit nothing; units that die or embark after the grid take nothing;
// a unit spawned after the grid emits but takes nothing until the next grid.
void eligibilityAndSpawn(const Types& t) {
    std::printf("eligibility and same-tick spawn:\n");
    std::vector<uint64_t> digests;
    std::vector<std::string> fails;
    for (int width : kWidths) {
        auto w = sceneWorld(t, width);
        const int quiet = w->spawn(&t.filler, 600.0f, 600.0f, 0.0f, 0);
        int muted[5];
        for (int k = 0; k < 5; ++k) muted[k] = w->spawn(&t.flatBuff, 620.0f + 10.0f * float(k), 640.0f, 0.0f, 0);
        w->unit(muted[0])->deadFor = 10;          // dead
        w->unit(muted[1])->inTransport = quiet;   // embarked
        w->unit(muted[2])->underConstruction = true;
        w->unit(muted[3])->stonedFor = 30;
        w->unit(muted[4])->frozenFor = 30;
        // Bodies the pass still finds in the grid but must skip: they die or embark after it.
        const int source = w->spawn(&t.flatBuff, 1600.0f, 1600.0f, 0.0f, 0);
        const int dies = w->spawn(&t.filler, 1620.0f, 1600.0f, 0.0f, 0);
        const int boards = w->spawn(&t.filler, 1600.0f, 1630.0f, 0.0f, 0);
        const int near = w->spawn(&t.filler, 1640.0f, 1660.0f, 0.0f, 1);
        // 51d3e0 tests no activation and no paralysis: an emitter made inactive
        // (as every shipped emitter is) or paralysed still emits.
        const int dormant = w->spawn(&t.flatBuff, 2600.0f, 400.0f, 0.0f, 0);
        w->unit(dormant)->active = false;w->unit(dormant)->paralyzedFor = 30;
        const int lit = w->spawn(&t.filler, 2620.0f, 400.0f, 0.0f, 0);
        // The revive case: spawned after the grid, beside a unit it reaches.
        const int target = w->spawn(&t.filler, 2000.0f, 2000.0f, 0.0f, 1);
        RetailReplayProbe::grid(*w);
        w->unit(dies)->deadFor = 0;
        w->unit(boards)->inTransport = source;
        const int risen = w->spawn(&t.flatBuff, 2040.0f, 2030.0f, 0.0f, 0);
        RetailReplayProbe::auras(*w);
        const Unit& q = *w->unit(quiet);
        if (!same(q.atkBuff, 1.0f) || !same(q.armBuff, 1.0f)) fails.push_back("an ineligible emitter reached a unit");
        if (!same(w->unit(dies)->armBuff, 1.0f) || !same(w->unit(boards)->armBuff, 1.0f))
            fails.push_back("a unit that died or embarked after the grid took a buff");
        const Aura& armour = t.flatBuff.auras[0];
        const Aura& attack = t.flatBuff.auras[1];
        const Unit& n = *w->unit(near);
        if (!same(n.armBuff, strength(armour, 40, 60))) fails.push_back("the live source missed its neighbour");
        if (!same(w->unit(lit)->armBuff, strength(armour, 20, 0)))
            fails.push_back("an inactive, paralysed emitter did not reach its neighbour");
        const Unit& tg = *w->unit(target);
        if (!same(tg.armBuff, strength(armour, 40, 30)) || !same(tg.atkBuff, strength(attack, 40, 30)))
            fails.push_back("a unit spawned after the grid did not emit");
        const Unit& rs = *w->unit(risen);
        if (!same(rs.armBuff, 1.0f) || !same(rs.atkBuff, 1.0f))
            fails.push_back("a unit spawned after the grid took a buff before the next grid");
        // The next grid holds it: its own auras reach it (distance 0).
        RetailReplayProbe::grid(*w);
        RetailReplayProbe::auras(*w);
        if (!same(w->unit(risen)->armBuff, strength(armour, 0, 0)))
            fails.push_back("the risen emitter missed itself once in the grid");
        digests.push_back(buffDigest(*w));
    }
    bool equal = true;
    for (uint64_t d : digests) equal &= d == digests[0];
    std::string detail;
    for (size_t i = 0; i < fails.size(); ++i)
        if (std::find(fails.begin(), fails.begin() + std::ptrdiff_t(i), fails[i]) == fails.begin() + std::ptrdiff_t(i))
            detail += (detail.empty() ? "" : "; ") + fails[i];
    check(fails.empty(), "eligibility, after-grid deaths and boardings, after-grid spawns", detail);
    check(equal, "every unit's buff bits equal at every width");
}
}  // namespace

int main() {
#ifndef NDEBUG
#ifdef _WIN32
    _putenv_s("TAK_VERIFY_POOL", "1");
#else
    setenv("TAK_VERIFY_POOL", "1", 1);
#endif
#endif
    std::printf("aura_recipient_test\n");
    const Types t;
    order(t);
    eligibilityAndSpawn(t);
    randomScenes(t);
    corners(t);
    crowd(t);
    if (g_fail) { std::printf("%d check(s) FAILED\n", g_fail); return 1; }
    std::printf("all checks passed\n");
    return 0;
}
