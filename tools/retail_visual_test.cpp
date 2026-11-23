#include <functional>
#include "client/retailquality.h"
#include "client/retailmodellighting.h"
#include "client/weaponanimationqueue.h"
#include "client/retaileffectframe.h"
#include "client/retailfeatureclock.h"
#include "client/retaileffectvisibility.h"
#include "client/retaillightningquad.h"
#include "client/retailflame.h"
#include "client/retailsmoke.h"
#include "client/retailviewport.h"
#include "client/retailpointparticle.h"
#include "client/retaildebris.h"
#include "client/retaildeathsfx.h"
#include "client/retailglow.h"
#include "client/retailblood.h"
#include "sim/retailprojectile.h"
#include "sim/retailsplash.h"
#include "sim/retailairgrid.h"
#include "sim/retailguided.h"
#include "sim/retailballistic.h"
#include "sim/retailstorm.h"
#include "sim/retailhweffect.h"
#include "sim/retailhweffectdata.h"
#include "tdf/tdf.h"
#include "client/retailaim.h"
#include "client/retailflightanimation.h"
#include "client/renderframe.h"
#include "client/featureindex.h"
#include "sim/retailpiecepose.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <map>
#include <tuple>
#include <vector>

static bool testFeatureIndex() {
    struct Point { float x,z,liftX,liftZ; };
    std::vector<Point> points;
    uint32_t rng=17;
    auto next=[&]{rng=rng*1664525u+1013904223u;return rng;};
    for(int i=0;i<12000;++i)
        points.push_back({float(int(next()%34000)-1024),float(int(next()%34000)-1024),
                          float(next()%128)/2,float(next()%256)/2});
    // Bucket boundaries, including negative projected positions.
    for(int i=-2;i<34;++i)for(float edge:{-0.01f,0.f,0.01f})
        points.push_back({i*1024.f+edge,i*1024.f+edge,0,0});
    tak::FeatureIndex index;
    auto rebuild=[&]{index.rebuild(points.size(),[&](size_t i) {
        const auto& p=points[i];return std::pair{p.x-p.liftX,p.z-p.liftZ};
    });};
    auto check=[&] {
        for(float zoom:{.05f,.1f,.25f,1.f,1.5f,4.f})for(int view=0;view<80;++view) {
            const float ox=float(int(next()%38000)-3000),oz=float(int(next()%38000)-3000);
            auto visible=[&](size_t i){const auto& p=points[i];
                const float x=(p.x-ox)*zoom-p.liftX*zoom,z=(p.z-oz)*zoom-p.liftZ*zoom;
                return x>=-200 && z>=-200 && x<=1480 && z<=1160;};
            std::vector<size_t> expected,actual;
            for(size_t i=0;i<points.size();++i)if(visible(i))expected.push_back(i);
            for(size_t i:index.query(ox-200/zoom-1,oz-200/zoom-1,ox+1480/zoom+1,oz+1160/zoom+1))
                if(visible(i))actual.push_back(i);
            if(actual!=expected)return false;
        }
        return true;
    };
    rebuild();if(!check() || index.query(4000,4000,5000,5000).size()>=points.size()/10)return false;
    points[0]={-4096,-2048,0,0};points.push_back({16384,8192,64,127});
    rebuild();if(!check())return false;
    points.clear();rebuild();return index.query(-100,-100,100,100).empty() && index.all().empty();
}

static bool testScriptEffectPosition() {
    uint32_t seed=0x51f3a217u;
    const auto random=[&] {
        seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;
        return seed;
    };
    for(int caseIndex=0;caseIndex<4096;++caseIndex) {
        std::vector<tak::sim::RetailModelPiece> model(2);
        model[0].parent=-1;model[0].scriptPiece=0;
        model[1].parent=0;model[1].scriptPiece=1;
        for(auto& node:model)for(auto& value:node.offset)value=int32_t(random());
        std::vector<tak::cob::RetailPiece> pose(2);
        for(auto& piece:pose) {
            for(auto& value:piece.move)value=int32_t(random());
            for(auto& value:piece.turn)value=int32_t(random());
        }
        std::array<int32_t,3> base{};
        for(auto& value:base)value=int32_t(random());
        const uint16_t heading=uint16_t(random()),pitch=uint16_t(random()),roll=uint16_t(random());
        const auto offset=tak::sim::retailPieceOrigin(model,pose,1,heading,pitch,roll);
        std::array<int32_t,3> expected{};
        for(size_t axis=0;axis<3;++axis)
            expected[axis]=std::bit_cast<int32_t>(uint32_t(base[axis])+uint32_t(offset[axis]));
        if(tak::sim::retailScriptEffectPosition(base,model,pose,1,heading,pitch,roll)!=expected) {
            std::fprintf(stderr,"script effect origin overflow/placement mismatch at case %d\n",caseIndex);
            return false;
        }
    }
    return true;
}

static bool testDeathSfxLifecycle() {
    using Family=tak::RetailDeathSfxFamily;
    for(const auto& [code,family]:std::array{
            std::pair{257,Family::Smoke},std::pair{258,Family::Smoke},
            std::pair{265,Family::Smoke},std::pair{260,Family::DamageFlame},
            std::pair{261,Family::DamageFlame},std::pair{262,Family::DamageFlame},
            std::pair{263,Family::Detached},std::pair{264,Family::Detached}}) {
        const auto actual=tak::retailDeathSfxFamily(code);
        if(!actual || *actual!=family) {
            std::fprintf(stderr,"death SFX family %d was classified incorrectly\n",code);
            return false;
        }
    }
    if(tak::retailDeathSfxFamily(259) || tak::retailDeathSfxFamily(256)) {
        std::fprintf(stderr,"native no-op/unknown SFX code was admitted as a death effect\n");
        return false;
    }
    if(!tak::retailOwnerVmStopsOnSetUnitValue(26) ||
       !tak::retailOwnerVmStopsOnSetUnitValue(31) ||
       tak::retailOwnerVmStopsOnSetUnitValue(30) ||
       tak::retailOwnerVmMayAdvance(true) ||
       !tak::retailOwnerVmMayAdvance(false) ||
       tak::retailOwnerVmRetirementDelayTicks(26)!=1 ||
       tak::retailOwnerVmRetirementDelayTicks(31)!=35 ||
       tak::retailOwnerVmRetirementDelayTicks(30)!=-1 ||
       tak::retailOwnerVmRetirementTick(100,26)!=101 ||
       tak::retailOwnerVmRetirementTick(100,31)!=135 ||
       tak::retailOwnerVmRetirementTick(0xfffffff0u,31)!=0x13u ||
       tak::retailOwnerVmRetirementTick(100,30).has_value()) {
        std::fprintf(stderr,"death COB VM did not honor its native SET26/SET31 stop boundary\n");
        return false;
    }
    if(tak::retailAttachedSfxOwnerRemoved(true,200,-1,120) ||
       tak::retailAttachedSfxOwnerRemoved(false,119,-1,120) ||
       !tak::retailAttachedSfxOwnerRemoved(false,120,-1,120) ||
       !tak::retailAttachedSfxOwnerRemoved(false,0,0,120) ||
       tak::retailAttachedSfxOwnerRemovedSnapshot(true,200,false,120) ||
       tak::retailAttachedSfxOwnerRemovedSnapshot(false,119,false,120) ||
       !tak::retailAttachedSfxOwnerRemovedSnapshot(false,120,false,120) ||
       !tak::retailAttachedSfxOwnerRemovedSnapshot(false,0,true,120)) {
        std::fprintf(stderr,"attached death SFX owner retirement boundary is incorrect\n");
        return false;
    }
    const uint32_t retirement=tak::retailAttachedSfxRetirementTick(0xfffffffcu,118,120);
    if(retirement!=0xfffffffeu || tak::retailTickAtOrAfter(0xfffffffdu,retirement) ||
       !tak::retailTickAtOrAfter(0u,retirement)) {
        std::fprintf(stderr,"attached death SFX retirement tick mishandles wraparound\n");
        return false;
    }
    if(tak::retailEarlierTick(120,1)!=1 || tak::retailEarlierTick(1,120)!=1 ||
       tak::retailEarlierTick(0xfffffffeu,1)!=0xfffffffeu ||
       tak::retailEarlierTick(1,0xfffffffeu)!=0xfffffffeu) {
        std::fprintf(stderr,"attached death SFX owner deadlines do not preserve the earliest wrap-safe tick\n");
        return false;
    }
    return true;
}

int main(int argc,char** argv) {
    if(argc==2 && std::strcmp(argv[1],"--model-shade")==0) {
        float nx,ny,nz;
        while(std::scanf("%f %f %f",&nx,&ny,&nz)==3)
            std::printf("%d\n",tak::retailModelShadeLevel(nx,ny,nz));
        return 0;
    }
    if(argc==1 && (tak::retailModelShadeLevel(0,16,0)!=20 ||
                   tak::retailModelShadeLevel(0,0,-16)!=11 ||
                   tak::retailModelShadeLevel(-16,0,0)!=13 ||
                   tak::retailModelShadeLevel(16,0,0)!=5)) {
        std::puts("FAIL: building light must illuminate camera-facing surfaces");return 1;
    }
    if(argc==1) {
        if(!testFeatureIndex()){std::puts("FAIL: scenery culling changed visibility/order");return 1;}
        if(!testScriptEffectPosition() || !testDeathSfxLifecycle())return 1;
        {
            // Three simulation steps between display frames: an intervening
            // shot and target clear must not disappear with the old snapshot.
            tak::WeaponAnimationQueue queue;
            tak::RetailWeaponAnimations events;
            events.add(tak::RetailWeaponAnimation::Aim,2,0x1200,0x3400);
            queue.push(100,7,events);
            events.clear();events.add(tak::RetailWeaponAnimation::Fire,2);
            queue.push(101,7,events);
            events.clear();events.add(tak::RetailWeaponAnimation::Clear,2);
            queue.push(102,7,events);
            events.clear();events.add(tak::RetailWeaponAnimation::Aim,0);
            queue.push(103,8,events);
            events.clear();
            // FireWeapon's callback may precede actual discharge. Capture the
            // later shot even though that tick has no new script callbacks.
            queue.push(104,7,events,4,123.0f,456.0f);
            std::vector<std::pair<int,tak::WeaponAnimationQueue::Shot>> shots;
            const auto collectShot=[&](int id,const auto& shot) { shots.emplace_back(id,shot); };
            std::vector<std::tuple<int,int,int,int,int>> actual;
            const auto collect=[&](int id,const tak::RetailWeaponAnimation& e) {
                actual.emplace_back(id,int(e.kind),e.slot,e.heading,e.pitch);
            };
            queue.drain(102,collect,collectShot);
            const decltype(actual) expected{{7,tak::RetailWeaponAnimation::Aim,2,0x1200,0x3400},
                {7,tak::RetailWeaponAnimation::Fire,2,0,0},
                {7,tak::RetailWeaponAnimation::Clear,2,0,0}};
            if(actual!=expected) {
                std::fprintf(stderr,"skipped render snapshots lost weapon callbacks or aiming arguments\n");return 1;
            }
            queue.drain(102,collect,collectShot);
            if(actual!=expected)return 1; // another render frame cannot replay callbacks
            queue.drain(103,collect,collectShot);
            if(actual.size()!=4 || std::get<0>(actual.back())!=8 || !shots.empty())return 1;
            queue.drain(104,collect,collectShot);
            if(shots.size()!=1 || shots[0].first!=7 || shots[0].second.tick!=104 ||
               shots[0].second.weapons!=4 || shots[0].second.x!=123 || shots[0].second.z!=456 ||
               actual.size()!=4)return 1;
            queue.drain(105,collect,collectShot);
            if(shots.size()!=1)return 1;
            events.clear();
            events.add(tak::RetailWeaponAnimation::Clear,0);
            for(int i=0;i<20;++i)events.add(tak::RetailWeaponAnimation::Switch,(i+1)%3);
            queue.push(106,7,events);
            events.clear();actual.clear();
            queue.drain(106,collect,collectShot);
            if(actual.size()!=21 || std::get<1>(actual[0])!=tak::RetailWeaponAnimation::Clear)return 1;
            for(size_t i=1;i<actual.size();++i)
                if(std::get<1>(actual[i])!=tak::RetailWeaponAnimation::Switch || std::get<2>(actual[i])!=int(i%3))return 1;
            events.add(tak::RetailWeaponAnimation::Hit,3,0x4000,0,123);
            queue.push(107,7,events);
            bool retainedHit=false;
            queue.drain(107,[&](int id,const auto& event) {
                if(event.kind==tak::RetailWeaponAnimation::Hit)
                    retainedHit=id==7 && event.slot==3 && event.heading==0x4000 && event.damage==123;
            },collectShot);
            if(!retainedHit)return 1;
        }

        struct BurnEvent { int id; uint64_t activationSequence; bool emit; };
        std::vector<BurnEvent> events{{13,1,true},{2,2,true},{9,3,true}};
        tak::retailOrderFeatureSmokeNewestFirst(events);
        if(events.size()!=3 || events[0].id!=9 || events[1].id!=2 || events[2].id!=13) {
            std::fprintf(stderr,"feature smoke order helper did not return 9,2,13 newest-first\n");return 1;
        }

        std::map<int,std::vector<tak::RetailSmokeParticle>> particles;
        auto seedParticle=[&](int id,uint32_t countdown,uint32_t frameLimit) {
            tak::RetailSmokeParticle particle;
            particle.position={id*65536,id*2*65536,-id*65536};
            particle.countdown=countdown;particle.frameLimit=frameLimit;
            particles[id].push_back(particle);
        };
        seedParticle(9,1,1);  // expires this update, after consuming its countdown RNG draw
        seedParticle(2,2,20); // survives without a random draw
        seedParticle(13,1,20);// consumes a draw and survives

        std::vector<std::pair<int,int>> order;
        std::vector<std::pair<int,uint32_t>> draws;
        int owner=0;
        uint32_t randomState=0;
        const auto random=[&] {
            const auto value=++randomState;draws.emplace_back(owner,value);return value;
        };
        std::map<int,std::vector<std::array<int32_t,3>>> emittedPositions;
        std::map<int,std::vector<uint32_t>> emittedFrameLimits;
        const auto update=[&](const BurnEvent& event) {
            owner=event.id;order.emplace_back(0,event.id);
            auto& list=particles[event.id];
            std::erase_if(list,[&](auto& particle) {
                return !particle.tick(100,-50,8155,random);
            });
        };
        const auto emit=[&](const BurnEvent& event) {
            owner=event.id;order.emplace_back(1,event.id);
            std::array<uint32_t,3> rolls{};
            for(auto& roll:rolls)roll=random();
            tak::RetailSmokeParticle particle;
            particle.position={event.id*65536,event.id*2*65536,-event.id*65536};
            particle.frameLimit=rolls[2];
            emittedPositions[event.id].push_back(particle.position);
            emittedFrameLimits[event.id].push_back(particle.frameLimit);
            particles[event.id].push_back(particle);
        };
        tak::retailStepFeatureSmoke(events,update,emit);
        const std::vector<std::pair<int,int>> expectedOrder{
            {0,9},{1,9},{0,2},{1,2},{0,13},{1,13}};
        const std::vector<std::pair<int,uint32_t>> expectedDraws{
            {9,1},{9,2},{9,3},{9,4},{2,5},{2,6},{2,7},
            {13,8},{13,9},{13,10},{13,11}};
        if(order!=expectedOrder || draws!=expectedDraws || particles[9].size()!=1 ||
           particles[2].size()!=2 || particles[13].size()!=2 ||
           emittedFrameLimits[9]!=std::vector<uint32_t>{4} ||
           emittedFrameLimits[2]!=std::vector<uint32_t>{7} ||
           emittedFrameLimits[13]!=std::vector<uint32_t>{11}) {
            std::fprintf(stderr,"feature smoke interleave/expiration regression failed: order=%zu draws=%zu counts=%zu,%zu,%zu limits=%u,%u,%u\n",
                order.size(),draws.size(),particles[9].size(),particles[2].size(),particles[13].size(),
                emittedFrameLimits[9].empty()?0:emittedFrameLimits[9].front(),
                emittedFrameLimits[2].empty()?0:emittedFrameLimits[2].front(),
                emittedFrameLimits[13].empty()?0:emittedFrameLimits[13].front());return 1;
        }
        for(const auto& [id,positions]:emittedPositions) {
            const auto& created=particles.at(id).back();
            if(positions.size()!=1 || created.position!=positions.front() || created.countdown!=8) {
                std::fprintf(stderr,"feature smoke new particle was advanced in its emission tick for feature %d\n",id);return 1;
            }
        }

        for(auto& event:events)event.emit=false;
        order.clear();
        tak::retailStepFeatureSmoke(events,update,emit);
        if(order!=std::vector<std::pair<int,int>>{{0,9},{0,2},{0,13}} ||
           emittedFrameLimits[9].size()!=1 || emittedFrameLimits[2].size()!=1 ||
           emittedFrameLimits[13].size()!=1) {
            std::fprintf(stderr,"feature smoke off-cadence tick emitted particles\n");return 1;
        }
        std::puts("PASS: feature smoke burns run newest-first, update then emit per burn, and retire expired particles before emission");
    }
    {
        using Call=tak::RetailFlightAnimationCall;
        tak::RetailFlightAnimationState state;
        std::vector<Call> calls;
        const auto update=[&](bool air,uint32_t flightSerial,uint32_t landingSerial,
                              bool transport=false) {
            calls.clear();
            tak::updateRetailFlightAnimation(state,air,flightSerial,landingSerial,transport,
                [&](Call call){calls.push_back(call);});
        };
        update(true,0,0);
        if(calls!=std::vector<Call>{Call::BeginFlight})return 1;
        update(true,0,1,true);
        if(calls!=std::vector<Call>{Call::EndTransport,Call::BeginLanding})return 1;
        update(true,0,1,true);
        if(!calls.empty())return 1;
        update(false,0,1,true);
        if(!calls.empty())return 1;
        update(true,0,1,true);
        if(calls!=std::vector<Call>{Call::BeginFlight})return 1;
        tak::RetailFlightAnimationState skipped;
        calls.clear();
        tak::updateRetailFlightAnimation(skipped,true,0,0,false,
            [&](Call call){calls.push_back(call);});
        calls.clear();
        tak::updateRetailFlightAnimation(skipped,false,0,1,true,
            [&](Call call){calls.push_back(call);});
        if(calls!=std::vector<Call>{Call::EndTransport,Call::BeginLanding})return 1;
        tak::RetailFlightAnimationState fallback;
        calls.clear();
        tak::updateRetailFlightAnimation(fallback,true,0,0,false,
            [&](Call call){calls.push_back(call);});
        calls.clear();
        tak::updateRetailFlightAnimation(fallback,false,0,0,false,
            [&](Call call){calls.push_back(call);});
        if(calls!=std::vector<Call>{Call::BeginLanding})return 1;
        tak::RetailFlightAnimationState landedAttack;
        calls.clear();
        tak::updateRetailFlightAnimation(landedAttack,false,1,0,false,
            [&](Call call){calls.push_back(call);});
        if(calls!=std::vector<Call>{Call::BeginFlight})return 1;
        calls.clear();
        tak::updateRetailFlightAnimation(landedAttack,true,1,0,false,
            [&](Call call){calls.push_back(call);});
        if(!calls.empty())return 1;
        tak::RetailFlightAnimationState takeoff;
        calls.clear();
        tak::updateRetailFlightAnimation(takeoff,true,1,0,false,
            [&](Call call){calls.push_back(call);});
        if(calls!=std::vector<Call>{Call::BeginFlight})return 1;
        tak::RetailFlightAnimationState multiple;
        calls.clear();
        tak::updateRetailFlightAnimation(multiple,false,2,0,false,
            [&](Call call){calls.push_back(call);});
        if(calls!=std::vector<Call>{Call::BeginFlight,Call::BeginFlight})return 1;
        calls.clear();
        tak::updateRetailFlightAnimation(multiple,true,2,0,false,
            [&](Call call){calls.push_back(call);});
        if(!calls.empty())return 1;
        if(argc==1)
            std::puts("PASS: flight/landing call-ins are mirrored once and suppress duplicate mode-transition fallbacks");
    }
    {
        using Call=tak::RetailFlightAnimationCall;
        tak::RetailFlightAnimationQueue queue;
        tak::RetailFlightAnimationState state;
        std::vector<Call> calls;
        const auto start=[&](Call call){calls.push_back(call);};
        const auto consume=[&](const auto& snapshot) {
            if(snapshot.unit!=7)std::abort();
            tak::updateRetailFlightAnimation(state,snapshot.airborne,
                snapshot.beginFlightSerial,snapshot.landingSerial,true,start);
        };
        queue.capture(1,7,true,1,0);queue.drain(1,consume);calls.clear();
        // Landing and takeoff happen on separate sim ticks, with no rendered
        // frame between. Collapsing these to counters reverses their order.
        queue.capture(2,7,true,1,1);
        queue.capture(3,7,false,1,1);
        queue.capture(4,7,true,2,1);
        queue.drain(4,consume);
        tak::updateRetailFlightAnimation(state,true,2,1,true,start);
        if(calls!=std::vector<Call>{Call::EndTransport,Call::BeginLanding,Call::BeginFlight}) {
            std::fprintf(stderr,"skipped render frames reordered landing and takeoff callbacks\n");return 1;
        }
        calls.clear();queue.drain(4,consume);
        queue.capture(5,7,true,2,1);queue.capture(6,7,true,2,2);
        queue.drain(5,consume);
        if(!calls.empty())return 1; // unchanged and future snapshots do not replay
        queue.drain(6,consume);
        if(calls!=std::vector<Call>{Call::EndTransport,Call::BeginLanding})return 1;
        calls.clear();queue.capture(7,7,false,2,2);queue.drain(7,consume);
        if(!calls.empty())return 1; // physical touchdown must not duplicate landing
    }
    if(argc==2 && std::strcmp(argv[1],"--smoke-viewport")==0) {
        float x,y,width,height;
        while(std::scanf("%f %f %f %f",&x,&y,&width,&height)==4)
            std::printf("%d\n",int(tak::retailViewportCenterAdmitted(x,y,width,height)));
        return 0;
    }
    if (argc==1) {
        for (const auto& durations:std::vector<std::vector<uint16_t>>{{},{0},{1},{2,2,2},{0,2,1},{65535,0,3}})
            for (bool loop:{false,true}) {
                tak::sim::RetailEffectClock clock;clock.start(durations);
                for (uint32_t age=0;age<200000;++age) {
                    const auto frame=tak::retailEffectFrame(durations,loop,age);
                    if (bool(frame)!=clock.active || (frame && *frame!=clock.frame)) {
                        std::fprintf(stderr,"effect frame mismatch at age %u\n",age);return 1;
                    }
                    clock.tick(durations,loop);
                }
            }
    }
    if(argc==1) {
        for(const auto& durations:std::vector<std::vector<uint16_t>>{{1},{0,2,1},{2,4,1,3},{65535,0,3}}) {
            for(bool loop:{false,true}) {
                tak::sim::RetailEffectClock clock;clock.start(durations);
                for(uint32_t elapsed=1;elapsed<200;++elapsed) {
                    const auto age=tak::retailFeatureSmokeAge(elapsed,0);
                    if(!age)return 1;
                    const auto sampled=tak::retailEffectFrame(durations,loop,*age);
                    const auto native=clock.active ? std::optional<size_t>{clock.frame} : std::nullopt;
                    if(sampled!=native) {
                        std::fprintf(stderr,"feature smoke frame phase mismatch at elapsed %u\n",elapsed);return 1;
                    }
                    clock.tick(durations,loop);
                }
            }
        }
        if(tak::retailFeatureSmokeAge(17,17) || tak::retailFeatureSmokeAge(16,17) ||
           tak::retailFeatureSmokeAge(0,0xffffffffu)!=std::optional<uint32_t>{0})return 1;
        std::puts("PASS: feature smoke samples the pre-advance burn-body frame across loop, expiry and tick-wrap cases");
    }
    if(argc==1) {
        constexpr float width=30.0f,height=40.0f;
        for(const auto& [x,y,admitted]:std::array<std::tuple<float,float,bool>,8>{
                std::tuple{-1.0f,20.0f,false},std::tuple{0.0f,20.0f,true},
                std::tuple{width,20.0f,true},std::tuple{width+1.0f,20.0f,false},
                std::tuple{20.0f,-1.0f,false},std::tuple{20.0f,0.0f,true},
                std::tuple{20.0f,height,true},std::tuple{20.0f,height+1.0f,false}})
            if(tak::retailViewportCenterAdmitted(x,y,width,height)!=admitted)return 1;
        std::puts("PASS: smoke sprite centers use retail's inclusive viewport edges");
    }
    if(argc==1) {
        const auto fireball=tak::retailEffectSpriteOrigin(100,200,31,33,1);
        const auto zoomed=tak::retailEffectSpriteOrigin(100,200,31,33,2);
        if(fireball.x!=69 || fireball.y!=167 || zoomed.x!=38 || zoomed.y!=134)
            return 1;
        std::puts("PASS: authored projectile sprites preserve per-frame anchors at native and zoomed scale");
    }
    if(argc==2 && std::strcmp(argv[1],"--debris-height")==0) {
        int x,z;std::array<int,4> heights;
        while(std::scanf("%d %d %d %d %d %d",&x,&z,&heights[0],&heights[1],&heights[2],&heights[3])==6)
            std::printf("%d\n",tak::retailDebrisTerrainHeight(x,z,2,2,[&](int cx,int cz){return heights[cz*2+cx];}));
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--blood-emit")==0) {
        std::array<int32_t,3> position;std::array<uint32_t,3> colors;
        std::array<unsigned,4> random;
        while(std::scanf("%d %d %d %u %u %u %u %u %u %u",&position[0],&position[1],&position[2],
            &colors[0],&colors[1],&colors[2],&random[0],&random[1],&random[2],&random[3])==10) {
            unsigned index=0;auto p=tak::RetailBloodParticle::emit(position,colors,[&]{return random[index++];});
            std::printf("%d %d %d %d %d %d %u %u\n",p.position[0],p.position[1],p.position[2],
                p.velocity[0],p.velocity[1],p.velocity[2],p.color,index);
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--blood-step")==0) {
        tak::RetailBloodParticle p;int gravity,height;unsigned sea;
        while(std::scanf("%d %d %d %d %d %d %d %u %d",&p.position[0],&p.position[1],
            &p.position[2],&p.velocity[0],&p.velocity[1],&p.velocity[2],&gravity,&sea,&height)==9) {
            int queries=0;auto result=p.tick(gravity,uint8_t(sea),[&](const auto&){++queries;return height;});
            std::printf("%d %d %d %d %d %d %d %d %d\n",int(result.alive),int(result.stain),
                p.position[0],p.position[1],p.position[2],p.velocity[0],p.velocity[1],p.velocity[2],queries);
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--debris-launch")==0) {
        unsigned flags,x,y,z;
        while(std::scanf("%u %u %u %u",&flags,&x,&y,&z)==4) {
            unsigned draws=0;const unsigned values[]={x,y,z};
            tak::RetailDebrisMotion state;
            const bool created=tak::RetailDebrisMotion::launch(flags,
                [&](unsigned bound) {const unsigned value=values[draws++];
                    if(value>=bound)std::abort();
                    return value;},state);
            std::printf("%d %u %d %d %d %u %u\n",int(created),draws,
                state.velocity[0],state.velocity[1],state.velocity[2],state.remaining,state.flags);
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--glow-class")==0) {
        unsigned kind,elapsed;
        while(std::scanf("%u %u",&kind,&elapsed)==2) {
            const auto envelope=tak::retailGlowEnvelope(kind);
            const bool alive=elapsed<=unsigned(envelope.duration);
            const auto radii=alive?tak::retailGlowRadii(int(elapsed),envelope.duration,
                envelope.begin,envelope.end):std::array<int32_t,2>{0,0};
            std::printf("%d %d %d\n",int(alive),radii[0],radii[1]);
        }
        return 0;
    }

    if(argc==2 && std::strcmp(argv[1],"--glow-radii")==0) {
        int elapsed,duration,begin,end;
        while(std::scanf("%d %d %d %d",&elapsed,&duration,&begin,&end)==4) {
            const auto radii=tak::retailGlowRadii(elapsed,duration,begin,end);
            std::printf("%d %d\n",radii[0],radii[1]);
        }
        return 0;
    }

    if(argc==2 && std::strcmp(argv[1],"--glow-mesh")==0) {
        float x,y,rx,ry;unsigned alpha;
        while(std::scanf("%f %f %f %f %u",&x,&y,&rx,&ry,&alpha)==5) {
            const auto vertices=tak::retailGlowMesh(x,y,rx,ry,uint8_t(alpha));
            for(const auto& v:vertices)std::printf("%.9g %.9g %u ",v.x,v.y,v.color);
            std::puts("");
        }
        return 0;
    }

    if(argc==2 && std::strcmp(argv[1],"--debris-step")==0) {
        tak::RetailDebrisMotion d;
        unsigned rx,ry,rz,sx,sy,sz,sea,wet,suppressed;
        int gravity,height;
        while(std::scanf("%d %d %d %d %d %d %u %u %u %u %u %u %u %u %u %d %u %u %d",
                &d.position[0],&d.position[1],&d.position[2],
                &d.velocity[0],&d.velocity[1],&d.velocity[2],
                &rx,&ry,&rz,&sx,&sy,&sz,&d.remaining,&d.flags,&sea,&gravity,
                &wet,&suppressed,&height)==19) {
            d.rotation={uint16_t(rx),uint16_t(ry),uint16_t(rz)};
            d.spin={uint16_t(sx),uint16_t(sy),uint16_t(sz)};
            int samples=0;
            auto result=d.tick(uint8_t(sea),gravity,wet!=0,suppressed!=0,
                [&](const auto&) {++samples;return height;});
            std::printf("%d %d %d %u %d %d %d %d %d %d %u %u %u %d\n",
                int(result.alive),result.effect,result.light,d.remaining,
                d.position[0],d.position[1],d.position[2],
                d.velocity[0],d.velocity[1],d.velocity[2],
                unsigned(d.rotation[0]),unsigned(d.rotation[1]),unsigned(d.rotation[2]),samples);
        }
        return 0;
    }

    if(argc==2 && std::strcmp(argv[1],"--point-emit")==0) {
        std::array<int32_t,3> origin,target;
        unsigned period,random;
        while(std::scanf("%d %d %d %d %d %d %u %u",&origin[0],&origin[1],&origin[2],
                &target[0],&target[1],&target[2],&period,&random)==8) {
            const auto point=tak::RetailPointParticle::emit(origin,target,period,[&] {return random;});
            std::printf("%d %d %d %d %d %d\n",point.position[0],point.position[1],point.position[2],
                point.velocity[0],point.velocity[1],point.velocity[2]);
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--point-step")==0) {
        tak::RetailPointParticle point;
        int terrain,seaLevel;
        while(std::scanf("%d %d %d %d %d %d %u %u %u %d %d",
                &point.position[0],&point.position[1],&point.position[2],
                &point.velocity[0],&point.velocity[1],&point.velocity[2],
                &point.stage,&point.period,&point.countdown,&terrain,&seaLevel)==11) {
            const bool alive=point.tick(seaLevel,[&](const auto&) {return terrain;});
            std::printf("%d %d %d %u %u %d\n",point.position[0],point.position[1],
                point.position[2],point.stage,point.countdown,int(alive));
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--smoke-step")==0) {
        tak::RetailSmokeParticle smoke;
        int wx,wz,gravity;unsigned random;
        while(std::scanf("%d %d %d %d %u %u %u %d %d %d %u",
                &smoke.position[0],&smoke.position[1],&smoke.position[2],
                &smoke.period,&smoke.frameLimit,&smoke.countdown,&smoke.frame,
                &wx,&wz,&gravity,&random)==11) {
            unsigned draws=0;
            const bool alive=smoke.tick(wx,wz,gravity,[&] {++draws;return random;});
            std::printf("%d %d %d %u %u %d %u\n",smoke.position[0],smoke.position[1],
                smoke.position[2],smoke.countdown,smoke.frame,int(alive),draws);
        }
        return 0;
    }

    if(argc==2 && std::strcmp(argv[1],"--flame-admission")==0) {
        std::array<int32_t,3> muzzle,aim,particle;
        int cx,cy;unsigned a,b;
        while(std::scanf("%d %d %d %d %d %d %d %d %d %d %d %u %u",
                &muzzle[0],&muzzle[1],&muzzle[2],&aim[0],&aim[1],&aim[2],
                &particle[0],&particle[1],&particle[2],&cx,&cy,&a,&b)==13) {
            const auto viewport=[&](const std::array<int,2>& point) {
                return point[0]>=cx && point[0]<=cx+320 && point[1]>=cy && point[1]<=cy+240;
            };
            const bool stream=tak::retailFlameStreamVisible(muzzle,aim,
                [&](const auto& point) {return viewport(tak::retailFlameEndpointPoint(point));},
                [&](const auto& point) {return (&point==&muzzle ? a:b)!=0;});
            std::printf("%d\n",int(stream && viewport(tak::retailFlameParticlePoint(particle))));
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--effect-visibility")==0) {
        int width,height;
        if(std::scanf("%d %d",&width,&height)!=2)return 2;
        std::vector<uint8_t> counts(size_t(width)*height);
        for(auto& count:counts) {unsigned value;if(std::scanf("%u",&value)!=1)return 2;count=uint8_t(value);}
        int x,y,z;
        while(std::scanf("%d %d %d",&x,&y,&z)==3)
            std::printf("%d\n",int(tak::RetailEffectVisibility::visible(counts,width,height,{x,y,z})));
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--effect-sight-lifecycle")==0) {
        unsigned now,count;
        while(std::scanf("%u %u",&now,&count)==2) {
            tak::RetailEffectVisibility view(16,16,
                std::vector<tak::sim::RetailExplorationHeight>(256,{0,0}));
            const auto output=[&] {
                std::printf("%zu ",view.retainedCount());
                for(auto value:view.counts())std::printf("%u ",unsigned(value));
            };
            for(unsigned i=0;i<count;++i) {
                int x,z;unsigned delay;
                if(std::scanf("%d %d %u",&x,&z,&delay)!=3)return 2;
                tak::sim::RetailSightFootprint sight;
                sight.x=int16_t(x);sight.z=int16_t(z);sight.distance=16;sight.sightHeight=1;
                view.set(sight,true);view.retain(sight,now,delay);output();
            }
            for(unsigned step=0;step<32;++step) {view.expire(now+step);output();}
            std::puts("");
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--storm-launch")==0) {
        int x,y,z,ax,ay,az,variation;unsigned speed,steps;
        while(std::scanf("%d %d %d %d %d %d %u %u %d",&x,&y,&z,&ax,&ay,&az,&speed,&steps,&variation)==9) {
            const auto result=tak::sim::retailStormLaunch({x,y,z},{ax,ay,az},speed,steps,variation);
            for(auto value:result.position)std::printf("%d ",value);
            for(auto value:result.baseVelocity)std::printf("%d ",value);
            for(float value:result.variation)std::printf("%u ",std::bit_cast<uint32_t>(value));
            std::puts("");
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--wander-random")==0) {
        unsigned seed,bits;
        while(std::scanf("%u %u",&seed,&bits)==2) {
            uint32_t state=seed;
            const double value=tak::sim::retailWanderRandom(state,std::bit_cast<float>(uint32_t(bits)));
            std::printf("%u %llu\n",state,static_cast<unsigned long long>(std::bit_cast<uint64_t>(value)));
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--effect-clock")==0) {
        unsigned count,loop,initial,steps;
        while(std::scanf("%u %u %u %u",&count,&loop,&initial,&steps)==4) {
            std::vector<uint16_t> durations(count);
            for(auto& duration:durations) {unsigned value;if(std::scanf("%u",&value)!=1)return 2;duration=uint16_t(value);}
            tak::sim::RetailEffectClock clock;clock.start(durations,initial);
            for(unsigned step=0;step<=steps;++step) {
                std::printf("%u %u %u ",unsigned(clock.frame),unsigned(clock.remaining),unsigned(clock.active));
                if(step<steps)clock.tick(durations,loop!=0);
            }
            std::puts("");
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--lightning-quad")==0) {
        int x0,y0,x1,y1,vw,vh,w,h,tw,th;
        while(std::scanf("%d %d %d %d %d %d %d %d %d %d",&x0,&y0,&x1,&y1,&vw,&vh,&w,&h,&tw,&th)==10) {
            const auto quad=tak::retailLightningQuad({x0,y0},{x1,y1},vw,vh,w,h,tw,th);
            std::printf("%d",int(quad.has_value()));
            if(quad)for(const auto& vertex:*quad)for(float value:{vertex.x,vertex.y,vertex.u,vertex.v})
                std::printf(" %u",std::bit_cast<uint32_t>(value));
            std::puts("");
        }
        return 0;
    }
    if(argc==3 && std::strcmp(argv[1],"--effect-definitions")==0) {
        const auto root=tak::tdf::parse(argv[2]);
        for(const auto& [name,node]:root.orderedChildren()) {
            const auto definition=tak::retailLightningDefinition(*node);
            const auto& effect=definition.initial;
            std::printf("%.*s %d %d %zu %u %d %d\n",int(name.size()),name.data(),effect.width,effect.height,
                effect.sources.size(),effect.capacity,effect.intensity,effect.decay);
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--effect-palette")==0) {
        int intensity,count;
        while(std::scanf("%d %d",&intensity,&count)==2) {
            if((intensity>>8)==0 || count<0)return 2;
            std::array<uint32_t,256> colors{};
            for(auto& color:colors)if(std::scanf("%u",&color)!=1)return 2;
            for(int i=0;i<count;++i) {
                int ramp,first,last;unsigned start,end;
                if(std::scanf("%d %d %d %u %u",&ramp,&first,&last,&start,&end)!=5)return 2;
                if(first<0 || first>255 || last<0 || last>255)return 2;
                if(ramp)tak::retailEffectColorRamp(colors,first,last,start,end);
                else colors[size_t(first)]=start;
            }
            const auto packed=tak::retailEffectPalette(colors,intensity);
            for(auto color:colors)std::printf("%u ",color);
            for(auto color:packed)std::printf("%u ",unsigned(color));
            std::puts("");
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--lightning-emitter")==0) {
        tak::RetailLightningEffect effect;unsigned seed;int count,steps,rise;
        while(std::scanf("%d %d %u %d %d %d %d %d %u",&effect.width,&effect.height,&effect.capacity,
                &effect.intensity,&effect.decay,&rise,&count,&steps,&seed)==9) {
            if(effect.width<3 || effect.height<4 || count<0 || steps<0)return 2;
            effect.rise=rise!=0;effect.nextSource=0;effect.particles.clear();
            effect.pixels.assign(size_t(effect.width)*effect.height,0);
            effect.sources.assign(size_t(count),{});
            for(auto& source:effect.sources) {
                int fade;
                if(std::scanf("%d %d %d %d %d %d",&source.position[0],&source.position[1],
                        &source.end[0],&source.end[1],&source.rate,&fade)!=6)return 2;
                source.fade=fade!=0;
            }
            unsigned calls=0;
            for(int step=0;step<steps;++step) {
                int emit;if(std::scanf("%d",&emit)!=1)return 2;
                const bool alive=effect.tick(emit!=0,[&] {
                    ++calls;seed=seed*214013u+2531011u;return (seed>>16)&32767u;
                });
                uint64_t hash=1469598103934665603ull;
                for(auto pixel:effect.pixels) {hash^=pixel;hash*=1099511628211ull;}
                std::printf("%d %u %u %u %zu %llu",int(alive),seed,calls,effect.nextSource,effect.particles.size(),
                    static_cast<unsigned long long>(hash));
                for(const auto& source:effect.sources)std::printf(" %d",source.countdown);
                for(const auto& particle:effect.particles)for(auto value:particle.position)std::printf(" %d",value);
                std::puts("");
            }
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--effect-lightning")==0) {
        int width,height,x0,y0,x1,y1,intensity,fade;unsigned seed;
        while(std::scanf("%d %d %d %d %d %d %d %d %u",&width,&height,&x0,&y0,&x1,&y1,&intensity,&fade,&seed)==9) {
            if(width<3 || height<4)return 2;
            std::vector<uint8_t> pixels(size_t(width)*height);unsigned calls=0;
            tak::retailEffectLightning(pixels,width,height,{x0,y0},{x1,y1},intensity,fade!=0,[&] {
                ++calls;seed=seed*214013u+2531011u;return (seed>>16)&32767u;
            });
            std::printf("%u %u",seed,calls);
            for(auto pixel:pixels)std::printf(" %u",unsigned(pixel));
            std::puts("");
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--effect-field")==0) {
        int width,height,rise,count;
        while(std::scanf("%d %d %d %d",&width,&height,&rise,&count)==4) {
            if(width<3 || height<4 || count<0)return 2;
            std::vector<uint8_t> pixels(size_t(width)*height);
            for(auto& pixel:pixels) {unsigned value;if(std::scanf("%u",&value)!=1)return 2;pixel=uint8_t(value);}
            std::vector<tak::RetailEffectParticle> particles(static_cast<size_t>(count));
            for(auto& particle:particles)for(auto* values:{&particle.position,&particle.velocity})
                for(auto& value:*values)if(std::scanf("%d",&value)!=1)return 2;
            tak::retailEffectAdvance(particles,width,height);
            tak::retailEffectDiffuse(pixels,width,height,rise!=0);
            tak::retailEffectStamp(pixels,width,particles);
            std::printf("%zu",particles.size());
            for(const auto& particle:particles)for(auto value:particle.position)std::printf(" %d",value);
            for(auto pixel:pixels)std::printf(" %u",unsigned(pixel));
            std::puts("");
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--splash-units")==0) {
        // 529dc0's unit pass over a fixture map: the window, owner, the
        // 20-unit memory, box distance and falloff, in meeting order.
        int width,height,owner,units,cells;unsigned aoe,edge;std::array<int32_t,3> impact;
        while(std::scanf("%d %d %u %u %d %d %d %d %d %d",&width,&height,&aoe,&edge,&owner,
                &impact[0],&impact[1],&impact[2],&units,&cells)==10) {
            struct Body { int live=0; tak::sim::RetailSplashBody body; };
            std::map<int,Body> bodies;
            for(int i=0;i<units;++i) {
                int id;Body b;
                if(std::scanf("%d %d",&id,&b.live)!=2)return 2;
                for(auto* values:{&b.body.origin,&b.body.low,&b.body.high})
                    for(auto& value:*values)if(std::scanf("%d",&value)!=1)return 2;
                bodies[id]=b;
            }
            std::vector<std::array<int,2>> words(size_t(width)*height);
            for(int i=0;i<cells;++i) {
                int x,z,ground,air;
                if(std::scanf("%d %d %d %d",&x,&z,&ground,&air)!=4)return 2;
                words[size_t(z)*width+x]={ground,air};
            }
            struct Host {
                const std::map<int,Body>& bodies;const std::vector<std::array<int,2>>& words;int width;
                int groundAt(int x,int z) const { return words[size_t(z)*width+x][0]; }
                int airAt(int x,int z) const { return words[size_t(z)*width+x][1]; }
                bool live(int id) const { const auto it=bodies.find(id);return it!=bodies.end() && it->second.live; }
                tak::sim::RetailSplashBody body(int id) const { return bodies.at(id).body; }
            } host{bodies,words,width};
            std::vector<std::pair<int,float>> hits;
            tak::sim::retailSplashUnits(impact,uint16_t(aoe),std::bit_cast<float>(uint32_t(edge)),owner,
                width,height,host,[&](int id,float fraction){hits.emplace_back(id,fraction);});
            std::printf("%zu",hits.size());
            for(const auto& [id,fraction]:hits)std::printf(" %d:%08x",id,std::bit_cast<uint32_t>(fraction));
            std::puts("");
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--projectile-proximity")==0) {
        std::array<int32_t,3> point,target;unsigned radius;
        while(std::scanf("%d %d %d %d %d %d %u",&point[0],&point[1],&point[2],
                &target[0],&target[1],&target[2],&radius)==7)
            std::printf("%d\n",int(tak::sim::retailProjectileProximity(point,target,uint16_t(radius))));
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--air-occupancy")==0) {
        // Persistent secondary occupancy over ticks: retirements (507050 mode 0),
        // silent deaths, then the clear (507050 mode 1) and insert (506c40) passes.
        int width,height,count,ticks;unsigned seed;
        while(std::scanf("%d %d %d %d %u",&width,&height,&count,&ticks,&seed)==5) {
            std::vector<tak::sim::RetailAirCollisionBody> bodies(static_cast<size_t>(count));
            std::vector<tak::sim::RetailAirOccupant> states(static_cast<size_t>(count));
            std::vector<int> live(static_cast<size_t>(count),1);
            for(int i=0;i<count;++i)bodies[size_t(i)].id=i+1;
            struct Host {
                std::vector<tak::sim::RetailAirCollisionBody>& bodies;
                std::vector<tak::sim::RetailAirOccupant>& states;
                std::vector<int>& live;
                tak::sim::RetailAirOccupant* state(int id) {
                    return id>=1 && id<=int(bodies.size()) && live[size_t(id-1)] ? &states[size_t(id-1)] : nullptr;
                }
                bool footprint(int id,int& x,int& z,int& fx,int& fz) const {
                    const auto& b=bodies[size_t(id-1)];x=b.x;z=b.z;fx=b.width;fz=b.height;return true;
                }
                bool airborne(int id) const { return bodies[size_t(id-1)].airborne; }
                void forEachFlyer(const std::function<void(int)>& f) const { for(size_t i=0;i<bodies.size();++i)if(live[i])f(int(i)+1); }
            } host{bodies,states,live};
            tak::sim::RetailAirOccupancy grid;grid.reset(width,height);
            unsigned calls=0;
            auto random=[&](unsigned n){++calls;seed=seed*214013u+2531011u;return ((seed>>16)&32767u)*n/32768u;};
            for(int tick=0;tick<ticks;++tick) {
                std::vector<int> event(static_cast<size_t>(count));
                for(int i=0;i<count;++i) {
                    auto& b=bodies[size_t(i)];int airborne;
                    if(std::scanf("%d %d %d %d %d %d",&b.x,&b.z,&b.width,&b.height,&airborne,&event[size_t(i)])!=6)return 2;
                    b.airborne=airborne!=0;
                }
                for(int i=0;i<count;++i) {
                    if(!live[size_t(i)])continue;
                    if(event[size_t(i)]==1)grid.remove(i+1,host,random);
                    if(event[size_t(i)])live[size_t(i)]=0;
                }
                for(int i=0;i<count;++i)grid.clear(i+1,host);
                for(int i=0;i<count;++i)grid.insert(i+1,host,random);
                std::printf("%u %u",seed,calls);
                for(int cell:grid.cells())std::printf(" %d",cell);
                std::puts("");
            }
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--landing-site")==0) {
        int width,height,fx,fz,maxDepth,minDepth,maxSlope,canFly,floater,player,sea,self,pool,features,words;
        int32_t x,z;
        while(std::scanf("%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",&width,&height,&fx,&fz,&maxDepth,
                &minDepth,&maxSlope,&canFly,&floater,&player,&sea,&x,&z,&self,&pool,&features,&words)==17) {
            std::vector<int> alive(static_cast<size_t>(pool)+1),blocking(static_cast<size_t>(features)),exploration(static_cast<size_t>(words));
            for(int i=1;i<=pool;++i)if(std::scanf("%d",&alive[size_t(i)])!=1)return 2;
            for(auto& v:blocking)if(std::scanf("%d",&v)!=1)return 2;
            for(auto& v:exploration)if(std::scanf("%d",&v)!=1)return 2;
            struct Cell { int ground,air,high,low,feature,back,yard; };
            std::vector<Cell> cells(static_cast<size_t>(width)*height);
            for(auto& c:cells)if(std::scanf("%d %d %d %d %d %d %d",&c.ground,&c.air,&c.high,&c.low,&c.feature,&c.back,&c.yard)!=7)return 2;
            struct Host {
                int w,h,seaLevel,self;
                std::vector<int>& alive;std::vector<int>& blocking;std::vector<int>& words;std::vector<Cell>& cells;
                int width() const { return w; }
                int height() const { return h; }
                uint16_t exploration(size_t i) const { return i<words.size() ? uint16_t(words[i]) : 0; }
                uint16_t feature(size_t c) const { return uint16_t(cells[c].feature); }
                int featureBack(size_t c) const { return cells[c].back; }
                int featureCount() const { return int(blocking.size()); }
                bool featureBlocking(int i) const { return size_t(i)>=blocking.size() || blocking[size_t(i)]; }
                bool yard(size_t c) const { return cells[c].yard; }
                bool other(int id) const { return id>0 && id<int(alive.size()) && alive[size_t(id)] && id!=self; }
                bool groundOther(size_t c) const { return other(cells[c].ground); }
                bool airOther(size_t c) const { return cells[c].air!=0xffff && other(cells[c].air); }
                int low(size_t c) const { return cells[c].low; }
                int high(size_t c) const { return cells[c].high; }
                int sea() const { return seaLevel; }
            };
            Host host{width,height,sea,self,alive,blocking,exploration,cells};
            const tak::sim::RetailLandingLimits limits{int16_t(fx),int16_t(fz),int16_t(maxDepth),int16_t(minDepth),
                uint8_t(maxSlope),canFly!=0,floater!=0,uint8_t(player)};
            std::printf("%d\n",int(tak::sim::retailLandingSiteFree(x,z,limits,host)));
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--air-collision-grid")==0) {
        int width,height,count;unsigned seed;
        while(std::scanf("%d %d %d %u",&width,&height,&count,&seed)==4) {
            std::vector<tak::sim::RetailAirCollisionBody> bodies(static_cast<size_t>(count));
            for(auto& body:bodies) {
                int airborne;
                if(std::scanf("%d %d %d %d %d %d",&body.id,&body.x,&body.z,&body.width,&body.height,&airborne)!=6)return 2;
                body.airborne=airborne!=0;
            }
            unsigned calls=0;
            const auto cells=tak::sim::retailAirCollisionGrid(width,height,bodies,[&](unsigned n) {
                ++calls;seed=seed*214013u+2531011u;return ((seed>>16)&32767u)*n/32768u;
            });
            std::printf("%u %u",seed,calls);
            for(int cell:cells)std::printf(" %d",cell);
            std::puts("");
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--projectile-environment")==0) {
        int y,speed,feature;unsigned flags,minimum,sea,waterPass;
        while(std::scanf("%d %d %u %u %u %d %u",&y,&speed,&flags,&minimum,&sea,&feature,&waterPass)==7) {
            const bool hit=tak::sim::retailProjectileHitsEnvironment(y,speed,flags,uint8_t(minimum),uint8_t(sea),
                feature<0 ? std::optional<uint8_t>{} : std::optional<uint8_t>{uint8_t(feature)},waterPass!=0);
            std::printf("%d %d\n",int(hit),speed);
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--projectile-in-unit")==0) {
        std::array<int32_t,3> point,origin;int top;unsigned heading;
        while(std::scanf("%d %d %d %d %d %d %d %u",&point[0],&point[1],&point[2],
                &origin[0],&origin[1],&origin[2],&top,&heading)==8) {
            tak::sim::RetailCollisionQuad quad;
            for(auto& vertex:quad)if(std::scanf("%d %d",&vertex[0],&vertex[1])!=2)return 2;
            std::printf("%d\n",int(tak::sim::retailProjectileInUnit(point,origin,top,uint16_t(heading),quad)));
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--flame-scan")==0) {
        std::array<int32_t,3> origin,step;unsigned range,speed,substeps,hit,clip;
        while(std::scanf("%d %d %d %d %d %d %u %u %u %u %u",&origin[0],&origin[1],&origin[2],
                &step[0],&step[1],&step[2],&range,&speed,&substeps,&hit,&clip)==11) {
            unsigned calls=0;uint64_t hash=1469598103934665603ull;
            const auto scan=tak::retailFlameScan(origin,step,range,speed,substeps,[&](auto& point) {
                ++calls;
                for(auto v:point) {hash^=uint32_t(v);hash*=1099511628211ull;}
                if(calls!=hit)return 0;
                if(clip) for(unsigned axis=0;axis<3;++axis)
                    point[axis]=std::bit_cast<int32_t>(uint32_t(point[axis])+axis+1u);
                return 2;
            });
            std::printf("%u %u %d %d %d %llu\n",calls,scan.lifetime,scan.endpoint[0],scan.endpoint[1],scan.endpoint[2],
                static_cast<unsigned long long>(hash));
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--flame-particle")==0) {
        int x,y,z,vx,vy,vz,life,remaining;unsigned frames;
        while(std::scanf("%d %d %d %d %d %d %d %d %u",&x,&y,&z,&vx,&vy,&vz,&life,&remaining,&frames)==9) {
            tak::RetailFlameParticle particle{{x,y,z},{vx,vy,vz},remaining,life,0};
            const int frame=particle.frame(uint16_t(frames));
            const bool alive=particle.tick();
            std::printf("%d %d %d %d %d %d\n",frame,int(alive),particle.remaining,
                particle.position[0],particle.position[1],particle.position[2]);
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--flame-velocity")==0) {
        std::array<int32_t,3> origin,endpoint;int life;unsigned a,b,c;
        while(std::scanf("%d %d %d %d %d %d %d %u %u %u",&origin[0],&origin[1],&origin[2],
                &endpoint[0],&endpoint[1],&endpoint[2],&life,&a,&b,&c)==10) {
            const auto v=tak::retailFlameVelocity(origin,endpoint,life,{uint16_t(a),uint16_t(b),uint16_t(c)});
            std::printf("%d %d %d\n",v[0],v[1],v[2]);
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--weapon-animation")==0) {
        unsigned heading,pitch,slot;
        while(std::scanf("%u %u %u",&heading,&pitch,&slot)==3) {
            tak::RetailWeaponAnimations events;
            events.add(tak::RetailWeaponAnimation::Aim,int(slot),uint16_t(heading),uint16_t(pitch));
            const auto& event=events.events[0];
            std::printf("%u %u %u\n",unsigned(event.heading),unsigned(event.pitch),unsigned(event.slot));
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--weapon-fire")==0) {
        unsigned nominal,roll,weaponFlags,typeFlags,unitFlags,aimFlags;
        while(std::scanf("%u %u %u %u %u %u",&nominal,&roll,&weaponFlags,&typeFlags,&unitFlags,&aimFlags)==6)
            std::printf("%u %u\n",unsigned(tak::sim::retailWeaponReload(uint16_t(nominal),uint16_t(roll))),
                tak::sim::retailWeaponFireEvent(weaponFlags,typeFlags,unitFlags,uint16_t(aimFlags)));
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--weapon-update")==0) {
        unsigned enabled,selected;
        while(std::scanf("%u %u",&enabled,&selected)==2) {
            unsigned flags[3],reloads[3],present[3],admit[3],ready[3],signal[3];
            for(int i=0;i<3;++i)
                if(std::scanf("%u %u %u %u %u %u",&flags[i],&reloads[i],&present[i],&admit[i],&ready[i],&signal[i])!=6)return 2;
            unsigned long long trace=0;uint32_t combatUntil=0;
            for(int i=0;i<3;++i) {
                tak::RetailAimState state{0,0,uint16_t(flags[i])};
                uint16_t reload=uint16_t(reloads[i]);
                if(enabled && present[i] && (selected==3 || selected==unsigned(i)))
                    tak::sim::retailWeaponUpdate(reload,state,100,combatUntil,
                        [&]{trace=trace*16+unsigned(i*4+1);return admit[i]!=0;},
                        [&]{trace=trace*16+unsigned(i*4+2);return ready[i]!=0;},
                        [&]{trace=trace*16+unsigned(i*4+3);if(signal[i])state.set(23);},
                        [&]{trace=trace*16+unsigned(i*4+4);state.projectileCreated();});
                flags[i]=state.flags;reloads[i]=reload;
            }
            std::printf("%llu %u",trace,combatUntil);
            for(int i=0;i<3;++i)std::printf(" %u %u",flags[i],reloads[i]);
            std::printf("\n");
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--aim-state")==0) {
        unsigned heading,pitch,flags,nextHeading,nextPitch,tolerance;
        int operation,available,aligned;
        while(std::scanf("%d %u %u %u %u %u %u %d %d",&operation,&heading,&pitch,&flags,
                &nextHeading,&nextPitch,&tolerance,&available,&aligned)==9) {
            tak::RetailAimState state{uint16_t(heading),uint16_t(pitch),uint16_t(flags)};
            bool result=false;
            if(operation==0)result=state.start(uint16_t(nextHeading),uint16_t(nextPitch));
            else if(operation==1)result=state.ready(uint16_t(nextHeading),uint16_t(nextPitch),
                uint16_t(tolerance),available!=0,aligned!=0);
            else state.set(operation);
            std::printf("%d %u %u %u\n",int(result),unsigned(state.heading),unsigned(state.pitch),unsigned(state.flags));
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--aim-facing")==0) {
        // 52fe30's unit-target branch, then 52fff0: the facing request a slot
        // posts (old request kept when it posts none), and the gate against it.
        std::array<int32_t,3> origin{},targetOrigin{},led{};
        int melee,mover,canFly,hover,pivot,heading,request,moverFlags,tolerance,flags,angleHeading,anglePitch;
        while(std::scanf("%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",&origin[0],&origin[2],
                &targetOrigin[0],&targetOrigin[2],&led[0],&led[2],&melee,&mover,&canFly,&hover,&pivot,
                &request,&moverFlags,&heading,&tolerance,&flags,&angleHeading,&anglePitch)==18) {
            const bool posted=tak::sim::retailPostsFacing(mover!=0,canFly!=0,hover!=0,uint16_t(pivot));
            if(posted) {
                request=tak::sim::retailSlotFacing(origin,targetOrigin,led,melee!=0);
                moverFlags|=0x10;
            }
            tak::RetailAimState state{uint16_t(angleHeading),uint16_t(anglePitch),uint16_t(flags)};
            state.start(uint16_t(angleHeading),uint16_t(anglePitch));
            const bool aligned=!posted || tak::sim::retailFacingAligned(uint16_t(heading),uint16_t(request),uint16_t(tolerance));
            const bool ready=state.ready(uint16_t(angleHeading),uint16_t(anglePitch),uint16_t(tolerance),true,aligned);
            std::printf("%d %d %d %d %u\n",int(posted),request,moverFlags,int(ready),unsigned(state.flags));
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--aim-lead")==0) {
        std::array<int32_t,3> source{},target{},velocity{};int32_t speed;int noLead;
        while(std::scanf("%d %d %d %d %d %d %d %d %d %d %d",&source[0],&source[1],&source[2],
            &target[0],&target[1],&target[2],&velocity[0],&velocity[1],&velocity[2],&speed,&noLead)==11) {
            const auto point=tak::retailAimLead(source,target,velocity,speed,noLead!=0);
            std::printf("%d %d %d\n",point[0],point[1],point[2]);
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--ballistic-aim")==0) {
        float x,y,z,speed,scale;int high,gravity;
        while(std::scanf("%f %f %f %f %f %d %d",&x,&y,&z,&speed,&scale,&high,&gravity)==7)
            std::printf("%u\n",unsigned(tak::retailBallisticPitch(x,y,z,speed,scale,high!=0,gravity)));
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--ballistic-inrange")==0) {
        int32_t dx,dy,dz,speed,range,gravity;float scale;int high;
        while(std::scanf("%d %d %d %d %f %d %d %d",&dx,&dy,&dz,&speed,&scale,&high,&gravity,&range)==8)
            std::printf("%d\n",int(tak::retailBallisticInRange(dx,dy,dz,speed,scale,high!=0,gravity,range)));
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--ballistic-projectile")==0) {
        char operation;
        while(std::scanf(" %c",&operation)==1) {
            tak::sim::RetailBallisticShot shot;
            if(operation=='L') {
                uint16_t body,relative,pitch;int32_t speed;
                if(std::scanf("%d %d %d %hu %hu %hu %d",&shot.position[0],&shot.position[1],
                    &shot.position[2],&body,&relative,&pitch,&speed)!=7)return 2;
                shot=tak::sim::retailBallisticLaunch(shot.position,body,relative,pitch,speed);
            } else if(operation=='T') {
                int32_t gravity;float adjustment;uint32_t steps;
                std::array<uint16_t,3> spin{};
                for(auto& value:shot.position)if(std::scanf("%d",&value)!=1)return 2;
                for(auto& value:shot.velocity)if(std::scanf("%d",&value)!=1)return 2;
                for(auto& value:shot.angles)if(std::scanf("%hu",&value)!=1)return 2;
                if(std::scanf("%d %f %u %hu %hu %hu",&gravity,&adjustment,&steps,
                    &spin[0],&spin[1],&spin[2])!=6)return 2;
                tak::sim::retailBallisticTick(shot,gravity,adjustment,steps,spin);
            } else return 2;
            std::printf("%d %d %d %d %d %d %u %u %u\n",shot.position[0],shot.position[1],
                shot.position[2],shot.velocity[0],shot.velocity[1],shot.velocity[2],
                unsigned(shot.angles[0]),unsigned(shot.angles[1]),unsigned(shot.angles[2]));
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--dropped-ballistic-projectile")==0) {
        std::array<int32_t,3> muzzle{},target{};int32_t gravity;
        while(std::scanf("%d %d %d %d %d %d %d",&muzzle[0],&muzzle[1],&muzzle[2],
                &target[0],&target[1],&target[2],&gravity)==7) {
            uint32_t ticks=0;
            const auto shot=tak::sim::retailDroppedBallisticLaunch(muzzle,target,gravity,&ticks);
            std::printf("%u %d %d %d %d %d %d %u %u %u\n",ticks,
                shot.position[0],shot.position[1],shot.position[2],shot.velocity[0],
                shot.velocity[1],shot.velocity[2],unsigned(shot.angles[0]),
                unsigned(shot.angles[1]),unsigned(shot.angles[2]));
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--guided-projectile")==0) {
        char operation;
        while(std::scanf(" %c",&operation)==1) {
            tak::sim::RetailGuidedShot shot;
            if(operation=='L') {
                uint16_t body,heading,pitch;int32_t speed;
                if(std::scanf("%d %d %d %hu %hu %hu %d",&shot.position[0],&shot.position[1],
                    &shot.position[2],&body,&heading,&pitch,&speed)!=7)return 2;
                shot=tak::sim::retailGuidedLaunch(shot.position,body,heading,pitch,speed);
            } else if(operation=='T') {
                std::array<int32_t,3> target{};float turn;int32_t speed;uint32_t steps;
                std::array<uint16_t,3> spin{};int updateAngles,hitAt;
                for(auto& value:shot.position)if(std::scanf("%d",&value)!=1)return 2;
                for(auto& value:shot.velocity)if(std::scanf("%d",&value)!=1)return 2;
                for(auto& value:shot.angles)if(std::scanf("%hu",&value)!=1)return 2;
                for(auto& value:target)if(std::scanf("%d",&value)!=1)return 2;
                if(std::scanf("%f %d %u %hu %hu %hu %d %d",&turn,&speed,&steps,
                    &spin[0],&spin[1],&spin[2],&updateAngles,&hitAt)!=8)return 2;
                unsigned collisions=0;
                const bool hit=tak::sim::retailGuidedTick(shot,target,turn,speed,steps,spin,
                    updateAngles!=0,[&](const auto&,int32_t&) {return ++collisions==unsigned(hitAt) ? 2 : 0;});
                std::printf("%d %d %d %d %d %d %u %u %u %u %u\n",shot.position[0],shot.position[1],
                    shot.position[2],shot.velocity[0],shot.velocity[1],shot.velocity[2],
                    unsigned(shot.angles[0]),unsigned(shot.angles[1]),unsigned(shot.angles[2]),
                    collisions,unsigned(hit));
                continue;
            } else return 2;
            std::printf("%d %d %d %d %d %d %u %u %u\n",shot.position[0],shot.position[1],
                shot.position[2],shot.velocity[0],shot.velocity[1],shot.velocity[2],
                unsigned(shot.angles[0]),unsigned(shot.angles[1]),unsigned(shot.angles[2]));
        }
        return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--flight-attitude")==0) {
        std::array<int32_t,3> state{},delta{};unsigned heading;int32_t bank,pitch,gravity;
        while(std::scanf("%d %d %d %d %d %d %u %d %d %d",&state[0],&state[1],&state[2],
                &delta[0],&delta[1],&delta[2],&heading,&bank,&pitch,&gravity)==10) {
            const auto angles=tak::sim::retailFlightAttitude(state,delta,uint16_t(heading),bank,pitch,gravity);
            std::printf("%d %d %d %u %u\n",state[0],state[1],state[2],unsigned(angles[0]),unsigned(angles[1]));
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--piece-bounds")==0) {
        unsigned count;
        while(std::scanf("%u",&count)==1) {
            tak::sim::RetailPieceBounds bounds;
            for(unsigned i=0;i<count;++i) {
                std::array<int32_t,3> vertex{};
                if(std::scanf("%d %d %d",&vertex[0],&vertex[1],&vertex[2])!=3)return 2;
                bounds.add(vertex);
            }
            const auto point=bounds.center();
            std::printf("%d %d %d\n",point[0],point[1],point[2]);
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--direct-aim")==0) {
        int32_t dx,dy,dz;unsigned heading;
        while(std::scanf("%d %d %d %u",&dx,&dy,&dz,&heading)==4) {
            const auto result=tak::sim::retailDirectAim(dx,dy,dz,uint16_t(heading));
            std::printf("%u %u\n",unsigned(result[0]),unsigned(result[1]));
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--animation-moverate")==0) {
        int speed,turn,dx,dz,slow,fast,road,water,flags,attached,flying;
        while(std::scanf("%d %d %d %d %d %d %d %d %d %d %d",&speed,&turn,&dx,&dz,&slow,
                &fast,&road,&water,&flags,&attached,&flying)==11) {
            const auto mult=tak::sim::Fixed::raw(flags&0x800 ? road : flags&0x1000 ? water : 65536);
            const auto horizontal=flying ? int32_t(tak::sim::isqrt64(
                uint64_t(int64_t(dx)*dx)+uint64_t(int64_t(dz)*dz))) : speed;
            std::printf("%u\n",tak::sim::retailAnimationMoveRate(speed,int16_t(turn),horizontal,
                (tak::sim::Fixed::raw(slow)*mult).v,(tak::sim::Fixed::raw(fast)*mult).v,flags&4,attached));
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--animation-occupancy")==0) {
        unsigned previous,mode,sea,waterline;int height,top;
        while(std::scanf("%u %u %d %u %u %d",&previous,&mode,&height,&sea,&waterline,&top)==6)
            std::printf("%u\n",tak::sim::retailAnimationOccupancy(previous,uint8_t(mode),
                int16_t(height),uint8_t(sea),uint8_t(waterline),int16_t(top)));
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--animation-queries")==0) {
        int dx,dz,base,road,water,flags,turn,moving,pivot,attached;
        while(std::scanf("%d %d %d %d %d %d %d %d %d %d",&dx,&dz,&base,&road,&water,
                         &flags,&turn,&moving,&pivot,&attached)==10) {
            const auto multiplier=tak::sim::Fixed::raw(flags&0x800 ? road : flags&0x1000 ? water : 65536);
            const auto maximum=tak::sim::Fixed::raw(base)*multiplier;
            std::printf("%d %d\n",tak::sim::retailHorizontalAnimationPercent(dx,dz,maximum.v,flags&4,attached),
                tak::sim::retailTurnAnimationPercent(int16_t(turn),uint16_t(moving),uint16_t(pivot),multiplier.v,attached));
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--vertical-animation")==0) {
        int velocity,speed,blocked,attached;
        while(std::scanf("%d %d %d %d",&velocity,&speed,&blocked,&attached)==4)
            std::printf("%d\n",tak::sim::retailFlightVerticalPercent(velocity,speed,blocked,attached));
        return 0;
    }
    if(argc==1) {
        tak::RetailAimState aim;uint16_t reload=0;uint32_t combatUntil=0;
        bool ready=true;int callbacks=0,projectiles=0;
        auto update=[&](uint32_t tick) {
            tak::sim::retailWeaponUpdate(reload,aim,tick,combatUntil,[]{return true;},
                [&]{return ready;},[&]{++callbacks;reload=20;},
                [&]{++projectiles;aim.projectileCreated();});
        };
        update(100);
        if(callbacks!=1 || projectiles || reload!=20 || combatUntil)return 1;
        ready=false;update(101);
        if(callbacks!=1 || projectiles || reload!=19)return 1;
        aim.set(23);update(102);
        if(callbacks!=1 || projectiles!=1 || reload!=18 || combatUntil!=702 || (aim.flags&0xf0))return 1;
        update(103);
        if(projectiles!=1)return 1;
        std::puts("PASS: weapon callback starts reload; delayed script acknowledgement creates exactly one projectile");
    }
    if (argc==1) {
        static_assert(sizeof(RenderOrder)<=64);
        tak::sim::UnitType producer;producer.isBuilder=true;producer.maxVel=tak::sim::Fixed::fromInt(1);
        tak::sim::Unit source;source.type=&producer;
        for(unsigned bits=0;bits<256;++bits) {
            tak::sim::Order o;
            o.x=tak::sim::Fixed::raw(int32_t(bits*17451)-150000);
            o.z=tak::sim::Fixed::raw(240000-int32_t(bits*9203));
            o.clickX=o.z;o.clickZ=o.x;o.buildX=o.x;o.buildZ=o.z;
            o.targetId=int(bits);o.reclaimFeat=-int(bits);o.repairTarget=int(bits+1);o.issuedTick=bits*37;
            o.load=bits&1;o.unload=bits&2;o.attackMove=bits&4;o.patrol=bits&8;o.guard=bits&16;o.goal=bits&32;
            if(bits&64) {
                o.reclaimArea=tak::sim::RetailReclaimArea{o.x.v,o.z.v,o.buildX.v,o.buildZ.v,uint8_t(bits&1 ? 2 : 0)};
            }
            if(bits&128){o.buildRectangle.emplace();o.buildType=&producer;}
            source.orders.push_back(o);
        }
        source.rally={source.orders[64],source.orders[32]};
        UnitR frame;frame.type=&producer;frame.repeatType=&producer;frame.captureOrders(source);
        if(frame.orders.size()!=source.orders.size() || frame.rally.size()!=2 ||
           !frame.hasQueuedBuild() || !frame.hasQueuedWork() || &frame.displayedOrders()!=&frame.rally)return 1;
        for(size_t i=0;i<source.orders.size();++i) {
            const auto& o=source.orders[i];const auto& r=frame.orders[i];
            if(r.x!=o.x || r.z!=o.z || r.clickX!=o.clickX || r.clickZ!=o.clickZ ||
               r.buildX!=o.buildX || r.buildZ!=o.buildZ || r.buildType!=o.buildType ||
               r.targetId!=o.targetId || r.reclaimFeat!=o.reclaimFeat || r.repairTarget!=o.repairTarget ||
               r.issuedTick!=o.issuedTick || r.load!=o.load || r.unload!=o.unload || r.attackMove!=o.attackMove ||
               r.patrol!=o.patrol || r.guard!=o.guard || r.goal!=o.goal ||
               r.reclaimArea!=bool(o.reclaimArea) || r.buildRectangle!=bool(o.buildRectangle))return 1;
            if(o.reclaimArea && r.areaApproached!=(o.reclaimArea->stage>=2))return 1;
        }
        source.orders.clear();source.rally.clear();frame.repeatType=nullptr;frame.captureOrders(source);
        if(!frame.orders.empty() || !frame.rally.empty() || frame.hasQueuedWork() || frame.hasQueuedBuild() ||
           &frame.displayedOrders()!=&frame.orders)return 1;
        std::puts("PASS: compact render orders preserve queue, rally, markers and work status");
        tak::sim::Order area;area.goal=true;
        area.manaBuildArea=tak::sim::ManaBuildArea{&producer,tak::sim::Fixed::fromInt(100),
            tak::sim::Fixed::fromInt(800),tak::sim::Fixed::fromInt(200),tak::sim::Fixed::fromInt(900),7,true};
        const RenderOrder r(area);
        if(!r.manaBuildArea || r.buildType!=&producer || r.areaNextSpot!=7 || !r.areaExploring ||
            r.x!=area.manaBuildArea->minX || r.z!=area.manaBuildArea->minZ ||
            r.buildX!=area.manaBuildArea->maxX || r.buildZ!=area.manaBuildArea->maxZ)return 1;
        std::puts("PASS: compact area orders retain bounds, progress and exploration state");
    }
    if (argc==1) {
        tak::sim::UnitType type;type.maxVel=tak::sim::Fixed::fromInt(1);
        type.modelTop=10*65536;type.waterline=2;
        tak::sim::Unit unit;unit.type=&type;
        UnitR frame;
        unit.groundY=tak::sim::Fixed::fromInt(101);frame.captureOccupancy(unit,100,0);
        if (frame.animationOccupancy!=4) return 1;
        unit.groundY=tak::sim::Fixed::fromInt(98);frame.captureOccupancy(unit,100,4);
        if (frame.animationOccupancy!=2) return 1;
        unit.groundY=tak::sim::Fixed::fromInt(94);frame.captureOccupancy(unit,100,2);
        if (frame.animationOccupancy!=2) return 1; // native retained band
        unit.groundY=tak::sim::Fixed::fromInt(89);frame.captureOccupancy(unit,100,2);
        if (frame.animationOccupancy!=3) return 1;
        type.canFly=true;unit.flightGroundMode=2;frame.captureOccupancy(unit,100,3);
        if (frame.animationOccupancy!=5) return 1;
        unit.flightGroundMode=1;unit.flightY=tak::sim::Fixed::fromInt(101);
        frame.captureOccupancy(unit,100,5);
        if (frame.animationOccupancy!=4) return 1;
        std::puts("PASS: animation snapshot preserves native ground, water and landing occupancy");
        type.canFly=false;type.animationMoveRate1=tak::sim::Fixed::fromInt(1);
        type.animationMoveRate2=tak::sim::Fixed::fromInt(2);
        unit.speed=tak::sim::Fixed::fromInt(3);frame.captureMovement(unit);frame.captureMoveRate(unit,0);
        if (frame.animationMoveRate!=3) return 1;
        type.roadMult=tak::sim::Fixed::fromInt(2);unit.groundTerrainFlags=0x800;
        frame.captureMovement(unit);frame.captureMoveRate(unit,0);
        if (frame.animationMoveRate!=2) return 1;
        unit.speed={};frame.captureMovement(unit);frame.captureMoveRate(unit,100);
        if (frame.animationMoveRate!=1) return 1;
        frame.captureMoveRate(unit,0);if (frame.animationMoveRate!=0) return 1;
        unit.speed=tak::sim::Fixed::fromInt(3);unit.bodyBlockStreak=2;
        frame.captureMovement(unit);frame.captureMoveRate(unit,100);
        if (frame.animationMoveRate!=0) return 1;
        type.canFly=true;unit.groundTerrainFlags=0;unit.flightVelocity={0,0,65536};
        frame.captureMovement(unit);frame.captureMoveRate(unit,0);
        if (frame.animationMoveRate!=1) return 1; // actual vector, not scalar speed
        unit.inTransport=1;frame.captureMovement(unit);frame.captureMoveRate(unit,100);
        if (frame.animationMoveRate!=0) return 1;
        std::puts("PASS: animation snapshot uses native speed tiers for boats, aircraft and pivots");
    }
    if (argc==1) {
        tak::sim::Player player;player.mana=500;player.storage=1000;
        for (int tick=0;tick<30;++tick) {
            player.creditMana(10.0/30);player.debitMana(4.0/30);
            player.displayResources.advance();
        }
        PlayerR frame;frame.captureEconomy(player);
        if (std::abs(frame.income-10)>0.001 || std::abs(frame.expenditure-4)>0.001 ||
            std::abs(frame.mana-506)>0.001 || frame.storage!=1000) return 1;
        player.displayResources.samples.back()={1000,2000};
        frame.captureEconomy(player);
        if (std::abs(frame.income-10)>0.001 || std::abs(frame.expenditure-4)>0.001) return 1;
        for (const auto [mana,index]:{std::pair{0.f,0},std::pair{500.f,11},
                                     std::pair{1000.f,23},std::pair{2000.f,23}}) {
            frame.mana=mana;if (frame.manaBulbFrame(24)!=index) return 1;
        }
        player.retailResources=tak::sim::RetailConstructionResources{};
        player.retailResources->capacity=25;player.mana=12.5;
        frame.captureEconomy(player);
        if (frame.storage!=25 || frame.manaBulbFrame(24)!=11) return 1;
        std::puts("PASS: HUD uses recorded resource rates, actual capacity and retail bulb indexing");
    }

    if (argc==2 && std::strcmp(argv[1],"--movement-animation")==0) {
        int blocked,attached,speed,maximum;
        while (std::scanf("%d %d %d %d",&blocked,&attached,&speed,&maximum)==4) {
            tak::sim::UnitType type;type.maxVel=tak::sim::Fixed::raw(maximum);
            tak::sim::Unit unit;unit.type=&type;unit.baseSpeed=type.maxVel;unit.speed=tak::sim::Fixed::raw(speed);unit.bodyBlockStreak=blocked;
            UnitR frame;frame.type=&type;frame.inTransport=attached;frame.captureMovement(unit);
            std::printf("%d %d\n",frame.animationSpeedPercent(),int(frame.walking()));
        }
        return 0;
    }
    if (argc==1) {
        tak::sim::UnitType type;type.maxVel=tak::sim::Fixed::fromInt(5);
        tak::sim::Unit unit;unit.type=&type;unit.baseSpeed=type.maxVel;unit.speed=tak::sim::Fixed::fromInt(1);
        UnitR frame;frame.type=&type;
        for (int blocked : {0,1,2,2,0}) {
            unit.bodyBlockStreak=blocked;frame.captureMovement(unit);
            if (frame.animationSpeedPercent()!=(blocked>=2?0:20) || frame.walking()!=(blocked<2) || frame.speed!=30.0f) return 1;
        }
    }
    if (argc==1) {
        using namespace tak::sim;
        UnitType type;type.maxVel=Fixed::fromInt(5);type.roadMult=Fixed::fromInt(2);
        Unit unit;unit.type=&type;unit.baseSpeed=Fixed::fromInt(4);unit.speed=Fixed::fromInt(2);
        UnitR frame;frame.type=&type;frame.captureMovement(unit);
        if(frame.animationSpeedPercent()!=50) return 1;
        unit.groundTerrainFlags=0x800;frame.captureMovement(unit);
        if(frame.animationSpeedPercent()!=25) return 1;
        type.canFly=true;unit.groundTerrainFlags=0;unit.flightVelocity={65536,0,131072};
        frame.captureMovement(unit);
        if(frame.animationSpeedPercent()!=56) return 1;
        unit.inTransport=1;frame.captureMovement(unit);
        if(frame.animationSpeedPercent()!=0) return 1;
        std::puts("PASS: animation normalization uses individual speed, terrain and actual flight velocity");
    }
    if (argc==1) {
        tak::sim::UnitType type;type.canFly=true;type.cruiseAlt=150;
        tak::sim::Unit unit;unit.type=&type;
        UnitR frame;frame.type=&type;
        // A low flyer next to high terrain must stay at its simulated height,
        // even with an active conjure order and a much higher clearance datum.
        frame.buildSiteId=42;frame.conjuring=true;
        for (const auto [height,mode] : {std::pair{40,1},std::pair{65,2},
                                        std::pair{190,2},std::pair{55,2},std::pair{40,1}}) {
            unit.flightY=tak::sim::Fixed::fromInt(height);unit.flightGroundMode=mode;
            frame.captureMovement(unit);
            // Later sim updates cannot alter the pinned render snapshot.
            unit.flightY=tak::sim::Fixed::fromInt(511);
            for (float datum : {0.0f,100.0f,215.0f}) {
                const auto [ground,altitude]=frame.flightRenderHeight(datum);
                if (ground<0 || altitude<0 || (ground+altitude)*0.5f!=height*0.5f ||
                    frame.flightGroundMode!=mode) return 1;
            }
        }
        std::puts("PASS: flyer projection follows captured height through ascent, hover and landing");
    }
    if (argc==2 && std::strcmp(argv[1],"--quality")==0) {
        int setting,smooth;double fps;float threshold,bias,probability;unsigned seed;
        while (std::scanf("%d %d %lf %f %f %f %u",&setting,&smooth,&fps,&threshold,&bias,&probability,&seed)==7) {
            tak::RetailVisualQuality state{smooth,probability};unsigned calls=0,origin=0;
            const bool chosen=state.choose(setting,fps,threshold,bias,[&](uint32_t caller) {
                ++calls;origin=caller;seed=seed*214013u+2531011u;return (seed>>16)&32767u;
            });
            std::printf("%d %u %u %u %u %d\n",state.smoothedFps,
                std::bit_cast<uint32_t>(state.probability),seed,calls,origin,int(chosen));
        }
        return 0;
    }
    {
        using tak::sim::RetailAirCollisionBody;
        std::vector<RetailAirCollisionBody> bodies;
        for(int id=1;id<=8;++id)bodies.push_back({id,2,2,1,1,true});
        unsigned draws=0;
        const auto grid=tak::sim::retailAirCollisionGrid(8,8,bodies,[&](unsigned n){++draws;return n-1;});
        if(grid[2*8+2]!=-1 || draws!=6)return 1;
        bodies={{1,7,2,1,1,true},{2,2,2,1,1,false}};
        const auto empty=tak::sim::retailAirCollisionGrid(8,8,bodies,[](unsigned){return 0;});
        if(std::any_of(empty.begin(),empty.end(),[](int cell){return cell!=0;}))return 1;
        // The persistent grid (clear every live flyer's recorded footprint,
        // then insert all) must equal a fresh build while every body stays live.
        tak::sim::RetailAirOccupancy persistent;
        std::vector<tak::sim::RetailAirOccupant> states;
        unsigned state=520640;
        auto next=[&] {state=state*214013u+2531011u;return state;};
        for(int frame=0;frame<200;++frame) {
            const int width=frame%40<20 ? 8 : 12,height=frame%40<20 ? 12 : 8;
            if(frame%20==0) {
                persistent.reset(width,height);
                const int count=frame%40==0 ? 0 : 1+int(next()%39);
                bodies.clear();states.assign(size_t(count),{});
                for(int i=0;i<count;++i)bodies.push_back({i*3+100,0,0,1+int(next()%4),1+int(next()%4),true});
            }
            for(auto& body:bodies) {
                body.x=frame%2 ? 2 : int(next()%16)-2;
                body.z=frame%2 ? 2 : int(next()%16)-2;
                body.airborne=next()%3!=0;
            }
            unsigned a=state,b=state;
            auto random=[](unsigned& seed,unsigned n) {
                seed=seed*214013u+2531011u;return ((seed>>16)&32767u)*n/32768u;
            };
            const auto fresh=tak::sim::retailAirCollisionGrid(width,height,bodies,[&](unsigned n){return random(a,n);});
            tak::sim::RetailAirCollisionHost host{bodies,states};
            for(const auto& body:bodies)persistent.clear(body.id,host);
            for(const auto& body:bodies)persistent.insert(body.id,host,[&](unsigned n){return random(b,n);});
            if(!std::equal(fresh.begin(),fresh.end(),persistent.cells().begin(),persistent.cells().end()) || a!=b) {
                std::fprintf(stderr,"persistent air occupancy differs after frame %d\n",frame);return 1;
            }
        }
        std::puts("PASS: airborne collision overflow, map-edge exclusion and grounded exclusion");
    }
    {
        auto parsed=tak::tdf::parseText(R"(
[effect] {
 [emitters] {
  [line lightning] { x=11; }
  [spray] { x=22; }
  [LINE LIGHTNING] { x=33; }
 }
}
[EFFECT] { label=last; }
)");
        // Copies must own both the overwritten definitions and the last one.
        const auto copy=parsed;parsed={};
        const auto sections=copy.orderedChildren();
        if(sections.size()!=2 || copy.childOrder.size()!=1 ||
           copy.child("effect")->valueOr("label","")!="last")return 1;
        const auto* emitters=sections[0].second->child("emitters");
        if(!emitters)return 1;
        const auto entries=emitters->orderedChildren();
        if(entries.size()!=3 || entries[0].first!="line lightning" || entries[1].first!="spray" ||
           entries[2].first!="line lightning" || entries[0].second->numberOr("x",0)!=11 ||
           entries[1].second->numberOr("x",0)!=22 || entries[2].second->numberOr("x",0)!=33 ||
           emitters->child("line lightning")->numberOr("x",0)!=33 || emitters->childOrder.size()!=2)return 1;
        std::puts("PASS: repeated effect sections preserve order, independent values and legacy named lookup");
    }
    {
        const auto root=tak::tdf::parseText(R"(
[effect] {
 width=8;
 height=8;
 maxparticles=3;
 startintensity=4;
 fadespeed=-0.5;
 [palette] {
  [ramp] {
   startindex=0;
   endindex=2;
   startcolor=128 0 0;
   endcolor=160 0 0;
  }
  [ramp] {
   startindex=2;
   endindex=4;
   startcolor=0 0 20;
   endcolor=0 0 40;
  }
 }
 [emitters] {
  [line lightning] {
   x=1;
   y=3;
   x1=6;
   y1=3;
   updaterate=1;
  }
  [line lightning] {
   x=2;
   y=4;
   x1=5;
   y1=4;
   updaterate=5;
  }
 }
}
)");
        const auto definition=tak::retailLightningDefinition(*root.child("effect"));
        auto effect=definition.initial;
        if(effect.sources.size()!=2 || effect.sources[0].position[0]!=256 || effect.sources[1].position[0]!=512 ||
           effect.capacity!=3 || effect.intensity!=1024 || effect.decay!=-128 ||
           definition.palette[1]!=0x3900 || definition.palette[2]!=0x7001 || definition.palette[4]!=0xf002)return 1;
        if(!effect.tick(true,[]{return 16384u;}) || effect.particles.size()!=2 ||
           effect.sources[0].countdown!=1 || effect.sources[1].countdown!=5)return 1;
        if(!definition.initial.particles.empty() || definition.initial.sources[1].countdown!=0)return 1;
        std::puts("PASS: named-effect loading retains distinct sources, overlapping ramps and fresh clone clocks");
    }
    {
        const auto quad=tak::retailLightningQuad({10,20},{110,20},640,480,256,31,256,32);
        if(!quad || (*quad)[0].x!=10 || (*quad)[0].y!=5 || (*quad)[1].x!=110 ||
           (*quad)[1].y!=5 || (*quad)[2].y!=35 || (*quad)[3].y!=35 ||
           (*quad)[0].v!=31.f/32.f || (*quad)[1].u!=1)return 1;
        if(tak::retailLightningQuad({-600,20},{-600,120},640,480,256,32,256,32))return 1;
        std::puts("PASS: lightning quad uses authored thickness, padded UVs and native clipping rejection");
    }
    {
        tak::RetailEffectVisibility view(16,16,
            std::vector<tak::sim::RetailExplorationHeight>(256,{0,0}));
        tak::sim::RetailSightFootprint sight{4,4,0,16,1,true};
        const std::array<int32_t,3> center{128*65536,0,128*65536};
        view.track(1,0,0,true,true,sight,1);
        view.track(1,0,0,true,true,sight,2);
        if(view.counts()[4*16+4]!=1 || !view.visible(center))return 1;
        view.track(2,1,0,true,true,sight,2);
        view.track(1,0,0,false,true,sight,3); // boarding removes immediately
        if(view.counts()[4*16+4]!=1 || view.retainedCount()!=0)return 1;
        view.track(2,1,0,false,false,sight,3); // allied death has no local delay
        if(view.visible(center))return 1;
        view.track(1,0,0,true,true,sight,4); // unload reactivates cached geometry
        view.track(1,0,0,false,false,sight,5);
        view.track(1,0,0,false,false,sight,6); // repeated dead snapshots do not extend
        view.expire(65);
        if(!view.visible(center) || view.retainedCount()!=1)return 1;
        view.expire(66);
        if(view.visible(center) || view.retainedCount()!=0)return 1;
        view.track(3,0,0,true,true,sight,67);
        sight.x=10;
        view.track(3,0,0,true,true,sight,68);
        if(view.visible(center) || !view.visible({320*65536,0,128*65536}))return 1;
        view.track(3,1,0,false,true,sight,69); // ownership/alliance loss
        if(view.visible({320*65536,0,128*65536}))return 1;
        std::puts("PASS: display sight follows movement, boarding, unloading, ownership and local/allied death");
    }
    // Environmental collision precedence matters: feature contact cannot bounce,
    // units-only ignores everything, and the water boundary is exclusive.
    auto environment=[](int y,unsigned flags,int feature,bool pass=false) {
        int32_t speed=-19;
        const bool hit=tak::sim::retailProjectileHitsEnvironment(y,speed,flags,10,20,
            feature<0 ? std::optional<uint8_t>{} : std::optional<uint8_t>{uint8_t(feature)},pass);
        return std::pair{hit,speed};
    };
    if(environment(15*65536,0x1000,5)!=std::pair{true,-19} ||
       environment(10*65536,0x1000,-1)!=std::pair{false,4} ||
       environment(10*65536,0x800,50)!=std::pair{false,-19} ||
       environment(20*65536,0,-1)!=std::pair{false,-19} ||
       environment(20*65536-1,0,-1)!=std::pair{true,-19} ||
       environment(19*65536,0x2000,-1)!=std::pair{false,-19} ||
       environment(19*65536,0,-1,true)!=std::pair{false,-19} ||
       environment(10*65536,0x2000,-1,true)!=std::pair{true,-19})return 1;
    std::puts("PASS: projectile feature, ground, bounce and water collision precedence");
    const auto unobstructed=tak::retailFlameScan({0,0,0},{16*65536,0,0},100,16*65536,2,[](auto&){return 0;});
    if(unobstructed.endpoint[0]!=96*65536 || unobstructed.lifetime!=4)return 1;
    const auto blocked=tak::retailFlameScan({0,0,0},{16*65536,0,0},100,16*65536,2,[](auto& point){
        if(point[0]<40*65536)return 0;
        point[0]=40*65536;return 2;
    });
    if(blocked.endpoint[0]!=40*65536 || blocked.lifetime!=2)return 1;
    std::puts("PASS: flame pre-scan preserves clipped endpoint and native flight duration");
    tak::RetailFlameParticle flame{{0,0,0},{65536,-32768,0},3,3,0};
    if(flame.frame(12)!=0 || !flame.tick() || flame.position[0]!=65536 || flame.frame(12)!=4)return 1;
    if(!flame.tick() || flame.frame(12)!=8 || flame.tick() || flame.position[0]!=131072)return 1;
    std::puts("PASS: flame particles advance, select lifetime-scaled frames and expire before final motion");
    // 0x5ad930 judges the turn by acos of a FLOAT cosine. Three native updates
    // (tools/re/probe_guided_projectile.py): a target a few raw units off dead
    // ahead whose cosine rounds to 1 (no turn: velocity kept), one exactly ahead
    // off every axis whose cosine is one float step under 1, and one on the Y
    // axis whose float dot rounds below |a||b| (both snap to steps*speed). The
    // true angle (atan2 of cross and dot) turned the first and kept the others.
    // The last two are a zero turn rate's turn by 0.
    struct GuidedRow {
        std::array<int32_t,3> position,velocity;std::array<uint16_t,3> angles;
        std::array<int32_t,3> target;float turn;int32_t speed;uint32_t steps;
        std::array<uint16_t,3> spin;bool updateAngles;
        std::array<int32_t,3> wantPosition,wantVelocity;std::array<uint16_t,3> wantAngles;
    };
    const GuidedRow guidedRows[]={
        {{-15925248,34734080,33030144},{2128713,3633724,-3757669},{0,27390,7263},
         {-11667821,42001529,25514807},0.2f,5644191,4,{3,65535,65530},false,
         {-7410396,49268976,17999468},{2128713,3633724,-3757669},{12,27386,7239}},
        {{47251456,190709760,41943040},{6767941,5188678,834962},{0,15176,6753},
         {74323220,211464472,45282888},0.002f,8569687,4,{8,7,8},false,
         {155549548,273737068,55303792},{27074523,20756827,3340188},{32,15204,6785}},
        {{-170393600,-16842752,108331008},{0,-1159817,0},{0,0,49152},
         {-170393600,-21482020,108331008},1.2f,1159817,2,{4,8,65531},true,
         {-170393600,-21482020,108331008},{0,-2319634,0},{8,16,49142}},
        // Turn rate 0 (a Lightning Ball has none) with the target exactly in
        // line in float: native still turns by 0 onto a zero axis, and the
        // shot stops. World skipped the turn and flew on.
        {{47251456,190709760,41943040},{6767941,5188678,834962},{0,15176,6753},
         {54019397,195898438,42778002},0.0f,8569687,4,{65529,8,7},false,
         {47251456,190709760,41943040},{0,0,0},{65508,15208,6781}},
        {{35436388,-45555584,61307816},{-870556,-401280,424872},{65530,53968,61395},
         {-3738633,-63613185,80427056},0.0f,1048576,1,{65530,6,65529},false,
         {35436388,-45555584,61307816},{0,0,0},{65524,53974,61388}},
    };
    for(const auto& row:guidedRows) {
        tak::sim::RetailGuidedShot shot{row.position,row.velocity,row.angles};
        tak::sim::retailGuidedTick(shot,row.target,row.turn,row.speed,row.steps,row.spin,
            row.updateAngles,[](const auto&,int32_t&) {return 0;});
        if(shot.position!=row.wantPosition || shot.velocity!=row.wantVelocity || shot.angles!=row.wantAngles) {
            std::printf("FAIL: guided update near dead ahead: velocity %d %d %d, native %d %d %d\n",
                shot.velocity[0],shot.velocity[1],shot.velocity[2],
                row.wantVelocity[0],row.wantVelocity[1],row.wantVelocity[2]);
            return 1;
        }
    }
    std::puts("PASS: guided steering judges a near-ahead target by the native float cosine");
    tak::RetailVisualQuality state;unsigned calls=0;
    for (int i=0;i<24;++i) state.choose(101,0.824,40,5,[&](uint32_t) { ++calls;return 12345u; });
    if (state.smoothedFps!=36 || calls!=5) return 1;
    std::puts("PASS: adaptive visual refresh crosses the random-draw threshold per visible unit");
}
