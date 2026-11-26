// Unit animations driven the way TotalA.exe drives them, on retail data.
//
// 1. A factory's output under construction rides the QueryBuildInfo piece:
//    BUILD attaches it (0x402948: 0x48aac0(output, factory, piece, 1)) and the
//    mover's attached branch (0x43d6d0 -> 0x43e060, 0x43e180) puts it on the
//    piece and turns it with the piece every tick. armvp spins its pad from
//    StartBuilding (spin pad around y-axis speed <30>), so the Flash on it
//    turns 182 BAM a tick with the pad.
// 2. A wheeled unit: armyork spins its six wheels in StartMoving and stops
//    them in StopMoving (0x43da70's call-ins on the movement tier, 0x4db350).
//    The display VM is fed the tier as GameView feeds it.
// 3. An aircraft: TotalA.exe sets the activation bit at take-off (0x43d210
//    mode 2 -> 0x48b090(1, 1)) and clears it as the landing starts down
//    (0x40f4c7) and on touchdown (0x43d210 mode 1); armfig's Activate opens
//    its wings (activatescr: wing1 x to -6) and Deactivate folds them. The sim
//    script's bit flips both ways and the display VM, mirroring it
//    (client/retailactivation.h), opens and folds the wings.
//
//   unit_animation_test <retail-install-dir>
#include "client/retailactivation.h"
#include "cob/vm.h"
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/retailanimationqueries.h"
#include "sim/sim.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace ta::sim;

namespace {

constexpr float kTick=1.f/30;
int failures=0;
void check(bool ok,const std::string& what) {
    std::printf("[%s] %s\n",ok?"PASS":"FAIL",what.c_str());
    if (!ok) ++failures;
}

void setup(World& w,const TypeRegistry& registry) {
    w.setSerialThreads(true);
    w.setPlayerCount(2);
    w.setTerrain(std::vector<uint8_t>(256*256,60),256,256,20);
    w.buildNavClasses(registry);
    w.setStartResources(0,1e6f,1e6f);w.setStartResources(1,1e6f,1e6f);
}

std::shared_ptr<const ta::cob::File> scriptOf(const UnitType* t) {
    return t->simulationScript ? t->simulationScript : t->productionScript;
}

int pieceIndex(const ta::cob::File& f,const char* name) {
    for (size_t i=0;i<f.pieces.size();++i) if (f.pieces[i]==name) return int(i);
    return -1;
}

int16_t bamDelta(uint16_t a,uint16_t b) { return int16_t(uint16_t(a-b)); }

} // namespace

int main(int argc,char** argv) {
    if (argc!=2) return 2;
    auto vfs=ta::hpi::mountRetailRoot(argv[1]);
    TypeRegistry registry;setupRegistry(registry,vfs);
    for (const char* id:{"armvp","armflash","armyork","armfig"})
        if (!registry.find(id) || !registry.find(id)->script()) { std::printf("FAIL missing type %s\n",id);return 1; }

    // 1. The output turns with armvp's spinning pad.
    {
        World w;setup(w,registry);
        const int vp=w.spawn(registry.find("armvp"),1600,1600,0.f,0);
        w.train(vp,registry.find("armflash"),1);
        int site=0;
        for (int i=0;i<600 && !site;++i) { w.tick(kTick); site=w.unit(vp)->productionSiteId; }
        check(site!=0,"armvp starts a Flash on its pad");
        const Unit* s=w.unit(site);
        check(s && s->buildPiece>=0,"the output is attached to the QueryBuildInfo piece");
        // Let the pad's spin settle, then follow the output for a second.
        for (int i=0;i<30;++i) w.tick(kTick);
        const uint16_t h0=uint16_t(w.unit(site)->heading.v);
        const auto x0=w.unit(site)->x,z0=w.unit(site)->z;
        int steady=0;uint16_t previous=h0;
        for (int i=0;i<30;++i) {
            w.tick(kTick);
            const Unit* u=w.unit(site);
            if (!u || !u->underConstruction) break;
            const int step=bamDelta(uint16_t(u->heading.v),previous);
            if (std::abs(std::abs(step)-182)<=1) ++steady;
            previous=uint16_t(u->heading.v);
        }
        std::printf("  output heading %u -> %u, %d of 30 ticks at the pad's 182 BAM\n",h0,previous,steady);
        check(steady>=29,"the output turns 182 BAM a tick with the pad (spin <30> deg/s)");
        check(w.unit(site)->x==x0 && w.unit(site)->z==z0,"and stays on the pad's point (it spins in place)");
        for (int i=0;i<3000 && w.unit(site) && w.unit(site)->underConstruction;++i) w.tick(kTick);
        check(w.unit(site) && !w.unit(site)->underConstruction && w.unit(site)->buildPiece==-1,
              "the finished Flash lets go of the pad");
    }

    // 2. armyork's wheels spin while it moves and stop when it stops.
    {
        World w;setup(w,registry);
        const UnitType* york=registry.find("armyork");
        const int id=w.spawn(york,1400,1600,1.5707963f,0);
        w.order(id,1700,1600,false);
        auto file=scriptOf(york);
        ta::cob::Vm vm(file);vm.enableRetailAnimation();
        vm.start("Create");
        const int wheel=pieceIndex(*file,"wheel1");
        check(wheel>=0,"armyork has a wheel1 piece");
        uint32_t rate=0;uint16_t heading=uint16_t(w.unit(id)->heading.v);
        std::vector<std::string> calls;
        float spunMoving=0,spunStopped=0;bool stopped=false;int stoppedTicks=0;
        for (int i=0;i<900;++i) {
            w.tick(kTick);
            const Unit* u=w.unit(id);
            // GameView's tier: client/renderframe.h captureMoveRate.
            const int16_t turn=bamDelta(uint16_t(u->heading.v),heading);heading=uint16_t(u->heading.v);
            const uint32_t now=retailAnimationMoveRate(u->speed.v,turn,u->speed.v,
                u->type->animationMoveRate1.v,u->type->animationMoveRate2.v,u->bodyBlockStreak>=2,false);
            retailMovementCallins(rate,now,[&](const char* name) { calls.push_back(name);vm.start(name); });
            rate=now;
            const float before=vm.pieces()[size_t(wheel)].rot[0];
            vm.tick(kTick);
            float spun=vm.pieces()[size_t(wheel)].rot[0]-before;
            if (spun<-3.14159f) spun+=6.2831853f;
            if (rate) spunMoving+=spun; else if (i>0 && !calls.empty()) { spunStopped+=std::fabs(spun);stopped=true;++stoppedTicks; }
        }
        std::string sequence;for (const auto& c:calls) sequence+=" "+c;
        std::printf("  call-ins:%s; moving %.2f rad, stopped %.3f rad over %d ticks\n",
                    sequence.c_str(),spunMoving,spunStopped,stoppedTicks);
        check(!calls.empty() && calls.front()=="StartMoving","moving starts StartMoving (0x43da70)");
        check(spunMoving>3.f,"the wheels roll forward while it moves (spin wheel x-axis <150>)");
        bool hasStop=false;for (const auto& c:calls) hasStop|=c=="StopMoving";
        check(hasStop && stopped && spunStopped<1e-3f,"StopMoving stops them dead once it arrives");
    }

    // 3. armfig opens its wings at take-off and folds them on landing.
    {
        World w;setup(w,registry);
        const UnitType* fig=registry.find("armfig");
        const int id=w.spawn(fig,1400,1600,1.5707963f,0);
        for (int i=0;i<30;++i) w.tick(kTick);
        check(!w.scriptGetWords(id).activated,"a landed fighter's script is not activated");
        w.order(id,1800,1600,false);
        auto file=scriptOf(fig);
        ta::cob::Vm vm(file);vm.enableRetailAnimation();
        vm.start("Create");
        const int wing=pieceIndex(*file,"wing1");
        bool shown=false,activatedSeen=false,deactivatedAfter=false;
        std::vector<int> calls;
        float openWing=0,closedWing=-1;
        uint32_t activatedAt=0,deactivatedAt=0,landedAt=0;
        for (int i=0;i<1500;++i) {
            w.tick(kTick);
            const Unit* u=w.unit(id);
            const bool on=w.scriptGetWords(id).activated;
            if (on && !activatedSeen) { activatedSeen=true;activatedAt=w.tickCount(); }
            if (activatedSeen && !on && !deactivatedAfter) { deactivatedAfter=true;deactivatedAt=w.tickCount(); }
            if (deactivatedAfter && u->flightGroundMode==1 && !landedAt) landedAt=w.tickCount();
            const int call=ta::retailDisplayActivation(shown,true,true,fig->onOffable,fig->activateWhenBuilt,on,u->active);
            if (call) { calls.push_back(call);vm.start(call>0 ? "Activate" : "Deactivate"); }
            vm.tick(kTick);
            const float x=vm.pieces()[size_t(wing)].move[0];
            if (activatedSeen && !deactivatedAfter) openWing=std::min(openWing,x);
            if (landedAt && w.tickCount()>landedAt+90) closedWing=x;
        }
        std::printf("  activated at tick %u, deactivated at %u, landed at %u; wing1 x open %.2f, folded %.2f\n",
                    activatedAt,deactivatedAt,landedAt,openWing,closedWing);
        check(activatedSeen,"take-off activates its script (0x43d210 mode 2 / 0x48b090(1, 1))");
        check(deactivatedAfter,"the landing deactivates it again (0x40f4c7 / 0x43d210 mode 1)");
        check(landedAt!=0,"and it lands");
        check(calls.size()==2 && calls[0]==1 && calls[1]==-1,"the display VM gets Activate, then Deactivate");
        check(openWing<-5.5f,"Activate runs activatescr: wing1 slides out to x -6 in flight");
        check(std::fabs(closedWing)<0.05f,"Deactivate folds it back to 0 on the ground");
    }

    std::printf("%s (%d failure%s)\n",failures ? "FAIL" : "PASS",failures,failures==1 ? "" : "s");
    return failures ? 1 : 0;
}
