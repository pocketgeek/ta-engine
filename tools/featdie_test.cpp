// featdie_test -- a feature destroyed by weapon damage plays its seqnamedie
// before its featuredead stage replaces it (icd 494ce0 mode 0, 4959c0's
// sprite branch, 495ef7; docs/retail-engine.md "Not a fade").
//
//   * The playback lasts the sequence's frames at max(1, delay) ticks each
//     (5373d0), read from the feature's own GAF; the shipped trees are 10
//     frames of 2 ticks.
//   * Meanwhile the feature keeps its cells and blocking, takes no damage,
//     cannot catch fire (494b6d, 49515d) and is not a reclaim target (496ea0:
//     the object's +0x5d bit 0x10 is clear).
//   * A burning feature takes no damage either (4962b6..4962be).
//   * Without a seqnamedie the replacement is at once (494ee3).
//   * The client's frame clock (client/featuredie.h) steps the same way.
// With a data directory it also checks the shipped timings.

#include "client/featuredie.h"
#include "gaf/featureburntiming.h"
#include "hpi/hpi.h"
#include "sim/sim.h"

#include <cstdio>
#include <string>

namespace tak::sim {
struct RetailReplayProbe {
    static void hit(World& w, const Weapon& weapon, float x, float z) {
        w.applyHit(weapon, x, z, 0, 0, nullptr);
    }
    static void ignite(World& w, int id) { w.igniteFeature(w.features_.at(w.featureIdx_.at(id))); }
    static void setDieLeft(World& w, int id, uint32_t left) {
        w.features_.at(w.featureIdx_.at(id)).dieLeft = left;
    }
    static bool reclaimTarget(const World& w, int id) { return w.reclaimTargetValid(id); }
};
}

using namespace tak::sim;

static int g_fail = 0;
static void check(bool cond, const char* what) {
    std::printf("  %-70s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ++g_fail;
}

// tree (0) -> stump (1) -> gone; a rock (2) with no seqnamedie.
static void build(World& w) {
    w.setTerrain(std::vector<uint8_t>(16 * 16, 0), 16, 16, 0);
    FeatType tree; tree.name = "tree"; tree.hp = 100; tree.dieTicks = 20; tree.deadType = 1;
    tree.blocking = true; tree.reclaimable = true; tree.energy = 10;
    tree.flamable = true; tree.hasBurnAnim = true; tree.burnTicks = 40; tree.burntType = 1;
    tree.spreadChance = 100; tree.sparkTicks = 2;
    FeatType stump; stump.name = "stump"; stump.hp = 50; stump.dieTicks = 6;
    stump.blocking = false; stump.reclaimable = true;
    FeatType rock; rock.name = "rock"; rock.hp = 100; rock.blocking = true; rock.reclaimable = true;
    w.setFeatureTypes({tree, stump, rock});
    w.addFeature(5 * 16 + 5, 5 * 16 + 8, 5 * 16 + 8, 10, 1, 1, true, 0);   // id 85
    w.addFeature(5 * 16 + 6, 6 * 16 + 8, 5 * 16 + 8, 10, 1, 1, true, 0);   // id 86, its neighbour
    w.addFeature(9 * 16 + 9, 9 * 16 + 8, 9 * 16 + 8, 10, 1, 1, true, 2);   // id 153, a rock
}

int main(int argc, char** argv) {
    std::printf("featdie_test\n");
    const float tx = 5 * 16 + 8, tz = 5 * 16 + 8;
    Weapon blow; blow.name = "blow"; blow.damage = 100;
    Weapon chip; chip.name = "chip"; chip.damage = 30;
    Weapon torch; torch.name = "torch"; torch.damage = 1; torch.fireStarter = true;
    {
        World w; build(w);
        for (int i = 0; i < 5; ++i) w.tick(1.0f / 30);
        check(w.featureReclaimable(*w.feature(85)), "a standing tree is a reclaim target");
        RetailReplayProbe::hit(w, blow, tx, tz);
        const Feature* f = w.feature(85);
        check(f->alive && f->type == 0 && f->dieLeft == 20 && f->blocks,
              "a lethal hit starts the 20-tick death; the tree keeps its type and blocking");
        check(!w.featureReclaimable(*f) && !RetailReplayProbe::reclaimTarget(w, 85),
              "a dying tree is not a reclaim target");
        RetailReplayProbe::hit(w, blow, tx, tz);
        RetailReplayProbe::hit(w, torch, tx, tz);
        RetailReplayProbe::ignite(w, 85);
        check(f->dieLeft == 20 && f->dmg == 100 && !f->burn,
              "a dying tree takes no damage and cannot catch fire");
        for (int i = 0; i < 19; ++i) w.tick(1.0f / 30);
        check(f->alive && f->type == 0 && f->dieLeft == 1 && f->blocks,
              "19 ticks later it is still the tree, on its last frame");
        w.tick(1.0f / 30);
        check(f->alive && f->type == 1 && f->dieLeft == 0 && !f->blocks && f->dmg == 0,
              "the 20th tick replaces it with its featuredead stage");
        check(w.featureReclaimable(*f), "the featuredead stage is a reclaim target again");
        RetailReplayProbe::hit(w, chip, tx, tz);
        RetailReplayProbe::hit(w, chip, tx, tz);
        check(f->type == 1 && f->dieLeft == 6, "the stage dies in its turn with its own seqnamedie");
        for (int i = 0; i < 6; ++i) w.tick(1.0f / 30);
        check(!f->alive, "and with no featuredead of its own it is gone");
    }
    {
        World w; build(w);
        RetailReplayProbe::hit(w, blow, 9 * 16 + 8, 9 * 16 + 8);
        check(!w.feature(153)->alive && !w.feature(153)->dieLeft, "a rock with no seqnamedie goes at once");
    }
    {
        World w; build(w);
        RetailReplayProbe::ignite(w, 85);
        RetailReplayProbe::hit(w, blow, tx, tz);
        const Feature* f = w.feature(85);
        check(f->burn && f->type == 0 && f->dmg == 0 && !f->dieLeft, "a burning tree takes no weapon damage");
    }
    {
        // The neighbour dies while 85's single spark is due: it is passed over.
        World w; build(w);
        RetailReplayProbe::ignite(w, 85);
        RetailReplayProbe::hit(w, blow, 6 * 16 + 8, tz);
        w.tick(1.0f / 30); w.tick(1.0f / 30);
        check(w.feature(85)->burn && !w.feature(86)->burn && w.feature(86)->dieLeft,
              "a spark does not light a dying neighbour");
    }
    {
        World a, b; build(a); build(b);
        RetailReplayProbe::hit(a, blow, tx, tz); RetailReplayProbe::hit(b, blow, tx, tz);
        for (int i = 0; i < 7; ++i) { a.tick(1.0f / 30); b.tick(1.0f / 30); }
        const uint64_t mid = a.stateHash();
        check(mid == b.stateHash(), "two peers agree mid-death");
        RetailReplayProbe::setDieLeft(b, 85, a.feature(85)->dieLeft + 1);
        check(b.stateHash() != mid, "the death countdown is in the lockstep hash");
    }
    {
        // Client clock: ten frames of two ticks, ending at tick 120.
        std::vector<int> ends; for (int i = 1; i <= 10; ++i) ends.push_back(2 * i);
        bool ok = true;
        for (int k = 0; k < 20; ++k) {
            const int e = tak::featuredie::elapsed(20, 120, 100 + k);
            ok = ok && e == k && tak::featuredie::frame(ends, e) == size_t(k / 2);
        }
        check(ok, "the client draws frame k/2 at tick k of the death");
        check(tak::featuredie::elapsed(20, 120, 90) == 0 && tak::featuredie::frame(ends, 25) == 9,
              "an early snapshot shows frame 0; the last frame holds");
    }
    if (argc > 1) {
        auto vfs = tak::hpi::mountRetailRoot(argv[1]);
        tak::gaf::FeatureBurnTiming timing(vfs);
        auto node = [&](const char* path, const char* section) -> tak::tdf::Node {
            const auto bytes = vfs.read(path);
            const auto root = tak::tdf::parseText(std::string(bytes.begin(), bytes.end()), path);
            for (const auto& [name, child] : root.children)
                if (tak::hpi::MountSet::key(name) == section) return child;
            return {};
        };
        check(timing.dieDuration(node("features/aramon/aratree.tdf", "aratree01")) == 20,
              "AraTree01's AraTreeKill01: 10 frames of 2 ticks");
        check(timing.dieDuration(node("features/veruna/vertree.tdf", "vertree01")) == 20,
              "VerTree01's VerTreeKill01: 20 ticks");
        check(timing.dieDuration(node("features/zhon/zonrock.tdf", "zonrock01")) == 0,
              "a Zhon rock has no seqnamedie");
    }
    std::printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
