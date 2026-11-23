#include "sim/sim.h"
#include "sim/retailflight.h"
#include "sim/retailairattack.h"
#include "sim/retailairseek.h"
#include "sim/retailairtoair.h"
#include "sim/retailairtraffic.h"
#include "sim/retailrng.h"
#include "sim/retailheight.h"
#include "sim/retailexploration.h"
#include "tdo/tdo.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace tak::sim;

int main(int argc, char** argv) {
    // tools/re/check_air_attack_run.py: one AirToGround (41bf80) handler call
    // per line, after the head's checks.
    if (argc==2 && std::strcmp(argv[1],"--air-run")==0) {
        unsigned stage,events,tick,deadline,pending,flags,heading,runLength,onMap,seed,automatic,target,retargets,out,next;
        int count,range,width,height,traffic,footX;
        RetailAirRun run;
        while (std::scanf("%u %u %u %u %u %u %d %d %d %d %d %d %d %u %d %u %u %d %d %u %d %d %u %u %u %u %u",&stage,&events,
                          &tick,&deadline,&pending,&flags,&count,&run.position.x,&run.position.y,&run.position.z,
                          &run.target.x,&run.target.y,&run.target.z,&heading,&range,&runLength,&onMap,&width,&height,
                          &seed,&traffic,&footX,&automatic,&target,&retargets,&out,&next)==27) {
            run.heading=uint16_t(heading);run.range=range;run.runLength=uint16_t(runLength);run.onMap=onMap!=0;
            run.width=width;run.height=height;run.traffic=int8_t(traffic);run.footX=int16_t(footX);
            run.automatic=automatic!=0;run.hasTarget=target!=0;
            RetailMissionState m;m.stage=uint8_t(stage);m.waitMask=0;m.deadline=deadline;m.pending=pending;m.flags=flags;
            struct Host {
                uint32_t& seed;RetailMissionState& m;bool retargets,out;uint32_t next;
                unsigned calls=0,begin=0,autoSlots=0,aim=0,installs=0,retargetCalls=0,stepOuts=0;int32_t stepOutD=0;
                unsigned outCalls=0,leaves=0,formations=0;
                RetailFlightGoal goal;
                uint32_t random(int n) { ++calls;return retailRandom(seed,n); }
                void beginFlight() { ++begin; }
                void slotsAuto() { ++autoSlots; }
                void aimSlot() { ++aim; }
                bool retarget() { ++retargetCalls;return retargets; }
                bool stepOut(int32_t d) { ++stepOuts;stepOutD=d;return true; }
                void install(const RetailFlightGoal& g) { ++installs;goal=g;m.pending&=~0x3700u; }
                bool outOfSlot() { ++outCalls;return out; }
                uint32_t nextFlags() const { return next; }
                void leave() { ++leaves; }
                bool formation() { ++formations;return true; }
            } host{seed,m,retargets!=0,out!=0,next};
            const int result=retailAirToGround(m,count,tick,events,run,host);
            std::printf("%d %u %u %u %u %u %d %u %d %d %d %d %u %u %u %u %u %u %u %d %u %u %u\n",result,unsigned(m.stage),
                        m.waitMask,m.deadline,m.pending,m.flags,count,host.installs,host.goal.point.x,host.goal.point.y,
                        host.goal.point.z,int(host.goal.radius),host.begin,host.autoSlots,host.aim,seed,host.calls,
                        host.retargetCalls,host.stepOuts,host.stepOutD,host.outCalls,host.leaves,host.formations);
        }
        return 0;
    }
    // tools/re/check_air_traffic.py: the air-traffic counter step
    // (51dc4b..51dce4), one "counter overlaps" pair per line.
    if (argc==2 && std::strcmp(argv[1],"--air-traffic")==0) {
        int counter,overlaps;
        while (std::scanf("%d %d",&counter,&overlaps)==2)
            std::printf("%d\n",int(retailAirTrafficStep(int8_t(counter),int8_t(overlaps))));
        return 0;
    }
    // tools/re/check_air_traffic.py: one mission's air-traffic check
    // (retailAirTrafficCheck) per line: the mission's handler called past its
    // head's own exits.
    if (argc==2 && std::strcmp(argv[1],"--traffic-check")==0) {
        unsigned mission,stage,tick,deadline,pending,flags,seed,built;int traffic,footX;
        while (std::scanf("%u %u %u %u %u %u %d %d %u %u",&mission,&stage,&tick,&deadline,&pending,&flags,&traffic,
                          &footX,&seed,&built)==10) {
            RetailMissionState m;m.stage=uint8_t(stage);m.waitMask=0;m.deadline=deadline;m.pending=pending;m.flags=flags;
            struct Host {
                uint32_t& seed;bool built;unsigned calls=0,pushes=0,stops=0;RetailStepOut kind=RetailStepOut::None;
                int32_t distance=0;
                uint32_t random(int n) { ++calls;return retailRandom(seed,n); }
                bool stepOut(RetailStepOut k,int32_t d) { ++pushes;kind=k;distance=d;return built; }
                void stopBuilding() { ++stops; }
            } host{seed,built!=0};
            const int result=retailAirTrafficCheck(RetailTrafficMission(mission),m,tick,int8_t(traffic),int16_t(footX),
                                                   flags,host);
            std::printf("%d %u %u %u %u %u %u %u %d %u %u %u\n",result,unsigned(m.stage),m.waitMask,m.deadline,m.pending,
                        m.flags,host.pushes,unsigned(host.kind),host.distance,host.stops,seed,host.calls);
        }
        return 0;
    }
    // tools/re/check_air_traffic.py: one VTOL_StepOut (4214b0) handler call
    // per line, with up to seven overlapping flyers.
    if (argc==2 && std::strcmp(argv[1],"--step-out")==0) {
        unsigned stage,events,tick,deadline,pending,flags,heading,flies,onMap,seed,n;int distance,width,height;
        RetailStepOutInput in;RetailFlightVector point;
        while (std::scanf("%u %u %u %u %u %u %d %d %d %d %d %d %u %d %u %u %d %d %u %u",&stage,&events,&tick,&deadline,
                          &pending,&flags,&point.x,&point.y,&point.z,&in.position.x,&in.position.y,&in.position.z,
                          &heading,&distance,&flies,&onMap,&width,&height,&seed,&n)==20) {
            std::vector<RetailFlightVector> others(n);
            for (auto& o:others) if (std::scanf("%d %d %d",&o.x,&o.y,&o.z)!=3) return 2;
            in.heading=uint16_t(heading);in.distance=distance;in.flies=flies!=0;in.onMap=onMap!=0;
            in.width=width;in.height=height;
            RetailMissionState m;m.stage=uint8_t(stage);m.waitMask=0;m.deadline=deadline;m.pending=pending;m.flags=flags;
            struct Host {
                uint32_t& seed;RetailMissionState& m;unsigned calls=0,installs=0;RetailFlightGoal goal;
                uint32_t random(int n) { ++calls;return retailRandom(seed,n); }
                void install(const RetailFlightGoal& g) { ++installs;goal=g;m.pending&=~0x3700u; }
            } host{seed,m};
            const int result=retailVtolStepOut(m,point,in,others,host);
            std::printf("%d %u %u %u %u %u %d %d %d %u %d %d %d %d %u %u %u\n",result,unsigned(m.stage),m.waitMask,
                        m.deadline,m.pending,m.flags,point.x,point.y,point.z,host.installs,host.goal.point.x,
                        host.goal.point.y,host.goal.point.z,int(host.goal.radius),unsigned(host.goal.flags),seed,host.calls);
        }
        return 0;
    }
    // tools/re/check_air_traffic.py: one AirToGroundHover (41d820) handler
    // call per line, after the head's target checks.
    if (argc==2 && std::strcmp(argv[1],"--air-hover")==0) {
        unsigned stage,events,tick,deadline,pending,flags,flies,target,still,automatic,onMap,seed,outOfSlot,formationOk,
                 retargets,stepOk,lostResult;
        int count,distance,traffic,footX,width,height;
        RetailAirHover in;RetailFlightVector targetPoint,lead,point;
        while (std::scanf("%u %u %u %u %u %u %d %d %d %d %d %d %d %d %d %d %d %d %d %d %u %u %u %u %d %d %u %d %d %u %u %u %u %u %u",
                          &stage,&events,&tick,&deadline,&pending,&flags,&count,&in.position.x,&in.position.y,
                          &in.position.z,&targetPoint.x,&targetPoint.y,&targetPoint.z,&lead.x,&lead.y,&lead.z,&point.x,
                          &point.y,&point.z,&distance,&flies,&target,&still,&automatic,&traffic,&footX,&onMap,&width,
                          &height,&seed,&outOfSlot,&formationOk,&retargets,&stepOk,&lostResult)==35) {
            in.distance=distance;in.flies=flies!=0;in.target=target!=0;in.targetStill=still!=0;
            in.automatic=automatic!=0;in.traffic=int8_t(traffic);in.footX=int16_t(footX);in.onMap=onMap!=0;
            in.width=width;in.height=height;
            RetailMissionState m;m.stage=uint8_t(stage);m.waitMask=0;m.deadline=deadline;m.pending=pending;m.flags=flags;
            struct Host {
                uint32_t& seed;RetailMissionState& m;RetailFlightVector tgt,led;bool out,formationOk,retargets,stepOk;int lostResult;
                unsigned calls=0,lost=0,outCalls=0,leaves=0,formations=0,begin=0,autoSlots=0,aims=0,targetPoints=0,leads=0,
                         retargetCalls=0,stepOuts=0,approaches=0,rings=0;
                int32_t stepOutD=0;RetailFlightGoal goal;
                uint32_t random(int n) { ++calls;return retailRandom(seed,n); }
                int lostTarget() { ++lost;return lostResult; }
                bool outOfSlot() { ++outCalls;return out; }
                uint32_t nextFlags() const { return 0; }   // check_air_traffic.py clears +0x66
                void leave() { ++leaves; }
                bool formation() { ++formations;return formationOk; }
                void beginFlight() { ++begin; }
                void slotsAuto() { ++autoSlots; }
                void aim() { ++aims; }
                RetailFlightVector targetPoint() { ++targetPoints;return tgt; }
                RetailFlightVector lead() { ++leads;return led; }
                bool retarget() { ++retargetCalls;return retargets; }
                bool stepOut(int32_t d) { ++stepOuts;stepOutD=d;return stepOk; }
                void approach(const RetailFlightGoal& g) { ++approaches;goal=g;m.pending&=~0x3700u; }
                void ring(const RetailFlightGoal& g) { ++rings;goal=g;m.pending&=~0x3700u; }
            } host{seed,m,targetPoint,lead,outOfSlot!=0,formationOk!=0,retargets!=0,stepOk!=0,int(lostResult)};
            const int result=retailAirToGroundHover(m,count,point,tick,events,in,host);
            std::printf("%d %u %u %u %u %u %d %d %d %d %u %u %d %d %d %d %u %u %u %u %u %u %u %u %u %u %u %u %u %d %u %u\n",
                        result,unsigned(m.stage),m.waitMask,m.deadline,m.pending,m.flags,count,point.x,point.y,point.z,
                        host.approaches,host.rings,host.goal.point.x,host.goal.point.y,host.goal.point.z,
                        int(host.goal.radius),unsigned(host.goal.flags),unsigned(host.goal.heading),host.lost,
                        host.outCalls,host.leaves,host.formations,host.begin,host.autoSlots,host.aims,host.targetPoints,
                        host.leads,host.retargetCalls,host.stepOuts,host.stepOutD,seed,host.calls);
        }
        return 0;
    }
    // tools/re/check_air_seek_attack.py: one VTOL_SeekAttack (4192f0) handler
    // call per line.
    if (argc==2 && std::strcmp(argv[1],"--air-seek")==0) {
        unsigned stage,events,tick,deadline,pending,flags,angle,armed,flies,target,onMap,seed,attacks,retargets,out,next;
        int range,width,height,traffic,footX;
        RetailAirSeek in;RetailFlightVector point;
        while (std::scanf("%u %u %u %u %u %u %u %d %d %d %d %d %d %d %u %u %u %u %d %d %u %u %u %d %d %u %u",&stage,&events,
                          &tick,&deadline,&pending,&flags,&angle,&point.x,&point.y,&point.z,&in.position.x,&in.position.y,
                          &in.position.z,&range,&armed,&flies,&target,&onMap,&width,&height,&seed,&attacks,
                          &retargets,&traffic,&footX,&out,&next)==27) {
            in.range=range;in.armed=armed!=0;in.flies=flies!=0;in.target=target!=0;in.onMap=onMap!=0;
            in.width=width;in.height=height;in.traffic=int8_t(traffic);in.footX=int16_t(footX);
            RetailMissionState m;m.stage=uint8_t(stage);m.waitMask=0;m.deadline=deadline;m.pending=pending;m.flags=flags;
            uint16_t heading=uint16_t(angle);
            struct Host {
                uint32_t& seed;RetailMissionState& m;bool attacks,retargets,out;uint32_t next;
                unsigned calls=0,attackCalls=0,begin=0,autoSlots=0,retargetCalls=0,installs=0,stepOuts=0;
                unsigned outCalls=0,leaves=0,formations=0;
                int32_t stepOutD=0;RetailFlightGoal goal;
                uint32_t random(int n) { ++calls;return retailRandom(seed,n); }
                bool attack() { ++attackCalls;return attacks; }
                void beginFlight() { ++begin; }
                void slotsAuto() { ++autoSlots; }
                bool retarget() { ++retargetCalls;return retargets; }
                bool stepOut(int32_t d) { ++stepOuts;stepOutD=d;return true; }
                void install(const RetailFlightGoal& g) { ++installs;goal=g;m.pending&=~0x3700u; }
                bool outOfSlot() { ++outCalls;return out; }
                uint32_t nextFlags() const { return next; }
                void leave() { ++leaves; }
                bool formation() { ++formations;return true; }
            } host{seed,m,attacks!=0,retargets!=0,out!=0,next};
            const int result=retailVtolSeekAttack(m,point,heading,tick,events,in,host);
            std::printf("%d %u %u %u %u %u %u %d %d %d %u %d %d %d %d %u %u %u %u %u %u %u %d %u %u %u\n",result,
                        unsigned(m.stage),m.waitMask,m.deadline,m.pending,m.flags,unsigned(heading),point.x,point.y,
                        point.z,host.installs,host.goal.point.x,host.goal.point.y,host.goal.point.z,
                        int(host.goal.radius),host.attackCalls,host.begin,host.autoSlots,host.retargetCalls,seed,
                        host.calls,host.stepOuts,host.stepOutD,host.outCalls,host.leaves,host.formations);
        }
        return 0;
    }
    // tools/re/check_air_traffic.py: one AirStrike (41b5e0) handler call per
    // line.
    if (argc==2 && std::strcmp(argv[1],"--air-strike")==0) {
        unsigned stage,events,tick,deadline,pending,flags,heading,armed,dropped,flies,unitLive,holdFire,target,
                 targetLive,carried,placed,last,automatic,runLength,seed,retargets,stepOk,out,next;
        int leash,ox,oz,cruiseAlt,gravity,speed,traffic,footX;
        RetailAirStrike in;RetailFlightVector point;
        while (std::scanf("%u %u %u %u %u %u %d %d %d %d %d %d %d %d %d %u %u %u %u %u %u %u %u %u %u %u %d %d %d %u %d %d %d %d %d %u %u %u",
                          &stage,&events,&tick,&deadline,&pending,&flags,&point.x,&point.y,&point.z,&in.position.x,
                          &in.position.y,&in.position.z,&in.targetPoint.x,&in.targetPoint.y,&in.targetPoint.z,&heading,
                          &armed,&dropped,&flies,&unitLive,&holdFire,&target,&targetLive,&carried,&placed,&last,&leash,
                          &ox,&oz,&runLength,&cruiseAlt,&gravity,&speed,&traffic,&footX,&automatic,&seed,&retargets)==38
               && std::scanf("%u %u %u",&stepOk,&out,&next)==3) {
            in.heading=uint16_t(heading);in.armed=armed!=0;in.dropped=dropped!=0;in.flies=flies!=0;
            in.unitLive=unitLive!=0;in.holdFire=holdFire!=0;in.target=target!=0;in.targetLive=targetLive!=0;
            in.targetCarried=carried!=0;in.targetPlaced=placed!=0;in.last=last!=0;in.leash=leash;
            in.origin={int16_t(ox),int16_t(oz)};in.runLength=uint16_t(runLength);in.cruiseAlt=int16_t(cruiseAlt);
            in.gravity=gravity;in.speed=int16_t(speed);in.traffic=int8_t(traffic);in.footX=int16_t(footX);
            in.automatic=automatic!=0;
            RetailMissionState m;m.stage=uint8_t(stage);m.waitMask=0;m.deadline=deadline;m.pending=pending;m.flags=flags;
            struct Host {
                uint32_t& seed;RetailMissionState& m;bool retargets,stepOk,out;uint32_t next;
                unsigned calls=0,begin=0,autoSlots=0,manual=0,aims=0,clears=0,installs=0,follows=0,retargetCalls=0,
                         stepOuts=0,seeks=0,seekTarget=0,outCalls=0,leaves=0,formations=0;
                int32_t stepOutD=0;RetailFlightGoal goal;RetailFlightVector aimed,seekAt;
                uint32_t random(int n) { ++calls;return retailRandom(seed,n); }
                void beginFlight() { ++begin; }
                void slotsAuto() { ++autoSlots; }
                void slotManual() { ++manual; }
                void aimGround(RetailFlightVector p) { ++aims;aimed=p; }
                void clearSlot() { ++clears; }
                void install(const RetailFlightGoal& g) { ++installs;goal=g;m.pending&=~0x3700u; }
                void follow(int32_t radius) { ++follows;goal=RetailFlightGoal{};goal.radius=int16_t(radius);m.pending&=~0x3700u; }
                bool retarget() { ++retargetCalls;return retargets; }
                bool stepOut(int32_t d) { ++stepOuts;stepOutD=d;return stepOk; }
                void seek(bool withTarget,RetailFlightVector at) { ++seeks;seekTarget=withTarget;seekAt=at; }
                bool outOfSlot() { ++outCalls;return out; }
                uint32_t nextFlags() const { return next; }
                void leave() { ++leaves; }
                bool formation() { ++formations;return true; }
            } host{seed,m,retargets!=0,stepOk!=0,out!=0,next};
            const int result=retailAirStrike(m,point,tick,events,in,host);
            std::printf("%d %u %u %u %u %u %d %d %d %u %u %d %d %d %d %u %u %u %u %d %d %u %u %u %u %d %u %u %d %d %d %u %u %u %u\n",
                        result,unsigned(m.stage),m.waitMask,m.deadline,m.pending,m.flags,point.x,point.y,point.z,
                        host.installs,host.follows,host.goal.point.x,host.goal.point.y,host.goal.point.z,
                        int(host.goal.radius),host.begin,host.autoSlots,host.manual,host.aims,host.aimed.x,host.aimed.z,
                        host.clears,host.retargetCalls,host.stepOuts,host.seeks,host.stepOutD,host.seekTarget,
                        seed,host.seekAt.x,host.seekAt.y,host.seekAt.z,host.calls,host.outCalls,host.leaves,
                        host.formations);
        }
        return 0;
    }
    // tools/re/check_air_traffic.py: one VTOL_SeekGuard (4197d0) handler call
    // per line.
    if (argc==2 && std::strcmp(argv[1],"--air-seek-guard")==0) {
        unsigned stage,events,tick,deadline,pending,flags,angle,armed,flies,onMap,seed,found,guards,out;
        int range,width,height,traffic,footX;
        RetailAirSeekGuard in;RetailFlightVector point;
        while (std::scanf("%u %u %u %u %u %u %u %d %d %d %d %d %d %d %u %u %u %d %d %u %u %u %d %d %u",&stage,&events,&tick,
                          &deadline,&pending,&flags,&angle,&point.x,&point.y,&point.z,&in.position.x,&in.position.y,
                          &in.position.z,&range,&armed,&flies,&onMap,&width,&height,&seed,&found,&guards,&traffic,
                          &footX,&out)==25) {
            in.range=range;in.armed=armed!=0;in.flies=flies!=0;in.onMap=onMap!=0;in.width=width;in.height=height;
            in.traffic=int8_t(traffic);in.footX=int16_t(footX);
            RetailMissionState m;m.stage=uint8_t(stage);m.waitMask=0;m.deadline=deadline;m.pending=pending;m.flags=flags;
            uint16_t heading=uint16_t(angle);
            struct Host {
                uint32_t& seed;RetailMissionState& m;bool finds,guards,out;
                unsigned calls=0,foundCalls=0,releases=0,guardCalls=0,installs=0,stepOuts=0,stops=0;
                unsigned outCalls=0,cancels=0,formations=0;
                RetailStepOut kind=RetailStepOut::None;int32_t stepOutD=0;RetailFlightGoal goal;
                uint32_t random(int n) { ++calls;return retailRandom(seed,n); }
                bool stepOut(RetailStepOut k,int32_t d) { ++stepOuts;kind=k;stepOutD=d;return true; }
                void stopBuilding() { ++stops; }
                bool found() { ++foundCalls;return finds; }
                void release() { ++releases;m.pending&=~0x3700u; }
                bool guard() { ++guardCalls;return guards; }
                void install(const RetailFlightGoal& g) { ++installs;goal=g;m.pending&=~0x3700u; }
                bool outOfSlot() { ++outCalls;return out; }
                void cancel() { ++cancels; }
                bool formation() { ++formations;return true; }
            } host{seed,m,found!=0,guards!=0,out!=0};
            const int result=retailVtolSeekGuard(m,point,heading,tick,events,in,host);
            std::printf("%d %u %u %u %u %u %u %d %d %d %u %d %d %d %d %u %u %u %u %u %u %d %u %u %u %u %u\n",result,
                        unsigned(m.stage),m.waitMask,m.deadline,m.pending,m.flags,unsigned(heading),point.x,point.y,
                        point.z,host.installs,host.goal.point.x,host.goal.point.y,host.goal.point.z,
                        int(host.goal.radius),host.foundCalls,host.releases,host.guardCalls,host.stepOuts,
                        unsigned(host.kind),host.stops,host.stepOutD,seed,host.calls,host.outCalls,host.cancels,
                        host.formations);
        }
        return 0;
    }
    // tools/re/check_air_seek_attack.py: the lost-target exits of AirToGround
    // (41c8f6) and AirToAir (41d654), one per line.
    if (argc==2 && std::strcmp(argv[1],"--air-lost")==0) {
        unsigned airToAir,alive,dying,last,holdFire,target,targetLive;
        while (std::scanf("%u %u %u %u %u %u %u",&airToAir,&alive,&dying,&last,&holdFire,&target,&targetLive)==7) {
            const auto push=retailAirLostTarget({airToAir!=0,alive!=0,dying!=0,last!=0,holdFire!=0,target!=0,
                                                 targetLive!=0});
            std::printf("%d %d\n",int(push.push),int(push.target));
        }
        return 0;
    }
    // tools/re/check_air_to_air.py: AirToAir (41ca50) and VTOL_Evade (41e3d0)
    // handler calls, the facing test 41d780 and the moving controller's
    // navigator calls (4e48d0, 4e4b70, 4e4be0), one per line.
    if (argc==2 && std::strcmp(argv[1],"--air-to-air")==0) {
        unsigned stage,events,tick,deadline,pending,flags,heading,targetHeading,distance,automatic,flies,onMap,seed,
                 retarget,evade,out,next;
        int count,speed,maxSpeed,width,height,traffic,footX,targetFootX;
        RetailAirDuel duel;RetailFlightVector lead;
        while (std::scanf("%u %u %u %u %u %u %d %d %d %d %u %d %d %d %u %d %d %d %d %d %d %d %d %u %u %u %u %d %d %u %u %u %d %d %d %u %u",
                          &stage,&events,&tick,&deadline,&pending,&flags,&count,&duel.position.x,&duel.position.y,
                          &duel.position.z,&heading,&duel.target.x,&duel.target.y,&duel.target.z,&targetHeading,
                          &duel.targetVelocity.x,&duel.targetVelocity.y,&duel.targetVelocity.z,&lead.x,&lead.y,
                          &lead.z,&speed,&maxSpeed,&distance,&automatic,&flies,&onMap,&width,&height,&seed,
                          &retarget,&evade,&traffic,&footX,&targetFootX,&out,&next)==37) {
            duel.heading=uint16_t(heading);duel.targetHeading=uint16_t(targetHeading);duel.speed=speed;
            duel.maxSpeed=maxSpeed;duel.distance=uint16_t(distance);duel.automatic=automatic!=0;duel.flies=flies!=0;
            duel.onMap=onMap!=0;duel.width=width;duel.height=height;duel.traffic=int8_t(traffic);
            duel.footX=int16_t(footX);duel.targetFootX=int16_t(targetFootX);
            RetailMissionState m;m.stage=uint8_t(stage);m.waitMask=0;m.deadline=deadline;m.pending=pending;m.flags=flags;
            struct Host {
                uint32_t& seed;RetailMissionState& m;RetailFlightVector led;bool retargets,evades,out;uint32_t next;
                unsigned calls=0,begin=0,aims=0,retargetCalls=0,releases=0,evadeCalls=0,points=0,intercepts=0,stepOuts=0;
                unsigned outCalls=0,leaves=0,formations=0;
                int32_t stepOutD=0;RetailFlightGoal goal;RetailInterceptGoal intercept;
                uint32_t random(int n) { ++calls;return retailRandom(seed,n); }
                void beginFlight() { ++begin; }
                void aim() { ++aims; }
                RetailFlightVector lead() const { return led; }
                bool retarget() { ++retargetCalls;return retargets; }
                void release() { ++releases; }
                bool evade() { ++evadeCalls;return evades; }
                // The fixture's last answer is whether 4d6c40 built the pushed mission.
                bool stepOut(int32_t d) { ++stepOuts;stepOutD=d;return evades; }
                void install(const RetailFlightGoal& g) { ++points;goal=g;m.pending&=~0x3700u; }
                void install(const RetailInterceptGoal& g) { ++intercepts;intercept=g;m.pending&=~0x3700u; }
                bool outOfSlot() { ++outCalls;return out; }
                uint32_t nextFlags() const { return next; }
                void leave() { ++leaves; }
                // As the step-out: the fixture's answer whether 4d6c40 built it.
                bool formation() { ++formations;return evades; }
            } host{seed,m,lead,retarget!=0,evade!=0,out!=0,next};
            const int result=retailAirToAir(m,count,tick,events,duel,host);
            std::printf("%d %u %u %u %u %u %d %u %u %d %d %d %d %d %d %d %d %d %d %u %u %u %u %u %u %u %u %u %d %u %u %u\n",
                        result,
                        unsigned(m.stage),m.waitMask,m.deadline,m.pending,m.flags,count,host.points,host.intercepts,
                        host.goal.point.x,host.goal.point.y,host.goal.point.z,int(host.goal.radius),
                        host.intercept.point.x,host.intercept.point.y,host.intercept.point.z,host.intercept.velocity.x,
                        host.intercept.velocity.y,host.intercept.velocity.z,unsigned(host.intercept.heading),
                        host.begin,host.aims,host.retargetCalls,host.releases,host.evadeCalls,seed,host.calls,
                        host.stepOuts,host.stepOutD,host.outCalls,host.leaves,host.formations);
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--air-evade")==0) {
        unsigned stage,events,tick,deadline,pending,flags,heading,turnRate,targetTurnRate,distance,flies,onMap,seed,out,
                 next,built;
        int width,height;
        RetailAirEvade e;RetailAirEscape in;
        while (std::scanf("%u %u %u %u %u %u %d %d %d %d %d %u %d %d %d %d %d %d %u %u %u %u %u %d %d %u %u %u %u",
                          &stage,&events,&tick,&deadline,&pending,&flags,&e.maneuver,&e.budget,&in.position.x,
                          &in.position.y,&in.position.z,&heading,&in.target.x,&in.target.y,&in.target.z,&in.maxSpeed,
                          &in.typeSpeed,&in.targetTypeSpeed,&turnRate,&targetTurnRate,&distance,&flies,&onMap,&width,
                          &height,&seed,&out,&next,&built)==29) {
            in.heading=uint16_t(heading);in.turnRate=uint16_t(turnRate);in.targetTurnRate=uint16_t(targetTurnRate);
            in.distance=uint16_t(distance);in.flies=flies!=0;in.onMap=onMap!=0;in.width=width;in.height=height;
            auto& m=e.mission;m.stage=uint8_t(stage);m.waitMask=0;m.deadline=deadline;m.pending=pending;m.flags=flags;
            struct Host {
                uint32_t& seed;RetailMissionState& m;bool out;uint32_t next;bool built;
                unsigned calls=0,points=0,intercepts=0,outCalls=0,leaves=0,formations=0;
                RetailFlightGoal goal;RetailInterceptGoal intercept;
                uint32_t random(int n) { ++calls;return retailRandom(seed,n); }
                void install(const RetailFlightGoal& g) { ++points;goal=g;m.pending&=~0x3700u; }
                void install(const RetailInterceptGoal& g) { ++intercepts;intercept=g;m.pending&=~0x3700u; }
                bool outOfSlot() { ++outCalls;return out; }
                uint32_t nextFlags() const { return next; }
                void leave() { ++leaves; }
                bool formation() { ++formations;return built; }
            } host{seed,m,out!=0,next,built!=0};
            const int result=retailAirEvade(e,tick,events,in,host);
            std::printf("%d %u %u %u %u %u %d %d %u %u %d %d %d %d %d %d %d %u %u %u %u %u %u\n",result,unsigned(m.stage),
                        m.waitMask,m.deadline,m.pending,m.flags,e.maneuver,e.budget,host.points,host.intercepts,
                        host.goal.point.x,host.goal.point.y,host.goal.point.z,int(host.goal.radius),
                        host.intercept.point.x,host.intercept.point.y,host.intercept.point.z,
                        unsigned(host.intercept.heading),seed,host.calls,host.outCalls,host.leaves,host.formations);
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--air-ahead")==0) {
        RetailFlightVector a,b;unsigned heading;int octant;
        while (std::scanf("%d %d %d %d %u %d",&a.x,&a.z,&b.x,&b.z,&heading,&octant)==6)
            std::printf("%d\n",int(retailAirAhead(a,b,uint16_t(heading),octant)));
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--air-intercept")==0) {
        RetailInterceptGoal g;RetailFlightVector owner;unsigned heading,turns,turnRate,body;
        while (std::scanf("%d %d %d %d %d %d %u %u %u %d %d %u",&g.point.x,&g.point.y,&g.point.z,&g.velocity.x,
                          &g.velocity.y,&g.velocity.z,&heading,&turns,&turnRate,&owner.x,&owner.z,&body)==12) {
            g.heading=uint16_t(heading);g.turns=turns!=0;owner.y=0;
            const auto answer=g.target(uint16_t(turnRate));
            std::printf("%d %d %d %d %d %d %d %d %d %u\n",answer.x,answer.y,answer.z,g.point.x,g.point.z,
                        g.velocity.x,g.velocity.y,g.velocity.z,int(g.accepts(owner)),
                        unsigned(g.headingFor(owner,uint16_t(body))));
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--hover-attack")==0) {
        using namespace tak::sim;
        RetailFlightVector p,t;int distance;unsigned seed;
        while (std::scanf("%d %d %d %d %d %u",&p.x,&p.z,&t.x,&t.z,&distance,&seed)==6) {
            p.y=t.y=0;unsigned calls=0;
            auto g=retailHoverAttackGoal(p,t,distance,[&](int n) {
                ++calls;seed=seed*1664525u+1013904223u;return seed%unsigned(n);
            });
            std::printf("%d %d %u %u\n",g.point.x,g.point.z,seed,calls);
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--sight-model-top")==0) {
        int count;
        while (std::scanf("%d",&count)==1) {
            if (count<1 || count>128) return 2;
            std::vector<tak::tdo::Object> nodes(size_t(count),tak::tdo::Object{});
            std::vector<int> parents(size_t(count),-1);
            for (int i=0;i<count;++i) {
                int vertices;auto& node=nodes[size_t(i)];
                if (std::scanf("%d %d %d",&parents[size_t(i)],&node.offsetRaw[1],&vertices)!=3 || vertices<0 || vertices>128) return 2;
                node.verticesRaw.resize(size_t(vertices));
                for (auto& v:node.verticesRaw) if (std::scanf("%d",&v[1])!=1) return 2;
            }
            for (int i=count-1;i>0;--i) {
                if (parents[size_t(i)]<0 || parents[size_t(i)]>=i) return 2;
                nodes[size_t(parents[size_t(i)])].children.push_back(std::move(nodes[size_t(i)]));
            }
            std::printf("%d\n",retailModelTop(nodes[0]));
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--exploration-heights")==0) {
        int width,height;unsigned sea;
        while (std::scanf("%d %d %u",&width,&height,&sea)==3) {
            if (width<2 || height<2 || width>128 || height>128) return 2;
            std::vector<uint8_t> input(size_t(width)*height);
            for (auto& h:input) {unsigned value;if (std::scanf("%u",&value)!=1) return 2;h=uint8_t(value);}
            const auto heights=retailExplorationHeights(width,height,uint8_t(sea),
                [&](int x,int z){return input[size_t(z)*width+x];});
            for (const auto& h:heights) std::printf("%u %u ",unsigned(h[0]),unsigned(h[1]));
            std::printf("\n");
        }
        return 0;
    }
    if (argc==2 && (std::strcmp(argv[1],"--sight-footprint")==0 || std::strcmp(argv[1],"--sight-update")==0)) {
        const bool update=std::strcmp(argv[1],"--sight-update")==0;
        int width,height,x,z,eye,distance;unsigned sh,oldActive,active,explore,player;
        while (std::scanf("%d %d %d %d %d %d %u %u %u %u %u",&width,&height,&x,&z,&eye,&distance,
                &sh,&oldActive,&active,&explore,&player)==11) {
            if (width<1 || height<1 || width>128 || height>128) return 2;
            int32_t px=0,py=0,pz=0;unsigned sea=0;
            if (update && std::scanf("%d %d %d %u",&px,&py,&pz,&sea)!=4) return 2;
            std::vector<RetailExplorationHeight> heights(size_t(width)*height);
            for (auto& h:heights) {
                unsigned a,b;if (std::scanf("%u %u",&a,&b)!=2) return 2;h={uint8_t(a),uint8_t(b)};
            }
            RetailSightFootprint sight{int16_t(x),int16_t(z),eye,int16_t(distance),uint8_t(sh),oldActive!=0};
            std::vector<int> visits;
            const auto heightAt=[&](int cx,int cz){return heights[size_t(cz)*width+cx];};
            const auto visit=[&](int cx,int cz,int delta,uint16_t mask){visits.insert(visits.end(),{cx,cz,delta,int(mask)});};
            if (update) retailUpdateSight(sight,Fixed::raw(px),Fixed::raw(py),Fixed::raw(pz),uint8_t(sea),
                explore!=0,width,height,uint8_t(player),heightAt,visit);
            else retailSightFootprint(sight,active!=0,explore!=0,width,height,uint8_t(player),heightAt,visit);
            std::printf("%u",unsigned(sight.active));
            if (update) std::printf(" %d %d %d",int(sight.x),int(sight.z),sight.eyeHeight);
            for (auto v:visits) std::printf(" %d",v);
            std::printf("\n");
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--ground-scan")==0) {
        int32_t x,y,z;unsigned heading,tick,half,boat,disabled,player,mask;
        int foot,sight,width,height,badIndex,badGrade,baseGrade;
        while (std::scanf("%d %d %d %u %u %u %u %d %d %u %d %d %u %u %d %d %d",
                &x,&y,&z,&heading,&tick,&half,&boat,&foot,&sight,&disabled,
                &width,&height,&player,&mask,&badIndex,&badGrade,&baseGrade)==17) {
            std::vector<int32_t> queries;int probe=0;
            const auto result=retailGroundScan(Fixed::raw(x),Fixed::raw(y),Fixed::raw(z),
                retailHeadingToPort(uint16_t(heading)),tick,uint8_t(half),boat!=0,
                int16_t(foot),int16_t(sight),disabled!=0,
                [&](Fixed px,Fixed py,Fixed pz) {
                    queries.insert(queries.end(),{0,px.v,py.v,pz.v});
                    return probe++==badIndex?badGrade:baseGrade;
                },[&](Fixed px,Fixed py,Fixed pz) {
                    return retailBoatScanVisible(px,py,pz,width,height,uint8_t(player),
                        [&](int cx,int cz) {
                            queries.insert(queries.end(),{1,cx,cz});return uint16_t(mask);
                        });
                });
            std::printf("%u %u %u",result.nextTick,unsigned(result.movementMode),unsigned(result.speedMode));
            for (auto q:queries) std::printf(" %d",q);
            std::printf("\n");
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--ground-travel")==0) {
        int32_t speed,maximum,accel,brake,road,water;unsigned rate,pitch,speedMode,heading,flags,mode;
        while (std::scanf("%d %d %d %d %u %d %d %u %u %u %u %u",&speed,&maximum,&accel,&brake,
                          &rate,&road,&water,&pitch,&speedMode,&heading,&flags,&mode)==12) {
            std::array<RetailSteeringPoint,3> points;
            for (auto& point:points) if (std::scanf("%d %d",&point.x.v,&point.z.v)!=2) return 2;
            World world;UnitType type;
            type.turnRate=int32_t(rate);type.accel=Fixed::raw(accel);type.brake=Fixed::raw(brake);
            type.roadMult=Fixed::raw(road);type.waterMult=Fixed::raw(water);
            Unit u;u.type=&type;u.speed=Fixed::raw(speed);u.baseSpeed=Fixed::raw(maximum);
            u.groundPitch=uint16_t(pitch);u.groundSpeedMode=uint8_t(speedMode);
            u.groundTerrainFlags=uint16_t(flags);u.groundMovementMode=uint8_t(mode);
            u.heading=retailHeadingToPort(uint16_t(heading));
            const auto step=world.steerGround(u,points[0],points[1],points[2],u.baseSpeed);
            std::printf("%u %d %d %d\n",unsigned(portHeadingToRetail(u.heading)),u.speed.v,step.s.v,step.c.v);
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--ground-brake")==0) {
        int32_t speed,maximum,brake,road,water,facing;unsigned pitch,mode,heading,flags,rate;
        while (std::scanf("%d %d %d %d %d %u %u %u %u %d %u",&speed,&maximum,&brake,&road,&water,
                          &pitch,&mode,&heading,&flags,&facing,&rate)==11) {
            World world;UnitType type;type.footX=type.footZ=1;
            type.turnInPlaceRate=int32_t(rate);type.brake=Fixed::raw(brake);type.roadMult=Fixed::raw(road);type.waterMult=Fixed::raw(water);
            Unit u;u.type=&type;u.x=u.z=Fixed::fromInt(128);
            u.speed=Fixed::raw(speed);u.baseSpeed=Fixed::raw(maximum);
            u.groundPitch=uint16_t(pitch);u.groundSpeedMode=uint8_t(mode);
            u.groundTerrainFlags=uint16_t(flags);u.heading=retailHeadingToPort(uint16_t(heading));
            world.brakeGround(u,facing<0?std::nullopt:std::optional<Bam>(retailHeadingToPort(uint16_t(facing))));
            std::printf("%d %d %d %u\n",u.speed.v,u.x.v-128*65536,u.z.v-128*65536,
                        unsigned(portHeadingToRetail(u.heading)));
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--ground-speed")==0) {
        int32_t speed,maximum,delta;unsigned pitch,mode,heading;
        while (std::scanf("%d %d %d %u %u %u",&speed,&maximum,&delta,&pitch,&mode,&heading)==6) {
            const Fixed value=fxMin(retailGroundSpeedCap(Fixed::raw(maximum),uint16_t(pitch),uint8_t(mode)),
                fxMax(Fixed(),Fixed::raw(speed)+Fixed::raw(delta)));
            const auto step=retailGroundStep(retailHeadingToPort(uint16_t(heading)),value);
            std::printf("%d %d %d\n",value.v,step.s.v,step.c.v);
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--ground-height")==0) {
        int width,height;
        while (std::scanf("%d %d",&width,&height)==2) {
            if (width<2 || height<2 || width>256 || height>256) return 2;
            std::vector<int> cells(size_t(width)*size_t(height));
            for (auto& h:cells) if (std::scanf("%d",&h)!=1 || h<0 || h>255) return 2;
            int32_t x,z,oldY; unsigned heading,sea,waterline,mode;
            if (std::scanf("%d %d %d %u %u %u %u",&x,&z,&oldY,&heading,&sea,&waterline,&mode)!=7) return 2;
            RetailGroundSupport support{};
            for (auto& point:support)
                if (std::scanf("%d %d",&point[0],&point[1])!=2) return 2;
            uint32_t clock,elapsed; unsigned phase; int32_t speed,effectiveSpeed;
            if (std::scanf("%u %u %u %d %d",&clock,&elapsed,&phase,&speed,&effectiveSpeed)!=5) return 2;
            int32_t bankScale,pitchScale;unsigned oldPitch,oldRoll;
            if (std::scanf("%d %d %u %u",&bankScale,&pitchScale,&oldPitch,&oldRoll)!=4) return 2;
            uint16_t pitch=uint16_t(oldPitch),roll=uint16_t(oldRoll);
            auto terrain=[&](int32_t px,int32_t pz) {
                return retailTerrainHeight(px,pz,width,height,[&](int cx,int cz) {
                    return cells[size_t(cz)*size_t(width)+size_t(cx)];
                });
            };
            const int sampled=terrain(x,z);
            const int32_t y=mode==2?retailFloatingHeight(uint8_t(sea),uint8_t(waterline)):
                mode>=3?retailSupportedHeight(x,z,oldY,uint16_t(heading),support,terrain,
                    [&](size_t i,int h){return mode==4?retailHoverHeightCorner(h,uint8_t(sea),i,
                        clock,uint16_t(phase),speed,effectiveSpeed,elapsed):h;},bankScale,pitchScale,&pitch,&roll):
                retailUprightHeight(sampled,mode==1,uint8_t(sea),uint8_t(waterline));
            std::printf("%d %d %u %u\n",sampled,y,unsigned(pitch),unsigned(roll));
        }
        return 0;
    }
    // Offline oracle interface for the emulator/live-trace comparison script.
    if (argc == 2 && std::strcmp(argv[1], "--oracle") == 0) {
        char kind;
        double input;
        int b;
        while (std::scanf(" %c %lf %d", &kind, &input, &b) == 3) {
            int a = int(input);
            if (kind == 'b') {
                uint32_t tick,seed;
                if (std::scanf("%u %u",&tick,&seed)!=2) return 2;
                RetailPlayerCacheClock clock{a!=0,uint32_t(b)};
                bool refresh=retailTickPlayerCache(clock,tick,[&](int n){return retailRandom(seed,n);});
                std::printf("%u %u %u\n",clock.lastRefresh,seed,unsigned(refresh));
            }
            else if (kind == 'l') {
                int pz,ax,ay,az,fx,fz,accept; uint32_t events,angle,seed;
                if (std::scanf("%d %d %d %d %d %d %u %u %u %d",&pz,&ax,&ay,&az,&fx,&fz,&events,&angle,&seed,&accept)!=10) return 2;
                bool orbit=false; int probes=0;
                const auto point=retailLandingSearch({a,b,pz},{ax,ay,az},int16_t(fx),int16_t(fz),events,angle,orbit,
                    [&](int n){return retailRandom(seed,n);},[&](auto){return ++probes==accept;});
                std::printf("%d %d %d %u %u %d %d\n",point.x,point.y,point.z,angle,seed,int(orbit),probes);
            }
            else if (kind == 'k') {
                unsigned stage; uint32_t seed; RetailMissionState m; int scans=0;
                if (std::scanf("%u %u %u %u",&stage,&m.waitMask,&m.deadline,&seed)!=4) return 2;
                m.stage=uint8_t(stage);
                const int result=retailGuardNoMove(m,uint32_t(a),(b&2)!=0,(b&8)!=0,
                    [&](int n){return retailRandom(seed,n);},[&]{++scans;});
                std::printf("%d %u %u %u %u %d\n",result,unsigned(m.stage),m.waitMask,m.deadline,seed,scans);
            }
            else if (kind == 'w') {
                RetailWind wind;
                uint32_t tick, seed, crtSeed; unsigned heading, flags;
                wind.minimum=a; wind.maximum=b;
                if (std::scanf("%u %u %d %d %d %u %u %u %u", &tick,&wind.nextTick,
                        &wind.x,&wind.z,&wind.speed,&heading,&flags,&seed,&crtSeed)!=9) return 2;
                wind.heading=uint16_t(heading); wind.flags=uint16_t(flags);
                retailTickWind(wind,tick,[&](int n){return retailRandom(seed,n);},
                               [&]{return retailCrtRandom(crtSeed);});
                std::printf("%u %d %d %d %u %u %u %u\n",wind.nextTick,wind.x,wind.z,wind.speed,
                            unsigned(wind.heading),unsigned(wind.flags),seed,crtSeed);
            }
            else if (kind == 'j') {
                int pz,tx,ty,tz,maximum; uint32_t seed;
                if (std::scanf("%d %d %d %d %d %u",&pz,&tx,&ty,&tz,&maximum,&seed)!=6) return 2;
                const auto goal=retailFlightPatrolGoal({a,b,pz},{tx,ty,tz},maximum,
                    [&](int n) { return retailRandom(seed,n); });
                std::printf("%d %d %d %d %u\n",goal.point.x,goal.point.y,goal.point.z,int(goal.radius),seed);
            }
            else if (kind == 'n') {
                int pz, ox, oy, oz, tx, ty, tz, heading, height, cruise, hasHeading, controllerHeading;
                if (std::scanf("%d %d %d %d %d %d %d %d %d %d %d %d", &pz, &ox, &oy, &oz,
                        &tx, &ty, &tz, &heading, &height, &cruise, &hasHeading, &controllerHeading) != 12) return 2;
                const auto nav = retailFlightNavigation({a,b,pz}, {ox,oy,oz}, {tx,ty,tz},
                    uint16_t(heading), height, cruise != 0, hasHeading != 0, uint16_t(controllerHeading));
                std::printf("%d %d %d %d %d %d %u\n", nav.destination.x, nav.destination.y,
                    nav.destination.z, nav.velocity.x, nav.velocity.y, nav.velocity.z, nav.heading);
            }
            else if (kind == 'v') {
                int vz, px, py, pz, tx, ty, tz, vx, vy, targetVz, speed, maximum, accel, lateral, heading, direct;
                if (std::scanf("%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
                        &vz, &px, &py, &pz, &tx, &ty, &tz, &vx, &vy, &targetVz,
                        &speed, &maximum, &accel, &lateral, &heading, &direct) != 16) return 2;
                const auto velocity = retailFlightVelocity({a,b,vz}, {px,py,pz}, {tx,ty,tz},
                    {vx,vy,targetVz}, speed, maximum, accel, lateral, uint16_t(heading), direct != 0);
                std::printf("%d %d %d\n", velocity.x, velocity.y, velocity.z);
            }
            else if (kind == 'q' || kind == 'Q') {
                int sx,sz,ex,ez,tx,tz,heading,rate;
                if (std::scanf("%d %d %d %d %d %d %d %d",&sx,&sz,&ex,&ez,&tx,&tz,&heading,&rate)!=8) return 2;
                int speedMode=0;
                if (kind=='Q' && std::scanf("%d",&speedMode)!=1) return 2;
                std::printf("%d\n",int(retailPruneGroundCorner(Fixed::raw(a),Fixed::raw(b),
                    {Fixed::raw(sx),Fixed::raw(sz)},{Fixed::raw(ex),Fixed::raw(ez)},
                    {Fixed::raw(tx),Fixed::raw(tz)},retailHeadingToPort(uint16_t(heading)),uint16_t(rate),
                    kind=='Q',uint8_t(speedMode))));
            }
            else if (kind == 'g') {
                int sx, sz, ex, ez, tx, tz, heading, speed, maximum, accel, brake, rate;
                if (std::scanf("%d %d %d %d %d %d %d %d %d %d %d %d", &sx, &sz, &ex, &ez,
                               &tx, &tz, &heading, &speed, &maximum, &accel, &brake, &rate) != 12) return 2;
                const auto delta = retailGroundAcceleration(Fixed::raw(a), Fixed::raw(b),
                    {Fixed::raw(sx), Fixed::raw(sz)}, {Fixed::raw(ex), Fixed::raw(ez)},
                    {Fixed::raw(tx), Fixed::raw(tz)}, retailHeadingToPort(uint16_t(heading)),
                    Fixed::raw(speed), Fixed::raw(maximum), Fixed::raw(accel), Fixed::raw(brake), uint16_t(rate));
                std::printf("%d\n", delta.v);
            }
            else if (kind == 'p') {
                int sx, sz, ex, ez, mode;
                if (std::scanf("%d %d %d %d %d", &sx, &sz, &ex, &ez, &mode) != 5) return 2;
                auto point = retailSteeringPoint(Fixed::raw(a), Fixed::raw(b),
                    {Fixed::raw(sx), Fixed::raw(sz)}, {Fixed::raw(ex), Fixed::raw(ez)}, unsigned(mode));
                std::printf("%d %d\n", point.x.v, point.z.v);
            }
            else if (kind == 'f') std::printf("%d\n", Fixed::fromRetailNumber(input).v);
            else if (kind == 'h') std::printf("%d\n", retailDirection(Fixed::raw(a), Fixed::raw(b)).v);
            else if (kind == 'i') std::printf("%u\n", retailSeed(uint32_t(a)));
            else if (kind == 'a') std::printf("%d\n", retailTurnTravel(Fixed::raw(a),
                                                uint16_t(b), uint16_t(uint32_t(b) >> 16)).v);
            else if (kind == 't') std::printf("%d %d\n", retailScaledSine(uint16_t(a), b),
                                                                  retailScaledCosine(uint16_t(a), b));
            else if (kind == 'c' || kind == 'd')
                std::printf("%d\n", footprintClamp(Fixed::fromInt(160), Fixed::raw(a), b, kind == 'd').v);
            else if (kind == 'o') std::printf("%d\n", footprintOrigin(Fixed::raw(a), b));
            else if (kind == 's') std::printf("%d\n", individualSpeed(Fixed::raw(a), unsigned(b)).v);
            else if (kind == 'r') {
                uint32_t seed = uint32_t(a);
                auto result = retailRandom(seed, b);
                std::printf("%u %u\n", result, seed);
            } else return 2;
        }
        return 0;
    }
    int failures = 0;
    // Golden from the preceding per-call Q60 implementation (GCC and Clang):
    // every 16-bit angle, a nonsymmetric pair, and signed overflow boundaries.
    uint64_t rotationHash=1469598103934665603ull;
    for (uint32_t angle=0;angle<65536;++angle)
        for (auto pair:{std::array<int32_t,2>{123456789,-987654321},
                       std::array<int32_t,2>{INT32_MIN,INT32_MAX}}) {
            retailRotatePair(pair[0],pair[1],uint16_t(angle));
            for (int32_t value:pair) for (unsigned byte=0;byte<4;++byte) {
                rotationHash^=(uint32_t(value)>>(byte*8))&255;
                rotationHash*=1099511628211ull;
            }
        }
    if (rotationHash!=0x930f68c69f30a3b6ull) {
        std::fprintf(stderr,"rotation coefficient cache differs from prior math\n");
        ++failures;
    }
    auto check = [&](bool ok, const char* what) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
        failures += !ok;
    };
    {
        World headless,viewed;UnitType type;
        type.id="explorer";type.maxHp=100;type.maxVel=Fixed::fromInt(2);type.upright=true;
        type.sight=128;type.sightHeight=32;
        for (World* world:{&headless,&viewed}) {
            world->setPlayerCount(3);world->setTeam(2,0);
            world->setTerrain(std::vector<uint8_t>(64*64,0),64,64,0);
            world->spawn(&type,256,256,{},0);world->tick(1.0f/30.0f);
        }
        const size_t center=8*32+8;
        check(headless.navigationExploration()[center]==5 && headless.navigationExploration()[0]==0,
              "navigation exploration reveals allies' cells without revealing the whole map");
        check(headless.navigationExploration()==viewed.navigationExploration() && headless.stateHash()==viewed.stateHash(),
              "navigation exploration and its hash do not depend on the local viewer");
        const auto revealed=headless.navigationExploration();
        headless.unit(headless.units().front().id)->hp=Fixed();
        headless.tick(1.0f/30.0f);
        check(headless.navigationExploration()==revealed,"exploration survives loss of its sight source");
        headless.resetForReplay();
        check(headless.navigationExploration().empty(),"replay reset removes old exploration");
    }
    for (bool shore:{false,true}) {
        World world;world.setPlayerCount(2);
        std::vector<uint8_t> terrain(64*64,0);
        if (shore) for (int z=36;z<64;++z) for (int x=0;x<64;++x) terrain[size_t(z)*64+x]=100;
        world.setTerrain(terrain,64,64,32);
        UnitType boat;boat.id="scan-boat";boat.maxHp=100;boat.maxVel=Fixed::fromInt(4);
        boat.floater=true;boat.domain=UnitType::Domain::Water;boat.minWaterDepth=13;boat.maxWaterDepth=255;
        boat.footX=boat.footZ=2;boat.halfCellTicks=6;boat.sight=shore?512:64;boat.sightHeight=32;
        const int id=world.spawn(&boat,512,512,{},1);
        world.unit(id)->heading=Bam(0);world.order(id,512,900,false);
        world.updateNavigationExploration();
        world.updateGroundTerrainFlags(*world.unit(id));
        world.tickNavigationMovement(*world.unit(id),world.unit(id)->baseSpeed);
        check(world.unit(id)->groundSpeedMode==(shore?2:1) && world.unit(id)->groundScanTick==unsigned(shore?18:12),
              shore?"boat shoreline scan selects strong slowdown and triple interval":
                    "boat unexplored-water scan selects moderate slowdown and double interval");
        if (!shore) {
            UnitType spotter=boat;spotter.id="spotter";spotter.canFly=true;spotter.sight=256;
            const int spotterId=world.spawn(&spotter,512,640,{},1);
            world.unit(spotterId)->flightGroundMode=2;
            world.updateNavigationExploration();world.unit(id)->groundScanTick=0;
            world.tickNavigationMovement(*world.unit(id),world.unit(id)->baseSpeed);
            check(world.unit(id)->groundSpeedMode==0 && world.unit(id)->groundScanTick==6,
                  "boat uses its owner's exploration and clears slowdown after the route is revealed");
        }
    }
    {
        World world;
        std::vector<uint16_t> roads(64*64,0xfffb);
        world.setTerrain(std::vector<uint8_t>(64*64,0),64,64,0,&roads);
        UnitType type;type.id="road-turn";type.canMove=true;type.upright=true;
        type.maxVel=Fixed::fromInt(4);type.accel=Fixed::fromRetailNumber(0.5);
        type.turnRate=1000;type.roadMult=Fixed::fromRetailNumber(1.5);
        const int id=world.spawn(&type,512,512);
        world.unit(id)->baseSpeed=Fixed::fromInt(4);world.unit(id)->heading=retailHeadingToPort(0);
        world.order(id,900,512,false);world.tick(1.0f/30.0f);
        check(portHeadingToRetail(world.unit(id)->heading)==uint16_t(-1500) &&
              world.unit(id)->speed==Fixed::fromRetailNumber(0.75),
              "live road movement scales turning and acceleration together");
    }
    {
        World world;UnitType type;type.turnRate=65535;type.roadMult=Fixed::fromInt(2);
        Unit u;u.type=&type;u.baseSpeed=Fixed::fromInt(4);u.groundTerrainFlags=0x800;
        u.heading=retailHeadingToPort(35720);
        const auto step=world.steerGround(u,{},{},{},u.baseSpeed);
        check(portHeadingToRetail(u.heading)==0 && step.s==Fixed() && step.c==Fixed(),
              "a coincident active waypoint uses native heading zero");
        type.turnRate=type.turnInPlaceRate=1;type.roadMult=Fixed::fromInt(1);
        u.heading=retailHeadingToPort(32768);
        world.steerGround(u,{},{},{},u.baseSpeed);
        check(portHeadingToRetail(u.heading)==32767,"active half-turn uses the native negative direction");
        u.heading=retailHeadingToPort(32768);u.speed=Fixed();
        world.brakeGround(u,retailHeadingToPort(0));
        check(portHeadingToRetail(u.heading)==32767,"stopped half-turn uses the native negative direction");
    }
    for (bool boat:{false,true}) {
        World world;
        world.setTerrain(std::vector<uint8_t>(64*64,0),64,64,boat?32:0);
        UnitType type;type.id=boat?"braking-boat":"braking-land";type.canMove=true;
        type.floater=boat;type.upright=!boat;type.maxVel=Fixed::fromInt(4);
        type.brake=Fixed::fromInt(1);type.waterline=2;
        type.domain=boat?UnitType::Domain::Water:UnitType::Domain::Ground;
        type.maxWaterDepth=boat?255:0;type.minWaterDepth=boat?13:0;
        const int id=world.spawn(&type,512,512);
        auto& u=*world.unit(id);u.baseSpeed=u.speed=Fixed::fromInt(4);
        u.heading=retailHeadingToPort(0);
        world.stop(id);
        for (int i=0;i<5;++i) world.tick(1.0f/30.0f);
        check(world.unit(id)->speed==Fixed() && world.unit(id)->z==Fixed::fromInt(506),
              boat?"stopped boat coasts to rest":"stopped land unit coasts to rest");
        check(world.unit(id)->groundY==Fixed::fromInt(boat?30:0),"braking retains surface height");
    }
    for (bool boat:{false,true}) {
        World world;
        world.setTerrain(std::vector<uint8_t>(64*64,0),64,64,boat?32:0);
        UnitType type;type.id=boat?"attacking-boat":"attacking-land";type.canMove=true;
        type.floater=boat;type.upright=!boat;type.maxVel=Fixed::fromInt(4);
        type.brake=Fixed::fromInt(1);type.turnInPlaceRate=4096;
        type.domain=boat?UnitType::Domain::Water:UnitType::Domain::Ground;
        type.maxWaterDepth=boat?255:0;type.minWaterDepth=boat?13:0;
        type.weapon.range=64;type.weapon.damage=1;type.weapon.aimTol=1;type.weapons={type.weapon};
        UnitType victim=type;victim.maxHp=10000;victim.weapons.clear();victim.weapon.damage=0;
        const int id=world.spawn(&type,512,512,0,0);
        const int target=world.spawn(&victim,560,512,0,1);
        world.unit(id)->baseSpeed=world.unit(id)->speed=Fixed::fromInt(4);
        world.unit(id)->heading=retailHeadingToPort(0);
        world.attack(id,target,false);
        world.tick(1.0f/30.0f);
        check(world.unit(id)->z==Fixed::fromInt(509) && world.unit(id)->speed==Fixed::fromInt(3) &&
              portHeadingToRetail(world.unit(id)->heading)==0,
              boat?"attacking boat coasts once without pivoting":"attacking land unit coasts once without pivoting");
        for (int i=0;i<3;++i) world.tick(1.0f/30.0f);
        check(world.unit(id)->z==Fixed::fromInt(506) && world.unit(id)->speed==Fixed() &&
              portHeadingToRetail(world.unit(id)->heading)==uint16_t(-4096),
              "combat pivot starts on the tick braking reaches zero");
        const int edge=world.spawn(&type,256,256,0,0);
        const int edgeTarget=world.spawn(&victim,256,316,0,1);
        world.unit(edge)->baseSpeed=world.unit(edge)->speed=Fixed::fromInt(4);
        world.unit(edge)->heading=retailHeadingToPort(0);
        world.attack(edge,edgeTarget,false);
        world.tick(1.0f/30.0f);
        check(world.unit(edge)->z==Fixed::fromInt(253) && world.unit(edge)->speed==Fixed::fromInt(3),
              "coasting out of attack range does not run movement twice in one tick");
    }
    {
        World world;
        std::vector<uint8_t> heights(64*64);
        for (int z=0;z<64;++z) for (int x=0;x<64;++x) heights[size_t(z)*64+x]=uint8_t(z*4);
        world.setTerrain(heights,64,64,0);
        UnitType mover;mover.id="tilting-ramp";mover.canMove=true;mover.maxVel=Fixed::fromInt(4);
        mover.footX=mover.footZ=2;mover.bankScale=mover.pitchScale=Fixed::fromInt(1);
        mover.groundSupport=RetailGroundSupport{{{{-8*65536,-8*65536}},{{8*65536,-8*65536}},
                                                {{8*65536,8*65536}},{{-8*65536,8*65536}}}};
        const int id=world.spawn(&mover,512,512);
        auto& u=*world.unit(id);u.heading=retailHeadingToPort(0);
        u.groundY=world.surfaceHeight(u,0,&u.groundPitch,&u.groundRoll);
        check(u.groundPitch==uint16_t(-2555) && u.groundRoll==0 && u.groundY==Fixed::fromInt(128),
              "model support retains the ground pitch used by the mover");
        u.baseSpeed=u.speed=Fixed::fromInt(4);
        world.order(id,512,128,false);
        world.tick(1.0f/30.0f);
        check(world.unit(id)->speed.v==222820 && world.unit(id)->z.v==512*65536-222820,
              "live ramp movement applies the prior support pitch to speed and displacement");
    }
    {
        World world;
        world.setTerrain(std::vector<uint8_t>(16*16,0),16,16,0);
        std::vector<uint16_t> features(16*16,0xffff);features[5*16+8]=0;
        RetailMapFeatureType rock;rock.name="rock";rock.blocking=true;
        world.setMapPlacementFeatures(features,{rock});
        UnitType rectangle;rectangle.id="rectangle";rectangle.canMove=true;rectangle.upright=true;
        rectangle.footX=2;rectangle.footZ=4;rectangle.maxVel=Fixed::fromInt(4);
        const int id=world.spawn(&rectangle,96,96);
        auto& u=*world.unit(id);u.baseSpeed=Fixed::fromInt(4);u.speed=u.baseSpeed;
        world.commitGroundStep(u,Fixed::fromInt(16),Fixed());
        check(u.x==Fixed::fromInt(112) && u.bodyBlockStreak==0,
              "movement commitment uses rectangular width beside an obstacle outside that footprint");
        world.commitGroundStep(u,Fixed::fromInt(16),Fixed());
        check(u.x.v==120*65536-1 && u.z==Fixed::fromInt(96) && u.speed==Fixed::fromInt(2) && u.bodyBlockStreak==1,
              "first refused rectangular step clamps to its original cell and halves speed");
        world.commitGroundStep(u,Fixed::fromInt(16),Fixed());
        check(u.x.v==120*65536-1 && u.speed.v==4*65536/5 && u.bodyBlockStreak==2,
              "repeated refusal retains ownership and caps speed at one fifth");
        const auto before=u.x;
        world.commitGroundStep(u,Fixed::fromInt(-1),Fixed());
        check(u.x==before-Fixed::fromInt(1) && u.bodyBlockStreak==2,
              "movement inside the current cell preserves the prior refusal state");
    }
    {
        World world;
        world.setTerrain(std::vector<uint8_t>(32*32,64),32,32,100);
        UnitType walker;walker.id="height-walker";walker.canMove=true;walker.upright=true;
        walker.maxVel=Fixed::fromInt(2);walker.maxWaterDepth=255;
        UnitType hover=walker;hover.id="height-hover";hover.canHover=true;
        UnitType boat=walker;boat.id="height-boat";boat.upright=false;boat.floater=true;boat.waterline=8;
        const int a=world.spawn(&walker,128,128),b=world.spawn(&hover,192,128),c=world.spawn(&boat,256,128);
        check(world.unit(a)->groundY==Fixed::fromInt(64) && world.unit(b)->groundY==Fixed::fromInt(100) &&
              world.unit(c)->groundY==Fixed::fromInt(92),"surface spawn uses upright, hover and boat height rules");
        world.tick(1.0f/30.0f);
        check((world.unit(a)->groundTerrainFlags&0x1000) && !(world.unit(b)->groundTerrainFlags&0x1000) &&
              (world.unit(c)->groundTerrainFlags&0x1000),"water flags follow stored body height rather than submerged terrain");
    }
    {
        World world;
        std::vector<uint8_t> heights(32*32);
        for (int z=0;z<32;++z) for (int x=0;x<32;++x) heights[size_t(z)*32+x]=uint8_t(x*4);
        world.setTerrain(heights,32,32,16);
        UnitType walker;walker.id="ramp-walker";walker.canMove=true;walker.upright=true;
        walker.maxVel=Fixed::fromInt(2);walker.maxWaterDepth=255;walker.minWaterDepth=-255;
        walker.waterMult=Fixed::fromRetailNumber(0.5);walker.roadMult=Fixed::fromInt(1);
        const int id=world.spawn(&walker,55,128);
        world.order(id,144,128,false);
        bool correct=true,crossed=false;
        for (int i=0;i<150;++i) {
            const int previous=world.unit(id)->groundY.floorInt();
            world.tick(1.0f/30.0f);
            const auto& u=*world.unit(id);
            correct=correct && u.groundY.floorInt()==u.x.floorInt()/4 &&
                bool(u.groundTerrainFlags&0x1000)==(previous<16);
            crossed=crossed || (previous<16 && u.groundY.floorInt()>=16);
        }
        check(correct && crossed,"ramp traversal updates height after movement and changes water speed flags on the following tick");
    }
    {
        World world;
        world.setGameSeed(123);
        world.setWindRange(25,5000);
        std::vector<World::RngObservation> draws;
        world.setRngObserver([&](const auto& draw){draws.push_back(draw);});
        world.tick(1.0f/30.0f);
        const auto first=world.wind();
        check(draws.size()==2 && draws[0].bound==4975 && draws[1].bound==16384 &&
              first.generation==1 && (first.flags&1), "live wind uses the shared speed and bearing RNG in order");
        for (uint32_t tick=1;tick<first.nextTick;++tick) world.tick(1.0f/30.0f);
        check(draws.size()==2 && world.wind().generation==1 && !(world.wind().flags&1),
              "wind waits through the exact deadline without advancing either game draw");
        world.tick(1.0f/30.0f);
        check(draws.size()==4 && world.wind().generation==2, "wind changes on the tick after its deadline");
        world.resetForReplay();
        world.tick(1.0f/30.0f);
        check(world.wind().speed==first.speed && world.wind().heading==first.heading &&
              world.wind().nextTick==first.nextTick, "replay reset restores both wind random streams");
        World calm;
        calm.setWindRange(0,0);
        std::vector<World::RngObservation> calmDraws;
        calm.setRngObserver([&](const auto& draw){calmDraws.push_back(draw);});
        calm.tick(1.0f/30.0f);
        check(calmDraws.size()==1 && calmDraws[0].bound==0 && calm.wind().heading==0,
              "zero-speed wind does not draw a new bearing");
    }
    {
        World world; world.setPlayerCount(2);
        for (int i=0;i<2;++i) world.player(i).cacheClock.enabled=true;
        std::vector<World::RngObservation> draws;
        world.setRngObserver([&](const auto& draw){draws.push_back(draw);});
        for (int i=0;i<6;++i) world.tick(1.0f/30.0f);
        check(draws.empty(),"player bookkeeping waits until its seventh tick");
        world.tick(1.0f/30.0f);
        check(draws.size()==2 && draws[0].bound==30 && draws[1].bound==30 &&
              world.player(0).cacheClock.lastRefresh==7 && world.player(1).cacheClock.lastRefresh==7,
              "both human and AI player slots consume their bookkeeping draws in the simulation");
        const auto hash=world.stateHash(); world.player(1).cacheClock.lastRefresh=8;
        check(world.stateHash()!=hash,"player bookkeeping deadline participates in lockstep hash");
        world.player(1).cacheClock.enabled=false;
        for (int i=0;i<7;++i) world.tick(1.0f/30.0f);
        check(draws.size()==3 && draws.back().bound==30,
              "absent player bookkeeping does not advance shared random state");
    }
    {
        RetailMissionState m;
        int scans=0,draws=0;
        auto random=[&](int bound){++draws; return bound-1;};
        retailGuardNoMove(m,42,true,true,random,[&]{++scans;});
        check(m.deadline==63 && m.waitMask==1 && scans==1 && draws==1,
              "armed stationary guard scans and schedules its randomized repeat");
        retailGuardNoMove(m,42,true,false,random,[&]{++scans;});
        check(m.deadline==192 && scans==1 && draws==1,
              "unarmed stationary guard sleeps without consuming random state");
        check(retailGuardNoMove(m,42,false,true,random,[&]{++scans;})==8 && scans==1,
              "disabled stationary guard retires without a scan");
    }
    {
        World world;
        world.setTerrain(std::vector<uint8_t>(64*64,10),64,64,0);
        UnitType flyer;flyer.canFly=flyer.canMove=true;flyer.footX=flyer.footZ=2;
        flyer.maxVel=Fixed::fromInt(4);flyer.accel=flyer.brake=Fixed::fromInt(1);
        flyer.turnRate=1200;flyer.cruiseAlt=80;
        const int id=world.spawn(&flyer,128,128);
        Order move;move.x=Fixed::fromFloat(300.25f);move.z=Fixed::fromFloat(700.5f);
        move.flightMoveMission=move.goal=true;move.mission.flags=0x1000403;
        Order patrol;patrol.x=Fixed::fromInt(500);patrol.z=Fixed::fromInt(500);
        patrol.patrol=patrol.goal=true;patrol.mission.flags=0x1001413;
        world.unit(id)->orders={move,patrol};
        world.tick(1.0f/30.0f);
        const auto& active=world.unit(id)->orders.front();
        check(active.flightMoveMission && active.x==Fixed::fromInt(304) && active.z==Fixed::fromInt(704) &&
              active.mission.stage==2 && active.mission.waitMask==0x701 && active.flightGoal &&
              active.flightGoal->radius>=80 && active.flightGoal->radius<=144,
              "initial flight move snaps to the body center and waits on its randomized radius");
        check(world.unit(id)->flightBeginCallbackSerial==1,
              "flight move dispatch exposes BeginFlight even without an authoritative script VM");
        const auto hash=world.stateHash();
        ++world.unit(id)->flightBeginCallbackSerial;
        check(world.stateHash()==hash,
              "display-only BeginFlight callback serial is excluded from lockstep state");
        --world.unit(id)->flightBeginCallbackSerial;
        world.unit(id)->orders.front().mission.pending|=0x100;
        world.tick(1.0f/30.0f);
        check(world.unit(id)->orders.size()==2 && world.unit(id)->orders.front().patrol &&
              !world.unit(id)->orders.front().flightMoveMission,
              "flight move arrival activates the patrol and its return anchor");
    }
    {
        World world;
        std::vector<uint8_t> heights(64*64,10);
        heights[16*64+16]=90; // far tile corner of the footprint's sector neighborhood
        heights[8*64+23]=200; // belongs only to the next sector's neighborhood
        world.setTerrain(heights,64,64,20);
        UnitType flyer;flyer.canFly=flyer.canMove=true;flyer.footX=flyer.footZ=2;
        flyer.maxVel=Fixed::fromInt(4);flyer.accel=flyer.brake=Fixed::fromInt(1);
        flyer.turnRate=1200;flyer.cruiseAlt=80;
        const int id=world.spawn(&flyer,128,128);
        world.order(id,800,800,false);
        world.tick(1.0f/30.0f);
        check(world.unit(id)->flightNavigation.destination.y==280*65536,
              "flight terrain links the center sector at spawn");
        auto* u=world.unit(id);
        u->x=Fixed::fromInt(130);u->z=Fixed::fromInt(128);
        u->flightSectorX=0;u->flightSectorZ=0;
        u->flightVelocity={};u->speed=Fixed();
        world.tick(1.0f/30.0f);
        check(u->flightNavigation.destination.y==170*65536 && u->flightSectorX==0,
              "flight terrain retains its sector between footprint relocations");
    }
    {
        std::vector<uint8_t> heights={100,120,100,120},roads(4,1);
        NavGrid::Limits limits;limits.maxSlope=30;limits.badSlope=15;
        NavGrid slope(heights,2,2,0,limits);
        check(slope.walkable(0,0) && slope.terrainGrade(0,0)==4,
              "passable steep terrain retains its costly search grade");
        slope.setRoads(&roads);
        check(slope.terrainGrade(0,0)==7,"roads suppress the soft slope penalty");
        limits.badMinWaterDepth=-110;
        NavGrid depth(heights,2,2,0,limits);depth.setRoads(&roads);
        check(depth.terrainGrade(0,0)==4,"roads retain preferred-depth penalties");
        limits.maxSlope=19;limits.badSlope=15;
        NavGrid blocked(heights,2,2,0,limits);blocked.setRoads(&roads);
        check(blocked.terrainGrade(0,0)==0,"roads cannot override a hard slope limit");
    }
    {
        World world;
        world.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
        UnitType flyer; flyer.canFly=flyer.canMove=flyer.vtolStandby=true;
        flyer.maxVel=Fixed::fromInt(4); flyer.accel=flyer.brake=Fixed::fromInt(1);
        flyer.turnRate=1200; flyer.cruiseAlt=80;
        const int id=world.spawn(&flyer,256,256);
        auto* u=world.unit(id); u->standbyActive=true; u->standbyState={1,0x21,1,0,0};
        world.tick(1.0f/30.0f); u=world.unit(id);
        check(u->standbyActive && u->standbyState.stage==1 && u->standbyState.deadline>=8 &&
              u->standbyState.deadline<=14 && u->orders.empty(),
              "grounded VTOL standby owns its scan/retry timer");
        u->flightGroundMode=2; u->flightY=Fixed::fromInt(180);
        u->standbyState={1,0,0xffffffffu,0,0};
        world.tick(1.0f/30.0f); u=world.unit(id);
        check(u->landing && u->landing->mission.stage==3 && !u->orders.empty() &&
              u->orders.front().landing && u->flightY<Fixed::fromInt(180),
              "airborne standby installs the landing mission and starts descent");
        for (int i=0;i<180;++i) world.tick(1.0f/30.0f);
        u=world.unit(id);
        check(!u->landing && u->flightGroundMode==1 && u->orders.empty() &&
              std::abs(u->flightY.v-100*65536)<=65536 &&
              u->flightLandingCallbackSerial==1,
              "landing arrival retires its controller, emits one callback, and restores grounded standby");
    }
    {
        // Exercise the real standby/controller path: settled flyers and an
        // earlier descent must both exclude a second landing on the same cells.
        for (bool simultaneous : {false,true}) {
            World world;
            world.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
            UnitType flyer;flyer.canFly=flyer.canMove=flyer.vtolStandby=true;
            flyer.maxVel=Fixed::fromInt(4);flyer.accel=flyer.brake=Fixed::fromInt(1);
            flyer.turnRate=1200;flyer.cruiseAlt=80;flyer.footX=2;flyer.footZ=3;
            const int first=world.spawn(&flyer,256,256);
            const int second=world.spawn(&flyer,256,256);
            for (int id : {first,second}) {
                auto* u=world.unit(id);
                u->flightGroundMode=(id==second || simultaneous) ? 2 : 1;
                u->flightY=Fixed::fromInt(u->flightGroundMode==2 ? 180 : 100);
                u->standbyActive=true;u->standbyState={1,0,0xffffffffu,0,0};
            }
            world.tick(1.0f/30.0f);
            check(world.unit(second)->flightLandingCallbackSerial==0,
                  simultaneous ? "simultaneous flyers reserve distinct landing footprints"
                               : "a landed flyer blocks another flyer's descent");
            bool overlap=false;
            for (int tick=0;tick<1800;++tick) {
                world.tick(1.0f/30.0f);
                const auto& a=*world.unit(first);const auto& b=*world.unit(second);
                if(a.flightGroundMode!=1 || b.flightGroundMode!=1)continue;
                const int ax=footprintOrigin(a.x,flyer.footX),az=footprintOrigin(a.z,flyer.footZ);
                const int bx=footprintOrigin(b.x,flyer.footX),bz=footprintOrigin(b.z,flyer.footZ);
                overlap |= ax<bx+flyer.footX && bx<ax+flyer.footX &&
                           az<bz+flyer.footZ && bz<az+flyer.footZ;
            }
            check(!overlap,"landed aircraft never share footprint cells");
            check(world.unit(first)->flightGroundMode==1 && world.unit(second)->flightGroundMode==1,
                  "blocked aircraft finds another landing site and settles");
        }
    }
    {
        // 509400: an airborne flyer owns the secondary occupant word of the
        // explored cells below it, so a landing there is refused and the
        // landing search relocates. Retail does not recheck a site once descent
        // has begun (the engine's earlier recheck was removed by user decision).
        World world;
        world.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
        UnitType flyer;flyer.canFly=flyer.canMove=flyer.vtolStandby=true;
        flyer.maxVel=Fixed::fromInt(4);flyer.accel=flyer.brake=Fixed::fromInt(1);
        flyer.turnRate=1200;flyer.cruiseAlt=80;flyer.footX=flyer.footZ=2;
        UnitType overhead=flyer;overhead.vtolStandby=false;
        const int id=world.spawn(&flyer,256,256);
        const int above=world.spawn(&overhead,256,256);
        world.unit(above)->flightGroundMode=2;world.unit(above)->flightY=Fixed::fromInt(300);
        auto* u=world.unit(id);u->flightGroundMode=2;u->flightY=Fixed::fromInt(180);
        world.tick(1.0f/30.0f); // explore, then build the persistent airborne grid
        u=world.unit(id);
        u->standbyActive=true;u->standbyState={1,0,0xffffffffu,0,0};
        world.tick(1.0f/30.0f);
        u=world.unit(id);
        check(!(u->landing && u->landing->mission.stage==3),
              "an airborne flyer overhead blocks the explored landing site below it");
        for(int tick=0;tick<1800 && world.unit(id)->flightGroundMode!=1;++tick)world.tick(1.0f/30.0f);
        u=world.unit(id);const auto* a=world.unit(above);
        const int ux=footprintOrigin(u->x,2),uz=footprintOrigin(u->z,2);
        const int ax=footprintOrigin(a->x,2),az=footprintOrigin(a->z,2);
        check(u->flightGroundMode==1 && (ux>=ax+2 || ax>=ux+2 || uz>=az+2 || az>=uz+2),
              "the blocked flyer lands clear of the airborne occupant");
        // A ground unit arriving under a descending flyer does not interrupt it.
        u->standbyActive=false;u->landing.reset();
        u->flightGroundMode=2;u->flightY=Fixed::fromInt(180);
        u->standbyActive=true;u->standbyState={1,0,0xffffffffu,0,0};
        int stage3=-1;
        for(int tick=0;tick<600 && stage3<0;++tick) {
            world.tick(1.0f/30.0f);
            u=world.unit(id);
            if(u->landing && u->landing->mission.stage==3)stage3=tick;
        }
        UnitType ground;ground.canMove=true;ground.maxVel=Fixed::fromInt(1);
        ground.footX=ground.footZ=2;
        world.spawn(&ground,u->x.toFloat(),u->z.toFloat());
        world.tick(1.0f/30.0f);
        u=world.unit(id);
        check(stage3>=0 && ((u->landing && u->landing->mission.stage==3) || u->flightGroundMode==1),
              "retail keeps an in-progress descent when a ground unit enters the site");
    }
    {
        // A stack of flyers told to land on one spot. Retail's landing test
        // lets them land on each other (descenders stay airborne in word +2,
        // where shared cells are redrawn every tick or become 0xffff, and
        // unexplored sites skip every test: tools/re/check_landing_overlap.py),
        // but its air traffic does not let a stack come down together: each
        // flyer's counter (unit +0x12a) climbs while it shares cells, and
        // VTOL_Standby and VTOL_LandIfCan step out of the stack at their next
        // wake (protocol 307), so they land apart. Legion refuses landed and
        // descending flyers' footprints too.
        struct Stack { int overlaps=0,landed=0,count=0; };
        auto stack=[](PathfindingMode mode,int airborne,int grounded,int lateSight) {
            World world;world.setPathfindingMode(mode);
            world.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
            UnitType flyer;flyer.canFly=flyer.canMove=flyer.vtolStandby=true;
            flyer.maxVel=Fixed::fromInt(4);flyer.accel=flyer.brake=Fixed::fromInt(1);
            flyer.turnRate=1200;flyer.cruiseAlt=80;flyer.footX=flyer.footZ=2;
            UnitType late=flyer;late.sight=lateSight;
            std::vector<int> ids;
            for(int i=0;i<grounded;++i) {
                const int id=world.spawn(&flyer,512,512,0.0f,0);
                auto* u=world.unit(id);u->flightGroundMode=1;u->flightY=Fixed::fromInt(100);
                ids.push_back(id);
            }
            if(grounded)world.tick(1.0f/30.0f);
            for(int i=0;i<airborne;++i) {
                // Late arrivals belong to player 1: with sight 0 the site stays
                // unexplored for them.
                const int id=world.spawn(grounded ? &late : &flyer,512,512,0.0f,grounded ? 1 : 0);
                auto* u=world.unit(id);u->flightGroundMode=2;u->flightY=Fixed::fromInt(180);
                u->standbyActive=true;u->standbyState={1,0,0xffffffffu,0,0};
                ids.push_back(id);
            }
            Stack result;result.count=int(ids.size());
            for(int tick=0;tick<4000;++tick) {
                world.tick(1.0f/30.0f);
                if(std::all_of(ids.begin(),ids.end(),[&](int id){return world.unit(id)->flightGroundMode==1;}))break;
            }
            for(size_t i=0;i<ids.size();++i) {
                const auto& a=*world.unit(ids[i]);
                if(a.flightGroundMode!=1)continue;
                ++result.landed;
                for(size_t j=i+1;j<ids.size();++j) {
                    const auto& b=*world.unit(ids[j]);
                    if(b.flightGroundMode!=1)continue;
                    const int ax=footprintOrigin(a.x,2),az=footprintOrigin(a.z,2);
                    const int bx=footprintOrigin(b.x,2),bz=footprintOrigin(b.z,2);
                    result.overlaps+=ax<bx+2 && bx<ax+2 && az<bz+2 && bz<az+2;
                }
            }
            return result;
        };
        const auto retail=stack(PathfindingMode::Retail,12,0,180);
        const auto legion=stack(PathfindingMode::Legion,12,0,180);
        std::printf("stacked landing, 12 flyers: Retail %d overlapping landed pairs, Legion %d (landed %d/%d)\n",
                    retail.overlaps,legion.overlaps,legion.landed,legion.count);
        check(retail.overlaps==0 && retail.landed==retail.count,
              "retail flyers ordered to one spot step out of the stack and land apart");
        check(legion.overlaps==0 && legion.landed==legion.count,
              "Legion flyers ordered to one spot all land without sharing footprint cells");
        const auto onLanded=stack(PathfindingMode::Legion,8,1,180);
        check(onLanded.overlaps==0 && onLanded.landed==onLanded.count,
              "Legion flyers arriving over a landed flyer all land clear of it and of each other");
        const auto unexploredRetail=stack(PathfindingMode::Retail,8,1,0);
        const auto unexploredLegion=stack(PathfindingMode::Legion,8,1,0);
        std::printf("unexplored landing over a landed flyer: Retail %d overlapping pairs, Legion %d\n",
                    unexploredRetail.overlaps,unexploredLegion.overlaps);
        check(unexploredLegion.overlaps==0 && unexploredLegion.landed==unexploredLegion.count,
              "Legion flyers never land on a flyer even at an unexplored site");
        // The same over a ground unit, standing or walking across the spot.
        // 509400 sees ground bodies in word +0 whether or not they move
        // (tools/re/check_landing_moving_ground.py), but skips every test at a
        // site the lander has not explored, so retail touches down on top --
        // a lone flyer: a stack steps out of itself first (above).
        auto onGround=[](PathfindingMode mode,bool walking,int flyers=4) {
            World world;world.setPathfindingMode(mode);
            world.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
            UnitType flyer;flyer.canFly=flyer.canMove=flyer.vtolStandby=true;
            flyer.maxVel=Fixed::fromInt(4);flyer.accel=flyer.brake=Fixed::fromInt(1);
            flyer.turnRate=1200;flyer.cruiseAlt=80;flyer.footX=flyer.footZ=2;flyer.sight=0;
            UnitType ground;ground.canMove=true;ground.maxHp=100;ground.footX=ground.footZ=2;
            ground.maxVel=Fixed::fromFloat(0.25f);ground.accel=ground.brake=Fixed::fromFloat(0.1f);
            ground.turnRate=1200;
            const int body=world.spawn(&ground,walking ? 448 : 512,512,0.0f,0);
            if(walking)world.order(body,600,512,false);
            std::vector<int> ids;
            for(int i=0;i<flyers;++i) {
                const int id=world.spawn(&flyer,512,512,0.0f,1);
                auto* u=world.unit(id);u->flightGroundMode=2;u->flightY=Fixed::fromInt(180);
                u->standbyActive=true;u->standbyState={1,0,0xffffffffu,0,0};
                ids.push_back(id);
            }
            int overlapTicks=0,landed=0;
            for(int tick=0;tick<1500;++tick) {
                world.tick(1.0f/30.0f);
                const auto& g=*world.unit(body);
                const int gx=footprintOrigin(g.x,2),gz=footprintOrigin(g.z,2);
                landed=0;
                bool over=false;
                for(int id:ids) {
                    const auto& a=*world.unit(id);
                    if(a.flightGroundMode!=1)continue;
                    ++landed;
                    const int ax=footprintOrigin(a.x,2),az=footprintOrigin(a.z,2);
                    over|=ax<gx+2 && gx<ax+2 && az<gz+2 && gz<az+2;
                }
                overlapTicks+=over;
            }
            return std::pair{overlapTicks,landed};
        };
        const auto groundRetail=onGround(PathfindingMode::Retail,false,1);
        const auto groundLegion=onGround(PathfindingMode::Legion,false);
        const auto walkLegion=onGround(PathfindingMode::Legion,true);
        std::printf("unexplored landing over a ground unit: Retail %d overlap ticks (one flyer), Legion %d, "
                    "Legion walking %d (landed %d of 1, %d/%d of 4)\n",groundRetail.first,groundLegion.first,
                    walkLegion.first,groundRetail.second,groundLegion.second,walkLegion.second);
        check(groundRetail.first>0,"retail lands on a ground unit at an unexplored site");
        check(groundLegion.first==0 && groundLegion.second==4,
              "Legion flyers land clear of a standing ground unit even at an unexplored site");
        // A unit that walks in under a descender (stage 3) is not refused:
        // the site was free when chosen, as in retail. Only count landings.
        check(walkLegion.second==4,"Legion flyers all land while a ground unit walks across the spot");
    }
    {
        World world;
        world.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
        UnitType fighter; fighter.canMove=true; fighter.maxHp=10000;
        fighter.maxVel=Fixed::fromInt(3); fighter.accel=fighter.brake=Fixed::fromFloat(0.1f);
        fighter.turnRate=1200; fighter.leash=90;
        // Upright, so both stand on the terrain: the response's scan (4d8370)
        // takes only an enemy its player sees.
        fighter.upright=true;
        fighter.weapon.damage=10; fighter.weapon.range=80; fighter.weapons.push_back(fighter.weapon);
        const int id=world.spawn(&fighter,256,256,0.0f,0);
        const int enemy=world.spawn(&fighter,304,256,0.0f,1);
        world.unit(enemy)->fireState=0;
        // The scan draws on the player's visible-enemy list (411700, rebuilt
        // every 7 ticks): let it be built first, with the unit off Fire At
        // Will so its Standby does not take the enemy meanwhile.
        world.unit(id)->fireState=1;
        for (int i=0;i<8;++i) world.tick(1.0f/30.0f);
        world.unit(id)->fireState=2;
        world.order(id,800,256,false);
        auto* u=world.unit(id);
        auto& move=u->orders.back(); move.groundResponse={-1,{}, {800,256}};
        move.mission={2,1,1,0,0};
        world.tick(1.0f/30.0f); u=world.unit(id);
        auto parent=std::find_if(u->orders.begin(),u->orders.end(),[](const Order& o){return o.groundResponse.mode==-1;});
        check(u->orders.front().targetId==enemy && parent!=u->orders.end() && parent->mission.stage==3 &&
              parent->groundResponse.origin.x==256 && parent->groundResponse.origin.z==256,
              "ground response interrupts movement while retaining its resume position and mission");
        world.unit(enemy)->hp=Fixed();
        world.tick(1.0f/30.0f); u=world.unit(id);
        u->x=Fixed::fromInt(100); u->z=Fixed::fromInt(256);
        // The combat host retires the dead target after mission dispatch;
        // the suspended parent is dispatched on the following update.
        for (int i=0;i<2;++i) world.tick(1.0f/30.0f);
        u=world.unit(id);
        parent=std::find_if(u->orders.begin(),u->orders.end(),[](const Order& o){return o.groundResponse.mode==-1;});
        auto back=std::find_if(u->orders.begin(),u->orders.end(),[](const Order& o){return o.groundResponse.mode==1;});
        check(parent!=u->orders.end() && parent->mission.stage==0 && back!=u->orders.end() &&
              back->missionTarget==std::pair{Fixed::fromInt(256),Fixed::fromInt(256)} &&
              back->x==Fixed::fromInt(264) && back->z==Fixed::fromInt(264) && back->missionRadius==22,
              "completed auxiliary attack installs a bounded return leg before resuming the original move");
    }
    {
        RetailFlightGoal goal;
        goal.flags = 0x30;
        goal.radius = 80;
        check(goal.accepts({80*65536-1,0,0}) && !goal.accepts({80*65536,0,0}),
              "flight patrol radius excludes its exact boundary");
        goal.flags = 8;
        check(goal.accepts({32768,65536,0}) && !goal.accepts({32769,0,0}) &&
              !goal.accepts({0,65537,0}), "flight point arrival checks horizontal and vertical tolerances");
        World world;
        world.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
        UnitType flyer;
        flyer.canFly = flyer.canMove = true;
        flyer.maxVel = Fixed::fromInt(4);
        flyer.accel = flyer.brake = Fixed::fromInt(1);
        flyer.turnRate = 1200;
        flyer.cruiseAlt = 80;
        const int id = world.spawn(&flyer,256,256);
        world.order(id,768,256,false);
        auto* unit = world.unit(id);
        check(unit->flightY == Fixed::fromInt(100), "flight spawn uses absolute terrain height");
        world.tick(1.0f/30.0f);
        unit = world.unit(id);
        check(unit->flightY == Fixed::fromInt(101) && unit->flightVelocity.y == 65536 &&
              unit->x > Fixed::fromInt(256) && unit->flightNavigation.destination.y == 180*65536,
              "live flight moves and climbs toward terrain-relative cruise height");
        const auto hash = world.stateHash();
        ++unit->flightVelocity.x;
        check(world.stateHash() != hash, "persistent flight momentum participates in lockstep hash");
        --unit->flightVelocity.x;
        // Switch through the public patrol command so its mission kind is
        // converted as well as its flags before injecting the arrival stage.
        world.patrolTo(id,768,256,false);
        auto& order = unit->orders.front();
        order.mission = {2,0x701,100,0,0};
        order.flightGoal = RetailFlightGoal{{unit->x.v,unit->flightY.v,unit->z.v},0x30,0,80};
        const auto before = unit->x;
        world.tick(1.0f/30.0f);
        unit = world.unit(id);
        check((unit->orders.front().mission.pending & 0x100) && unit->x != before,
              "patrol arrival signals the dispatcher while retaining motion on the arrival tick");
        world.tick(1.0f/30.0f);
        unit = world.unit(id);
        check(unit->orders.size() == 1 && unit->orders.front().mission.stage == 2 &&
              !(unit->orders.front().mission.pending & 0x100) &&
              unit->orders.front().flightGoal->point.x != before.v,
              "patrol dispatcher consumes arrival and installs the next randomized goal");
    }
    {
        const auto climbing = retailFlightVelocity({}, {}, {10*65536,10*65536,0}, {},
            0, 4*65536, 65536, 65536, 0);
        check(climbing.x == 65536 && climbing.y == 65536 && climbing.z == 0,
              "flight accelerates toward its destination while climbing independently");
        const auto descending = retailFlightVelocity({}, {0,10*65536,0}, {}, {},
            8*65536, 8*65536, 65536, 65536, 0);
        check(descending.x == 0 && descending.y == -4*65536 && descending.z == 0,
              "flight doubles its descent limit directly above the destination");
        const auto close = retailFlightNavigation({}, {}, {16*65536,65536,0},
            1234, 100*65536, false, false, 0);
        check(close.heading == 1234 && close.destination.y == 65536,
              "close flight destination preserves its heading and target altitude");
        const auto far = retailFlightNavigation({}, {}, {161*65536,65536,0},
            1234, 100*65536, false, true, 4321);
        check(far.heading == 4321 && far.destination.y == 100*65536 && far.velocity.y == 65536,
              "flight cruise override follows destination velocity measurement");
    }
    {
        World world;
        std::vector<uint8_t> heights(32 * 32, 64);
        for (int z = 16; z < 32; ++z)
            for (int x = 0; x < 32; ++x) heights[size_t(z) * 32 + x] = 192;
        world.setTerrain(heights, 32, 32, 0);
        UnitType mover;
        mover.canMove = true;
        mover.footX = mover.footZ = 2;
        check(world.cellScore(&mover, 12, 13, 0) >= kCellThreshold,
              "flat ground north of a cliff is not blocked by screen projection");
        check(world.cellScore(&mover, 12, 16, 0) < kCellThreshold,
              "the actual cliff slope still blocks movement");
    }
    // Actual signed fixed-point positions/origins from the live Hunter capture.
    check(Fixed::fromRetailNumber(1.8).v == 117964, "Hunter nominal speed truncates like retail");
    check(retailTurnTravel(Fixed::fromInt(2), 16384, 4096) == Fixed::fromInt(8) &&
          retailTurnTravel(Fixed::fromInt(2), 16384, 0) == Fixed(),
          "turn travel uses matching angle units and retail zero-rate behavior");
    check(retailHeadingToPort(0).v == 32768 && portHeadingToRetail(retailHeadingToPort(5692)) == 5692,
          "retail heading conversion is confined to the import/export boundary");
    {
        World world;
        world.setTerrain(std::vector<uint8_t>(128 * 64, 100), 128, 64, 20);
        UnitType mover;
        mover.canMove = true;
        mover.maxVel = mover.brake = Fixed::fromInt(2);
        mover.turnRate = 4096;
        const int id = world.spawn(&mover, 128.25f, 128.25f);
        world.order(id, 1024, 128, false);
        auto* unit = world.unit(id);
        unit->heading = Bam(16384);
        unit->speed = unit->baseSpeed = Fixed::fromInt(2);
        unit->orders.front().segmentX = Fixed::fromInt(133);
        unit->orders.front().segmentZ = Fixed::fromInt(128);
        unit->orders.front().hasSegment = true;
        Order intermediate;
        intermediate.x = Fixed::fromInt(133);
        intermediate.z = intermediate.segmentZ = Fixed::fromInt(128);
        intermediate.segmentX = Fixed::fromInt(112);
        intermediate.hasSegment = true;
        unit->orders.insert(unit->orders.begin(), intermediate);
        world.tick(1.0f / 30.0f);
        check(world.unit(id)->orders.size() == 1 && world.unit(id)->x > Fixed::fromFloat(128.25f),
              "five-pixel navigator advance keeps moving on the same tick");
    }
    for (int blocked=0; blocked<2; ++blocked) {
        World world;
        std::vector<uint8_t> terrain(64*64,100);
        if (blocked) terrain[15*64+16]=240;
        world.setTerrain(terrain,64,64,20);
        UnitType mover;
        mover.canMove=true; mover.footX=mover.footZ=1;
        mover.maxVel=mover.brake=Fixed::fromInt(2); mover.turnRate=2500;
        const int id=world.spawn(&mover,256,256);
        world.order(id,800,256,false);
        auto* u=world.unit(id);
        u->orders.back().mission={2,0x2701,100,0,0};
        u->orders.back().segmentX=Fixed::fromInt(272);
        u->orders.back().segmentZ=Fixed::fromInt(272);
        u->orders.back().hasSegment=true;
        Order corner;
        corner.x=corner.z=Fixed::fromInt(272);
        corner.segmentX=Fixed::fromInt(240); corner.segmentZ=Fixed::fromInt(272);
        corner.hasSegment=true;
        u->orders.insert(u->orders.begin(),corner);
        u->groundScanTick=0;
        world.tick(1.0f/30.0f);
        check(world.unit(id)->orders.size()==size_t(blocked ? 2 : 1),
              blocked ? "nearby impassable footprint preserves the corner" :
                        "clear-ground scan removes a nearby corner before steering");
    }
    const auto hunterStep = retailGroundStep(retailHeadingToPort(5692), Fixed::raw(106521));
    check(hunterStep.s.v == -54769 && hunterStep.c.v == -91372,
          "first captured Hunter displacement includes retail angle quantization and rounding");
    {
        World world;
        world.setTerrain(std::vector<uint8_t>(128 * 64, 100), 128, 64, 20);
        UnitType mover;
        mover.canMove = true;
        mover.maxVel = mover.brake = Fixed::fromInt(2);
        mover.turnRate = 32767;
        int id = world.spawn(&mover, 128, 192);
        world.order(id, 768, 128, false);
        auto* unit = world.unit(id);
        auto& order = unit->orders.front();
        order.segmentX = Fixed::fromInt(128);
        order.segmentZ = Fixed::fromInt(128);
        order.hasSegment = true;
        // This is an existing route, after controller initialization. Stage 0
        // intentionally replaces a direct segment with the current position.
        order.mission={2,0x2701,100,0,0};
        auto aim = retailSteeringPoint(unit->x, unit->z,
            {order.segmentX, order.segmentZ}, {order.x, order.z});
        Bam expected = retailDirection(aim.x - unit->x, aim.z - unit->z);
        Bam direct = retailDirection(order.x - unit->x, order.z - unit->z);
        world.tick(1.0f / 30.0f);
        check(world.unit(id)->heading == expected && expected != direct,
              "ground mover steers toward the adjusted route segment point");
    }
    {
        World world;
        world.setTerrain(std::vector<uint8_t>(128 * 64, 100), 128, 64, 20);
        UnitType mover;
        mover.canMove = true;
        mover.maxVel = mover.brake = Fixed::fromInt(2);
        mover.turnRate = 4096;
        int id = world.spawn(&mover, 128, 128);
        auto* unit = world.unit(id);
        unit->speed = unit->baseSpeed = Fixed::fromInt(2);
        world.order(id, 1024, 128, false);
        world.tick(1.0f / 30.0f);
        check(world.unit(id)->speed == Fixed::fromInt(2) && world.unit(id)->x > Fixed::fromInt(128),
              "a distant quarter-turn destination does not incorrectly brake the mover");
    }
    check(footprintOrigin(Fixed::raw(218628095), 2) == 207 &&
          footprintOrigin(Fixed::raw(140480695), 2) == 133 &&
          footprintOrigin(Fixed::raw(216588010), 2) == 206,
          "recorded Hunter footprint origins");
    check(footprintOrigin(Fixed::raw(168 * 65536 - 1), 2) == 9 &&
          footprintOrigin(Fixed::fromInt(168), 2) == 10 &&
          footprintOrigin(Fixed::raw(-1), 1) == -1,
          "subpixel and negative footprint boundaries");
    check(footprintWaypoint(10, 2) == Fixed::fromInt(160) &&
          footprintWaypoint(10, 3) == Fixed::fromInt(168),
          "even and odd waypoint anchors");
    uint32_t seed = 12345;
    check(retailRandom(seed, 0) == 0 && retailRandom(seed, 1) == 0 && seed == 12345,
          "small RNG bounds consume no draw");
    check(retailRandom(seed, 100) == 15 && seed == 207482415,
          "retail RNG output and successor seed");
    seed = 0;
    check(retailRandom(seed, 10) == 7 && seed == 0x7fffffff,
          "retail zero-seed behavior");
    UnitType type;
    type.maxVel = Fixed::fromInt(2);
    World a, b;
    std::vector<World::RngObservation> draws;
    a.setRngObserver([&](const World::RngObservation& draw) { draws.push_back(draw); });
    const int first = a.spawn(&type, 100, 100);
    b.spawn(&type, 100, 100);
    const int second = a.spawn(&type, 200, 100);
    b.spawn(&type, 200, 100);
    check(a.unit(first)->baseSpeed != a.unit(second)->baseSpeed &&
          a.stateHash() == b.stateHash(), "individual speed variation is deterministic");
    check(draws.size() == 6 && draws[0].bound == 0 && draws[1].bound==65536 && draws[2].bound==201 && draws[0].tick == 0 &&
          draws[0].seedAfter==draws[0].seedBefore && draws[0].seedAfter == draws[1].seedBefore &&
          draws[1].seedAfter==draws[2].seedBefore && a.unit(first)->variationPhase==draws[1].result &&
          std::strstr(draws[0].caller.function_name(), "spawn") != nullptr,
          "RNG observer records ordered consumers without changing simulation state");
    b.unit(first)->baseSpeed.v++;
    check(a.stateHash() != b.stateHash(), "individual speed participates in lockstep hash");
    a.resetForReplay(); b.resetForReplay();
    a.spawn(&type, 100, 100); b.spawn(&type, 100, 100);
    check(a.stateHash() == b.stateHash(), "world reset resets creation RNG");
    check(draws.size() == 9 && draws[6].seedBefore == draws[0].seedBefore,
          "RNG observer persists across replay reset without advancing its seed");
    {
        World traced, plain;
        UnitType mover = type;
        mover.canMove = true;
        mover.footX = mover.footZ = 2;
        std::vector<World::RngObservation> movementDraws;
        traced.setRngObserver([&](const World::RngObservation& draw) { movementDraws.push_back(draw); });
        for (World* world : {&traced, &plain}) {
            world->setTerrain(std::vector<uint8_t>(256 * 64, 100), 256, 64, 20);
            world->setPathService(true);
            int id = world->spawn(&mover, 128, 256);
            world->order(id, 3200, 256, false);
        }
        bool same = true;
        for (int tick = 0; tick < 600; ++tick) {
            traced.tick(1.0f / 30.0f);
            plain.tick(1.0f / 30.0f);
            same &= traced.stateHash() == plain.stateHash();
        }
        check(same, "RNG tracing preserves every movement tick's state hash");
        check(std::any_of(movementDraws.begin(), movementDraws.end(), [](const auto& draw) {
            return draw.tick > 0 && std::strstr(draw.caller.function_name(), "World::tick") != nullptr;
        }), "path RNG tracing identifies the consuming call site through its wrapper");
    }
    return failures ? 1 : 0;
}
