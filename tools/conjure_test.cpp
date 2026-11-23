// conjure_test -- the Zhon monarch's (zonhunt) conjure animation, driven through the
// real COB VM the way the client does. The conjure gesture (RestoreWatcher looping
// `build`) and the airborne body pose (FlightControl -> `attack`) both gate on the
// ACTIVE static (6), which the client sets via setSFXoccupy(5) while the flyer works.
// This pins that the arms actually move -- and that they FREEZE if the flyer is not
// kept active, which is the failure the engine's altitude `busy` test guards against
// (traced with tools/re/emuphase.py + cobtool; verified live driving the Vm).
//
//   conjure_test <retail-install-dir>
#include "hpi/hpi.h"
#include "cob/cob.h"
#include "cob/vm.h"
#include "sim/matchsetup.h"
#include "sim/retailanimationqueries.h"
#include <array>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace tak;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
}

static void checkStopHover(sim::World& world, const sim::TypeRegistry& registry, int id) {
    net::Command command{};
    command.kind=net::Cmd::Stop;command.player=0;command.unitId=id;
    sim::applyCommand(world,registry,command);
    for (int tick=0;tick<30;++tick) world.tick(1.0f/30.0f);
    const auto* builder=world.unit(id);
    check(builder && !builder->buildSiteId && !builder->productionSiteId &&
          !builder->conjureHoverTarget && !builder->conjureHoverGoal,
          "Stop detaches the conjurer from its site and clears the hover controller");
    // A stopped flyer may install an ordinary landing order.
    check(builder && !builder->repeatType && builder->buildQueue.empty() &&
          std::none_of(builder->orders.begin(),builder->orders.end(),
                       [](const sim::Order& order) { return order.buildType!=nullptr; }),
          "stopped conjurer does not restart the canceled job");
}

// Does any arm/hand piece animate over `frames` ticks?
static bool armMoves(cob::Vm& vm, int frames) {
    const cob::File& f = vm.file();
    static const std::set<std::string> arms =
        {"armup_L", "armup_R", "armlow_L", "armlow_R", "hand_R", "hand_L"};
    for (int i = 0; i < frames; ++i) {
        vm.tick(1.0f / 30.0f);
        for (size_t p = 0; p < vm.pieces().size() && p < f.pieces.size(); ++p) {
            if (!arms.count(f.pieces[p])) continue;
            const auto& ps = vm.pieces()[p];
            for (int a = 0; a < 3; ++a)
                if (ps.moving[a] || ps.turning[a] || ps.spin[a] != 0.0f) return true;
        }
    }
    return false;
}

static std::shared_ptr<const cob::File> load(const hpi::Vfs& vfs) {
    return std::make_shared<const cob::File>(
        cob::load(vfs.read("scripts/zonhunt.cob"), "zonhunt.cob"));
}

// Paired with tools/re/check_zhon_construction_flight_trace.py. This mode takes
// the exact native 41ef00 orbit point and initial mover state, then runs the
// asset-backed Zhon Monarch through World flight movement on the same flat
// terrain. The separate under-construction site is represented by its fixed
// center so the trace can report Monarch-to-site X/Z alongside absolute Y.
static int constructionFlightTrace(int argc,char** argv) {
    if(argc!=15) {
        std::fprintf(stderr,"usage: conjure_test --construction-flight-trace <install> <steps> <terrain> <start-x> <start-y> <start-z> <start-heading> <goal-x> <goal-z> <goal-heading> <site-x> <site-z> <speed>\n");
        return 2;
    }
    const auto number=[](const char* text) { return int32_t(std::strtol(text,nullptr,0)); };
    const int steps=std::clamp(number(argv[3]),1,10000);
    const int terrain=std::clamp(number(argv[4]),0,255);
    const int32_t startX=number(argv[5]),startY=number(argv[6]),startZ=number(argv[7]);
    const uint16_t startHeading=uint16_t(number(argv[8]));
    const int32_t goalX=number(argv[9]),goalZ=number(argv[10]);
    const uint16_t goalHeading=uint16_t(number(argv[11]));
    const int32_t siteX=number(argv[12]),siteZ=number(argv[13]);
    const int32_t speed=number(argv[14]);

    hpi::Vfs vfs=hpi::mountRetailRoot(argv[2]);
    sim::TypeRegistry registry;sim::setupRegistry(registry,vfs,false);
    const auto* monarch=registry.find("zonhunt");
    if(!monarch || !monarch->canFly) return 2;
    sim::World world;
    world.setTerrain(std::vector<uint8_t>(128*128,uint8_t(terrain)),128,128,20);
    world.setPathService(false);
    const int id=world.spawn(monarch,float(startX)/65536.0f,float(startZ)/65536.0f,0,0);
    auto* unit=world.unit(id);
    unit->x=sim::Fixed::raw(startX);unit->z=sim::Fixed::raw(startZ);
    unit->flightY=sim::Fixed::raw(startY);unit->baseSpeed=sim::Fixed::raw(speed);
    unit->speed=sim::Fixed();unit->flightVelocity={};
    unit->heading=sim::retailHeadingToPort(startHeading);
    unit->flightNavigation={{startX,startY,startZ},{},startHeading};
    unit->flightSectorX=startX>>23;unit->flightSectorZ=startZ>>23;
    sim::Order order;
    order.x=sim::Fixed::raw(goalX);order.z=sim::Fixed::raw(goalZ);order.goal=true;
    order.flightGoal=sim::RetailFlightGoal{{goalX,0,goalZ},0x60,goalHeading,0};
    unit->orders.push_back(order);

    std::printf("PROFILE %d %d %d %d %d %d %d\n",monarch->maxVel.v,
        monarch->accel.v,monarch->brake.v,monarch->roadMult.v,monarch->turnRate,
        int(monarch->cruiseAlt),monarch->buildDist);
    for(int tick=1;tick<=steps;++tick) {
        world.tick(1.0f/30.0f);
        unit=world.unit(id);
        const auto& navigation=unit->flightNavigation;
        const auto& velocity=unit->flightVelocity;
        std::printf("%d %d %d %d %u %d %d %d %d %d %d %d %d %d %d %u %d %d\n",
            tick,unit->x.v,unit->flightY.v,unit->z.v,
            unsigned(sim::portHeadingToRetail(unit->heading)),unit->speed.v,
            velocity.x,velocity.y,velocity.z,navigation.destination.x,
            navigation.destination.y,navigation.destination.z,navigation.velocity.x,
            navigation.velocity.y,navigation.velocity.z,unsigned(navigation.heading),
            int32_t(uint32_t(unit->x.v)-uint32_t(siteX)),
            int32_t(uint32_t(unit->z.v)-uint32_t(siteZ)));
    }
    return 0;
}

// Paired with check_zhon_construction_flight_trace.py's persistent dispatcher
// pass. Unlike --construction-flight-trace, this starts an unfinished placed
// site inside build range and leaves the builder's build job active. The unit
// scripts are disabled in these two fixture types so this isolated trace owns
// the shared game RNG exactly as native 41ef00 does: one rand(50) mission test
// per tick, plus the four draws when it creates a new orbit point.
static int persistentConstructionFlightTrace(int argc,char** argv) {
    if(argc!=13) {
        std::fprintf(stderr,"usage: conjure_test --persistent-construction-flight-trace <install> <steps> <terrain> <start-x> <start-y> <start-z> <start-heading> <site-x> <site-z> <speed> <seed>\n");
        return 2;
    }
    const auto number=[](const char* text) { return int32_t(std::strtol(text,nullptr,0)); };
    const int steps=std::clamp(number(argv[3]),1,10000);
    const int terrain=std::clamp(number(argv[4]),0,255);
    const int32_t startX=number(argv[5]),startY=number(argv[6]),startZ=number(argv[7]);
    const uint16_t startHeading=uint16_t(number(argv[8]));
    const int32_t siteX=number(argv[9]),siteZ=number(argv[10]);
    const int32_t speed=number(argv[11]);
    const uint32_t seed=uint32_t(number(argv[12]));
    hpi::Vfs vfs=hpi::mountRetailRoot(argv[2]);
    sim::TypeRegistry registry;sim::setupRegistry(registry,vfs,false);
    const auto* monarch=registry.find("zonhunt");const auto* output=registry.find("zonter");
    if(!monarch || !output || !monarch->canFly) return 2;

    sim::UnitType builderType=*monarch;
    builderType.simulationScript.reset();builderType.productionScript.reset();
    sim::UnitType siteType=*output;
    siteType.buildTime=10000;
    siteType.simulationScript.reset();siteType.productionScript.reset();
    sim::World world;
    world.setTerrain(std::vector<uint8_t>(128*128,uint8_t(terrain)),128,128,20);
    world.setPathService(false);
    const int builderId=world.spawn(&builderType,float(startX)/65536.0f,
                                    float(startZ)/65536.0f,0,0);
    const int siteId=world.spawn(&siteType,float(siteX)/65536.0f,
                                float(siteZ)/65536.0f,0,0);
    auto* builder=world.unit(builderId);auto* site=world.unit(siteId);
    builder->x=sim::Fixed::raw(startX);builder->z=sim::Fixed::raw(startZ);
    builder->flightY=sim::Fixed::raw(startY);builder->baseSpeed=sim::Fixed::raw(speed);
    builder->speed=sim::Fixed();builder->flightVelocity={};
    builder->heading=sim::retailHeadingToPort(startHeading);
    builder->flightNavigation={{startX,startY,startZ},{},startHeading};
    builder->flightSectorX=startX>>23;builder->flightSectorZ=startZ>>23;
    site->x=site->homeX=sim::Fixed::raw(siteX);
    site->z=site->homeZ=sim::Fixed::raw(siteZ);
    site->underConstruction=site->buildBegun=site->beingBuilt=true;
    site->hp=sim::Fixed::fromFloat(siteType.maxHp*0.05f);
    builder->buildSiteId=siteId;
    sim::Order activeBuild;activeBuild.buildType=&siteType;activeBuild.goal=true;
    builder->orders.push_back(activeBuild);
    world.setGameSeed(seed);
    std::vector<sim::World::RngObservation> rngEvents;
    world.setRngObserver([&](const sim::World::RngObservation& observation) {
        rngEvents.push_back(observation);
    });

    std::printf("PROFILE %d %d %d %d %d %d %d\n",monarch->maxVel.v,
        monarch->accel.v,monarch->brake.v,monarch->roadMult.v,monarch->turnRate,
        int(monarch->cruiseAlt),monarch->buildDist);
    uint32_t previousAnimationOccupancy=0;
    for(int tick=1;tick<=steps;++tick) {
        rngEvents.clear();
        world.tick(1.0f/30.0f);
        builder=world.unit(builderId);site=world.unit(siteId);
        const auto& navigation=builder->flightNavigation;
        const auto& velocity=builder->flightVelocity;
        const auto goal=builder->conjureHoverGoal;
        const uint32_t rngState=rngEvents.empty()?0:rngEvents.back().seedAfter;
        const uint32_t animationOccupancy=sim::retailAnimationOccupancy(
            previousAnimationOccupancy,builder->flightGroundMode,
            int16_t((builder->type->canFly ? builder->flightY : builder->groundY).floorInt()),
            uint8_t(world.mapSea()),uint8_t(builder->type->waterline),
            int16_t(builder->type->modelTop>>16));
        const bool occupancyChanged=animationOccupancy!=previousAnimationOccupancy;
        previousAnimationOccupancy=animationOccupancy;
        std::printf("TRACE %d %d %d %d %u %d %d %d %d %d %d %d %d %d %d %u %d %d %d %d %d %u %u %d %d %d %u %zu %zu %d %u %d\n",
            tick,builder->x.v,builder->flightY.v,builder->z.v,
            unsigned(sim::portHeadingToRetail(builder->heading)),builder->speed.v,
            velocity.x,velocity.y,velocity.z,navigation.destination.x,
            navigation.destination.y,navigation.destination.z,navigation.velocity.x,
            navigation.velocity.y,navigation.velocity.z,unsigned(navigation.heading),
            int32_t(uint32_t(builder->x.v)-uint32_t(siteX)),
            int32_t(uint32_t(builder->z.v)-uint32_t(siteZ)),
            goal?goal->point.x:0,goal?goal->point.y:0,goal?goal->point.z:0,
            unsigned(goal?goal->flags:0),unsigned(goal?goal->heading:0),
            site->underConstruction?1:0,builder->buildSiteId,
            builder->conjureHoverTarget,rngState,rngEvents.size(),
            builder->orders.size(),!builder->orders.empty() &&
                builder->orders.front().buildType==&siteType,
            unsigned(animationOccupancy),int(occupancyChanged));
        for(const auto& event:rngEvents)
            std::printf("RNG %u %d %u %u %u\n",event.tick,event.bound,
                event.seedBefore,event.seedAfter,event.result);
    }
    return 0;
}

int main(int argc, char** argv) {
    if(argc>1 && std::string(argv[1])=="--construction-flight-trace")
        return constructionFlightTrace(argc,argv);
    if(argc>1 && std::string(argv[1])=="--persistent-construction-flight-trace")
        return persistentConstructionFlightTrace(argc,argv);
    if (argc < 2) { std::printf("usage: conjure_test <install>\n"); return 2; }
    hpi::Vfs vfs = hpi::mountRetailRoot(argv[1]);
    auto f = load(vfs);
    {
        const sim::RetailFlightVector builder{sim::Fixed::fromInt(1000).v,0,
                                                sim::Fixed::fromInt(1000).v};
        const sim::RetailFlightVector site{sim::Fixed::fromInt(1120).v,0,
                                            sim::Fixed::fromInt(1060).v};
        // Captured from native 41ef00 with seed 50 and builddistance 100.
        const std::array<int,4> values{7132,5832,7,2};
        const std::array<int,4> bounds{0x2492,0x2492,8,8};
        std::array<int,4> consumed{};size_t index=0;
        const auto goal=sim::retailConstructionHoverGoal(builder,site,100,[&](int bound) {
            if (index>=values.size()) return 0;
            consumed[index]=bound;
            return values[index++];
        });
        check(index==values.size() && consumed==bounds,
              "placed conjure orbit consumes the native seed-50 draws in order");
        check(goal.point.x==68223960 && goal.point.y==0 && goal.point.z==66009400 &&
              goal.flags==0x60 && goal.heading==43016,
              "placed conjure orbit matches the native seed-50 goal and heading");
    }
    for (bool crusades : {false,true}) {
        sim::TypeRegistry registry;sim::setupRegistry(registry,vfs,crusades);
        {
            sim::World placement;
            placement.setTerrain(std::vector<uint8_t>(128*128,100),128,128,20);
            const auto* barracks=registry.find("arakeep");
            placement.spawn(registry.find("araking"),720,800,0,0);
            check(placement.canPlace(barracks,800,800),
                  "builder beside narrow barracks side does not obstruct placement");
            placement.spawn(registry.find("arasword"),800,800,0,0);
            check(!placement.canPlace(barracks,800,800),
                  "unit inside barracks footprint still blocks placement");
        }
        // A rectangular barracks must remain placeable when its builder reaches
        // a short side. A circular exclusion based on its long side rejects him.
        for (const auto [dx,dz]:{std::pair{-160,0},std::pair{160,0},
                                std::pair{0,-192},std::pair{0,192}}) {
            sim::World placement;
            placement.setTerrain(std::vector<uint8_t>(128*128,100),128,128,20);
            const auto* barracks=registry.find("arakeep");
            auto builderType=*registry.find("araking");
            builderType.income=0; // Isolate work admission from monarch income.
            const int builder=placement.spawn(&builderType,800+dx,800+dz,0,0);
            placement.queueBuild(builder,barracks,800,800,false);
            check(!placement.unit(builder)->orders.empty(),"barracks construction order accepted");
            bool started=false;
            for (int tick=0;tick<300 && !started;++tick) {
                placement.tick(1.0f/30.0f);
                started=placement.unit(builder)->buildSiteId!=0;
            }
            std::printf("barracks approach balance=%d offset=%d,%d\n",int(crusades),dx,dz);
            check(started,"ground builder starts rectangular barracks from each side");
            if (started) {
                placement.player(0).mana=10000;
                for (int tick=0;tick<60;++tick) placement.tick(1.0f/30.0f);
                auto* worker=placement.unit(builder);
                check(worker->constructionEmissions[0]>0,"successful placed construction emits worker events");
                const auto* site=placement.unit(worker->buildSiteId);
                check(site && site->constructionEmissions[1]>0,"successful placed construction emits site events");
                const auto hash=placement.stateHash();
                ++worker->constructionEmissions[0];
                check(placement.stateHash()==hash,"cosmetic construction events do not change lockstep hash");
                check(worker->cosmeticConstructionEmitter.has_value(),"headless work creates cosmetic particle state");
                const auto savedEmitter=worker->cosmeticConstructionEmitter;
                const auto savedRandom=worker->constructionVisualRandom;
                worker->cosmeticConstructionEmitter.reset();
                worker->constructionVisualRandom^=0x12345678u;
                check(placement.stateHash()==hash,"particle contents and cosmetic RNG do not change lockstep hash");
                worker->cosmeticConstructionEmitter=savedEmitter;
                worker->constructionVisualRandom=savedRandom;
                if (site) {
                    const int siteId=site->id;
                    const auto workerBefore=worker->constructionEmissions;
                    const auto siteBefore=site->constructionEmissions;
                    auto expectedParticles=worker->cosmeticConstructionEmitter;
                    if (expectedParticles) expectedParticles->advance();
                    placement.player(0).mana=0;
                    placement.tick(1.0f/30.0f);
                    check(placement.unit(builder)->constructionEmissions==workerBefore &&
                          placement.unit(siteId)->constructionEmissions==siteBefore,
                          "mana-starved construction emits no work particles");
                    const auto& actualParticles=placement.unit(builder)->cosmeticConstructionEmitter;
                    bool matching=expectedParticles && actualParticles &&
                        expectedParticles->particles.size()==actualParticles->particles.size();
                    if (matching) for (size_t i=0;i<expectedParticles->particles.size();++i) {
                        const auto& a=expectedParticles->particles[i];
                        const auto& b=actualParticles->particles[i];
                        matching=matching && a.x==b.x && a.y==b.y && a.z==b.z &&
                            a.speed==b.speed && a.ceiling==b.ceiling && a.displayAge==b.displayAge;
                    }
                    check(matching,"particles age on a headless tick without rendering or new emissions");
                    placement.player(0).mana=10000;
                    auto* finishing=placement.unit(siteId);
                    finishing->hp=sim::Fixed::fromFloat(finishing->type->maxHp)-sim::Fixed::raw(1);
                    placement.tick(1.0f/30.0f);
                    check(!placement.unit(siteId)->underConstruction &&
                          placement.unit(builder)->constructionEmissions[0]==workerBefore[0]+1 &&
                          placement.unit(siteId)->constructionEmissions[1]==siteBefore[1]+1,
                          "completion tick emits exactly one worker and site event");
                }
            }
        }
        for (const auto& [name,type]:registry.types()) {
            const auto& menu=registry.buildable(name);
            if (type.isStructure()) continue;
            // Repeat production sets its output down beside the producer, so it
            // takes a mobile type; a structure on the menu is placed (Build).
            const sim::UnitType* output=nullptr;
            for (const auto& entry:menu)
                if (const auto* t=registry.find(entry);t && !t->isStructure()) { output=t;break; }
            if (!output) continue;
            sim::World world;
            world.setTerrain(std::vector<uint8_t>(128*128,100),128,128,20);
            const int id=world.spawn(&type,800,800,0,0);
            const int enemy=world.spawn(registry.find("zonter"),1200,1200,0,1);
            world.order(id,1500,1500,false);
            net::Command c{};c.unitId=id;c.player=0;c.kind=net::Cmd::RepeatTrain;
            std::snprintf(c.type,sizeof(c.type),"%s",output->id.c_str());
            sim::applyCommand(world,registry,c);
            check(world.unit(id)->repeatType==output,"mobile infinite production starts through command path");
            check(world.unit(id)->orders.empty(),"infinite production retires previous movement orders");
            std::printf("infinite producer=%s balance=%d\n",name.c_str(),int(crusades));
            for (bool emptyQueue : {false,true}) {
                if (emptyQueue) world.unit(id)->buildQueue.clear();
                // Resource/output stalls and between-item gaps must remain locked.
                world.player(0).mana=0;
                const auto before=world.stateHash();
                world.unit(id)->repeatType=nullptr;
                check(world.stateHash()!=before,"infinite production lock participates in lockstep hash");
                world.unit(id)->repeatType=output;
                for (int kind=0;kind<=int(net::Cmd::SetSquad);++kind) {
                    // Stance sets the stance its output starts with (checked below).
                    c.kind=net::Cmd(kind);if (c.kind==net::Cmd::Stop || c.kind==net::Cmd::Move || c.kind==net::Cmd::Patrol ||
                                            c.kind==net::Cmd::Attack || c.kind==net::Cmd::AttackMove ||
                                            c.kind==net::Cmd::Stance) continue;
                    c.targetId=enemy;c.x=1500;c.z=1500;
                    for (int queued : {0,1}) {
                        c.queue=uint8_t(queued);sim::applyCommand(world,registry,c);
                        check(world.stateHash()==before && world.unit(id)->repeatType==output,
                              "unrelated commands stay rejected during infinite mobile production");
                    }
                }
                // Fire at a point (past SetSquad in the enum): retail's SUPPRESS would
                // end the repeat; our rally rule (C9) refuses it -- plain, Shift or Ctrl.
                c.kind=net::Cmd::AttackGround;c.targetId=0;c.x=1500;c.z=1500;
                for (uint8_t queued : {uint8_t(net::kQueueReplace),uint8_t(net::kQueueAppend|net::kQueueToggle),
                                       uint8_t(net::kQueueFront|net::kQueueToggle)}) {
                    c.queue=queued;sim::applyCommand(world,registry,c);
                    check(world.stateHash()==before && world.unit(id)->repeatType==output &&
                          world.unit(id)->orders.empty(),
                          "AttackGround is refused during infinite mobile production");
                }
            }
            {
                net::Command stance{};stance.kind=net::Cmd::Stance;stance.player=0;stance.unitId=id;stance.targetId=1;
                sim::applyCommand(world,registry,stance);
                check(world.unit(id)->outputStance==1 && world.unit(id)->repeatType==output,
                      "a stance during infinite mobile production sets its output's and keeps it producing");
                stance.targetId=sim::World::kStanceNone;sim::applyCommand(world,registry,stance);
                check(world.unit(id)->outputStance==-1 && world.unit(id)->repeatType==output,
                      "clearing it gives its output its own default again");
            }
            for (auto kind : {net::Cmd::Move, net::Cmd::Patrol, net::Cmd::Attack, net::Cmd::AttackMove}) {
                world.unit(id)->rally.clear();
                c.kind=kind;c.queue=0;c.x=1400;c.z=1400;c.targetId=enemy;c.player=1;
                sim::applyCommand(world,registry,c);
                check(world.unit(id)->rally.empty(),"another player cannot set producer rally");
                c.player=0;sim::applyCommand(world,registry,c);
                const size_t initial=world.unit(id)->rally.size();
                check(initial==(kind==net::Cmd::Patrol ? 2u : 1u),"mobile producer accepts replacement rally");
                c.queue=1;c.x=1600;sim::applyCommand(world,registry,c);
                check(world.unit(id)->rally.size()==initial+1 &&
                      world.unit(id)->rally.back().patrol==(kind==net::Cmd::Patrol) &&
                      world.unit(id)->rally.back().attackMove==(kind==net::Cmd::Patrol || kind==net::Cmd::AttackMove) &&
                      (world.unit(id)->rally.back().targetId==enemy)==(kind==net::Cmd::Attack),
                      "queued move/patrol/attack/fight-move extends producer rally");
                check(world.unit(id)->orders.empty() && world.unit(id)->repeatType==output &&
                      world.unit(id)->x==sim::Fixed::fromInt(800) && world.unit(id)->z==sim::Fixed::fromInt(800),
                      "rally commands leave the builder stationary and repeating");
            }
            c.kind=net::Cmd::Stop;c.player=1;sim::applyCommand(world,registry,c);
            check(world.unit(id)->repeatType==output,"another player cannot unlock the producer");
            c.player=0;sim::applyCommand(world,registry,c);
            check(!world.unit(id)->repeatType && world.unit(id)->buildQueue.empty(),
                  "Stop clears infinite production");
            check(world.unit(id)->rally.empty(),"Stop clears the producer's rally plan");
            // Same command batch: do not require a later tick/render snapshot to unlock.
            c.kind=net::Cmd::Move;c.queue=0;sim::applyCommand(world,registry,c);
            check(!world.unit(id)->orders.empty(),"Move immediately after Stop works normally");
            c.kind=net::Cmd::Stop;sim::applyCommand(world,registry,c);
            c.kind=net::Cmd::Train;c.targetId=1;sim::applyCommand(world,registry,c);
            c.kind=net::Cmd::Move;sim::applyCommand(world,registry,c);
            check(!world.unit(id)->orders.empty() && !world.unit(id)->repeatType,
                  "finite production does not lock mobile orders");
        }
        sim::World world;
        const int id=world.spawn(registry.find("arakeep"),800,800,0,0);
        world.setRepeat(id,registry.find("arasword"));
        net::Command c{};c.unitId=id;c.kind=net::Cmd::Move;c.x=1200;c.z=1200;
        sim::applyCommand(world,registry,c);
        check(!world.unit(id)->rally.empty(),"stationary infinite factories retain rally commands");
    }
    for (bool crusades : {false,true}) {
        sim::TypeRegistry registry;sim::setupRegistry(registry,vfs,crusades);
        const auto* monarch=registry.find("zonhunt");
        const auto* hunter=registry.find("zonter");
        if (!monarch || !hunter) return 2;
        sim::UnitType output=*hunter;output.buildTime=10000;
        for (int heading : {0,16384,32768,49152}) {
            sim::World world;
            world.setTerrain(std::vector<uint8_t>(128*128,100),128,128,20);
            world.buildNavClasses(registry);
            const int id=world.spawn(monarch,1000,1000,sim::radiansFromBam(sim::Bam(heading)),0);
            world.train(id,&output,1);
            if (heading==0) world.setRepeat(id,&output);
            for (int tick=0;tick<90;++tick) world.tick(1.0f/30.0f);
            if (heading==0) {
                const auto before=world.stateHash();
                net::Command c{};c.unitId=id;c.x=1500;c.z=1500;
                c.kind=net::Cmd::Move;sim::applyCommand(world,registry,c);
                c.kind=net::Cmd::Build;std::snprintf(c.type,sizeof(c.type),"zonlode");
                sim::applyCommand(world,registry,c);
                // The Move became the output's rally (hashed state); everything
                // else, the conjure site and hover included, must be untouched.
                check(world.unit(id)->rally.size()==1,"the Move set the output rally");
                world.unit(id)->rally.clear();
                check(world.stateHash()==before,"rejected orders preserve active conjure site and hover state");
            }
            const auto* builder=world.unit(id);
            const auto* site=world.unit(builder->productionSiteId);
            check(site && site->underConstruction,"queued Hunter remains under construction");
            if (site) {
                const auto wanted=sim::retailHeadingToPort(uint16_t(sim::retailDirection(builder->x-site->x,builder->z-site->z).v));
                check(std::abs(sim::bamDiff(wanted,builder->heading))<8192,
                      crusades ? "Crusades Zhon monarch faces her conjured unit" : "standard Zhon monarch faces her conjured unit");
                check(builder->conjureHoverTarget==site->id,
                      "queued conjurer uses the active site's hover controller");
            }
            checkStopHover(world,registry,id);
        }
    }
    for (bool crusades : {false,true}) {
        sim::TypeRegistry registry;sim::setupRegistry(registry,vfs,crusades);
        const auto* monarch=registry.find("zonhunt");
        sim::UnitType output=*registry.find("zonter");output.buildTime=10000;
        for (const char* flyerName : {"zonhunt","arafly","tarpries","tarprie2"}) {
            const auto* flyer=registry.find(flyerName);
            check(flyer && flyer->canFly && flyer->isBuilder,"shipped flying builder is registered");
            if(!flyer)continue;
            sim::World approachWorld;
            approachWorld.setTerrain(std::vector<uint8_t>(128*128,100),128,128,20);
            approachWorld.buildNavClasses(registry);
            const int worker=approachWorld.spawn(flyer,1000,1000,0,0);
            approachWorld.queueBuild(worker,registry.find("zonter"),1300,1000,false);
            approachWorld.tick(1.f/30);
            const auto* unit=approachWorld.unit(worker);
            check(unit->flightGroundMode==2 && unit->flightBeginCallbackSerial==1,
                  "each shipped flying builder starts a placed-build takeoff");
            if(!unit->orders.empty()) {
                const auto& goal=unit->orders.front();
                check(goal.flightGoal && goal.flightGoal->flags==0x30 &&
                      goal.flightGoal->radius==int(flyer->buildDist)*2 &&
                      goal.flightGoal->point.x==goal.buildX.v && goal.flightGoal->point.z==goal.buildZ.v,
                      "each flying builder approaches the site with its authored doubled radius");
            } else check(false,"flying builder retains its distant approach");
            sim::World remote;
            remote.setTerrain(std::vector<uint8_t>(128*128,100),128,128,20);
            remote.buildNavClasses(registry);
            const int remoteWorker=remote.spawn(flyer,1000,1000,0,0);
            const int remoteSite=remote.startBuild(remoteWorker,registry.find("zonter"),1600,1000);
            check(remoteSite!=0,"direct flying build accepts a distant site for approach");
            for(int tick=0;tick<5;++tick)remote.tick(1.0f/30.0f);
            check(remoteSite && !remote.unit(remoteSite)->buildBegun,
                  "direct flying build does not perform work before reaching the native admission radius");
        }
        for (const auto [dx,dz] : {std::pair{128,0},std::pair{0,128},std::pair{-128,0},std::pair{0,-128},
                                  std::pair{128,128},std::pair{-128,128},std::pair{-128,-128},std::pair{128,-128},
                                  std::pair{64,0},std::pair{0,64}}) {
            sim::World world;
            world.setTerrain(std::vector<uint8_t>(128*128,100),128,128,20);
            world.buildNavClasses(registry);
            const int id=world.spawn(monarch,1000,1000,0,0);
            world.queueBuild(id,&output,float(1000+dx),float(1000+dz),false);
            check(!world.unit(id)->orders.empty(),"placed conjure accepts the selected site");
            world.tick(1.0f/30.0f);
            const auto* takingOff=world.unit(id);
            check(takingOff->flightGroundMode==2 && takingOff->flightBeginCallbackSerial==1,
                  "placed flying construction starts takeoff before its site exists");
            check(!takingOff->buildSiteId && !takingOff->orders.empty(),
                  "first placed-build update retains the approach mission");
            check(takingOff->flightY==sim::Fixed::fromInt(101),
                  "already-satisfied flying build approach still applies the native first climb step");
            if (!takingOff->orders.empty()) {
                const auto& approach=takingOff->orders.front();
                check(approach.flightGoal && approach.flightGoal->flags==0x30 &&
                      approach.flightGoal->radius==int(monarch->buildDist)*2 &&
                      approach.flightGoal->point.x==approach.buildX.v &&
                      approach.flightGoal->point.z==approach.buildZ.v,
                      "flying pre-site approach uses the snapped site and twice builddistance");
            }
            world.tick(1.0f/30.0f);
            const auto* allocating=world.unit(id);
            check(allocating->buildSiteId!=0 && allocating->flightY==sim::Fixed::fromInt(102),
                  "site-allocation update continues the native flight body without pausing ascent");
            std::set<std::pair<int,int>> hoverPositions;
            for (int tick=2;tick<300;++tick) {
                world.tick(1.0f/30.0f);
                const auto* b=world.unit(id);
                if (tick>90 && b->buildSiteId)
                    hoverPositions.emplace(b->x.floorInt(),b->z.floorInt());
            }
            check(hoverPositions.size()>8,"placed conjurer moves around the site while working");
            const auto* builder=world.unit(id);
            const auto* site=world.unit(builder->buildSiteId);
            check(site && site->underConstruction,"placed Hunter remains under construction");
            if (site) {
                const auto wanted=sim::retailHeadingToPort(uint16_t(sim::retailDirection(builder->x-site->x,builder->z-site->z).v));
                const float distance=sim::fxLen(builder->x-site->x,builder->z-site->z).toFloat();
                check(std::abs(distance-float(monarch->buildDist))<24.0f,
                      "placed flying conjurer holds its build distance from the site");
                check(std::abs(sim::bamDiff(wanted,builder->heading))<8192,"placed conjure faces the actual site");
                check(builder->conjureHoverTarget==site->id,"hover controller stays attached to the placed site");
            }
            checkStopHover(world,registry,id);
        }
    }
    check(f->scriptIndex("RestoreWatcher") >= 0 && f->scriptIndex("FlightControl") >= 0 &&
              f->scriptIndex("build") >= 0,
          "zonhunt carries the conjure threads (RestoreWatcher/FlightControl/build)");

    std::printf("[conjure while ACTIVE -- the client's normal case]\n");
    {
        cob::Vm vm(f);
        vm.enableRetailAnimation();
        vm.start("Create");
        for (int i = 0; i < 30; ++i) vm.tick(1.0f / 30.0f);
        vm.start("setSFXoccupy", {5});   // active (static 6) -- client sets this while busy
        vm.start("BeginFlight");
        for (int i = 0; i < 30; ++i) vm.tick(1.0f / 30.0f);
        vm.start("StartBuilding");
        check(armMoves(vm, 120), "the conjure arm gesture plays (build loops via RestoreWatcher)");
        std::set<int> wingPoses, armPoses;
        for (int i=0;i<180;++i) {
            vm.tick(1.0f/30.0f);
            wingPoses.insert(int(vm.pieces()[26].rot[2]*10000));
            armPoses.insert(int(vm.pieces()[7].rot[1]*10000));
        }
        check(wingPoses.size()>8,"conjuring repeats the airborne wing flap");
        check(armPoses.size()>8,"conjuring repeats the arm gesture throughout the job");
    }

    std::printf("[conjure while NOT active -- documents the freeze the busy test prevents]\n");
    {
        cob::Vm vm(f);
        vm.enableRetailAnimation();
        vm.start("Create");
        for (int i = 0; i < 30; ++i) vm.tick(1.0f / 30.0f);
        vm.start("setSFXoccupy", {0});   // NOT active (static 6 clear)
        for (int i = 0; i < 30; ++i) vm.tick(1.0f / 30.0f);
        vm.start("StartBuilding");
        check(!armMoves(vm, 120),
              "with the flyer inactive the arms FREEZE (why the engine keeps it airborne)");
    }

    std::printf("conjure_test: %s (%d failed)\n", fails ? "FAILED" : "all passed", fails);
    return fails ? 1 : 0;
}
