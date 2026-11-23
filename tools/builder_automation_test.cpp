#include "sim/matchsetup.h"
#include "net/protocol.h"
#include "client/renderframe.h"
#include "client/buildghost.h"
#include "hpi/hpi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
using namespace tak;
namespace {
int failures=0;
void check(bool ok,const char* label) {
    std::printf("%s %s\n",ok?"PASS":"FAIL",label);failures+=!ok;
}
sim::UnitType worker(bool air=false) {
    sim::UnitType t;t.id="worker";t.isBuilder=t.canMove=t.canReclaim=t.canPatrol=true;
    t.canFly=air;t.upright=true;t.maxHp=100;t.footX=t.footZ=2;t.maxVel=sim::Fixed::fromInt(3);
    t.turnRate=t.turnInPlaceRate=10000;t.sight=160;t.sightHeight=24;t.workerTime=100;
    t.buildDist=80;t.storage=10000;return t;
}
sim::UnitType lodestone() {
    sim::UnitType t;t.id="lode";t.side="ARA";t.maxHp=100;t.maxVel={};
    t.footX=t.footZ=4;t.onMana=true;t.buildTime=100;t.buildCost=10;return t;
}
void setup(sim::World& w,sim::PathfindingMode mode,bool serial=true) {
    w.setSerialThreads(serial);w.setPathfindingMode(mode);
    w.setTerrain(std::vector<uint8_t>(128*128,100),128,128,20);
    w.setPathService(true);w.player(0).mana=10000;
}
void ticks(sim::World& w,int n) {for(int i=0;i<n;++i)w.tick(1.f/30);}
// Nearest-first area order. Deposit INDEX order is deliberately not distance
// order (the old area job walked the deposits in index order).
const std::vector<std::pair<float,float>> kOrderSpots{{1600,400},{400,400},{1000,400},{400,1200}};
int spotOf(const sim::World& w,const sim::Unit& site) {
    int best=-1;float bestD=1e30f;
    for(size_t i=0;i<w.manaSpots().size();++i) {
        const float dx=site.x.toFloat()-w.manaSpots()[i].first,dz=site.z.toFloat()-w.manaSpots()[i].second;
        if(dx*dx+dz*dz<bestD) {bestD=dx*dx+dz*dz;best=int(i);}
    }
    return best;
}
// Every builder's sites, in the order it started them (one entry per site).
std::vector<std::vector<int>> runAreaOrder(sim::World& w,const std::vector<int>& ids,int limit=12000) {
    std::vector<std::vector<int>> visited(ids.size());std::vector<int> last(ids.size(),0);
    for(int tick=0;tick<limit;++tick) {
        bool busy=false;
        for(size_t i=0;i<ids.size();++i) {
            const auto* b=w.unit(ids[i]);busy|=!b->orders.empty();
            if(b->buildSiteId && b->buildSiteId!=last[i]) {
                last[i]=b->buildSiteId;visited[i].push_back(spotOf(w,*w.unit(b->buildSiteId)));
            }
        }
        if(!busy)break;
        w.tick(1.f/30);
    }
    return visited;
}
uint64_t areaNearestFirst(bool serial=true) {
    uint64_t hash=0;
    const std::pair<float,float> starts[]{{256,400},{1800,400},{400,1400}};
    const std::vector<int> expected[]{{1,2,0,3},{0,2,1,3},{3,1,2,0}};
    for(int s=0;s<3;++s) {
        sim::World w;setup(w,sim::PathfindingMode::Retail,serial);auto builder=worker();auto lode=lodestone();
        w.setManaSpots(kOrderSpots);
        const int id=w.spawn(&builder,starts[s].first,starts[s].second,0,0);
        w.queueManaBuildArea(id,&lode,0,0,2000,2000,false);
        const auto got=runAreaOrder(w,{id});
        check(got[0]==expected[s] && w.unit(id)->orders.empty(),
              s==0 ? "area build visits deposits nearest first (from the west)" :
              s==1 ? "area build visits deposits nearest first (from the east)" :
                     "area build visits deposits nearest first (from the south)");
        hash^=w.stateHash()*uint64_t(s+1);
    }
    return hash;
}
void areaNearestEdgeCases() {
    auto builder=worker();auto lode=lodestone();
    {   // Exact tie: equal distance is broken by deposit index, either way round.
        for(bool swapped:{false,true}) {
            sim::World w;setup(w,sim::PathfindingMode::Retail);
            w.setManaSpots(swapped ? std::vector<std::pair<float,float>>{{600,400},{1400,400}}
                                   : std::vector<std::pair<float,float>>{{1400,400},{600,400}});
            const int id=w.spawn(&builder,1000,400,0,0);
            w.queueManaBuildArea(id,&lode,0,0,2000,2000,false);
            const auto got=runAreaOrder(w,{id});
            check(got[0]==std::vector<int>({0,1}),swapped ? "equidistant deposits: lower index first (west)"
                                                            : "equidistant deposits: lower index first (east)");
        }
    }
    {   // A deposit taken mid-run drops out; the job continues nearest first.
        sim::World w;setup(w,sim::PathfindingMode::Retail);w.setManaSpots(kOrderSpots);
        const int id=w.spawn(&builder,256,400,0,0);
        w.queueManaBuildArea(id,&lode,0,0,2000,2000,false);
        std::vector<int> got{-1};
        for(int tick=0;tick<6000 && !w.unit(id)->buildSiteId;++tick)w.tick(1.f/30);
        got[0]=spotOf(w,*w.unit(w.unit(id)->buildSiteId));
        const int other=w.spawn(&lode,1000,400,0,0);w.blockFoot(lode,1000,400,true);
        const auto rest=runAreaOrder(w,{id});
        got.insert(got.end(),rest[0].begin(),rest[0].end());
        check(w.unit(other)->alive() && got==std::vector<int>({1,1,3,0}) && w.unit(id)->orders.empty(),
              "deposit occupied mid-run is skipped and the rest stay nearest first");
    }
    {   // Shift-queued behind a move: the area starts from the move's end.
        sim::World w;setup(w,sim::PathfindingMode::Retail);w.setManaSpots(kOrderSpots);
        const int id=w.spawn(&builder,256,400,0,0);
        w.order(id,400,1400,false);w.queueManaBuildArea(id,&lode,0,0,2000,2000,true);
        const auto got=runAreaOrder(w,{id});
        check(got[0]==std::vector<int>({3,1,2,0}),"Shift-queued area plans from the end of the previous move");
    }
    {   // Two builders, side by side, on the same area: claims split the deposits.
        sim::World w;setup(w,sim::PathfindingMode::Retail);w.setManaSpots(kOrderSpots);
        const int a=w.spawn(&builder,256,400,0,0),b=w.spawn(&builder,256,464,0,0);
        w.queueManaBuildArea(a,&lode,0,0,2000,2000,false);w.queueManaBuildArea(b,&lode,0,0,2000,2000,false);
        const auto got=runAreaOrder(w,{a,b});
        std::vector<int> all=got[0];all.insert(all.end(),got[1].begin(),got[1].end());std::sort(all.begin(),all.end());
        const int built=int(std::count_if(w.units().begin(),w.units().end(),[&](const auto& u) {
            return u.alive() && u.type==&lode && !u.underConstruction;}));
        check(!got[0].empty() && !got[1].empty() && got[0].front()!=got[1].front() &&
              all==std::vector<int>({0,1,2,3}) && built==4,
              "two builders on one area each take a different nearest deposit, none twice");
        std::printf("two-builder split: a=");for(int i:got[0])std::printf("%d",i);
        std::printf(" b=");for(int i:got[1])std::printf("%d",i);std::printf("\n");
    }
}
uint64_t areaBuild(sim::PathfindingMode mode,bool air,bool serial=true) {
    sim::World w;setup(w,mode,serial);auto builder=worker(air);builder.sight=100;auto lode=lodestone();
    w.setManaSpots({{512,256},{896,256},{1280,640}});
    const int id=w.spawn(&builder,256,400,0,0);
    w.addFeature(7,896,256,1,1,1,true);
    w.queueManaBuildArea(id,&lode,1050,400,400,128,false);
    check(w.unit(id)->orders.size()==1 && w.unit(id)->orders.front().manaBuildArea,
          "reversed box remains one bounded area order");
    UnitR snapshot;snapshot.captureOrders(*w.unit(id));
    check(snapshot.hasQueuedBuild() && snapshot.hasQueuedWork() && snapshot.orders.front().manaBuildArea &&
          snapshot.orders.front().buildType==&lode,
          "render snapshot reports pending area construction");
    w.tick(1.f/30);
    check(w.unit(id)->orders.size()==2 && !w.unit(id)->orders.front().manaBuildArea &&
          w.unit(id)->buildSiteId==0,"unexplored deposit starts an exploration approach before any construction");
    bool exploredAtBirth=true;
    for(int tick=0;tick<6000 && !w.unit(id)->orders.empty();++tick) {
        w.tick(1.f/30);
        if (const auto* site=w.unit(w.unit(id)->buildSiteId)) {
            const int x=site->x.floorInt()/32,z=site->z.floorInt()/32;
            exploredAtBirth&=(w.navigationExploration()[size_t(z)*64+x]&1)!=0;
        }
    }
    const int complete=int(std::count_if(w.units().begin(),w.units().end(),[&](const auto& u) {
        return u.alive() && u.type==&lode && !u.underConstruction;
    }));
    check(complete==2 && w.unit(id)->orders.empty(),"area builds every enclosed deposit and completes in both path modes, ground and air");
    check(exploredAtBirth && !w.feature(7)->alive,"area explores first and reuses obstacle-clearing construction");
    check(w.canPlace(&lode,1280,640,0),"deposit outside the box stays untouched");
    return w.stateHash();
}
void areaPolicy() {
    sim::World w;setup(w,sim::PathfindingMode::Retail);auto builder=worker();auto lode=lodestone();
    const int id=w.spawn(&builder,256,400,0,0);w.setManaSpots({{512,256},{896,256}});
    w.order(id,320,400,false);w.queueManaBuildArea(id,&lode,400,128,1050,400,true);
    check(w.unit(id)->orders.back().manaBuildArea && w.unit(id)->orders.front().groundMission,
          "Shift area queues behind existing movement");
    w.cancelBuilds(id);w.stop(id);check(w.unit(id)->orders.empty(),"Stop cancels area construction and exploration");
    w.queueManaBuildArea(id,&lode,400,128,1050,400,false);
    w.cancelBuilds(id);w.order(id,640,640,false);
    check(w.unit(id)->orders.size()==1 && !w.unit(id)->orders.front().manaBuildArea,
          "replacement move cancels the persistent area");
    auto other=lode;other.onMana=false;
    w.queueManaBuildArea(id,&other,0,0,1024,1024,false);
    check(w.unit(id)->orders.size()==1 && !w.unit(id)->orders.front().manaBuildArea,
          "area construction rejects non-lodestone types");
    w.queueManaBuildArea(id,&lode,std::numeric_limits<float>::quiet_NaN(),0,1024,1024,false);
    check(w.unit(id)->orders.size()==1 && !w.unit(id)->orders.front().manaBuildArea,"invalid area coordinates leave prior orders intact");
    w.unit(id)->repeatType=&lode;
    w.queueManaBuildArea(id,&lode,0,0,1024,1024,false);
    check(!w.unit(id)->orders.front().manaBuildArea,"infinite mobile production retains its Stop-only construction gate");
    w.unit(id)->repeatType=nullptr;w.stop(id);
    const int occupied=w.spawn(&lode,512,256,0,1);w.blockFoot(lode,512,256,true);
    w.queueManaBuildArea(id,&lode,400,128,1050,400,false);ticks(w,6000);
    check(w.unit(occupied)->player==1 && std::count_if(w.units().begin(),w.units().end(),[&](const auto& u) {
        return u.alive() && u.player==0 && u.type==&lode && !u.underConstruction;
    })==1 && w.unit(id)->orders.empty(),"occupied enemy deposit is skipped without blocking later sites");
}
uint64_t patrolRepair(sim::PathfindingMode mode,bool air,bool serial=true) {
    sim::World w;setup(w,mode,serial);auto builder=worker(air);auto target=lodestone();target.onMana=false;
    const int id=w.spawn(&builder,256,512,0,0);
    const int damaged=w.spawn(&target,640,592,0,0);w.unit(damaged)->hp=sim::Fixed::fromInt(25);
    w.blockFoot(target,640,592,true);w.patrol(id,1024,512);
    bool repaired=false,patrolRetained=false;
    for(int tick=0;tick<2400;++tick) {
        w.tick(1.f/30);
        if (w.unit(damaged)->hp==sim::Fixed::fromInt(100)) repaired=true;
        if (repaired && w.unit(id)->x.toFloat()>900) patrolRetained=true;
    }
    check(repaired && patrolRetained,"ground and flying patrols repair an encountered building and then continue their route");
    check(std::count_if(w.unit(id)->orders.begin(),w.unit(id)->orders.end(),[](const auto& o){return o.goal && o.patrol;})==2,
          "automatic repair preserves the original two patrol waypoints");
    check(w.player(0).mana<10000,"automatic repairs consume ordinary repair mana");
    return w.stateHash();
}
void repairPolicy() {
    sim::World w;setup(w,sim::PathfindingMode::Retail);auto builder=worker();builder.sight=200;
    const int id=w.spawn(&builder,256,512,0,0);auto target=lodestone();target.onMana=false;
    const int ally=w.spawn(&target,352,512,0,1);w.setTeam(1,0);w.unit(ally)->hp=sim::Fixed::fromInt(50);
    w.patrol(id,1024,512);ticks(w,35);
    check(w.unit(id)->repairId==ally || w.unit(ally)->hp>sim::Fixed::fromInt(50),"patrolling builders repair teammates as well as their own units");
    w.player(0).mana=0;const auto hp=w.unit(ally)->hp;ticks(w,15);
    check(w.unit(ally)->hp==hp,"automatic repair pauses when mana is exhausted");
    w.setTeam(1,1);ticks(w,3);
    check(!w.unit(id)->repairId && std::none_of(w.unit(id)->orders.begin(),w.unit(id)->orders.end(),[](const auto& o){return o.repairTarget!=0;}),
          "automatic repair ends when a target becomes hostile");
    w.cancelBuilds(id);w.stop(id);check(!w.unit(id)->repairId && w.unit(id)->orders.empty(),"Stop cancels repair and patrol together");
    // A departing ally walks off (RepairUnit 0x407190): the repair pauses
    // while it moves and gives up after its looks, or once the builder is
    // its own sightdistance from where it left the patrol (mission +0x56);
    // either way it never chases it across the map.
    auto walker=worker();walker.isBuilder=false;walker.buildTime=1000;walker.buildCost=10;
    const int mover=w.spawn(&walker,288,560,0,1);w.unit(mover)->hp=sim::Fixed::fromInt(50);
    w.unit(ally)->hp=sim::Fixed::fromInt(100);
    w.setTeam(1,0);w.player(0).mana=10000;w.patrol(id,1024,512);
    for(int t=0;t<300 && w.unit(id)->repairId!=mover;++t)w.tick(1.f/30);
    for(int t=0;t<300 && w.unit(mover)->hp==sim::Fixed::fromInt(50);++t)w.tick(1.f/30);
    // The detour's leg and its leash origin (where the builder left the patrol).
    const auto detour=[&]()->const sim::Order* {
        for(const auto& o:w.unit(id)->orders)if(o.patrolRepair && o.repairTarget==mover)return &o;
        return nullptr;
    };
    const bool started=detour()!=nullptr;
    const uint32_t issued=started?detour()->issuedTick:0;
    const float ox=started?detour()->clickX.toFloat():0,oz=started?detour()->clickZ.toFloat():0;
    w.order(mover,1800,560,false);
    auto moverHp=w.unit(mover)->hp;bool healedMoving=false,ended=false;
    for(int t=0;t<900 && !ended;++t) {
        const bool moving=w.unit(mover)->speed!=sim::Fixed();
        w.tick(1.f/30);
        healedMoving|=moving && w.unit(mover)->hp>moverHp;moverHp=w.unit(mover)->hp;
        // The first detour ended (a later scan may start another).
        ended=!detour() || detour()->issuedTick!=issued;
    }
    const float chased=std::hypot(w.unit(id)->x.toFloat()-ox,w.unit(id)->z.toFloat()-oz);
    check(started && ended && !healedMoving && chased<280 && w.unit(mover)->x.toFloat()<1700,
          "automatic repairs do not chase a departing ally across the map");
    w.unit(mover)->hp=sim::Fixed::fromInt(100);
    w.cancelBuilds(id);w.stop(id);w.unit(ally)->hp={};w.unit(ally)->x=sim::Fixed::fromInt(352);
    w.patrol(id,1024,512);ticks(w,35);
    check(!w.unit(id)->repairId,"patrol repair does not resurrect a dying target");
}
void repairEligibility() {
    for(auto mode:{sim::PathfindingMode::Retail}) {
        sim::World w;setup(w,mode);auto builder=worker();builder.sight=200;
        auto target=lodestone();target.onMana=false;target.footX=target.footZ=2;
        const int id=w.spawn(&builder,256,512,0,0);
        const int enemy=w.spawn(&target,272,512,0,1);w.unit(enemy)->hp=sim::Fixed::fromInt(20);
        const int unfinished=w.spawn(&target,272,544,0,0);
        w.unit(unfinished)->hp=sim::Fixed::fromInt(20);w.unit(unfinished)->underConstruction=true;
        const int cargo=w.spawn(&target,272,480,0,0);
        w.unit(cargo)->hp=sim::Fixed::fromInt(20);w.unit(cargo)->inTransport=id;
        const int unreachable=w.spawn(&target,384,512,0,0);w.unit(unreachable)->hp=sim::Fixed::fromInt(20);
        w.blockCells(20,0,2,128,true);
        const int mobile=w.spawn(&builder,256,608,0,0);w.unit(mobile)->hp=sim::Fixed::fromInt(20);
        // RepairUnit works once the footprints touch (or a failed approach
        // ends within builddistance): the builder first walks round the bodies.
        w.patrol(id,288,736);ticks(w,300);
        check(w.unit(mobile)->hp>sim::Fixed::fromInt(20) && w.unit(enemy)->hp==sim::Fixed::fromInt(20) &&
              w.unit(unreachable)->hp==sim::Fixed::fromInt(20) && w.unit(cargo)->hp==sim::Fixed::fromInt(20),
              "patrol repairs mobile allies and skips enemies, construction, cargo and disconnected targets");
        w.cancelBuilds(id);w.stop(id);w.unit(mobile)->hp=sim::Fixed::fromInt(20);
        w.setPatrolRepairs(false);w.patrol(id,288,736);ticks(w,120);
        check(w.unit(mobile)->hp==sim::Fixed::fromInt(20) && !w.unit(id)->repairId,
              "with patrol repairs off (World::setPatrolRepairs), a patrol repairs nothing");
    }
}
void movingRepair() {
    for(auto mode:{sim::PathfindingMode::Retail})
        for(bool air:{false,true}) {
            sim::World w;setup(w,mode);auto builder=worker(air);builder.sight=300;builder.workerTime=10;
            auto allyType=worker();allyType.buildTime=100;allyType.buildCost=10;
            const int id=w.spawn(&builder,256,512,0,0);
            const int ally=w.spawn(&allyType,352,512,0,0);w.unit(ally)->hp=sim::Fixed::fromInt(20);
            w.patrol(id,1024,512);ticks(w,35);
            check(w.unit(id)->repairId==ally,"moving-ally fixture starts a patrol repair");
            w.unit(ally)->x=sim::Fixed::fromInt(448);w.unit(ally)->z=sim::Fixed::fromInt(608);
            ticks(w,900);
            check(w.unit(ally)->hp==sim::Fixed::fromInt(100) &&
                  std::count_if(w.unit(id)->orders.begin(),w.unit(id)->orders.end(),[](const auto& o){return o.goal && o.patrol;})==2,
                  "patrol repair refreshes a nearby ally's position and retains its patrol");
        }
}
void areaCommandPolicy(const sim::TypeRegistry& registry) {
    const auto* lode=registry.find("aralode");const auto* foreign=registry.find("zonmana");
    const sim::UnitType* source=nullptr;
    for(const auto& [name,type]:registry.types()) {
        const auto& menu=registry.buildable(name);
        if(type.isBuilder && !type.isStructure() &&
           std::find(menu.begin(),menu.end(),"aralode")!=menu.end()) {source=&type;break;}
    }
    check(source && lode && foreign,"area command authorization fixture has retail builders and lodestones");
    if(!source || !lode || !foreign) return;
    auto builder=*source;builder.builderLimited=true;
    sim::World w;setup(w,sim::PathfindingMode::Retail);w.setManaSpots({{512,512}});
    const int id=w.spawn(&builder,256,512,0,1);
    net::Command c;c.kind=net::Cmd::BuildManaArea;c.unitId=id;c.x=400;c.z=400;c.x2=600;c.z2=600;
    std::snprintf(c.type,sizeof c.type,"aralode");sim::applyCommand(w,registry,c);
    check(w.unit(id)->orders.empty(),"area command cannot control another player's builder");
    c.player=1;sim::applyCommand(w,registry,c);
    check(w.unit(id)->orders.size()==1 && w.unit(id)->orders.front().manaBuildArea,
          "owned builder accepts an authorized lodestone area command");
    w.stop(id);std::snprintf(c.type,sizeof c.type,"zonmana");sim::applyCommand(w,registry,c);
    check(w.unit(id)->orders.empty(),"area command rejects a lodestone absent from the builder's build menu");
}
void realLodestones(const char* install) {
    auto vfs=hpi::mountRetailRoot(install);
    for(bool crusades:{false,true}) {
        sim::TypeRegistry registry;sim::setupRegistry(registry,vfs,crusades);
        areaCommandPolicy(registry);
        for(const char* race:{"ara","tar","ver","zon","cre"}) {
            const auto* basic=registry.find(std::string(race)+"lode");
            const auto* divine=registry.find(std::string(race)+"mana");
            check(basic && divine,"all factions have basic and Divine lodestone types");
            if (!basic || !divine) continue;
            const sim::UnitType* builder=nullptr;
            for(const auto& [name,type]:registry.types()) {
                const auto& menu=registry.buildable(name);
                if (type.isBuilder && !type.isStructure() &&
                    std::find(menu.begin(),menu.end(),divine->id)!=menu.end()) {builder=&type;break;}
            }
            check(basic && divine && builder,"all factions have an authorized Divine lodestone builder");
            if (!builder) continue;
            for(auto mode:{sim::PathfindingMode::Retail}) {
                sim::World w;setup(w,mode);w.buildNavClasses(registry);
                w.setManaSpots({{512,512},{896,512}});
                w.setSacredSites({{31,31,2,2,1},{55,31,2,2,1}});
                const int id=w.spawn(builder,512,800,0,0);
                auto bank=lodestone();bank.id="bank";bank.onMana=false;bank.income=10000;bank.storage=100000;
                w.spawn(&bank,128,128,0,0);w.player(0).mana=50000;
                const int old=w.startBuild(id,basic,512,512,sim::World::Approach::None);
                check(old!=0,"area-upgrade fixture admits the basic lodestone");if (!old) continue;
                w.unit(old)->underConstruction=false;w.unit(old)->hp=sim::Fixed::fromInt(basic->maxHp);
                w.cancelBuilds(id);w.stop(id);
                w.queueManaBuildArea(id,divine,464,464,944,560,false);
                bool replacement=false;
                for(int tick=0;tick<12000 && !w.unit(id)->orders.empty();++tick) {
                    w.tick(1.f/30);
                    if (const auto* site=w.unit(w.unit(id)->buildSiteId))
                        replacement|=site->lodestoneReplacement.has_value();
                }
                check(replacement && std::count_if(w.units().begin(),w.units().end(),[&](const auto& u) {
                    return u.alive() && u.type==divine && !u.underConstruction;
                })==2 && w.unit(id)->orders.empty(),"area upgrades its own basic lodestone and builds Divine on the empty spot, every faction/balance/path mode");
                std::printf("area-upgrade %s crusades=%d path=%d hash=%016llx\n",race,crusades,int(mode),
                            (unsigned long long)w.stateHash());
            }
        }
    }
}
}

// Retail's fire-at-will sweep (icd 40f330): auto weapon slots fire whatever
// the unit's order, without stopping or turning it.
sim::UnitType gunner(bool builder,int32_t aimTol) {
    sim::UnitType t=worker();t.id=builder?"gunbuilder":"gunner";t.isBuilder=builder;
    t.maxVel=sim::Fixed::fromInt(2);t.turnRate=600;t.turnInPlaceRate=600;
    sim::Weapon w;w.name="bolt";w.range=300;w.reload=0.3f;w.damage=10;w.projVel=900;w.aimTol=aimTol;
    t.weapon=w;t.weapons={w};return t;
}
sim::UnitType dummy() {
    sim::UnitType t;t.id="dummy";t.maxHp=5000;t.footX=t.footZ=2;t.maxVel=sim::Fixed::fromInt(1);
    t.canMove=true;t.upright=true;t.turnRate=t.turnInPlaceRate=0;return t;
}
sim::UnitType hut() {
    sim::UnitType t;t.id="hut";t.maxHp=100;t.maxVel={};t.footX=t.footZ=2;t.buildTime=100;t.buildCost=10;return t;
}
void fireSweep(sim::PathfindingMode mode) {
    {   // 1 + 3: a builder walking to its site fires at an enemy it passes,
        // keeps its build order and keeps moving; placement takes the slots
        // (targets cleared, no fire while building) and completion gives
        // them back.
        const auto b=gunner(true,33000);const auto e=dummy();const auto h=hut();
        sim::World w;setup(w,mode);w.setPlayerCount(2);w.player(0).mana=10000;
        const int id=w.spawn(&b,300,1000,0,0),foe=w.spawn(&e,700,1110,0,1);
        w.queueBuild(id,&h,820,1000,false);
        bool hitEnRoute=false,orderKept=true,advanced=false,firedBuilding=false,clearedAtPlacement=false;
        float hitX=0;int siteSeen=0;
        for(int tick=0;tick<900;++tick) {
            const sim::Fixed before=w.unit(foe)->hp;
            w.tick(1.f/30);
            const auto& u=*w.unit(id);
            if(!siteSeen && !u.buildSiteId) {
                if(u.orders.empty() || u.orders.front().buildType!=&h)orderKept=false;
                if(w.unit(foe)->hp<before && !hitEnRoute) {hitEnRoute=true;hitX=u.x.toFloat();}
                if(hitEnRoute && u.x.toFloat()>hitX+20)advanced=true;
            }
            if(u.buildSiteId && !siteSeen) {
                siteSeen=u.buildSiteId;
                clearedAtPlacement=u.slotTarget==std::array<int32_t,3>{} && u.slotAuto==0;
            }
            if(u.buildSiteId && u.justFired)firedBuilding=true;
            if(siteSeen && !u.buildSiteId)break;
        }
        check(hitEnRoute,"sweep: a builder on its way to a site damages an enemy it passes");
        check(orderKept,"sweep: firing en route leaves the build order at the front");
        check(advanced,"sweep: the builder keeps walking after it opens fire");
        check(siteSeen && clearedAtPlacement,"sweep: placement clears every slot target and makes the slots manual");
        check(!firedBuilding,"sweep: no auto fire while the builder works its site");
        // The idle builder then acquires the enemy itself (an attack order,
        // which owns the slots again); what must be gone is the work mode.
        ticks(w,2);
        check(w.unit(id)->slotManual!=sim::World::kSlotsWork,"sweep: the job's manual mode retires once the job ends");
    }
    {   // 2: a unit on a plain move fires at an enemy ahead while still moving.
        const auto g=gunner(false,1024);const auto e=dummy();
        sim::World w;setup(w,mode);w.setPlayerCount(2);
        const int id=w.spawn(&g,300,1000,0,0);w.spawn(&e,760,1008,0,1);
        w.order(id,640,1000,false);
        bool firedMoving=false;
        for(int tick=0;tick<300 && !firedMoving;++tick) {
            w.tick(1.f/30);
            const auto& u=*w.unit(id);
            firedMoving=u.justFired && !u.orders.empty() && u.speed>sim::Fixed();
        }
        check(firedMoving,"sweep: a unit on a plain move fires at an enemy ahead without stopping");
    }
    {   // A ready target behind a turn-in-place unit is not fired at while it moves.
        const auto g=gunner(false,1024);const auto e=dummy();
        sim::World w;setup(w,mode);w.setPlayerCount(2);
        const int id=w.spawn(&g,400,1000,0,0);w.spawn(&e,250,1000,0,1);
        w.order(id,1100,1000,false);
        bool firedBehind=false,swept=false;
        for(int tick=0;tick<240;++tick) {
            w.tick(1.f/30);
            const auto& u=*w.unit(id);
            if(u.orders.empty())break;
            swept=swept || u.slotTarget[0]!=0;
            firedBehind=firedBehind || u.justFired;
        }
        check(swept,"sweep: the enemy behind is swept into the slot");
        check(!firedBehind,"sweep: a moving turn-in-place unit does not fire at a target behind it");
    }
    {   // An attack order owns the slots: the sweep never assigns them, and
        // auto returns once the order is gone.
        const auto g=gunner(false,1024);const auto e=dummy();
        sim::World w;setup(w,mode);w.setPlayerCount(2);
        const int id=w.spawn(&g,400,1000,0,0),a=w.spawn(&e,600,1000,0,1);w.spawn(&e,400,1200,0,1);
        w.attack(id,a,false);
        bool manual=true;
        for(int tick=0;tick<120;++tick) {
            w.tick(1.f/30);
            const auto& u=*w.unit(id);
            manual=manual && u.slotAuto==0 && u.slotTarget==std::array<int32_t,3>{} &&
                !u.orders.empty() && u.orders.front().targetId==a;
        }
        check(manual,"sweep: an explicit attack keeps its slots manual and untouched by the sweep");
        w.stop(id);w.unit(id)->fireState=0;ticks(w,2);
        check(w.unit(id)->slotAuto==7,"sweep: the slots return to auto after the attack order");
    }
}
// The build being worked keeps its order at the front of the queue, and its site is a
// real unit drawn in the painter list. The queued-build ghosts (drawn over every body)
// must skip that order, or a flyer conjuring it shows UNDER a copy of its own site;
// the orders still waiting keep their ghosts.
void workedBuildDrawsNoGhost(bool air) {
    sim::World w;setup(w,sim::PathfindingMode::Retail);auto builder=worker(air);auto lode=lodestone();
    w.setManaSpots({{600,400},{1000,400}});
    const int id=w.spawn(&builder,400,400,0,0);
    w.queueBuild(id,&lode,600,400,false);
    w.queueBuild(id,&lode,1000,400,true);
    const sim::Unit* site=nullptr;
    for(int tick=0;tick<1200 && !site;++tick) {
        w.tick(1.f/30);
        const auto* b=w.unit(id);
        if(const auto* s=b->buildSiteId ? w.unit(b->buildSiteId) : nullptr; s && s->buildBegun) site=s;
    }
    const auto& b=*w.unit(id);
    // The rendered snapshot reads the same fields.
    UnitR rb,rs;rb.captureOrders(b);rb.buildSiteId=b.buildSiteId;
    if(site) {rs.type=site->type;rs.underConstruction=site->underConstruction;}
    const bool worked=site && b.orders.size()==2 && b.orders.front().buildType==&lode;
    check(worked && !render::buildGhostDrawn(b,0,site) && render::buildGhostDrawn(b,1,site) &&
          !render::buildGhostDrawn(rb,0,&rs) && render::buildGhostDrawn(rb,1,&rs),
          air ? "conjuring flyer: worked build draws no queued ghost, the next one does" :
                "conjuring walker: worked build draws no queued ghost, the next one does");
    // Without its site (not reached yet, or gone) the front order is just queued.
    check(render::buildGhostDrawn(b,0,static_cast<const sim::Unit*>(nullptr)),
          "front build without a site still draws its ghost");
}
int main(int argc,char** argv) {
    for(auto mode:{sim::PathfindingMode::Retail})
        for(bool air:{false,true}) {areaBuild(mode,air);patrolRepair(mode,air);}
    check(areaBuild(sim::PathfindingMode::Retail,false,true)==areaBuild(sim::PathfindingMode::Retail,false,false),
          "area build serial/threaded hashes agree");
    check(patrolRepair(sim::PathfindingMode::Retail,false,true)==patrolRepair(sim::PathfindingMode::Retail,false,false),
          "patrol repair serial/threaded hashes agree");
    areaPolicy();repairPolicy();repairEligibility();movingRepair();
    fireSweep(sim::PathfindingMode::Retail);
    fireSweep(sim::PathfindingMode::Legion);   // the sweep is pathfinder-independent
    check(areaNearestFirst(true)==areaNearestFirst(false),"nearest-first area serial/threaded hashes agree");
    areaNearestEdgeCases();
    for(bool air:{false,true}) workedBuildDrawsNoGhost(air);
    net::Command c;c.kind=net::Cmd::BuildManaArea;c.unitId=1;c.x=100;c.z=200;c.x2=800;c.z2=900;
    std::snprintf(c.type,sizeof c.type,"lode");net::Writer out;out.cmd(c);
    net::Reader in(out.b.data(),out.b.size());auto decoded=in.cmd();
    check(in.ok && decoded.kind==c.kind && decoded.x2==800 && decoded.z2==900 &&
          std::string(decoded.type)=="lode","lodestone-area wire roundtrip preserves its type and both corners");
    net::Reader shortIn(out.b.data(),out.b.size()-1);(void)shortIn.cmd();
    check(!shortIn.ok,"truncated lodestone-area command is rejected");
    net::Writer mixed;net::Command move;move.unitId=7;move.x=650;
    mixed.cmd(move);mixed.cmd(c);move.unitId=8;mixed.cmd(move);
    net::Reader batch(mixed.b.data(),mixed.b.size());
    const auto first=batch.cmd(),area=batch.cmd(),last=batch.cmd();
    check(batch.ok && batch.p==batch.end && first.unitId==7 && area.x2==800 && last.unitId==8,
          "area commands preserve framing between ordinary movement commands");
    if (argc>1) realLodestones(argv[1]);
    std::printf("%d failures\n",failures);return failures?1:0;
}
