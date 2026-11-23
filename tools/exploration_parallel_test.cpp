#include "sim/sim.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Compare the permanent-reveal shortcut against the full retail traversal,
// including cached footprint state, over uneven terrain and changing viewers.
static bool checkRevealShortcut() {
    using namespace tak::sim;
    constexpr int width=48,height=40;
    std::vector<uint16_t> full(width*height),skip(width*height);
    RetailSightFootprint a,b;
    a.distance=b.distance=160;
    a.sightHeight=b.sightHeight=24;
    size_t fullReads=0,skipReads=0;
    for (int step=0;step<400;++step) {
        const auto x=Fixed::fromInt(((step*7)%52-2)*32);
        const auto z=Fixed::fromInt(((step*11)%44-2)*32);
        const auto y=Fixed::fromInt((step*13)%180);
        const uint16_t viewers=uint16_t(1u<<(step%4)) | (step<200 ? 3 : 12);
        auto heights=[](int cx,int cz) {
            return RetailExplorationHeight{uint8_t((cx*7+cz*3)%180),
                                           uint8_t((cx*3+cz*11)%150)};
        };
        retailUpdateSight(a,x,y,z,30,true,width,height,0,
            [&](int cx,int cz){++fullReads;return heights(cx,cz);},
            [&](int cx,int cz,int delta,uint16_t){if(delta>0) full[cz*width+cx]|=viewers;});
        retailUpdateSight(b,x,y,z,30,true,width,height,0,
            [&](int cx,int cz){++skipReads;return heights(cx,cz);},
            [&](int cx,int cz,int delta,uint16_t){if(delta>0) skip[cz*width+cx]|=viewers;},
            [&](int cx,int cz,bool active){return active && (skip[cz*width+cx]&viewers)!=viewers;});
        if (full!=skip || a.x!=b.x || a.z!=b.z || a.eyeHeight!=b.eyeHeight ||
            a.active!=b.active) return false;
    }
    return skipReads<fullReads;
}

// The exploration-only updater (no removal traversal, block skipping) must
// leave the same masks and cached footprint as the full retail traversal.
static bool checkBlockExploration() {
    using namespace tak::sim;
    constexpr int width=45,height=37,shift=2,bw=(width+3)>>shift,bh=(height+3)>>shift;
    std::vector<uint16_t> full(width*height),fast(width*height),blocks(bw*bh,0);
    std::array<RetailSightFootprint,6> a{},b{};
    for (size_t i=0;i<a.size();++i) {
        a[i].distance=b[i].distance=int16_t(96+64*i);
        a[i].sightHeight=b[i].sightHeight=uint8_t(i%3==0 ? 0 : 8*i);
    }
    auto heights=[](int cx,int cz) {
        return RetailExplorationHeight{uint8_t((cx*7+cz*3)%180),uint8_t((cx*3+cz*11)%150)};
    };
    size_t fullReads=0,fastReads=0;
    for (int step=0;step<3000;++step) {
        const size_t i=size_t(step)%a.size();
        const auto x=Fixed::fromInt(((step*7)%52-2)*32+step%32);
        const auto z=Fixed::fromInt(((step*11)%44-2)*32+step%17);
        const auto y=Fixed::fromInt((step*13)%180);
        const uint16_t viewers=uint16_t((1u<<(step%5)) | (step<1500 ? 3 : 12));
        retailUpdateSight(a[i],x,y,z,30,true,width,height,0,
            [&](int cx,int cz){++fullReads;return heights(cx,cz);},
            [&](int cx,int cz,int delta,uint16_t){if(delta>0) full[cz*width+cx]|=viewers;});
        retailUpdateExploration(b[i],x,y,z,30,width,height,shift,
            [&](int cx,int cz){++fastReads;return heights(cx,cz);},
            [&](int bx,int bz){return (blocks[bz*bw+bx]&viewers)==viewers;},
            [&](int cx,int cz){return (fast[cz*width+cx]&viewers)!=viewers;},
            [&](int cx,int cz){fast[cz*width+cx]|=viewers;},
            [&](int bx,int bz) {
                uint16_t all=0xffff;
                for (int cz=bz<<shift;cz<std::min(height,(bz+1)<<shift);++cz)
                    for (int cx=bx<<shift;cx<std::min(width,(bx+1)<<shift);++cx) all&=fast[cz*width+cx];
                blocks[bz*bw+bx]=all;
            });
        if (full!=fast || a[i].x!=b[i].x || a[i].z!=b[i].z || a[i].eyeHeight!=b[i].eyeHeight ||
            a[i].active!=b[i].active || a[i].distance!=b[i].distance) return false;
    }
    return fastReads<fullReads &&
        std::any_of(blocks.begin(),blocks.end(),[](uint16_t v){return v!=0;});
}

// World keeps its per-block summaries of the plane between passes instead of
// rebuilding them from the whole plane every pass. A world that drops them
// before every pass (editNavigationExploration: the old per-pass rebuild) must
// reach the same plane and hash as one that keeps them, and the kept ones must
// equal a rebuild after every pass and after every change made to the plane
// outside the pass. Covers plane sizes with partial edge blocks, moving and
// stationary units, an alliance change, the Mapping and NowISee power codes
// (a bit added to and taken from every cell), a harness write that removes
// bits, new terrain of the same size, and a reused world (resetForReplay) of
// another size. workers<0: serial; otherwise a pool of that many workers.
static bool checkKeptBlocks(int workers,std::vector<uint64_t>& hashes) {
    using namespace tak::sim;
    UnitType near{},far{};
    near.name=near.id="sight-near";far.name=far.id="sight-far";
    for (auto* type:{&near,&far}) {
        type->maxHp=100;type->maxVel=Fixed::fromInt(1);type->canMove=true;type->footX=type->footZ=1;
    }
    near.sight=64;near.sightHeight=24;   // radius 4 cells, under a sight cone
    far.sight=96;far.sightHeight=0;      // radius 6, the whole disc
    World kept,rebuilt;
    for (auto* world:{&kept,&rebuilt}) {
        if (workers<0) world->setSerialThreads(true); else world->setSimWorkers(workers);
    }
    const std::string width=workers<0 ? "serial" : std::to_string(workers)+" workers";
    auto fail=[&](const char* what,int step) {
        std::fprintf(stderr,"kept exploration blocks (%s): %s at step %d\n",width.c_str(),what,step);
        return false;
    };
    // Planes of 45x37 cells (partial edge blocks); new terrain of the same
    // size under the same units (setTerrain alone: the last map's summaries
    // must not survive it); then a reused world (resetForReplay) at 100x33.
    const std::array<std::array<int,2>,3> maps{{{90,74},{90,74},{200,66}}};
    int step=0,growing=0;   // growing: passes that revealed cells while some stayed unexplored
    for (size_t m=0;m<maps.size();++m) {
        const int mw=maps[m][0],mh=maps[m][1];
        std::vector<uint8_t> terrain(size_t(mw)*mh);
        for (int z=0;z<mh;++z) for (int x=0;x<mw;++x)
            terrain[size_t(z)*mw+x]=uint8_t((x*7+z*3+int(m)*40)%180);
        for (auto* world:{&kept,&rebuilt}) {
            if (m==1) { world->setTerrain(terrain,mw,mh,30);continue; }
            if (m) world->resetForReplay();
            world->setPlayerCount(4);
            world->setPowerCodes(true);
            world->setTerrain(terrain,mw,mh,30);
            for (int i=0;i<4096;++i)
                world->spawn(i%5 ? &near : &far,float((i%64)*16),float((i/64)*16),0,i%4);
        }
        for (int round=0;round<10;++round,++step) {
            for (auto* world:{&kept,&rebuilt}) {
                world->setDoubleSight(round>=8);
                world->setTeam(1,round<2 ? 1 : 0);
                // Player 2's shroud off, then back on (its exploration forgotten).
                if (round==3 || round==5) world->applyPowerCode(2,PowerCode::Mapping,PowerArg::None,0);
                if (round==6) world->applyPowerCode(3,PowerCode::NowISee,PowerArg::None,0);
                if (round==7) {
                    auto& plane=world->editNavigationExploration();
                    for (size_t i=0;i<plane.size();++i)
                        if (i%3!=1) plane[i]&=uint16_t(~(i%2 ? 0x3u : 0x9u));
                }
                if (round==4 || round==9) continue;   // nobody moves
                // Each player's units in an 8x8-cell cluster that drifts across
                // (and off) the map, so the plane fills in round by round.
                for (size_t i=0;i<world->units().size();++i) {
                    auto& u=world->units()[i];
                    const int p=int(i%4),k=int(i/4);
                    const int cx=(step*5+p*11)%(mw/2+8)-4+k%8,cz=(step*3+p*7)%(mh/2+8)-4+k/8%8;
                    u.x=Fixed::fromInt(cx*32+int(i%32));
                    u.z=Fixed::fromInt(cz*32+int(i/32%32));
                    u.groundY=Fixed::fromInt(int((i+size_t(step)*9)%200));
                    u.underConstruction=(i+size_t(step))%17==0;
                }
            }
            if ((round==3 || round==5 || round==6) && !kept.explorationBlocksMatchPlane())
                return fail("a power code left the summaries unlike the plane",step);
            rebuilt.editNavigationExploration();   // rebuilt by this pass, as every pass used to
            const auto before=kept.navigationExploration();
            kept.tick(1.f/30);rebuilt.tick(1.f/30);
            const auto& after=kept.navigationExploration();
            growing+=after!=before && std::any_of(after.begin(),after.end(),[](uint16_t v){return (v&15)!=15;});
            if (kept.navigationExploration()!=rebuilt.navigationExploration())
                return fail("plane differs from the per-pass rebuild",step);
            if (kept.stateHash()!=rebuilt.stateHash()) return fail("hash differs from the per-pass rebuild",step);
            if (!kept.explorationBlocksMatchPlane()) return fail("summaries unlike the plane after the pass",step);
            hashes.push_back(kept.stateHash());
        }
    }
    // The fixture must keep exploring partly explored planes, or it shows little.
    std::printf("kept exploration blocks (%s): %d of %d passes revealed part of a partly explored plane\n",
                width.c_str(),growing,step);
    if (growing<20) return fail("too few passes revealed part of a partly explored plane",step);
    return true;
}

int main() {
    if (!checkRevealShortcut()) {
        std::fputs("exploration shortcut differs from full traversal\n",stderr);return 1;
    }
    if (!checkBlockExploration()) {
        std::fputs("block exploration differs from full traversal\n",stderr);return 1;
    }
    // The pooled pass keeps a list of touched blocks per pool slot
    // (explorationTouched_): 3 workers, and the widest pool there is (63, the
    // top of SIMULATION THREADS and --sim-threads), whose every slot may run.
    {
        std::vector<uint64_t> serialHashes;
        if (!checkKeptBlocks(-1,serialHashes)) return 1;
        for (const int workers:{3,int(tak::sim::SimPool::kMaxWorkers)}) {
            std::vector<uint64_t> workerHashes;
            if (!checkKeptBlocks(workers,workerHashes)) return 1;
            if (serialHashes!=workerHashes) {
                std::fprintf(stderr,"kept exploration blocks: %d workers differ from serial\n",workers);return 1;
            }
        }
    }
    using namespace tak::sim;
    UnitType type{};
    type.name=type.id="sight-probe";type.maxHp=100;
    type.maxVel=Fixed::fromInt(1);type.canMove=true;
    type.footX=type.footZ=1;type.sight=160;type.sightHeight=24;
    World serial,parallel,wide;
    serial.setSerialThreads(true);parallel.setSimWorkers(3);wide.setSimWorkers(int(SimPool::kMaxWorkers));
    std::vector<uint8_t> terrain(128*128);
    for (int z=0;z<128;++z) for (int x=0;x<128;++x)
        terrain[size_t(z)*128+x]=uint8_t((x*7+z*3)%180);
    for (auto* world:{&serial,&parallel,&wide}) {
        world->setTerrain(terrain,128,128,30);
        for (int i=0;i<4096;++i) world->spawn(&type,float((i%64)*16),float((i/64)*16),0,i%4);
    }
    for (int step=0;step<16;++step) {
        for (auto* world:{&serial,&parallel,&wide}) {
            world->setDoubleSight(step>=8);
            world->setTeam(1,step<8 ? 0 : 1);
            for (size_t i=0;i<world->units().size();++i) {
                auto& u=world->units()[i];
                // Overlapping revealers, map edges, changing eye heights,
                // construction exclusions, and an alliance change.
                u.x=Fixed::fromInt(int((i*13+step*7)%132)*16-32);
                u.z=Fixed::fromInt(int((i*7+step*11)%132)*16-32);
                u.groundY=Fixed::fromInt(int((i+step*9)%200));
                u.underConstruction=(i+size_t(step))%17==0;
            }
            world->tick(1.f/30);
        }
        for (const auto* pooled:{&parallel,&wide})
            if (serial.navigationExploration()!=pooled->navigationExploration() ||
                serial.stateHash()!=pooled->stateHash()) {
                std::fprintf(stderr,"parallel exploration (%s workers) differs at step %d\n",
                             pooled==&parallel ? "3" : "63",step);return 1;
            }
    }
    if (std::none_of(parallel.navigationExploration().begin(),parallel.navigationExploration().end(),
                     [](uint16_t value){return value!=0;})) return 1;
    std::puts("parallel exploration (3 and 63 workers) matches serial masks and world hashes");
}
