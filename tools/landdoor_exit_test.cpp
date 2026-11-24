// Whether idle units at a land factory's door hold its outputs in (user report
// after 0.9.19: "idle units at a land factory's door don't move aside; only the
// Navy Yard got that fix").
//
// Two factories per side (Arm and Core Kbot Labs and Vehicle Plants), each
// making six of a small and of a large output, in both
// pathfinding modes, on a flat loaded map (feature plane, so movement and the
// site test take the native rectangular footprint test). Measured per output:
// ticks from completion until it is clear of the factory's rectangle and until
// it is clear of the door (the rectangle grown kDoorCells). Then the same with
// a dozen idle units of the owner parked 8 px apart just off the door, with
// one idle unit on the pad, with the dozen guarding one of them, with the dozen
// guarding the factory, and with one guard of the factory fighting an enemy at
// its door. Then three for the guard's once-per-output key (review of
// 39f8cfe6: it was the newest output id near whichever factory, so a key earned
// at another factory, or at an output since gone, skipped every later step
// aside): the guarding dozen with the factory rallied far east (each output
// walks out of the door's search radius before the next one's site is
// refused), a dozen guarding a twin factory south of the door that builds its
// six first, and the guarding dozen with a key already set from another
// factory's step aside (Stale: the two layouts before did not trip the old key
// here, this one does when the key is not kept per factory). Last, an output
// guarding its idle factory must move nobody.
//
// Retail moves no idle body (BUGGER_OFF is stored, never acted on: 0x50d50e;
// BUILD stage 2's footprint test 0x401deb holds the next site; the mover only
// clamps and slows, 0x4dbe2c), so the crowd and the unit on the pad hold the
// factory there as here before the change. Now the owner's idle units in front
// of the factory move aside (World::clearYardDoor), other players' units stay,
// and the factory's own parked outputs are moved again only off the
// pad or when one holds up the next output (`nudged` counts an output of this
// factory given an order after it came to rest: with the Navy Yard's first rule,
// every own parked unit in the door's area, 127 over these 64 runs; now 26).
//
// Guards step aside too (owner's decision after the 0.9.19 known limit, on the
// Kingdoms data: under Legion a dozen guards at a Creon Academy's door let 0 of
// 6 out; Retail Barracks Catapults took 1455 ticks for six against 1227 with
// the door clear).
// Each guard must end back on its guard near its ward, step aside at most once
// for each output (counted per output finished, a refusal of the next one's
// site since then apart), and a guard fighting must not be moved.
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <cstdlib>
#include <string>
#include <vector>

using namespace ta;
namespace {
int failures=0,cases=0;
std::string only;
void check(bool ok,const std::string& label) {
    std::printf("[%s] %s\n",ok?"PASS":"FAIL",label.c_str());++cases;if(!ok)++failures;
}
struct Rect {int x,z,w,h;};
Rect footprint(const sim::Unit& u) {
    return {sim::footprintOrigin(u.x,u.type->footX),sim::footprintOrigin(u.z,u.type->footZ),u.type->footX,u.type->footZ};
}
bool intersects(const Rect& a,const Rect& b) {return a.x<b.x+b.w && b.x<a.x+a.w && a.z<b.z+b.h && b.z<a.z+a.h;}
constexpr int kCells=256,kDoorCells=4,kOutputs=6;

// Guard: the dozen guard one of themselves. Ward: the dozen guard the factory
// (they stand in its guard ring, the door's side included). Fight: one guard of
// the factory at the door with an attack on an enemy put ahead of its guard.
// Rally: Guard with the factory rallied far east. Twin: the dozen guard a twin
// factory south of this one's door (their ring's north side is this door),
// which builds its six first. Stale: Guard with each guard's step-aside key
// set from a far factory's door at a serial this one never reaches first.
enum class Crowd {None,Door,Pad,Guard,Ward,Fight,Rally,Twin,Stale};
const char* crowdName(Crowd c) {
    return c==Crowd::None?"none":c==Crowd::Door?"door":c==Crowd::Pad?"pad":c==Crowd::Guard?"guard":
           c==Crowd::Ward?"ward":c==Crowd::Fight?"fight":c==Crowd::Rally?"rally":c==Crowd::Twin?"twin":"stale";
}
bool guardCrowd(Crowd c) {return c==Crowd::Guard || c==Crowd::Rally || c==Crowd::Stale;}
// The sim's door area (clearYardDoor's kDoorCells) for telling which output a
// move aside was for.
constexpr int kSimDoorCells=8;

struct Result {
    int produced=0,stuck=0,lastDone=-1,inDoor=0,nudged=0,crowdMoved=0;
    // Ward: guards back on their guard of the factory and near it at the end,
    // the farthest one's distance from the factory's centre (px), and moves
    // aside of one unit while the same output was the first one leaving.
    int guarding=0,farthest=0,twice=0,asides=0,kept=0;
    // Guards: the farthest any one strayed from where it stood when production began (px).
    int stray=0;
    std::vector<int> clear,door;
    static int median(std::vector<int> v) {
        if(v.empty())return -1;
        std::sort(v.begin(),v.end());return v[v.size()/2];
    }
    static int max(const std::vector<int>& v) {return v.empty()?-1:*std::max_element(v.begin(),v.end());}
};

Result run(const sim::TypeRegistry& registry,sim::PathfindingMode mode,const char* factoryName,const char* outputName,
           const char* crowdTypeName,Crowd crowd) {
    sim::World w;w.setSerialThreads(true);w.setPathfindingMode(mode);
    w.setTerrain(std::vector<uint8_t>(kCells*kCells,100),kCells,kCells,20);
    w.setMapPlacementFeatures(std::vector<uint16_t>(kCells*kCells,0xffff),{});
    w.buildNavClasses(registry);w.setPathService(true);
    auto factory=*registry.find(factoryName);factory.workerTime*=4;factory.storage=1000000;
    const auto* output=registry.find(outputName);
    const auto* crowdType=registry.find(crowdTypeName);
    // On its cell grid as a placed building stands, at the middle of its
    // buildangle range: every shipped ground factory's pad then lies south.
    const float fx=float(112*16+factory.footX%2*8),fz=float(112*16+factory.footZ%2*8);
    const int id=w.spawn(&factory,fx,fz,float(int16_t(factory.orientation))*6.28318530718f/65536.f,0);
    const Rect yard=footprint(*w.unit(id));
    const Rect door{yard.x-kDoorCells,yard.z-kDoorCells,yard.w+2*kDoorCells,yard.h+2*kDoorCells};
    const float doorZ=float((yard.z+yard.h)*16);
    // Two rows of six, 8 px between bodies, the first row 24 px off the door.
    const float gap=float(crowdType->footX*16+8);
    std::vector<int> idle;
    // The twin: one more of the factory south of it, three cells between them.
    const int twin=crowd==Crowd::Twin ? w.spawn(&factory,fx,fz+float((factory.footZ+3)*16),
        float(int16_t(factory.orientation))*6.28318530718f/65536.f,0) : 0;
    if(crowd==Crowd::Door || guardCrowd(crowd) || crowd==Crowd::Ward || crowd==Crowd::Twin)
        for(int i=0;i<12;++i)
            idle.push_back(w.spawn(crowdType,fx-2.5f*gap+float(i%6)*gap,doorZ+24+float(i/6)*gap,0,0));
    if(crowd==Crowd::Pad)idle.push_back(w.spawn(crowdType,fx,doorZ-24,0,0));
    if(guardCrowd(crowd))for(size_t i=1;i<idle.size();++i)w.guard(idle[i],idle[0],false);
    if(crowd==Crowd::Rally)w.order(id,fx+1200,fz,false);
    if(crowd==Crowd::Stale) {
        const int far=w.spawn(&factory,fx-1200,fz,float(int16_t(factory.orientation))*6.28318530718f/65536.f,0);
        for(size_t i=1;i<idle.size();++i) {auto* u=w.unit(idle[i]);u->doorYieldFactory=far;u->doorYieldSerial=1;}
    }
    if(crowd==Crowd::Ward)for(int k:idle)w.guard(k,id,false);
    if(crowd==Crowd::Twin)for(int k:idle)w.guard(k,twin,false);
    // An enemy east of the door, kept alive, and a guard of the factory beside
    // it attacking it, its guard queued under the attack.
    int enemy=0;
    if(crowd==Crowd::Fight) {
        const float ex=float((yard.x+yard.w+2)*16),ez=doorZ+24;
        enemy=w.spawn(crowdType,ex,ez,0,1);
        idle.push_back(w.spawn(crowdType,ex-gap,ez,0,0));
        w.attack(idle[0],enemy,false);w.guard(idle[0],id,true);
    }
    const auto keepAlive=[&] {
        if(!enemy)return;
        for(int k:{enemy,idle[0]})if(auto* u=w.unit(k);u && u->alive())u->hp=sim::Fixed::fromInt(u->maximumHp());
    };
    for(int t=0;t<(crowd==Crowd::Ward || crowd==Crowd::Twin ? 300 : 30);++t) {w.tick(1.f/30);keepAlive();}
    // The twin's six first, until each is off its yard (or 20000 ticks).
    if(twin) {
        w.player(0).mana=1000000;w.train(twin,output,kOutputs);
        const Rect twinYard=footprint(*w.unit(twin));
        for(int t=0;t<20000;++t) {
            w.player(0).mana=1000000;w.tick(1.f/30);
            int out=0;
            for(const auto& u:w.units())
                out+=u.alive() && u.type==output && !u.underConstruction && u.builtBy==twin && !intersects(footprint(u),twinYard);
            if(out==kOutputs)break;
        }
        for(int t=0;t<300;++t)w.tick(1.f/30);
    }
    std::vector<std::pair<float,float>> crowdAt;
    for(int k:idle)crowdAt.emplace_back(w.unit(k)->x.toFloat(),w.unit(k)->z.toFloat());
    w.player(0).mana=1000000;w.train(id,output,kOutputs);
    struct Out {int id,done,clear=-1,door=-1;bool rested=false;};
    std::vector<Out> outs;Result r;int tick=0;
    bool exitOrder=false;
    const Rect simDoor{yard.x-kSimDoorCells,yard.z-kSimDoorCells,yard.w+2*kSimDoorCells,yard.h+2*kSimDoorCells};
    // Per crowd unit: the move aside it was last seen on (its start point), and
    // the first output leaving at each new one.
    std::vector<std::optional<std::pair<sim::Fixed,sim::Fixed>>> lastAside(idle.size());
    std::vector<std::vector<int>> asideFor(idle.size());
    int finished=0;bool refusedSince=false;
    for(;tick<20000;++tick) {
        w.player(0).mana=1000000;w.tick(1.f/30);keepAlive();
        // The first output leaving the door: the oldest of the factory's
        // outputs with orders in the door area.
        int leaving=0;
        for(const auto& u:w.units())
            if(u.alive() && u.type==output && !u.underConstruction && u.builtBy==id && !u.orders.empty() &&
               intersects(footprint(u),simDoor) && (!leaving || u.id<leaving))leaving=u.id;
        // The guards (all but the first, their ward) never get the door's move.
        // The fighter only while its attack lasts (the enemy can still die
        // to the outputs: its guard then may step aside like any other).
        for(size_t i=guardCrowd(crowd) ? 1 : 0;i<idle.size();++i)if(const auto* u=w.unit(idle[i]);u && u->alive()) {
            const bool fighting=std::any_of(u->orders.begin(),u->orders.end(),
                [](const sim::Order& o){return o.targetId && !o.guard;});
            if(crowd!=Crowd::Fight || fighting)for(const auto& o:u->orders)exitOrder|=bool(o.productionExit);
        }
        {   // A refusal episode: the next site refused since the last output finished.
            const auto* f=w.unit(id);
            const int now=int(std::count_if(w.units().begin(),w.units().end(),
                [&](const sim::Unit& o){return o.builtBy==id && !o.underConstruction;}));
            if(now!=finished) {finished=now;refusedSince=false;}
            refusedSince|=!f->productionSiteId && !f->buildQueue.empty() && w.tickCount()<f->productionRetryTick;
        }
        for(size_t i=0;i<idle.size();++i)if(const auto* u=w.unit(idle[i]);u && u->alive()) {
            std::optional<std::pair<sim::Fixed,sim::Fixed>> aside;
            for(const auto& o:u->orders)if(o.productionExit)aside=o.productionExit;
            if(aside && aside!=lastAside[i]) {
                ++r.asides;
                // What it made way for: the factory's outputs finished so far,
                // and whether its next one's site has been refused since.
                leaving=finished*2+(refusedSince ? 1 : 0)+1;
                if(leaving && std::find(asideFor[i].begin(),asideFor[i].end(),leaving)!=asideFor[i].end())++r.twice;
                asideFor[i].push_back(leaving);
            }
            lastAside[i]=aside;
        }
        if(crowd==Crowd::Ward || crowd==Crowd::Twin || guardCrowd(crowd))
            for(size_t i=guardCrowd(crowd) ? 1 : 0;i<idle.size();++i)if(const auto* u=w.unit(idle[i]);u && u->alive()) {
                const double dx=u->x.toFloat()-crowdAt[i].first,dz=u->z.toFloat()-crowdAt[i].second;
                r.stray=std::max(r.stray,int(std::sqrt(dx*dx+dz*dz)));
            }
        for(const auto& u:w.units()) {
            if(!u.alive() || u.type!=output || u.underConstruction || u.id==id || u.builtBy!=id)continue;
            if(std::find(idle.begin(),idle.end(),u.id)!=idle.end())continue;
            auto at=std::find_if(outs.begin(),outs.end(),[&](const Out& o){return o.id==u.id;});
            if(at==outs.end()) {outs.push_back({u.id,tick});at=outs.end()-1;r.lastDone=tick;}
            if(at->clear<0 && !intersects(footprint(u),yard))at->clear=tick;
            if(at->clear>=0 && u.orders.empty())at->rested=true;
            else if(at->rested && !u.orders.empty()) {++r.nudged;at->rested=false;}
            if(at->door<0 && !intersects(footprint(u),door))at->door=tick;
        }
        if(int(outs.size())==kOutputs && std::all_of(outs.begin(),outs.end(),[](const Out& o){return o.clear>=0;}))break;
    }
    for(int settle=0;settle<1500;++settle) {w.tick(1.f/30);keepAlive();}
    r.produced=int(outs.size());
    // The guards: the dozen guarding the factory, or all but the first,
    // guarding it.
    const int wardId=crowd==Crowd::Ward ? id : crowd==Crowd::Twin ? twin : guardCrowd(crowd) ? idle[0] : 0;
    if(wardId)for(size_t i=guardCrowd(crowd) ? 1 : 0;i<idle.size();++i)
        if(const auto* u=w.unit(idle[i]),*g=w.unit(wardId);u && u->alive() && g && g->alive()) {
        const int k=idle[i];
        const auto& o=u->orders;
        const int dx=u->x.floorInt()-g->x.floorInt(),dz=u->z.floorInt()-g->z.floorInt();
        const int d=int(std::sqrt(double(dx)*dx+double(dz)*dz));
        r.farthest=std::max(r.farthest,d);
        // Near: within the guard ring's outer edge (World::guardRing) and three
        // bodies more, for a crowd that does not all fit in the band.
        const int outer=(std::max(g->type->footX,g->type->footZ)+u->type->footX+2)*16+3*16+3*u->type->footX*16;
        const bool aside=std::any_of(o.begin(),o.end(),[](const sim::Order& q){return bool(q.productionExit);});
        const bool ok=!o.empty() && o[sim::World::currentLeg(o)].guard && o[sim::World::currentLeg(o)].targetId==wardId &&
            !aside && d<=outer;
        r.guarding+=ok;
        // Its guard still in its orders (current, or queued behind a move aside).
        r.kept+=std::any_of(o.begin(),o.end(),[&](const sim::Order& q){return q.guard && q.targetId==wardId;});
        if(!ok)std::printf("  guard %d at %d,%d (ward %d,%d): orders=%zu guard=%d aside=%d d=%d outer=%d\n",k,
            u->x.floorInt(),u->z.floorInt(),g->x.floorInt(),g->z.floorInt(),o.size(),
            int(!o.empty() && o[sim::World::currentLeg(o)].guard),int(aside),d,outer);
    }
    for(const auto& o:outs) {
        if(o.clear>=0)r.clear.push_back(o.clear-o.done);
        if(o.door>=0)r.door.push_back(o.door-o.done);
        if(const auto* u=w.unit(o.id);u && u->alive() && (intersects(footprint(*u),yard) || !u->orders.empty()))++r.stuck;
    }
    // The strip straight out of the door (6 cells deep) and the yard itself.
    const Rect strip{yard.x,yard.z,yard.w,yard.h+6};
    for(int k:idle)if(const auto* u=w.unit(k);u && u->alive())r.inDoor+=intersects(footprint(*u),strip);
    r.crowdMoved=exitOrder;
    std::printf("exit mode=%s factory=%s output=%s crowd=%s produced=%d/%d stuck=%d lastDone=%d inDoor=%d nudged=%d "
                "exitOrder=%d asides=%d twice=%d guarding=%d farthest=%d stray=%d clear med=%d max=%d door med=%d max=%d hash=%016llx\n",
        mode==sim::PathfindingMode::Legion?"Legion":"Retail",factoryName,outputName,crowdName(crowd),r.produced,kOutputs,
        r.stuck,r.lastDone,r.inDoor,r.nudged,int(exitOrder),r.asides,r.twice,r.guarding,r.farthest,r.stray,Result::median(r.clear),Result::max(r.clear),
        Result::median(r.door),Result::max(r.door),(unsigned long long)w.stateHash());
    return r;
}

// An older output guarding its factory is not leaving the door (review of
// 4af4e0c4): with the factory idle, an idle unit in front of it stays put for
// as long as the guard stays.
bool guardOutputMovesNobody(const sim::TypeRegistry& registry,sim::PathfindingMode mode,const char* factoryName,
                            const char* outputName,const char* crowdTypeName) {
    sim::World w;w.setSerialThreads(true);w.setPathfindingMode(mode);
    w.setTerrain(std::vector<uint8_t>(kCells*kCells,100),kCells,kCells,20);
    w.setMapPlacementFeatures(std::vector<uint16_t>(kCells*kCells,0xffff),{});
    w.buildNavClasses(registry);w.setPathService(true);
    auto factory=*registry.find(factoryName);factory.workerTime*=4;factory.storage=1000000;
    const auto* output=registry.find(outputName);
    const float fx=float(112*16+factory.footX%2*8),fz=float(112*16+factory.footZ%2*8);
    const int id=w.spawn(&factory,fx,fz,float(int16_t(factory.orientation))*6.28318530718f/65536.f,0);
    const Rect yard=footprint(*w.unit(id));
    w.player(0).mana=1000000;w.train(id,output,1);
    int out=0;
    for(int t=0;t<3000 && !out;++t) {
        w.player(0).mana=1000000;w.tick(1.f/30);
        for(const auto& u:w.units())
            if(u.alive() && u.type==output && !u.underConstruction && u.orders.empty() && !intersects(footprint(u),yard))
                out=u.id;
    }
    if(!out)return false;
    w.guard(out,id,false);
    const int idle=w.spawn(registry.find(crowdTypeName),fx+float(yard.w*16),float((yard.z+yard.h)*16+40),0,0);
    bool moved=false;
    for(int t=0;t<600;++t) {
        w.tick(1.f/30);
        if(const auto* u=w.unit(idle);u && u->alive())for(const auto& o:u->orders)moved|=bool(o.productionExit);
    }
    return !moved;
}
}

int main(int argc,char** argv) {
    if(argc!=2 && argc!=3)return 2;
    if(argc==3)only=argv[2];
    auto vfs=hpi::mountRetailRoot(argv[1]);
    sim::TypeRegistry registry;sim::setupRegistry(registry,vfs);
    struct Case {const char* factory;const char* small;const char* large;const char* crowd;};
    const Case factories[]{
        {"armlab","armpw","armham","armpw"},
        {"armvp","armflash","armbull","armflash"},
        {"corlab","corak","corthud","corak"},
        {"corvp","corgator","correap","corgator"},
    };
    for(auto mode:{sim::PathfindingMode::Retail,sim::PathfindingMode::Legion})
        for(const auto& c:factories)
            for(const char* output:{c.small,c.large}) {
                const std::string label=std::string(mode==sim::PathfindingMode::Legion?"Legion ":"Retail ")+
                    c.factory+" "+output+": ";
                if(!only.empty() && label.find(only)==std::string::npos)continue;
                if(!registry.find(c.factory) || !registry.find(output) || !registry.find(c.crowd)) {
                    check(false,label+"types present");continue;
                }
                const Result clear=run(registry,mode,c.factory,output,c.crowd,Crowd::None);
                check(clear.produced==kOutputs && int(clear.clear.size())==kOutputs && clear.stuck==0,
                      label+"every output leaves, median "+std::to_string(Result::median(clear.clear))+" ticks");
                check(clear.nudged<=2,label+"at most two parked outputs are moved again ("+std::to_string(clear.nudged)+")");
                const Result door=run(registry,mode,c.factory,output,c.crowd,Crowd::Door);
                check(door.produced==kOutputs && int(door.clear.size())==kOutputs && door.stuck==0,
                      label+"a dozen idle units at the door hold no output in");
                check(door.produced==kOutputs && door.lastDone<=clear.lastDone+clear.lastDone*15/100,
                      label+"with them the factory builds near its clear-door rate ("+std::to_string(door.lastDone)+
                      " vs "+std::to_string(clear.lastDone)+" ticks)");
                check(door.inDoor==0,label+"the idle units made way");
                check(door.nudged<=2,label+"at most two of its parked outputs are moved again ("+std::to_string(door.nudged)+")");
                const Result pad=run(registry,mode,c.factory,output,c.crowd,Crowd::Pad);
                check(pad.produced==kOutputs && int(pad.clear.size())==kOutputs && pad.inDoor==0,
                      label+"an idle unit on the pad moves off it and the factory builds");
                const Result guard=run(registry,mode,c.factory,output,c.crowd,Crowd::Guard);
                check(guard.produced==kOutputs && int(guard.clear.size())==kOutputs && guard.stuck==0,
                      label+"a dozen at the door guarding one of them hold no output in");
                // 20%, not 15%: guards stepping aside face their motion (protocol 336, 2026-11-19); on
                // the Kingdoms data a Veruna Keep's guard crowd went 1400 -> 1439 against a 1242 clear
                // door.
                check(guard.produced==kOutputs && guard.lastDone<=clear.lastDone+clear.lastDone*20/100,
                      label+"with them the factory builds near its clear-door rate ("+std::to_string(guard.lastDone)+
                      " vs "+std::to_string(clear.lastDone)+" ticks)");
                check(guard.guarding==11,label+"each guard ends back on its guard near its ward ("+
                      std::to_string(guard.guarding)+" of 11, farthest "+std::to_string(guard.farthest)+" px)");
                check(guard.twice==0,label+"no guard of one of them moves aside twice for one output ("+
                      std::to_string(guard.twice)+")");
                const Result rally=run(registry,mode,c.factory,output,c.crowd,Crowd::Rally);
                check(rally.produced==kOutputs && int(rally.clear.size())==kOutputs,
                      label+"with the factory rallied far off, the guards still let all six out ("+
                      std::to_string(rally.produced)+")");
                check(rally.twice==0,label+"rallied, no guard moves aside twice for one output ("+
                      std::to_string(rally.twice)+")");
                const Result twin=run(registry,mode,c.factory,output,c.crowd,Crowd::Twin);
                check(twin.produced==kOutputs && int(twin.clear.size())==kOutputs,
                      label+"guards that made way at a twin factory's door still let all six out here ("+
                      std::to_string(twin.produced)+")");
                // Kept, not back on it: a guard may end still pressing on its
                // move aside in the lane between the two factories (under Legion
                // two Kingdoms Creon guards did, as at 39f8cfe6, the key apart).
                check(twin.kept==12,label+"after both, each guard keeps its guard ("+
                      std::to_string(twin.kept)+" of 12, "+std::to_string(twin.guarding)+" back on it)");
                const Result stale=run(registry,mode,c.factory,output,c.crowd,Crowd::Stale);
                check(stale.produced==kOutputs && int(stale.clear.size())==kOutputs && stale.stuck==0 &&
                      stale.lastDone<=clear.lastDone+clear.lastDone*15/100,
                      label+"guards that stepped aside at another factory's door still let all six out here ("+
                      std::to_string(stale.produced)+", "+std::to_string(stale.lastDone)+" ticks)");
                const Result ward=run(registry,mode,c.factory,output,c.crowd,Crowd::Ward);
                check(ward.produced==kOutputs && int(ward.clear.size())==kOutputs && ward.stuck==0,
                      label+"a dozen guarding the factory at its door let all six out ("+std::to_string(ward.produced)+")");
                check(ward.guarding==12,label+"each guard ends back on its guard near the factory ("+
                      std::to_string(ward.guarding)+" of 12, farthest "+std::to_string(ward.farthest)+" px)");
                check(ward.twice==0,label+"no guard moves aside twice for one output ("+std::to_string(ward.twice)+")");
                const Result fight=run(registry,mode,c.factory,output,c.crowd,Crowd::Fight);
                check(!fight.crowdMoved,label+"a guard fighting at the door is not moved aside");
                check(guardOutputMovesNobody(registry,mode,c.factory,output,c.crowd),
                      label+"an output guarding its idle factory moves no idle unit aside");
            }
    std::printf("landdoor exit: %d cases, %d failures\n",cases,failures);
    return failures?1:0;
}
