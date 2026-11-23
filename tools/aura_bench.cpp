// aura_bench -- World::tickAuras (the AdjustArmor/AdjustAttack stat-aura pass) alone.
//
// A screening harness, not a test. It builds one world, buckets it once
// (rebuildGrid, as World::tick does just before tickAbilities and tickAuras),
// then runs the aura pass --passes times on that frozen world and prints the
// per-pass time distribution and the work counters that explain it. Positions do
// not change between passes and the buffs settle after the first one (relax, then
// max/min), so every measured pass does the same work. The tick counter is pinned
// to 3: the crowd stride (3 above 8000 units) admits it and the 1 Hz heal pulse
// (tickCounter % 30 == 0) stays out.
//
// Layouts (--layout):
//   sparse  --units spread over a wide square, --emitters spread among them: an
//           emitter covers ~25 units and auras seldom overlap.
//   dense   --units packed into one battle blob, --emitters inside it: every unit
//           sits under ~20 overlapping auras. --debuff gives every third emitter
//           a hostile aura as well (no retail unit has one; mods can).
//   scn     --scn FILE (a decompressed tools/scenarios/we-timing/*.scn.gz) with
//           --data: the recording's own bodies and emitters (R-2h-1800: ~11.9k
//           units, ~230 Smiths).
// Workers: --workers N (World::setSimWorkers) or --serial (no pool). Run it on the
// timing cores through /home/pocket_geek/tak-tmp/tools/timing.sh.
// --relax-only strips every aura from the world's types before the first pass, so
// the pass is the relax walk alone: the fixed cost under the aura application.
//
// Output:
//   AURABENCH layout=.. units=.. grid_units=.. emitters=.. entries=.. cells=.. pairs=.. hits=.. recipients=..
//   AURATIME passes=.. workers=.. mean_us=.. median_us=.. p95_us=.. p99_us=.. max_us=.. min_us=.. p10_us=.. retained_bytes=..
//   (min and p10 are the least disturbed passes: read them on a busy host)
//   AURAHASH buffs=..   (a digest of every unit's atkBuff/armBuff bits after the last pass)
// The counters come from the tool's own walk over the grid (the scan forEachNear
// makes), so they do not depend on how the pass is implemented: pairs are the
// (aura, unit in a scanned cell) candidates, hits the ones the aura refreshes.
#include "legion_scn.h"
#include "sim/sim.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace tak::sim {
struct RetailReplayProbe {
    static void pin(World& w, uint32_t tick) { w.tickCounter_ = tick; w.rebuildGrid(); }
    static void auras(World& w, float dt) { w.tickAuras(dt); }
    struct Grid { int w, h; float ox, oz, cell; const std::vector<int>* head; const std::vector<int>* next; };
    static Grid grid(const World& w) { return {w.gW_, w.gH_, w.gOx_, w.gOz_, w.gCell_, &w.gHead_, &w.gNext_}; }
    // The aura scratch the world keeps between passes, whatever the pass keeps.
    template <class W> static size_t retained(const W& w) {
        size_t n = 0;
        if constexpr (requires { w.auraChunks_; }) {
            n += w.auraChunks_.capacity() * sizeof(w.auraChunks_[0]);
            for (const auto& c : w.auraChunks_) n += c.capacity() * sizeof(c[0]);
        }
        if constexpr (requires { w.auraSources_; }) n += w.auraSources_.capacity() * sizeof(w.auraSources_[0]);
        if constexpr (requires { w.auraTileFirst_; }) n += w.auraTileFirst_.capacity() * sizeof(w.auraTileFirst_[0]);
        if constexpr (requires { w.auraTileList_; }) n += w.auraTileList_.capacity() * sizeof(w.auraTileList_[0]);
        if constexpr (requires { w.auraTiles_; }) n += w.auraTiles_.capacity() * sizeof(w.auraTiles_[0]);
        if constexpr (requires { w.auraTileMasks_; }) {
            n += w.auraTileMasks_.capacity() * sizeof(w.auraTileMasks_[0]);
            for (const auto& c : w.auraTileMasks_) n += c.capacity() * sizeof(c[0]);
        }
        return n;
    }
};
}  // namespace tak::sim

using namespace tak::sim;

namespace {
struct Counters { size_t emitters = 0, entries = 0, cells = 0, pairs = 0, hits = 0, recipients = 0, gridUnits = 0; };

// The aura scan exactly as tickAuras + forEachNear make it, counting instead of applying.
Counters count(const World& w) {
    Counters c;
    const auto g = RetailReplayProbe::grid(w);
    const auto& units = w.units();
    std::vector<uint8_t> hit(units.size(), 0);
    if (g.w > 0)
        for (int cell = 0; cell < g.w * g.h; ++cell)
            for (int i = (*g.head)[size_t(cell)]; i >= 0; i = (*g.next)[size_t(i)]) ++c.gridUnits;
    for (const Unit& s : units) {
        if (!s.type || s.type->auras.empty()) continue;
        if (!s.alive() || s.embarked() || s.underConstruction || !s.active || s.incapacitated()) continue;
        bool any = false;
        for (const Aura& a : s.type->auras) {
            if (a.kind == Aura::Kind::Joy || a.radius <= 0) continue;
            any = true;
            ++c.entries;
            if (g.w <= 0) continue;
            const float x = s.x.toFloat(), z = s.z.toFloat(), radius = float(a.radius);
            const float r2 = float(a.radius * a.radius);
            const int r = int(radius / g.cell) + 1;
            const int cx = int((x - g.ox) / g.cell), cz = int((z - g.oz) / g.cell);
            for (int gz = std::max(0, cz - r); gz <= std::min(g.h - 1, cz + r); ++gz)
                for (int gx = std::max(0, cx - r); gx <= std::min(g.w - 1, cx + r); ++gx) {
                    ++c.cells;
                    for (int i = (*g.head)[size_t(gz) * size_t(g.w) + size_t(gx)]; i >= 0; i = (*g.next)[size_t(i)]) {
                        ++c.pairs;
                        const Unit& e = units[size_t(i)];
                        if (!e.alive() || e.embarked() || !e.type) continue;
                        if (!w.allied(e.player, s.player) != a.affectsEnemy) continue;
                        const float dx = (e.x - s.x).toFloat(), dz = (e.z - s.z).toFloat();
                        if (dx * dx + dz * dz > r2) continue;
                        ++c.hits;
                        hit[size_t(i)] = 1;
                    }
                }
        }
        c.emitters += any;
    }
    for (uint8_t h : hit) c.recipients += h;
    return c;
}

uint64_t buffDigest(const World& w) {
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](uint32_t v) { h = (h ^ v) * 1099511628211ULL; };
    for (const Unit& u : w.units()) { mix(std::bit_cast<uint32_t>(u.atkBuff)); mix(std::bit_cast<uint32_t>(u.armBuff)); }
    return h;
}

double pct(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0;
    const size_t i = std::min(sorted.size() - 1, size_t(p * double(sorted.size() - 1) + 0.5));
    return sorted[i];
}

int usage() {
    std::fputs("aura_bench [--layout sparse|dense|scn] [--units N] [--emitters M] [--debuff] [--scn FILE --data DIR]\n"
               "           [--workers N | --serial] [--passes P] [--warm W] [--seed S] [--relax-only]\n", stderr);
    return 2;
}
}  // namespace

int main(int argc, char** argv) try {
    std::string layout = "sparse", scnPath, data;
    int units = 12000, emitters = 200, workers = -1, passes = 300, warm = 20;
    unsigned seed = 1;
    bool serial = false, debuff = false, relaxOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) throw std::invalid_argument("missing value for " + a); return argv[++i]; };
        if (a == "--layout") layout = next();
        else if (a == "--units") units = std::stoi(next());
        else if (a == "--emitters") emitters = std::stoi(next());
        else if (a == "--workers") workers = std::stoi(next());
        else if (a == "--serial") serial = true;
        else if (a == "--debuff") debuff = true;
        else if (a == "--relax-only") relaxOnly = true;
        else if (a == "--passes") passes = std::stoi(next());
        else if (a == "--warm") warm = std::stoi(next());
        else if (a == "--seed") seed = unsigned(std::stoul(next()));
        else if (a == "--scn") scnPath = next();
        else if (a == "--data") data = next();
        else return usage();
    }
    if (passes < 1 || warm < 0) return usage();

    // Retail aura values: the Aramon Smith (AdjustArmor 1.4, edge 0.6, radius 200) and the
    // Zhon Shaman (AdjustArmor + AdjustAttack 1.2, edge 0.1, radius 200); the hostile one
    // is a mod-style debuff.
    UnitType soldier{}, big{}, smith{}, shaman{}, hexer{};
    auto mobile = [](UnitType& t, const char* id, int foot) {
        t.name = t.id = id; t.maxHp = 100; t.maxVel = Fixed::fromInt(1); t.canMove = true;
        t.footX = t.footZ = foot; t.sight = 160;
    };
    mobile(soldier, "soldier", 1);
    mobile(big, "big", 2);
    mobile(smith, "smith", 1);
    smith.auras = {Aura{Aura::Kind::Armor, 1.4f, false, 200, 0.6f}};
    mobile(shaman, "shaman", 1);
    shaman.auras = {Aura{Aura::Kind::Armor, 1.2f, false, 200, 0.1f}, Aura{Aura::Kind::Attack, 1.2f, false, 200, 0.1f}};
    mobile(hexer, "hexer", 1);
    hexer.auras = {Aura{Aura::Kind::Attack, 1.15f, false, 160, 0.5f}, Aura{Aura::Kind::Armor, 0.8f, true, 160, 0.5f},
                   Aura{Aura::Kind::Attack, 0.85f, true, 120, 1.0f}};

    std::unique_ptr<tak::scn::Built> built;
    std::unique_ptr<World> own;
    World* world = nullptr;
    if (layout == "scn") {
        if (scnPath.empty() || data.empty()) throw std::invalid_argument("--layout scn needs --scn FILE and --data DIR");
        const auto scn = tak::scn::load(scnPath);
        built = tak::scn::build(scn, {PathfindingMode::Retail, serial, 0, data.c_str()});
        if (!built->skipped.empty()) throw std::runtime_error(built->skipped);
        world = built->world.get();
    } else if (layout == "sparse" || layout == "dense") {
        own = std::make_unique<World>();
        world = own.get();
        const bool dense = layout == "dense";
        const int side = dense ? 2400 : 8000;            // px: the blob, or the wide square
        const int mapCells = (side + 1024) / 16;         // 16 px terrain cells, with a margin
        world->setPlayerCount(8);
        world->setTerrain(std::vector<uint8_t>(size_t(mapCells) * size_t(mapCells), 100), mapCells, mapCells, 20);
        for (int p = 0; p < 8; ++p) world->setTeam(p, p % 2);   // two alliances of four
        uint32_t rng = seed * 2654435761u + 12345u;
        auto rnd = [&](int n) { rng = rng * 1664525u + 1013904223u; return int((rng >> 8) % uint32_t(n)); };
        const int every = emitters > 0 ? std::max(1, units / emitters) : 0;
        int placed = 0;
        for (int i = 0; i < units; ++i) {
            const UnitType* t = i % 7 == 3 ? &big : &soldier;
            if (every && i % every == 0 && placed < emitters) {
                t = dense ? (debuff && placed % 3 == 0 ? &hexer : (placed % 2 ? &shaman : &smith)) : &smith;
                ++placed;
            }
            const float x = float(512 + rnd(side)), z = float(512 + rnd(side));
            world->spawn(t, x, z, 0.0f, i % 8);
        }
    } else return usage();
    if (relaxOnly)   // types are shared and const in the registry: a bench may strip them
        for (const Unit& u : world->units())
            if (u.type) const_cast<UnitType*>(u.type)->auras.clear();
    if (serial) world->setSerialThreads(true);
    else if (workers >= 0) world->setSimWorkers(workers);

    RetailReplayProbe::pin(*world, 3);
    const Counters c = count(*world);
    std::printf("AURABENCH layout=%s units=%zu grid_units=%zu emitters=%zu entries=%zu cells=%zu pairs=%zu hits=%zu recipients=%zu\n",
                layout.c_str(), world->units().size(), c.gridUnits, c.emitters, c.entries, c.cells, c.pairs, c.hits, c.recipients);
    std::fflush(stdout);
    const float dt = 1.0f / 30;
    for (int i = 0; i < warm; ++i) RetailReplayProbe::auras(*world, dt);
    std::vector<double> us;
    us.reserve(size_t(passes));
    for (int i = 0; i < passes; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        RetailReplayProbe::auras(*world, dt);
        us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
    }
    double sum = 0;
    for (double v : us) sum += v;
    std::sort(us.begin(), us.end());
    const std::string width = serial ? "serial" : workers >= 0 ? std::to_string(workers) : "default";
    std::printf("AURATIME passes=%d workers=%s mean_us=%.1f median_us=%.1f p95_us=%.1f p99_us=%.1f max_us=%.1f min_us=%.1f "
                "p10_us=%.1f retained_bytes=%zu\n",
                passes, width.c_str(), sum / double(passes), pct(us, 0.5), pct(us, 0.95), pct(us, 0.99), us.back(), us.front(),
                pct(us, 0.10), RetailReplayProbe::retained(*world));
    std::printf("AURAHASH buffs=%016llx\n", (unsigned long long)buffDigest(*world));
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "aura_bench: %s\n", e.what());
    return 1;
}
