// Stable generational ownership of per-unit render cache entries
// (client/geometryowners.h, rendering workstream C). A unit keeps one entry
// across painter-order changes and short absences; destroyed units, reused ids,
// replacement models and renderer resets never hand stale content to anyone;
// retained memory is bounded and returned after demand falls; payload pointers
// survive pool growth and are safe for one worker each.
#include "client/geometryowners.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

// Stands in for GameView::UnitGeom: a key (cached content) and bulk data.
struct Payload {
    std::vector<int> key;
    std::vector<char> data;
    int writes = 0;
    void makeCold() { key.clear(); data.clear(); }
    size_t retainedBytes() const { return sizeof(*this) + key.capacity() * sizeof(int) + data.capacity(); }
    void releaseMemory() { std::vector<int>().swap(key); std::vector<char>().swap(data); }
};
using Pool = tak::render::StableEntryPool<Payload, 16>;   // small chunks: exercise growth/trim
using tak::render::UnitOwner;

int typeA, typeB, modelX;   // addresses only
UnitOwner unit(int id, uint32_t epoch = 1, const void* type = &typeA, const void* model = nullptr) {
    return {id, epoch, type, model};
}
// "Build" an entry: give it content tied to its owner.
void build(Payload* p, int id) { p->key.assign({id, 7}); p->data.assign(1000, char(id)); }
bool built(const Payload* p, int id) { return p->key.size() == 2 && p->key[0] == id; }
const auto allAlive = [](const UnitOwner&) { return true; };
}

int main() {
    using namespace tak::render;
    {   // HandleSlots: lowest index first, generations never repeat, trailing trim.
        HandleSlots slots(8);
        RenderHandle h[4];
        for (auto& x : h) x = slots.acquire();
        slots.release(h[2]); slots.release(h[0]);
        const RenderHandle a = slots.acquire();
        check(a.index == 0, "free indices are reused lowest first");
        slots.release(h[3]); slots.release(h[1]); slots.release(a);
        check(slots.trimTrailing() == 0 && slots.allocated() == 0, "all-free tail trims to empty");
        std::set<uint32_t> seen{h[0].generation, h[1].generation, h[2].generation, h[3].generation, a.generation};
        const RenderHandle b = slots.acquire();
        check(b.index == 0 && !seen.count(b.generation), "recreated index gets a never-used generation");
        check(!slots.valid(h[0]) && !slots.valid(a) && slots.valid(b), "stale handles stay stale after trim");
        const RenderHandle c = slots.acquire(), d = slots.acquire();
        slots.release(c);
        check(slots.trimTrailing() == 3 && slots.valid(d), "trim stops at a live index");
    }
    {   // OwnerIndex against a reference map, with deletions inside probe runs.
        OwnerIndex index;
        std::unordered_map<int, uint32_t> ref;
        std::mt19937 rng(12345);
        bool ok = true;
        for (int step = 0; step < 200000; ++step) {
            const int id = int(rng() % 3000) * ((rng() & 1) ? 1 : 64) + 1;   // clustered + spread ids
            if (rng() % 3) { const uint32_t v = rng() % 100000; index.insert(id, v); ref[id] = v; }
            else { const bool erased = index.erase(id); ok &= erased == (ref.erase(id) == 1); }
            if (step % 5000 == 0) index.compact();
        }
        for (const auto& [id, v] : ref) ok &= index.find(id) == v;
        for (int id = -5; id < 200000; id += 7) ok &= (index.find(id) != OwnerIndex::kNone) == (ref.count(id) == 1);
        check(ok && index.size() == ref.size(), "id index matches a reference map");
        for (const auto& [id, v] : ref) index.erase(id);
        index.compact();
        check(index.size() == 0 && index.bytes() <= 64 * (sizeof(int) + sizeof(uint32_t)), "id index shrinks when empty");
    }
    {   // Reordering: depth-order changes never move a unit to another entry.
        Pool pool;
        pool.beginFrame(0);
        Payload* p1 = pool.claim(unit(1)); Payload* p2 = pool.claim(unit(2)); Payload* p3 = pool.claim(unit(3));
        build(p1, 1); build(p2, 2); build(p3, 3);
        pool.reclaim(allAlive);
        pool.beginFrame(16);
        Payload* q3 = pool.claim(unit(3)); Payload* q1 = pool.claim(unit(1)); Payload* q2 = pool.claim(unit(2));
        pool.reclaim(allAlive);
        check(q1 == p1 && q2 == p2 && q3 == p3, "reordered units keep their entries");
        check(built(q1, 1) && built(q2, 2) && built(q3, 3), "reordered units keep their content");
        check(pool.stats().acquired == 0 && pool.stats().visible == 3, "no acquisition on a reorder");
        // A unit entering AHEAD of the others in depth order evicts nobody.
        pool.beginFrame(32);
        Payload* n = pool.claim(unit(9));
        check(pool.claim(unit(3)) == p3 && pool.claim(unit(1)) == p1 && pool.claim(unit(2)) == p2,
              "a newcomer ahead in depth order does not displace existing entries");
        check(n && n != p1 && n != p2 && n != p3 && n->key.empty(), "the newcomer gets its own cold entry");
        pool.reclaim(allAlive);
        // Duplicate claim in one frame is refused (two workers must not share an entry).
        pool.beginFrame(48);
        check(pool.claim(unit(1)) == p1 && pool.claim(unit(1)) == nullptr && pool.stats().duplicates == 1,
              "second claim of one owner in a frame is refused");
        check(pool.claim(unit(1, 2)) == nullptr, "an identity change after a claim this frame is refused too");
        check(built(p1, 1), "a refused claim leaves the claimed entry intact");
    }
    {   // Visibility churn: short absences retain, long ones expire; budgets bound the retained set.
        EntryPoolConfig cfg; cfg.retainMs = 1000; cfg.retainMinFrames = 5;
        Pool pool(cfg);
        uint64_t now = 0;
        pool.beginFrame(now);
        Payload* p = pool.claim(unit(1)); build(p, 1);
        Payload* stay = pool.claim(unit(2)); build(stay, 2);
        pool.reclaim(allAlive);
        for (int f = 0; f < 10; ++f) {   // unit 1 out of view for 10 frames (~160 ms)
            now += 16; pool.beginFrame(now);
            pool.claim(unit(2));
            pool.reclaim(allAlive);
        }
        check(pool.stats().retained == 1 && pool.stats().visible == 1, "absent unit is retained");
        now += 16; pool.beginFrame(now);
        check(pool.claim(unit(1)) == p && built(p, 1), "returning unit gets its entry and content back");
        pool.reclaim(allAlive);
        for (int f = 0; f < 70; ++f) {   // > retainMs and > retainMinFrames
            now += 16; pool.beginFrame(now);
            pool.claim(unit(2));
            pool.reclaim(allAlive);
        }
        check(pool.stats().retained == 0, "a long absence expires the entry");
        now += 16; pool.beginFrame(now);
        Payload* back = pool.claim(unit(1));
        check(back && back->key.empty(), "after expiry the unit starts cold");
        pool.reclaim(allAlive);
        // A long hitch (time jumps) does not purge entries absent for a single frame.
        now += 5000; pool.beginFrame(now);
        pool.claim(unit(2));
        pool.reclaim(allAlive);
        check(pool.stats().retained == 1 && pool.stats().releasedExpired == 0,
              "retainMinFrames protects against one slow frame");

        // Byte budget: oldest invisible first.
        EntryPoolConfig small; small.retainBytes = 3 * (sizeof(Payload) + 1000 + 2 * sizeof(int)) + 100;
        Pool budget(small);
        budget.beginFrame(0);
        for (int id = 1; id <= 6; ++id) build(budget.claim(unit(id)), id);
        budget.reclaim(allAlive);
        budget.beginFrame(10);
        for (int id = 1; id <= 6; ++id) if (id > 3) budget.claim(unit(id));   // 1-3 leave first
        budget.reclaim(allAlive);
        budget.beginFrame(20);
        budget.reclaim(allAlive);                                             // now 4-6 leave too
        check(budget.stats().retained == 3 && budget.stats().releasedBudget == 3,
              "byte budget releases the oldest absent entries");
        budget.beginFrame(30);
        Payload* r4 = budget.claim(unit(4)); Payload* r1 = budget.claim(unit(1));
        check(built(r4, 4) && r1->key.empty(), "newest absent entries survive the budget, oldest start cold");
        budget.reclaim(allAlive);
        EntryPoolConfig few; few.retainEntries = 2;
        Pool counted(few);
        counted.beginFrame(0);
        for (int id = 1; id <= 5; ++id) build(counted.claim(unit(id)), id);
        counted.reclaim(allAlive);
        counted.beginFrame(10);
        counted.reclaim(allAlive);
        check(counted.stats().retained == 2 && counted.stats().releasedBudget == 3, "entry-count budget");
    }
    {   // Destruction: released at once, cold, recycled without inheritance.
        Pool pool;
        std::set<int> dead;
        const auto alive = [&](const UnitOwner& o) { return !dead.count(o.id); };
        pool.beginFrame(0);
        Payload* p = pool.claim(unit(5)); build(p, 5);
        const RenderHandle h5 = pool.handleOf(5);
        pool.reclaim(alive);
        dead.insert(5);
        pool.beginFrame(16);
        pool.reclaim(alive);
        check(pool.stats().releasedDestroyed == 1 && !pool.valid(h5), "destroyed unit's entry is released");
        check(p->key.empty(), "released entry is cold at once");
        pool.beginFrame(32);
        Payload* q = pool.claim(unit(6));
        check(q == p && q->key.empty(), "recycled entry (warm memory) starts cold");
        check(pool.handleOf(6).generation != h5.generation, "recycled entry carries a new generation");
        pool.reclaim(alive);
    }
    {   // ID / generation reuse, replacement models, type change.
        Pool pool;
        pool.beginFrame(0);
        Payload* p = pool.claim(unit(7)); build(p, 7);
        const RenderHandle h = pool.handleOf(7);
        pool.reclaim(allAlive);
        pool.beginFrame(16);
        Payload* q = pool.claim(unit(7, 2));   // world reset: id 7 is another unit
        check(q && q->key.empty() && pool.stats().replaced == 1, "reused id in a new epoch starts cold");
        check(!pool.valid(h) && pool.valid(pool.handleOf(7)) && pool.handleOf(7).generation != h.generation,
              "old handle is stale after id reuse");
        build(q, 7);
        pool.reclaim(allAlive);
        pool.beginFrame(32);
        Payload* r = pool.claim(unit(7, 2, &typeA, &modelX));   // replacement model
        check(r && r->key.empty() && pool.stats().replaced == 1, "replacement model starts cold");
        build(r, 7);
        pool.reclaim(allAlive);
        pool.beginFrame(48);
        Payload* s = pool.claim(unit(7, 2, &typeB, &modelX));   // type changed under the same id
        check(s && s->key.empty() && pool.stats().replaced == 1, "type change starts cold");
        pool.reclaim(allAlive);
        // The alive() predicate sees identity, so a retained entry of a replaced
        // unit is released even before the unit is in view again.
        build(s, 7);
        const auto aliveAsOther = [](const UnitOwner& o) { return o.type == &typeA; };
        pool.beginFrame(64);
        pool.reclaim(aliveAsOther);
        check(pool.stats().releasedDestroyed == 1 && s->key.empty(), "retained entry of a changed identity is released");
    }
    {   // Renderer reset: ownership survives, content does not.
        Pool pool;
        pool.beginFrame(0);
        Payload* p = pool.claim(unit(1)); build(p, 1);
        Payload* r = pool.claim(unit(2)); build(r, 2);
        pool.reclaim(allAlive);
        pool.beginFrame(16);
        pool.claim(unit(1));
        pool.reclaim(allAlive);   // unit 2 retained
        pool.makeAllCold();
        pool.beginFrame(32);
        check(pool.claim(unit(1)) == p && p->key.empty(), "visible entry kept but cold after reset");
        check(pool.claim(unit(2)) == r && r->key.empty(), "retained entry kept but cold after reset");
        check(pool.stats().acquired == 0, "reset acquires nothing");
        pool.reclaim(allAlive);
    }
    {   // Pointer stability across growth; one worker per entry.
        Pool pool;
        pool.beginFrame(0);
        std::vector<Payload*> first;
        for (int id = 1; id <= 10; ++id) { first.push_back(pool.claim(unit(id))); build(first.back(), id); }
        std::vector<Payload*> all = first;
        for (int id = 11; id <= 2000; ++id) all.push_back(pool.claim(unit(id)));   // many new chunks
        bool ok = true;
        for (int id = 1; id <= 10; ++id) ok &= built(first[size_t(id - 1)], id);
        check(ok && pool.chunks() >= 2000 / 16, "growth keeps earlier payloads in place");
        std::set<Payload*> distinct(all.begin(), all.end());
        check(distinct.size() == all.size() && !distinct.count(nullptr), "every unit has a distinct entry");
        pool.reclaim(allAlive);
        // Workers write their own entries concurrently (ThreadSanitizer/ASan builds
        // see any overlap); then the render thread checks every entry.
        std::vector<std::thread> threads;
        const size_t workers = 4;
        for (size_t w = 0; w < workers; ++w)
            threads.emplace_back([&, w] {
                for (size_t i = w; i < all.size(); i += workers) { build(all[i], int(i + 1)); ++all[i]->writes; }
            });
        for (auto& t : threads) t.join();
        ok = true;
        for (size_t i = 0; i < all.size(); ++i) ok &= built(all[i], int(i + 1)) && all[i]->writes == 1;
        check(ok, "parallel per-entry writes land in their own entries");
        pool.beginFrame(16);
        ok = true;
        for (int id = 1; id <= 2000; ++id) {
            Payload* p = pool.claim(unit(id));
            ok &= p == all[size_t(id - 1)] && pool.ownerOf(p) && pool.ownerOf(p)->id == id;
        }
        check(ok, "next frame resolves the same entries; ownerOf agrees");
        pool.reclaim(allAlive);
    }
    {   // Memory recovery: after the crowd leaves, entries expire, trim and the tail frees.
        EntryPoolConfig cfg; cfg.retainMs = 100; cfg.retainMinFrames = 2; cfg.freeTrimMs = 200; cfg.freeBytes = 64 * 1024;
        Pool pool(cfg);
        uint64_t now = 0;
        pool.beginFrame(now);
        for (int id = 1; id <= 500; ++id) build(pool.claim(unit(id)), id);
        pool.reclaim(allAlive);
        const uint32_t peak = pool.allocated();
        size_t heldPeak = 0;
        pool.forEachPayload([&](const Payload& p) { heldPeak += p.retainedBytes(); });
        for (int f = 0; f < 40; ++f) { now += 16; pool.beginFrame(now); pool.claim(unit(1)); pool.reclaim(allAlive); }
        size_t held = 0;
        pool.forEachPayload([&](const Payload& p) { held += p.retainedBytes(); });
        check(pool.stats().retained == 0 && pool.stats().visible == 1, "absent crowd expired");
        check(pool.allocated() <= 16 && peak >= 500, "trailing chunks are deallocated");
        check(held * 20 < heldPeak, "retained bytes return after demand falls");
        check(pool.stats().indexBytes <= 64 * (sizeof(int) + sizeof(uint32_t)), "id index shrinks");
        // Warm free budget trims released capacity beyond freeBytes even before freeTrimMs.
        EntryPoolConfig warm; warm.freeBytes = 4 * 1100; warm.freeTrimMs = 1u << 30;
        Pool w(warm);
        std::set<int> dead;
        const auto alive = [&](const UnitOwner& o) { return !dead.count(o.id); };
        w.beginFrame(0);
        for (int id = 1; id <= 20; ++id) build(w.claim(unit(id)), id);
        w.reclaim(alive);
        for (int id = 1; id <= 20; ++id) dead.insert(id);
        w.beginFrame(10);
        w.reclaim(alive);
        check(w.stats().releasedDestroyed == 20 && w.stats().warmFreeBytes <= warm.freeBytes &&
              w.stats().trimmed >= 16, "warm free capacity is bounded");
    }
    {   // Randomized model check against an independent owner model.
        EntryPoolConfig cfg; cfg.retainMs = 64; cfg.retainMinFrames = 3; cfg.retainBytes = 40 * 1200;
        cfg.freeTrimMs = 128; cfg.freeBytes = 16 * 1200;
        Pool pool(cfg);
        std::mt19937 rng(777);
        uint32_t epoch = 1;
        std::map<int, const void*> aliveType;      // id -> type, this epoch
        int nextId = 1;
        bool ok = true;
        uint32_t maxLive = 0;
        size_t peakVisibleRetained = 0;
        for (int frame = 0; frame < 3000; ++frame) {
            // Events: spawns, deaths, type swaps, an occasional world reset.
            if (frame % 977 == 976) { ++epoch; aliveType.clear(); nextId = 1; }
            for (int s = 0; s < 3; ++s) aliveType[nextId++] = (rng() & 1) ? &typeA : &typeB;
            for (int d = 0; d < 2 && !aliveType.empty(); ++d) {
                auto it = aliveType.begin(); std::advance(it, rng() % aliveType.size()); aliveType.erase(it);
            }
            if (!aliveType.empty() && rng() % 10 == 0) {
                auto it = aliveType.begin(); std::advance(it, rng() % aliveType.size());
                it->second = it->second == &typeA ? &typeB : &typeA;
            }
            pool.beginFrame(uint64_t(frame) * 16);
            std::vector<std::pair<int, Payload*>> claimed;
            std::set<Payload*> used;
            for (const auto& [id, type] : aliveType) {
                if (rng() % 3 == 0) continue;   // out of view this frame
                const UnitOwner o = unit(id, epoch, type);
                Payload* p = pool.claim(o);
                ok &= p != nullptr && used.insert(p).second;
                if (!p) continue;
                // Content is either absent (cold) or exactly this owner's.
                ok &= p->key.empty() || (p->key.size() == 3 && p->key[0] == id && p->key[1] == int(epoch) &&
                                         p->key[2] == (type == &typeA ? 1 : 2));
                p->key.assign({id, int(epoch), type == &typeA ? 1 : 2});
                p->data.assign(size_t(500 + rng() % 1000), 0);
                claimed.emplace_back(id, p);
            }
            pool.reclaim([&](const UnitOwner& o) {
                auto it = aliveType.find(o.id);
                return o.epoch == epoch && it != aliveType.end() && it->second == o.type;
            });
            for (const auto& [id, p] : claimed) ok &= pool.ownerOf(p) && pool.ownerOf(p)->id == id;
            maxLive = std::max(maxLive, pool.live());
            peakVisibleRetained = std::max(peakVisibleRetained, size_t(pool.stats().visible + pool.stats().retained));
            ok &= pool.stats().retainedBytes <= cfg.retainBytes && pool.stats().warmFreeBytes <= cfg.freeBytes;
            ok &= pool.live() == pool.stats().visible + pool.stats().retained;
        }
        check(ok, "randomized frames: distinct entries, no foreign content, budgets hold");
        check(pool.allocated() <= ((peakVisibleRetained + pool.stats().warmFree + 15) / 16 + 4) * 16 ||
              pool.allocated() <= maxLive + 64 + 16 * 4,
              "index space tracks the live set, not every unit ever spawned");
        std::printf("randomized: %d ids spawned in the last epoch, peak live %u, allocated %u\n",
                    nextId - 1, maxLive, pool.allocated());
    }
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("geometry_owners: all checks passed\n");
    return 0;
}
