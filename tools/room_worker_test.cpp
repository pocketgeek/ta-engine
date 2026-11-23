#include "server/roomworker.h"
#include "server/roomtick.h"
#include "ai/ai.h"
#include "sim/matchsetup.h"
#include <chrono>
#include <cstdio>
#include <limits>
using namespace tak;
using namespace std::chrono_literals;
int main() {
    int failures=0;
    auto check=[&](bool ok,const char* label) {if(!ok){std::fprintf(stderr,"FAIL: %s\n",label);++failures;}};
    sim::UnitType type;type.id="fighter";type.maxHp=100;type.canMove=true;
    type.maxVel=sim::Fixed::fromInt(2);type.footX=type.footZ=2;
    type.turnRate=type.turnInPlaceRate=1000;
    sim::TypeRegistry registry;
    sim::World serial,async,client;
    for(auto* w:{&serial,&async,&client}) {
        w->setSerialThreads(true);w->setPlayerCount(2);
        w->setTeam(0,0);w->setTeam(1,1);
        w->setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
        w->spawn(&type,100,100,0,0);w->spawn(&type,600,600,0,1);
    }
    std::vector<ai::Controller> baselineAi,workerAi;
    ai::Profile profile;
    baselineAi.emplace_back(1,registry,profile,123,ai::Difficulty::Normal,std::vector<std::pair<float,float>>{{100,100}});
    workerAi.emplace_back(1,registry,profile,123,ai::Difficulty::Normal,std::vector<std::pair<float,float>>{{100,100}});
    srv::RoomWorker worker,slow;
    std::promise<void> release,entered;auto gate=release.get_future().share();
    auto blocked=slow.submit([&]{entered.set_value();gate.wait();return 7;});
    entered.get_future().wait();
    auto independent=worker.submit([]{return 42;});
    bool ready=independent.wait_for(2s)==std::future_status::ready;
    check(ready,"another room finishes while a room is blocked");
    check(blocked.wait_for(0s)==std::future_status::timeout,"slow room remains blocked");
    release.set_value();check(blocked.get()==7,"slow room completes after release");
    check(independent.get()==42,"independent room result");
    for(uint32_t tick=0;tick<180;++tick) {
        srv::TickInput in;in.tick=tick;in.simThreads=1;
        in.replayBudget=std::numeric_limits<uint64_t>::max();
        if(tick%20==0) {
            net::Command c;c.kind=net::Cmd::Move;c.player=0;c.unitId=1;c.x=200+float(tick);c.z=200;
            in.commands.push_back(c);c.z=300;in.scheduled.push_back(c);
        }
        if(tick==160)in.events.push_back({net::Event::Kind::Leave,1});
        auto future=worker.submit([&,in]{return srv::simulateRoomTick(async,registry,workerAi,in);});
        auto expected=srv::simulateRoomTick(serial,registry,baselineAi,in);
        auto result=future.get();
        check(result.bundle==expected.bundle && result.hash==expected.hash,"parallel and serial ticks agree");
        net::Reader reader(result.bundle.data(),result.bundle.size());
        check(reader.u32()==tick,"wire tick order");auto count=reader.u32();
        for(uint32_t i=0;i<count;++i)sim::applyCommand(client,registry,reader.cmd());
        count=reader.u32();for(uint32_t i=0;i<count;++i) {
            net::Event event{net::Event::Kind(reader.u8()),reader.u8()};sim::applyEvent(client,event);
        }
        check(reader.ok && reader.p==reader.end,"complete tick bundle");
        client.tick(1.0f/net::kServerHz);
        check(async.stateHash()==serial.stateHash() && async.stateHash()==client.stateHash(),"worker, baseline and wire client remain deterministic");
    }
    // A hash nobody reads (no human slot, no replay trail) is skipped; the
    // trail alone still gets its hash and posDigest every 10th period.
    for(uint32_t tick=180;tick<=300;++tick) {
        srv::TickInput none;none.tick=tick;none.simThreads=1;
        none.replayBudget=std::numeric_limits<uint64_t>::max();
        srv::TickInput trail=none;trail.clientHashes=false;
        none.clientHashes=none.replayTrail=false;
        auto skipped=srv::simulateRoomTick(async,registry,workerAi,none);
        auto trailed=srv::simulateRoomTick(serial,registry,baselineAi,trail);
        const bool trailTick=tick%(10*uint32_t(net::kHashPeriod))==0;
        check(!skipped.hash && !skipped.posDigest,"an unread hash is skipped");
        check(trailed.hash.has_value()==trailTick && (trailed.posDigest!=0)==trailTick,"the replay trail keeps its checkpoints");
        if(trailTick)check(*trailed.hash==serial.stateHash() && trailed.posDigest==serial.posDigest(),"trail checkpoint is the world's");
        check(skipped.bundle==trailed.bundle && async.stateHash()==serial.stateHash(),"skipping the hash changes nothing else");
    }
    auto before=async.stateHash();srv::TickInput exhausted;exhausted.simThreads=1;
    auto limited=worker.submit([&]{return srv::simulateRoomTick(async,registry,workerAi,exhausted);}).get();
    check(limited.resourceLimited && before==async.stateHash(),"replay limit prevents simulation advancement");
    auto failed=worker.submit([]()->int{throw std::runtime_error("test failure");});
    bool caught=false;try{(void)failed.get();}catch(const std::runtime_error&){caught=true;}
    check(caught,"worker exception reaches owner");
    check(worker.submit([]{return 9;}).get()==9,"worker survives failed job");
    std::printf("room worker: %d failures\n",failures);return failures?1:0;
}
