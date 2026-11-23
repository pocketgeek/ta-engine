// Legion display gait pacing (client/gaitpace.h + cob::Vm::setGaitRate).
//
// A walker's gait is a script playing keyframes on sleeps, CALLed by a
// controller polling GET 29. The display slows only the thread playing a
// gait script (and the piece motion it started), so the controller, aim and
// fire threads keep full time. These cases pin the pace function, the gait
// script classification and the VM stepping on synthetic scripts; with a
// scripts directory argument they also run the shipped araarch walk cycle.
#include "client/gaitpace.h"
#include "cob/cob.h"
#include "cob/vm.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace tak;

static int fails=0;
static void check(bool ok,const std::string& what,const std::string& detail="") {
    std::printf("  [%s] %s%s\n",ok ? "PASS" : "FAIL",what.c_str(),
                detail.empty() ? "" : (" -- "+detail).c_str());
    if(!ok)++fails;
}

namespace {
constexpr uint32_t kPushConst=0x10021001,kPushStatic=0x10021004,kPopStatic=0x10023004,
    kAdd=0x10031000,kSleep=0x10013000,kJump=0x10064000,kTurn=0x10002000,kReturn=0x10065000;

// Two counting loops (gait "walk" bumps static 0, poller "MoveControl" static 1,
// both every 100 ms) and two one-shot turns at 100 angle units per tick
// (gait "walk_legs" on piece 0, "AimWeapon" on piece 1, "AimLegs" on piece 0).
cob::File synthetic() {
    cob::File f;
    f.pieces={"legs","torso"};f.numStatics=2;
    const auto counter=[&](uint32_t at,uint32_t which) {
        return std::vector<uint32_t>{kPushStatic,which,kPushConst,1,kAdd,kPopStatic,which,
                                     kPushConst,100,kSleep,kJump,at};
    };
    const auto turn=[](uint32_t piece) {
        return std::vector<uint32_t>{kPushConst,3000,kPushConst,30000,kTurn,piece,1,kReturn};
    };
    f.scripts={{"walk",0},{"MoveControl",12},{"walk_legs",24},{"AimWeapon",32},{"AimLegs",40}};
    for(const auto& part:{counter(0,0),counter(12,1),turn(0),turn(1),turn(0)})
        f.code.insert(f.code.end(),part.begin(),part.end());
    return f;
}

struct Run { int walk=0,poll=0;int32_t legs=0,torso=0; };
Run run(float rate,int ticks) {
    const auto file=std::make_shared<const cob::File>(synthetic());
    const auto gait=cob::gaitCode(*file);
    cob::Vm vm(file);vm.enableRetailAnimation();vm.setGaitCode(gait);
    vm.setGaitRate(rate);
    vm.start("walk");vm.start("MoveControl");vm.start("walk_legs");vm.start("AimWeapon");
    for(int i=0;i<ticks;++i)vm.tick(1.0f/30.0f);
    Run r;r.walk=vm.getStatic(0);r.poll=vm.getStatic(1);
    r.legs=vm.retailPieces()[0].turn[1];r.torso=vm.retailPieces()[1].turn[1];
    return r;
}
}

int main(int argc,char** argv) {
    std::printf("pace function\n");
    {
        const int64_t max=0x20000;   // 2 px per tick
        float rate=1;
        for(int i=0;i<60;++i)rate=legionGaitRate(rate,max,0,1,max,true);
        check(rate==1.0f,"a body covering its full maximum keeps full pace",std::to_string(rate));
        rate=1;
        for(int i=0;i<60;++i)rate=legionGaitRate(rate,int64_t(max*0.92),0,1,max,true);
        check(rate==1.0f,"turning/slope losses above 90% still read as full pace",std::to_string(rate));
        rate=1;
        for(int i=0;i<60;++i)rate=legionGaitRate(rate,0,max/2,1,max,true);
        check(std::abs(rate-0.5f/0.9f)<0.01f,"half the maximum converges to half pace (of the 90% full band)",
              std::to_string(rate));
        rate=1;
        for(int i=0;i<60;++i)rate=legionGaitRate(rate,0,0,1,max,true);
        check(rate<0.01f,"a moving-state body that covers nothing slows toward a stop",std::to_string(rate));
        const float one=legionGaitRate(1,0,0,1,max,true);
        check(one>0.7f && one<0.8f,"one still tick only eases the pace (smoothed)",std::to_string(one));
        float shuffle=1;
        for(int i=0;i<90;++i)shuffle=legionGaitRate(shuffle,i%8==0 ? max : 0,0,1,max,true);
        check(shuffle<0.45f,"a body stepping one tick in eight walks well under half pace",std::to_string(shuffle));
        float back=0;int recover=0,slow=0;
        for(rate=1;rate>0.1f;++slow)rate=legionGaitRate(rate,0,0,1,max,true);
        for(back=0;back<0.9f;++recover)back=legionGaitRate(back,max,0,1,max,true);
        check(recover<slow,"a body that gets going again recovers its pace faster than it slowed",
              std::to_string(recover)+" vs "+std::to_string(slow)+" ticks");
        check(legionGaitRate(0.1f,0,0,1,max,false)==1.0f,"stopping (GET 29 zero) snaps back to full pace");
        check(legionGaitRate(0.3f,max,0,0,max,true)==0.3f,"a re-captured tick keeps the previous pace");
        check(legionGaitRate(0.3f,0,0,0xfffffff0u,max,true)==1.0f,"a backwards seek (wrapped tick gap) reseeds at full pace");
        const float two=legionGaitRate(1,0,max/2,2,max,true);
        const float stepped=legionGaitRate(legionGaitRate(1,0,max/4,1,max,true),0,max/4,1,max,true);
        check(std::abs(two-stepped)<1e-5f,"a two-tick gap matches two one-tick steps",
              std::to_string(two)+" vs "+std::to_string(stepped));
    }
    std::printf("gait scripts\n");
    {
        cob::File f;
        const char* names[]={"walk","walk_legs","Walk_Road","def_walk","swim","swim_legs","stumble_water",
            "row","fastrow","slowrow","drive","wheel_left","SuperDynamicWheelSpinner",
            "SwimControl","MeleeControl","MoveWatcher","fly","soar","tread","sail","towalk","graze","Create"};
        for(size_t i=0;i<std::size(names);++i) {
            f.scripts.push_back({names[i],uint32_t(i*2)});
            f.code.push_back(kJump);f.code.push_back(uint32_t(i*2));
        }
        const auto gait=cob::gaitCode(f);
        for(size_t i=0;i<std::size(names);++i) {
            const bool expect=i<13;
            check(gait.size()==f.code.size() && gait[i*2]==expect && gait[i*2+1]==expect,
                  std::string(names[i])+(expect ? " is a gait script" : " is not"));
        }
        cob::File none;none.scripts={{"Create",0}};none.code={kReturn};
        check(cob::gaitCode(none).empty(),"a script with no gait leaves the map empty");
    }
    std::printf("vm stepping\n");
    {
        const Run full=run(1,90),half=run(0.5f,90),stop=run(0,90);
        std::printf("    walk/poll/legs/torso: full %d/%d/%d/%d half %d/%d/%d/%d stop %d/%d/%d/%d\n",
            full.walk,full.poll,full.legs,full.torso,half.walk,half.poll,half.legs,half.torso,
            stop.walk,stop.poll,stop.legs,stop.torso);
        check(full.walk==full.poll && full.walk==31,"full pace: gait and poller step together");
        check(std::abs(half.walk-16)<=1,"half pace: the gait loop plays half its frames",std::to_string(half.walk));
        check(half.poll==full.poll,"half pace: the GET 29 poller keeps full time");
        check(std::abs(half.legs-full.legs/2)<=100,"half pace: gait-started piece motion runs at half speed",
              std::to_string(half.legs)+" vs "+std::to_string(full.legs));
        check(half.torso==full.torso,"half pace: aim motion keeps full speed");
        check(stop.walk==1 && stop.legs==0,"zero pace freezes the gait mid-cycle");
        check(stop.poll==full.poll && stop.torso==full.torso,"zero pace leaves every other thread alone");
        // Back to full pace: the frozen cycle resumes at once and keeps full time.
        const auto file=std::make_shared<const cob::File>(synthetic());
        const auto gait=cob::gaitCode(*file);
        cob::Vm vm(file);vm.enableRetailAnimation();vm.setGaitCode(gait);
        vm.setGaitRate(0);vm.start("walk");
        for(int i=0;i<30;++i)vm.tick(1.0f/30.0f);
        vm.setGaitRate(1);
        for(int i=0;i<30;++i)vm.tick(1.0f/30.0f);
        check(vm.getStatic(0)==11,"restored pace resumes the cycle at full time",std::to_string(vm.getStatic(0)));
        // A non-gait thread that re-commands a gait piece takes its motion over.
        {
            cob::Vm taken(file);taken.enableRetailAnimation();taken.setGaitCode(gait);
            taken.setGaitRate(0.5f);taken.start("walk_legs");
            for(int i=0;i<30;++i)taken.tick(1.0f/30.0f);
            taken.start("AimLegs");
            for(int i=0;i<30;++i)taken.tick(1.0f/30.0f);
            const int32_t legs=taken.retailPieces()[0].turn[1];
            check(std::abs(legs-4500)<=100,"a non-gait command releases the piece to full speed",std::to_string(legs));
        }
        // A VM without the map (retail mode, flyers) ignores the rate entirely.
        cob::Vm plain(file);plain.enableRetailAnimation();plain.setGaitRate(0);plain.start("walk");
        for(int i=0;i<90;++i)plain.tick(1.0f/30.0f);
        check(plain.getStatic(0)==31,"no gait map: the rate has no effect");
    }
    if(argc>=2) {
        std::printf("shipped araarch walk\n");
        const auto file=std::make_shared<const cob::File>(cob::load(std::string(argv[1])+"/araarch.cob"));
        const auto gait=cob::gaitCode(*file);
        const int hip=[&]{for(size_t i=0;i<file->pieces.size();++i)if(file->pieces[i]=="Hip")return int(i);return -1;}();
        const auto cycle=[&](float rate) {
            cob::Vm vm(file);vm.enableRetailAnimation();vm.setGaitCode(gait);
            vm.onGet=[](int32_t id,const std::vector<int32_t>&)->int32_t{return id==29 ? 100 : 0;};
            vm.start("Create");
            for(int i=0;i<30;++i)vm.tick(1.0f/30.0f);
            vm.setGaitRate(rate);
            int changes=0;int32_t last=vm.retailPieces()[size_t(hip)].move[1];
            for(int i=0;i<300;++i) {
                vm.tick(1.0f/30.0f);
                const int32_t now=vm.retailPieces()[size_t(hip)].move[1];
                if(now!=last)++changes;
                last=now;
            }
            return changes;
        };
        check(hip>=0,"araarch has its Hip piece");
        {
            // Full pace with the map is the old display, piece for piece, tick for tick.
            cob::Vm paced(file),plain(file);
            paced.enableRetailAnimation();paced.setGaitCode(gait);plain.enableRetailAnimation();
            for(auto* vm:{&paced,&plain}) {
                vm->onGet=[](int32_t id,const std::vector<int32_t>&)->int32_t{return id==29 ? 100 : 0;};
                vm->start("Create");
            }
            bool same=true;
            for(int i=0;i<600 && same;++i) {
                if(i==200)for(auto* vm:{&paced,&plain})vm->start("AimWeapon",{4000,1000});
                if(i==260)for(auto* vm:{&paced,&plain})vm->start("FireWeapon",{0});
                paced.tick(1.0f/30.0f);plain.tick(1.0f/30.0f);
                same=std::equal(paced.retailPieces().begin(),paced.retailPieces().end(),plain.retailPieces().begin()) &&
                     paced.threadPcs()==plain.threadPcs();
            }
            check(same,"full pace with the gait map is identical to the unpaced VM (walk, aim, fire)");
        }
        const int full=cycle(1),half=cycle(0.5f),quarter=cycle(0.25f);
        std::printf("    Hip keyframes in 10 s: full %d half %d quarter %d\n",full,half,quarter);
        check(full>20,"full pace walks");
        check(std::abs(half*2-full)<=full/8+2,"half pace plays half the keyframes");
        check(std::abs(quarter*4-full)<=full/6+4,"quarter pace plays a quarter of the keyframes");
    }
    std::printf("%s\n",fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
