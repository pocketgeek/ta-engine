// featgen_test -- the contract behind World::featGeneration().
//
// The renderer's feature sync (GameView::syncBurningFeatures) used to take simMutex_
// and rescan every feature every cosmetic step to discover changes that, measured over
// 45s, happened on 0% of steps: ~1ms of pure LOCK WAIT per step for nothing (the scan
// itself was 0.012ms). It now skips both locked scans while featGeneration() is
// unchanged, which makes this counter load-bearing in two directions:
//
//   * A MISSED bump silently freezes feature visuals -- a tree that ignites, burns to
//     its next stage or is reclaimed away would never update on screen.
//   * A SPURIOUS bump (e.g. bumping every tick) puts the lock wait straight back and
//     the optimization is worth nothing.
//
// Both directions are asserted here, including ignition timestamps retained for
// viewers that skip simulation ticks. The test probe reaches private ignition.

#include "sim/sim.h"
#include "sim/matchsetup.h"
#include "hpi/hpi.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace tak::sim {
struct RetailReplayProbe {
    static void ignite(World& world,int id) {
        world.igniteFeature(world.features_.at(world.featureIdx_.at(id)));
    }
    static void visualIgnition(World& world,int id,uint32_t tick) {
        world.features_.at(world.featureIdx_.at(id)).burnStarted=tick;
    }
    static void visualBurnSequence(World& world,int id,uint64_t sequence) {
        world.features_.at(world.featureIdx_.at(id)).burnSequence=sequence;
    }
    static int grade(const World& world,int x,int z) { return world.mapFeatureGrade(x,z); }
    static void cache(World& world,int id) { world.prepareSearchGrade(id,false); }
    static int cached(const World& world,int x,int z) {
        const auto& plane=world.searchGrades_.front();
        return plane.cells.at(size_t(z)*plane.nav->width()+x)&7;
    }
    static void retire(World& world,int id) { world.retireCorpse(*world.unit(id)); }
    static void replace(World& world,int id,int type) {
        world.swapFeature(world.features_.at(world.featureIdx_.at(id)),type);
    }
    static void igniteIndex(World& world,size_t i) { world.igniteFeature(world.features_.at(i)); }
    static void replaceIndex(World& world,size_t i,int type) { world.swapFeature(world.features_.at(i),type); }
    static void fade(World& world,int id) { Unit caster;world.finishReclaim(caster,id); }
    static uint32_t fading(const World& world) { return world.fadingFeatures_; }
    // Set burning marks (0 on a base tree without them: the test builds there too).
    template <class W> static uint32_t marked(const W& world) {
        if constexpr (requires { world.burningMarked_; }) return world.burningMarked_;
        else return 0;
    }
    static void burning(World& world) { world.tickBurning(); }
    static void fades(World& world) { world.tickFeatureFades(); }
    // The full passes over features_ that tickBurning and tickFeatureFades made
    // before they walked marked indices (61f294ac), kept verbatim as the
    // reference the production walks must match step for step.
    static void referenceBurning(World& w) {
        if (w.featTypes_.empty()) return;
        for (size_t i = 0; i < w.features_.size(); ++i) {
            Feature& f = w.features_[i];
            if (!f.alive || !f.burn) continue;
            if (f.burnLeft <= 1) {
                f.burnLeft = 0;
                w.swapFeature(f, w.featTypes_[size_t(f.type)].burntType);
                continue;
            }
            --f.burnLeft;
            if (f.spreadIn > 0 && --f.spreadIn == 0) {
                int cx = f.x.floorInt() / 16 - f.fx / 2, cz = f.z.floorInt() / 16 - f.fz / 2;
                for (int dz = -3; dz <= 3; ++dz)
                    for (int dx = -3; dx <= 3; ++dx) {
                        if (dx == 0 && dz == 0) continue;
                        int nx = cx + dx, nz = cz + dz;
                        if (nx < 0 || nz < 0 || nx >= w.terW_ || nz >= w.terH_) continue;
                        auto it = w.featureIdx_.find(nz * w.terW_ + nx);
                        if (it == w.featureIdx_.end()) continue;
                        Feature& nf = w.features_[it->second];
                        if (!nf.alive || nf.burn || nf.fadeLeft || nf.dieLeft || nf.type < 0) continue;
                        const FeatType& nft = w.featTypes_[size_t(nf.type)];
                        if (!nft.flamable || !nft.hasBurnAnim) continue;
                        if (w.burnRand(100) < nft.spreadChance) w.igniteFeature(nf);
                    }
            }
        }
    }
    static void referenceFades(World& w) {
        if (!w.fadingFeatures_) return;
        for (auto& f:w.features_) {
            if (!f.fadeLeft || --f.fadeLeft) continue;
            --w.fadingFeatures_;
            if (!f.alive) continue;
            f.alive=false;
            w.removeMapFeature(footprintOrigin(f.x,f.fx),footprintOrigin(f.z,f.fz));
            w.bumpFeatGen();
            if (f.blocks)
                w.blockCells(footprintOrigin(f.x,f.fx),footprintOrigin(f.z,f.fz),f.fx,f.fz,false);
        }
    }
};
}

using tak::sim::World;

static int g_fail = 0;
static void check(bool cond, const char* what) {
    std::printf("  %-64s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ++g_fail;
}

int main() {
    std::printf("featgen_test\n");

    // A lobby registry rebuild may reuse a corpse-bearing unit's address for
    // a corpse-less drake. The stale mapping stayed invisible until its death,
    // when only the client gave it a permanent corpse (live replay tick 77549).
    for (int reset=0;reset<3;++reset) {
        using namespace tak::sim;
        World reused,fresh;
        UnitType type;type.id="old-building";type.maxHp=100;
        FeatType corpse;corpse.name="old-corpse";corpse.decomposeTicks=0;
        reused.setFeatureTypes({corpse});
        reused.mapCorpse(&type,0);reused.mapStatue(&type,0,0);
        type=UnitType{};type.id="corpse-less-flyer";type.maxHp=100;type.canFly=true;
        if (reset==0) reused.clearFeatures();
        else if (reset==1) reused.resetForReplay();
        else {
            tak::hpi::Vfs vfs;
            tak::tnt::Map map;
            registerMapFeatures(reused,map,vfs);
        }
        check(reused.corpseTypeOf(&type)==-1 && reused.statueTypeOf(&type,false)==-1 &&
              reused.statueTypeOf(&type,true)==-1,
              "feature rebuild/replay reset discards all old unit-type mappings");
        // Keep an unrelated permanent feature at index zero, as on a real map.
        reused.setFeatureTypes({corpse});fresh.setFeatureTypes({corpse});
        for (World* world:{&reused,&fresh}) {
            const int id=world->spawn(&type,128,128,0,0);
            world->unit(id)->hp={};
            world->tick(1.0f/30);
        }
        check(reused.units().front().corpseUntil==0 &&
              reused.stateHash()==fresh.stateHash(),
              "corpse-less death after registry reuse matches a fresh referee");
    }

    {
        using namespace tak::sim;
        World fire;
        FeatType tree;tree.name="tree";tree.flamable=true;tree.hasBurnAnim=true;tree.burnTicks=30;
        fire.setFeatureTypes({tree});
        fire.addFeature(77,100,100,10,1,1,false,0);
        for(int i=0;i<17;++i)fire.tick(1.0f/30);
        const auto generation=fire.featGeneration();
        RetailReplayProbe::ignite(fire,77);
        check(fire.feature(77)->burnStarted==17 && fire.feature(77)->burnSequence==1 &&
              fire.featGeneration()!=generation,
              "ignition preserves its tick and activation order for late viewers");
        for(int i=0;i<9;++i)fire.tick(1.0f/30);
        RetailReplayProbe::ignite(fire,77);
        check(fire.feature(77)->burnStarted==17,
              "repeated ignition and skipped visual ticks do not restart the clock");
        const auto hash=fire.stateHash();
        RetailReplayProbe::visualIgnition(fire,77,1000);
        check(fire.stateHash()==hash,"visual ignition timestamp does not affect lockstep hash");
        RetailReplayProbe::visualBurnSequence(fire,77,1000);
        check(fire.stateHash()==hash,"visual burn activation order does not affect lockstep hash");
        for(int i=0;i<20;++i)fire.tick(1.0f/30);
        check(fire.feature(77)->alive && fire.feature(77)->burnLeft==1,
              "burn remains present through the final authored display tick");
        fire.tick(1.0f/30);
        check(!fire.feature(77)->alive,"burn retires exactly at authored lifetime");
    }

    for (uint32_t lifetime : {1u,2u}) {
        using namespace tak::sim;
        World fire;
        fire.setTerrain(std::vector<uint8_t>(16*16,0),16,16,0);
        FeatType tree;tree.name="spark-tree";tree.flamable=true;tree.hasBurnAnim=true;
        tree.burnTicks=lifetime;tree.spreadChance=100;tree.sparkTicks=0;
        fire.setFeatureTypes({tree});
        fire.addFeature(85,88,88,10,1,1,false,0);
        fire.addFeature(86,104,88,10,1,1,false,0);
        RetailReplayProbe::ignite(fire,85);
        const auto firstOrder=fire.feature(85)->burnSequence;
        fire.tick(1.0f/30);
        check(bool(fire.feature(86)->burn)==(lifetime>1),
              lifetime==1 ? "expiry suppresses a simultaneous spark into the adjacent tree"
                          : "a spark before expiry still ignites the adjacent tree");
        if(lifetime>1)
            check(firstOrder<fire.feature(86)->burnSequence,
                  "spread ignition receives a newer active-list order than its source");
    }

    World w;
    // Headless: no fog work, no renderer. Matches how the referee runs.

    const uint32_t atStart = w.featGeneration();

    // Ticking an empty world must not move the counter. This is the property that makes
    // the render-side skip worth anything -- if idle ticks bumped it, the sync would
    // take the lock every step exactly as before.
    for (int i = 0; i < 30; ++i) w.tick(1.0f / 30.0f);
    check(w.featGeneration() == atStart,
          "an idle world does not bump the generation (no spurious rescans)");

    // Adding a feature must move it: the renderer has to notice a new feature to give
    // it a visual instance at all.
    w.addFeature(1, 100.0f, 100.0f, 10.0f, 1, 1, false, -1);
    const uint32_t afterOne = w.featGeneration();
    check(afterOne != atStart, "adding a feature bumps the generation");
    check(w.features().size() == 1, "and the feature is actually there");

    // Distinct adds must be distinguishable, not collapsed into one edge.
    w.addFeature(2, 200.0f, 200.0f, 10.0f, 1, 1, false, -1);
    check(w.featGeneration() != afterOne, "a second add bumps it again");

    // ...and ticking after the adds must go quiet again, so the sync settles back to
    // the lock-free path instead of staying permanently dirty.
    const uint32_t afterAdds = w.featGeneration();
    for (int i = 0; i < 30; ++i) w.tick(1.0f / 30.0f);
    check(w.featGeneration() == afterAdds,
          "ticking with features present still does not bump it");

    // Occupancy must follow runtime features, including replacement geometry.
    {
        using namespace tak::sim;
        World map;
        std::vector<uint8_t> heights(32*32,0);
        std::vector<uint16_t> cells(32*32,0xffff);
        map.setTerrain(heights,32,32,0);
        map.setMapPlacementFeatures(cells,{});
        FeatType statue;statue.name="statue";statue.fx=2;statue.fz=3;statue.blocking=true;
        FeatType stump;stump.name="stump";stump.fx=1;stump.fz=1;stump.blocking=true;
        FeatType wall;wall.name="wall";wall.blocking=true;wall.indestructible=true;
        FeatType cycle=statue;cycle.name="cycle";cycle.deadType=3;cycle.burntType=2;
        map.setFeatureTypes({statue,stump,wall,cycle});
        UnitType mover;mover.footX=mover.footZ=1;mover.maxHp=100;
        mover.maxVel=Fixed::fromInt(1);mover.canMove=true;
        const int unit=map.spawn(&mover,72,72,0,0);
        RetailReplayProbe::cache(map,unit);
        const int emptyGrade=RetailReplayProbe::cached(map,9,10);
        const int id=8*32+8;
        map.addFeature(id,8*16+16,8*16+24,0,2,3,true,0);
        check(RetailReplayProbe::grade(map,9,10)==1,
              "a newly created statue blocks its entire rectangular footprint");
        check(RetailReplayProbe::grade(map,10,10)==7,
              "new feature occupancy stops at its footprint edge");
        check(RetailReplayProbe::cached(map,9,10)==1,
              "creation updates an already active search cache");
        RetailReplayProbe::replace(map,id,1);
        check(RetailReplayProbe::cached(map,9,10)==emptyGrade,
              "shrinking a feature refreshes cached grades over its old footprint");
        check(RetailReplayProbe::grade(map,8,8)==1 && RetailReplayProbe::grade(map,9,10)==7,
              "a smaller replacement frees the old footprint tails");
        RetailReplayProbe::replace(map,id,3);
        check(RetailReplayProbe::grade(map,9,10)==0,
              "a cyclic replacement graph with an indestructible successor is not clearable");
        check(RetailReplayProbe::cached(map,9,10)==0,
              "replacement refreshes cached clearability as well as live occupancy");
        RetailReplayProbe::replace(map,id,-1);
        check(RetailReplayProbe::cached(map,9,10)==emptyGrade,
              "removal refreshes the active search cache");
        check(RetailReplayProbe::grade(map,8,8)==7 && RetailReplayProbe::grade(map,9,10)==7,
              "removal clears the expanded footprint");
        map.addFeature(id,8*16+8,8*16+8,0,1,1,true,2);
        const auto count=map.features().size();
        map.addFeature(id,8*16+16,8*16+24,0,2,3,true,0);
        check(map.features().size()==count && RetailReplayProbe::grade(map,8,8)==0,
              "placement cannot overwrite an indestructible feature");
        const int removable=8*32+7;
        map.addFeature(removable,7*16+8,8*16+8,0,1,1,true,1);
        map.addFeature(removable,7*16+16,8*16+24,0,2,3,true,0);
        check(!map.feature(removable)->alive && RetailReplayProbe::grade(map,7,8)==7 &&
              RetailReplayProbe::grade(map,8,8)==0,
              "a refused placement retains earlier removals in scan order");
    }

    {
        using namespace tak::sim;
        World map;
        map.setTerrain(std::vector<uint8_t>(32*32,100),32,32,0);
        map.setMapPlacementFeatures(std::vector<uint16_t>(32*32,0xffff),{});
        UnitType building;building.footX=building.footZ=2;building.maxHp=100;
        building.corpseAdjX=-1;building.corpseAdjZ=2;
        auto death=std::make_shared<tak::cob::File>();
        death->scripts={{"Killed",0}};
        death->code={0x10022000,0x10022000,0x10022000,
            0x10021001,1,0x10023002,1,0x10021001,0,0x10065000};
        building.simulationScript=death;
        FeatType wreck;wreck.name="wreck";wreck.fx=3;wreck.fz=1;wreck.blocking=true;
        map.setFeatureTypes({wreck});map.mapCorpse(&building,0);
        const int id=map.spawn(&building,176,176,0,0);
        map.unit(id)->hp=Fixed();map.tick(1.0f/30);
        check(RetailReplayProbe::grade(map,10,10)==7,
              "a dying building releases its original body footprint");
        check(map.unit(id)->corpseBlocks && RetailReplayProbe::grade(map,9,12)==1 &&
              RetailReplayProbe::grade(map,11,12)==1 && RetailReplayProbe::grade(map,9,13)==7,
              "the death lifecycle places the wreck's dimensions at the offset unit origin");
        map.unit(id)->corpseUntil=map.unit(id)->deadFor+1;
        map.tick(1.0f/30);
        check(!map.unit(id)->corpseBlocks && RetailReplayProbe::grade(map,11,12)==7,
              "decomposition clears the wreck footprint, including cells outside the old unit");
    }
    {
        using namespace tak::sim;
        World map;
        map.setTerrain(std::vector<uint8_t>(32*32,100),32,32,0);
        map.setMapPlacementFeatures(std::vector<uint16_t>(32*32,0xffff),{});
        UnitType mover;mover.footX=mover.footZ=1;mover.maxHp=100;
        mover.maxVel=Fixed::fromInt(1);mover.canMove=true;
        FeatType statue;statue.name="frozen";statue.fx=2;statue.fz=3;statue.blocking=true;
        statue.reclaimable=true;
        FeatType wall;wall.name="wall";wall.blocking=true;wall.indestructible=true;
        map.setFeatureTypes({statue,wall});map.mapStatue(&mover,-1,0);
        const int id=map.spawn(&mover,168,168,0,0);
        map.unit(id)->frozenFor=100;map.unit(id)->hp=Fixed();map.tick(1.0f/30);
        check(map.unit(id)->corpseBlocks && RetailReplayProbe::grade(map,11,12)==1,
              "a frozen mobile unit immediately places the statue's full footprint");
        map.addFeature(10*32+10,168,168,0,1,1,true,1);
        check(!map.unit(id)->corpseBlocks && RetailReplayProbe::grade(map,11,12)==7,
              "overwriting a statue retires its owner and frees the old footprint");
        RetailReplayProbe::retire(map,id);
        check(RetailReplayProbe::grade(map,10,10)==0,
              "retiring an old corpse cannot remove its replacement");
        const int refused=map.spawn(&mover,168,168,0,0);
        map.unit(refused)->frozenFor=100;map.unit(refused)->hp=Fixed();map.tick(1.0f/30);
        check(!map.unit(refused)->corpseBlocks && RetailReplayProbe::grade(map,10,10)==0,
              "an indestructible obstacle refuses statue placement without being cleared");
        const int reclaimable=map.spawn(&mover,248,168,0,0);
        map.unit(reclaimable)->frozenFor=100;map.unit(reclaimable)->hp=Fixed();map.tick(1.0f/30);
        // A mobile builder (World::reclaim's caster test): the clear is its
        // order, walk, work and fade, not the idle clear this used to ride on.
        UnitType builder=mover;builder.canReclaim=true;builder.isBuilder=true;
        const int worker=map.spawn(&builder,216,168,0,0);
        map.reclaim(worker,-reclaimable,false);
        for (int i=0;i<600 && map.unit(reclaimable)->corpseBlocks;++i) map.tick(1.0f/30);
        check(!map.unit(reclaimable)->corpseBlocks && RetailReplayProbe::grade(map,16,12)==7,
              "ordered corpse reclaim clears the actual statue footprint");
    }

    {
        using namespace tak::sim;
        World map;
        map.setTerrain(std::vector<uint8_t>(32*32,100),32,32,0);
        map.setMapPlacementFeatures(std::vector<uint16_t>(32*32,0xffff),{});
        UnitType mover;mover.footX=mover.footZ=1;mover.maxHp=100;
        mover.maxVel=Fixed::fromInt(1);mover.canMove=true;
        UnitType victim=mover;victim.corpseAdjZ=6;
        FeatType statue;statue.name="offset statue";statue.blocking=true;
        map.setFeatureTypes({statue});map.mapStatue(&victim,-1,0);
        const int observer=map.spawn(&mover,72,72,0,0);
        const int id=map.spawn(&victim,168,168,0,0);
        RetailReplayProbe::cache(map,observer);
        const int clear=RetailReplayProbe::cached(map,20,10);
        map.unit(id)->frozenFor=100;map.unit(id)->hp=Fixed();map.tick(1.0f/30);
        check(RetailReplayProbe::cached(map,10,10)==clear &&
              RetailReplayProbe::cached(map,10,16)==1,
              "an offset statue clears the old cached body and blocks its new anchor");
    }

    // Burning and fading walk marked feature indices instead of every feature.
    // Differential: two identically built worlds take the same scripted
    // ignitions, swaps (fires and fades put out early: stale marks) and fades
    // (burning ones included), then one runs the production walks and the
    // other the reference full passes. Every feature field, the RNG draws and
    // the state hash must agree after every step.
    for (uint32_t seed : {1u,2u,3u,4u}) {
        using namespace tak::sim;
        constexpr int kSide=48;
        auto build=[&](World& w) {
            w.setTerrain(std::vector<uint8_t>(kSide*kSide,100),kSide,kSide,0);
            w.setMapPlacementFeatures(std::vector<uint16_t>(kSide*kSide,0xffff),{});
            FeatType tree;tree.name="tree";tree.flamable=tree.hasBurnAnim=true;
            tree.spreadChance=55;tree.sparkTicks=4;tree.burnTicks=12;tree.burntType=1;
            FeatType burnt;burnt.name="burnt";
            FeatType bush;bush.name="bush";bush.flamable=bush.hasBurnAnim=true;
            bush.spreadChance=90;bush.sparkTicks=1;bush.burnTicks=3;bush.burntType=-1;
            FeatType stump;stump.name="stump";stump.flamable=stump.hasBurnAnim=true;stump.fx=stump.fz=2;
            stump.spreadChance=40;stump.sparkTicks=10;stump.burnTicks=25;stump.burntType=4;stump.blocking=true;
            // Bigger than its source: installing it can replace burning neighbours.
            FeatType ash;ash.name="ash";ash.fx=ash.fz=3;ash.blocking=true;
            FeatType rock;rock.name="rock";rock.blocking=true;
            w.setFeatureTypes({tree,burnt,bush,stump,ash,rock});
            uint32_t lcg=seed*2654435761u;
            auto next=[&](uint32_t n){ lcg=lcg*1664525u+1013904223u;return (lcg>>8)%n; };
            for (int k=0;k<900;++k) {   // random cells in random order: index order != map order
                const int type=int(next(10))<5 ? 0 : int(next(10))<5 ? 2 : int(next(10))<6 ? 3 : 5;
                const int fx=type==3 ? 2 : 1,cx=int(next(kSide-2)),cz=int(next(kSide-2));
                w.addFeature(cz*kSide+cx,float(cx*16+fx*8),float(cz*16+fx*8),0,fx,fx,
                             type==3 || type==5,type);
            }
        };
        World production,reference;
        build(production);build(reference);
        std::vector<World::RngObservation> drawsP,drawsR;
        production.setRngObserver([&](const World::RngObservation& o){ drawsP.push_back(o); });
        reference.setRngObserver([&](const World::RngObservation& o){ drawsR.push_back(o); });
        const size_t count=production.features().size();
        check(count==reference.features().size() && count>256,"differential worlds build alike");
        uint32_t lcg=seed;
        auto next=[&](uint32_t n){ lcg=lcg*22695477u+1u;return (lcg>>10)%n; };
        bool same=true;
        size_t burned=0,faded=0,maxBurning=0;
        for (int step=0;step<500 && same;++step) {
            const uint32_t eventSeed=lcg;
            for (World* w : {&production,&reference}) {   // the same events on both
                lcg=eventSeed;
                for (int e=int(next(4));e-->0;) {
                    const size_t i=next(uint32_t(count));
                    switch (next(8)) {
                    case 0: case 1: case 2: RetailReplayProbe::igniteIndex(*w,i);break;
                    case 3: RetailReplayProbe::replaceIndex(*w,i,int(next(3))==0 ? -1 : 1);break;
                    case 4: RetailReplayProbe::fade(*w,w->features()[i].id);break;
                    default: break;
                    }
                }
            }
            RetailReplayProbe::burning(production);RetailReplayProbe::fades(production);
            RetailReplayProbe::referenceBurning(reference);RetailReplayProbe::referenceFades(reference);
            size_t nowBurning=0;
            for (size_t i=0;i<count;++i) {
                const auto& a=production.features()[i];const auto& b=reference.features()[i];
                same=same && a.id==b.id && a.alive==b.alive && a.burn==b.burn && a.burnLeft==b.burnLeft &&
                     a.spreadIn==b.spreadIn && a.type==b.type && a.fadeLeft==b.fadeLeft && a.dmg==b.dmg &&
                     a.blocks==b.blocks && a.fx==b.fx && a.fz==b.fz && a.x==b.x && a.z==b.z &&
                     a.burnSequence==b.burnSequence && a.burnStarted==b.burnStarted;
                nowBurning+=a.alive && a.burn;
                burned+=a.alive && a.burn && a.burnLeft==1;
                faded+=a.fadeLeft==1;
            }
            maxBurning=std::max(maxBurning,nowBurning);
            same=same && drawsP.size()==drawsR.size() &&
                 RetailReplayProbe::fading(production)==RetailReplayProbe::fading(reference) &&
                 production.stateHash()==reference.stateHash();
            for (size_t d=0;same && d<drawsP.size();++d)
                same=drawsP[d].bound==drawsR[d].bound && drawsP[d].seedBefore==drawsR[d].seedBefore &&
                     drawsP[d].result==drawsR[d].result;
        }
        std::printf("  seed %u: %zu features, %zu draws, peak %zu burning, %zu burn-outs, %zu fade-outs\n",
                    seed,count,drawsP.size(),maxBurning,burned,faded);
        check(same && drawsP.size()>100 && maxBurning>20 && burned>50 && faded>20,
              "marked burn/fade walks match the full passes step for step (fields, draws, hash)");
    }

    // The marks follow features_ as it grows mid-fire (addFeature across mark
    // words, as rubble from a destroyed corpse does) and through world reuse
    // (clearFeatures, resetForReplay), and every mark is dropped once the
    // fires and fades are over, so the no-fire early exit comes back.
    {
        using namespace tak::sim;
        constexpr int kSide=40;
        auto setup=[&](World& w) {
            w.setTerrain(std::vector<uint8_t>(kSide*kSide,100),kSide,kSide,0);
            FeatType tree;tree.name="tree";tree.flamable=tree.hasBurnAnim=true;
            tree.spreadChance=70;tree.sparkTicks=8;tree.burnTicks=30;tree.burntType=1;
            FeatType burnt;burnt.name="burnt";
            w.setFeatureTypes({tree,burnt});
        };
        auto plant=[&](World& w,int from,int to) {   // ids are cells, row-major
            for (int c=from;c<to;++c) w.addFeature(c,float(c%kSide*16+8),float(c/kSide*16+8),0,1,1,false,0);
        };
        World production,reference;
        bool same=true,reusedMidFire=true;
        auto compare=[&] {
            same=same && production.features().size()==reference.features().size() &&
                 production.stateHash()==reference.stateHash() &&
                 RetailReplayProbe::fading(production)==RetailReplayProbe::fading(reference);
            for (size_t i=0;same && i<production.features().size();++i) {
                const auto& a=production.features()[i];const auto& b=reference.features()[i];
                same=a.alive==b.alive && a.burn==b.burn && a.burnLeft==b.burnLeft &&
                     a.spreadIn==b.spreadIn && a.type==b.type && a.fadeLeft==b.fadeLeft;
            }
        };
        for (int round=0;round<3;++round) {
            if (round>0)   // reused while it still burns and fades
                reusedMidFire=reusedMidFire && RetailReplayProbe::fading(production)>0 &&
                              std::any_of(production.features().begin(),production.features().end(),
                                          [](const Feature& f){ return f.alive && f.burn; });
            for (World* w : {&production,&reference}) {
                if (round==1) w->clearFeatures();
                if (round==2) w->resetForReplay();
                setup(*w);plant(*w,0,60);
                const int rock=kSide*kSide-1;   // far from the fire: its fade outlives the round
                w->addFeature(rock,float(rock%kSide*16+8),float(rock/kSide*16+8),0,1,1,false,1);
            }
            for (int step=0;step<(round<2 ? 12 : 300);++step) {
                for (World* w : {&production,&reference}) {
                    if (step==0) RetailReplayProbe::ignite(*w,round*7+5);
                    if (step==3) plant(*w,60,200);   // grows features_ while it burns
                    if (step==6) RetailReplayProbe::fade(*w,kSide*kSide-1);
                }
                RetailReplayProbe::burning(production);RetailReplayProbe::fades(production);
                RetailReplayProbe::referenceBurning(reference);RetailReplayProbe::referenceFades(reference);
                compare();
            }
        }
        size_t burntOut=0;
        for (const auto& f:production.features()) burntOut+=f.type==1;
        check(same && reusedMidFire,"marks follow features_ growth mid-fire, clearFeatures and resetForReplay");
        check(burntOut>20 && !RetailReplayProbe::marked(production) && !RetailReplayProbe::fading(production),
              "every burning mark is dropped once the fires are out");
    }

    // Same-tick spread follows features_ order: a spark into a HIGHER index is
    // burned later in the same tick, one into a LOWER index waits a tick --
    // within one 64-bit mark word and across words. Hashes printed for the
    // base-vs-candidate comparison (tools/featburn_bench.cpp workloads too).
    for (const bool higher : {true,false})
        for (const int gap : {1,70}) {
            using namespace tak::sim;
            World fire;
            fire.setTerrain(std::vector<uint8_t>(96*96,100),96,96,0);
            FeatType tree;tree.name="tree";tree.flamable=tree.hasBurnAnim=true;
            tree.spreadChance=100;tree.sparkTicks=0;tree.burnTicks=10;tree.burntType=1;
            FeatType rock;rock.name="rock";
            fire.setFeatureTypes({tree,rock});
            const int source=10*96+10,target=10*96+11;
            int padded=0;   // non-burning rocks far away push the second tree's index out
            auto pad=[&](int n){
                for (int k=0;k<n;++k,++padded)
                    fire.addFeature(60*96+padded,float(padded%96*16+8),float((60+padded/96)*16+8),0,1,1,false,1);
            };
            pad(3);
            fire.addFeature(higher ? source : target,higher ? 168 : 184,168,0,1,1,false,0);
            pad(gap);
            fire.addFeature(higher ? target : source,higher ? 184 : 168,168,0,1,1,false,0);
            RetailReplayProbe::ignite(fire,source);
            fire.tick(1.0f/30);
            const uint64_t first=fire.stateHash();
            const auto* t=fire.feature(target);
            check(t->burn && t->burnLeft==(higher ? 9u : 10u),
                  higher ? (gap==1 ? "spread to a higher index burns the same tick (same mark word)"
                                   : "spread to a higher index burns the same tick (later mark word)")
                         : (gap==1 ? "spread to a lower index waits for the next tick (same mark word)"
                                   : "spread to a lower index waits for the next tick (earlier mark word)"));
            for (int i=0;i<40;++i) fire.tick(1.0f/30);
            std::printf("  spread %s gap %d: hash after 1 tick %016llx, after 41 %016llx\n",
                        higher ? "higher" : "lower",gap,static_cast<unsigned long long>(first),
                        static_cast<unsigned long long>(fire.stateHash()));
        }

    std::printf(g_fail ? "featgen_test: %d FAILURE(S)\n" : "featgen_test: all passed\n",
                g_fail);
    return g_fail ? 1 : 0;
}
