// WE E4.3: the tick passes that run on the SimPool (the spatial rebuild, the
// turn reset and economy walk, the producer walk, the ability lists, the aura
// relax and gathers, the turn-delta walk) give the serial result. One world
// above every pool threshold (kSpatialMinParallel, kPrologueMinParallel,
// kPostMinParallel: 2048 units; more than 8 aura emitters) is ticked serially
// and at every kind of width a player or a server operator can choose
// (SIMULATION THREADS, --sim-threads): AUTO, and 0, 1, 2, 3, 5, 7, 15, 23, 31
// and 63 (SimPool::kMaxWorkers) workers; the state hashes must agree every
// tick, and so must every unit's aura buffs, bit for bit: the state hash does
// not fold atkBuff / armBuff, and nothing in this world turns a buff into
// hashed state. The aura tiles are the one pass whose chunk size follows the
// width (tiles / (threads * 8), World::applyAuraTiles): this world's 96 tiles
// fold in chunks of 6, 4, 3, 2 and 1 tiles across those widths. A debug
// build also sets TAK_VERIFY_POOL, which redoes each pooled pass serially and
// aborts on any difference in its lists, sums, chains or stamps.
//
// Then the width itself (World::simPool): AUTO is read once and kept, whatever
// SimPool::defaultWorkers() says later (a CPU mask that changes mid-game), also
// over restoreFrom, until setSimWorkers chooses again; and a pool with an
// asynchronous batch open (Legion's tile prefetch) is not replaced by a new
// width before the batch is joined.
#include "sim/sim.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <vector>
#if defined(__linux__)
#include <sched.h>
#endif

namespace tak::sim {
struct RetailReplayProbe {
    static SimPool& pool(World& w) { return w.simPool(); }
};
}  // namespace tak::sim

namespace {
[[maybe_unused]] void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}
// Every unit's aura buffs, bit for bit (as aura_recipient_test's buffDigest).
uint64_t buffDigest(const tak::sim::World& w) {
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](uint32_t v) { h = (h ^ v) * 1099511628211ULL; };
    for (const tak::sim::Unit& u : w.units()) { mix(std::bit_cast<uint32_t>(u.atkBuff)); mix(std::bit_cast<uint32_t>(u.armBuff)); }
    return h;
}
// Make SimPool::defaultWorkers() give another count than `was`, as a CPU mask
// that changes during a game does: a debug build through TAK_SIM_WORKERS, a
// release build on Linux by narrowing this thread's mask to one CPU (so from 8
// usable CPUs up). False where neither can change it.
bool changeDefaultWorkers(unsigned was) {
#ifndef NDEBUG
    setEnv("TAK_SIM_WORKERS", was == 5 ? "6" : "5");
#elif defined(__linux__)
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof(set), &set) == 0)
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
            if (CPU_ISSET(cpu, &set)) {
                cpu_set_t one;
                CPU_ZERO(&one);
                CPU_SET(cpu, &one);
                sched_setaffinity(0, sizeof(one), &one);
                break;
            }
#endif
    return tak::sim::SimPool::defaultWorkers() != was;
}
}  // namespace

int main() {
#ifndef NDEBUG
    setEnv("TAK_VERIFY_POOL", "1");
#endif
    using namespace tak::sim;
    UnitType soldier{}, big{}, shaman{}, lode{}, reclaimer{}, flyer{};
    auto mobile = [](UnitType& t, const char* id, int foot) {
        t.name = t.id = id; t.maxHp = 100; t.maxVel = Fixed::fromInt(1); t.canMove = true;
        t.footX = t.footZ = foot; t.sight = 160;
    };
    mobile(soldier, "soldier", 1);
    mobile(big, "big", 2);
    mobile(shaman, "shaman", 1);
    shaman.auras = {Aura{Aura::Kind::Armor, 1.25f, false, 96, 0.5f}, Aura{Aura::Kind::Attack, 0.8f, true, 80, 1.0f}};
    mobile(reclaimer, "reclaimer", 1);
    reclaimer.canReclaim = true;
    mobile(flyer, "flyer", 1);
    flyer.canFly = true;
    lode.name = lode.id = "lode"; lode.maxHp = 400; lode.maxVel = Fixed(); lode.footX = lode.footZ = 2;
    lode.income = 1.5f; lode.storage = 200;
    const UnitType* types[] = {&soldier, &soldier, &soldier, &big, &shaman, &reclaimer, &flyer, &lode};
    std::vector<uint8_t> terrain(160 * 160);
    for (int z = 0; z < 160; ++z) for (int x = 0; x < 160; ++x) terrain[size_t(z) * 160 + x] = uint8_t((x * 5 + z * 3) % 40);
    // -1: serial (setSerialThreads); kAuto: AUTO, the width nothing chose (the last world).
    constexpr int kAuto = -2;
    const int widths[] = {-1, 0, 1, 2, 3, 5, 7, 15, 23, 31, int(SimPool::kMaxWorkers), kAuto};
    constexpr int kWorlds = int(std::size(widths));
    std::unique_ptr<World> worlds[kWorlds];
    for (int w = 0; w < kWorlds; ++w) {
        auto& world = worlds[w];
        world = std::make_unique<World>();
        world->setPlayerCount(4); world->setTerrain(terrain, 160, 160, 30);
        world->setTeam(1, 0);
        if (widths[w] == -1) world->setSerialThreads(true); else if (widths[w] != kAuto) world->setSimWorkers(widths[w]);
        for (int i = 0; i < 2600; ++i) {
            const UnitType* t = types[size_t(i) % 8];
            const int id = world->spawn(t, float(64 + (i * 37) % 2300), float(64 + (i * 53) % 2300), 0.0f, i % 4);
            if (Unit* u = world->unit(id); u && t == &lode && i % 3 == 0) u->underConstruction = true;
        }
    }
    // One tick of every world (orders every 20th step); false, with the width that
    // differs from the serial world named, on a state hash or a buff bit apart.
    int step = 0;
    auto tickAll = [&] {
        uint64_t first = 0, firstBuffs = 0;
        for (int w = 0; w < kWorlds; ++w) {
            World& world = *worlds[w];
            if (step % 20 == 0)
                for (size_t i = 0; i < world.units().size(); i += 3) {
                    const Unit& u = world.units()[i];
                    if (u.alive() && u.type && !u.type->isStructure())
                        world.order(u.id, float(64 + (i * 91 + size_t(step) * 13) % 2300),
                                    float(64 + (i * 29 + size_t(step) * 7) % 2300), false);
                }
            world.tick(1.f / 30);
            const uint64_t h = world.stateHash(), buffs = buffDigest(world);
            if (w == 0) { first = h; firstBuffs = buffs; }
            else if (h != first || buffs != firstBuffs) {
                std::fprintf(stderr, "workers=%d differs from serial at step %d (%s)\n", widths[w], step,
                             h != first ? "state hash" : "aura buffs");
                return false;
            }
        }
        ++step;
        return true;
    };
    size_t buffed = 0, debuffed = 0;   // the most units under a buff / a debuff on one tick
    while (step < 120) {
        if (!tickAll()) return 1;
        size_t up = 0, down = 0;
        for (const Unit& u : worlds[0]->units()) { up += u.armBuff > 1; down += u.atkBuff < 1; }
        buffed = std::max(buffed, up); debuffed = std::max(debuffed, down);
    }
    if (buffed < 100 || debuffed < 100) {   // or the buff comparison above compared nothing
        std::fprintf(stderr, "the auras reached too few units (%zu buffed, %zu debuffed)\n", buffed, debuffed);
        return 1;
    }
    const uint64_t hash = worlds[0]->stateHash(), buffs = buffDigest(*worlds[0]);

    // A width change while a batch is open waits for the join (Legion starts a
    // batch on one simPool() call and helps and joins it through later ones).
    {
        World world;
        world.setSimWorkers(2);
        SimPool& open = RetailReplayProbe::pool(world);
        std::atomic<unsigned> ran{0};
        auto job = [&](size_t, unsigned) { ran.fetch_add(1, std::memory_order_relaxed); };
        open.start(64, job);
        world.setSimWorkers(5);
        SimPool& during = RetailReplayProbe::pool(world);
        if (&during != &open) { std::fprintf(stderr, "a pool with a batch open was replaced\n"); return 1; }
        during.help();
        RetailReplayProbe::pool(world).finish();
        if (ran.load() != 64 || RetailReplayProbe::pool(world).workers() != 5) {
            std::fprintf(stderr, "after the join: %u of 64 jobs ran, %u workers (want 5)\n", ran.load(),
                         RetailReplayProbe::pool(world).workers());
            return 1;
        }
    }

    // AUTO is this World's for good once its pool is built.
    World& autoWorld = *worlds[kWorlds - 1];
    const unsigned was = RetailReplayProbe::pool(autoWorld).workers();
    if (was != SimPool::defaultWorkers()) {
        std::fprintf(stderr, "AUTO runs %u workers, the default is %u\n", was, SimPool::defaultWorkers());
        return 1;
    }
    World fresh;                                  // AUTO not read yet, here or in its copy
    const std::unique_ptr<World> unread = fresh.clone();
    if (RetailReplayProbe::pool(fresh).workers() != was) { std::fprintf(stderr, "a new World's AUTO\n"); return 1; }
    const char* autoKept = "not checked here (nothing can change the default)";
    if (changeDefaultWorkers(was)) {
        const unsigned now = SimPool::defaultWorkers();
        for (int i = 0; i < 4; ++i) if (!tickAll()) return 1;
        fresh.restoreFrom(*unread);               // a copy that never read AUTO does not make this World read it again
        if (RetailReplayProbe::pool(autoWorld).workers() != was || RetailReplayProbe::pool(fresh).workers() != was) {
            std::fprintf(stderr, "AUTO followed the default from %u to %u workers in a running World\n", was, now);
            return 1;
        }
        fresh.setSimWorkers(-1);                  // AUTO chosen again: read again
        if (RetailReplayProbe::pool(fresh).workers() != now) {
            std::fprintf(stderr, "AUTO chosen again must read the default again (%u workers)\n", now);
            return 1;
        }
        autoKept = "kept when the default changed";
    }
    std::printf("pooled passes match serial at AUTO and 0/1/2/3/5/7/15/23/31/63 workers (hash %016llx, buffs %016llx, "
                "most units buffed %zu, debuffed %zu); an open batch keeps its pool; AUTO's width %s\n",
                (unsigned long long)hash, (unsigned long long)buffs, buffed, debuffed, autoKept);
}
