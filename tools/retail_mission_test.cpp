#include "sim/retailmission.h"
#include "sim/retailflight.h"
#include "sim/retailrng.h"
#include "sim/retailpark.h"
#include "sim/pathsearch.h"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace tak::sim;
struct Action { int result, stage; uint32_t mask, pending, events; bool disable; };
struct Fixture {
    std::vector<RetailMissionState> states;
    std::vector<int> queue;
    std::vector<Action> actions;
    std::ostringstream trace;
    uint32_t seed = 1, events = 0;
    bool live = true;
    size_t calls = 0;
    bool enabled() const { return live; }
    RetailMissionState* head() { return queue.empty() ? nullptr : &states[size_t(queue.front())]; }
    bool hasNext(const RetailMissionState& m) const {
        return std::find(queue.begin(), queue.end(), int(&m - states.data())) + 1 != queue.end();
    }
    uint32_t random(int n) { return retailRandom(seed, n); }
    void idle() {}
    void remove(RetailMissionState& m) {
        const int id = int(&m - states.data());
        trace << "R " << id << '\n';
        std::erase(queue, id);
    }
    void rotate(RetailMissionState& m) {
        const int id = int(&m - states.data());
        trace << "Q " << id << '\n';
        std::erase(queue, id); queue.push_back(id);
    }
    void clear() { trace << "C\n"; queue.clear(); }
    int handle(RetailMissionState& m, uint32_t fired) {
        trace << "H " << (&m - states.data()) << ' ' << unsigned(m.stage) << ' '
              << fired << ' ' << m.pending << ' ' << events << '\n';
        const auto& a = actions[std::min(calls++, actions.size() - 1)];
        if (a.stage >= 0) m.stage = uint8_t(a.stage);
        m.waitMask = a.mask; m.pending |= a.pending; events |= a.events;
        if (a.disable) live = false;
        return a.result;
    }
    void print() {
        std::cout << trace.str() << "F " << seed << ' ' << events << ' ' << live << ' ' << queue.size();
        for (int id : queue) std::cout << ' ' << id;
        std::cout << '\n';
        for (const auto& m : states)
            std::cout << "S " << unsigned(m.stage) << ' ' << m.waitMask << ' ' << m.deadline
                      << ' ' << m.pending << ' ' << m.flags << '\n';
    }
};
struct GroundFixture {
    RetailMissionState state;
    uint32_t tick, seed, events, radius;
    int foot, owner, active;
    unsigned resets = 0;
    bool present = true;
    std::vector<uint32_t> bounds;
    std::function<void()> onReset;
    bool enabled() const { return true; }
    RetailMissionState* head() { return present ? &state : nullptr; }
    bool hasNext(const RetailMissionState&) const { return false; }
    void idle() {}
    void remove(RetailMissionState&) { present = false; if (active == owner) active = 0; }
    void rotate(RetailMissionState&) {}
    void clear() { remove(state); }
    uint32_t random(int n) { bounds.push_back(uint32_t(n)); return retailRandom(seed,n); }
    int handle(RetailMissionState& m, uint32_t fired) {
        return retailPlainGroundMove(m,radius,tick,fired,int16_t(foot),false,
            [&](int n) { return random(n); }, [&](uint32_t) {
                ++resets;
                if (active == owner) active = 0;
                m.pending &= ~0x3700u;
                if (onReset) onReset();
            });
    }
};
int main(int argc, char** argv) {
    if (argc==2 && std::string(argv[1])=="--ground-patrol") {
        uint32_t tick,seed,events,radius,mask,deadline,pending,flags;
        int stage,foot,mover,respond;
        while (std::cin>>tick>>seed>>events>>stage>>radius>>mask>>deadline>>pending>>flags>>foot>>mover>>respond) {
            RetailMissionState m{uint8_t(stage),mask,deadline,pending,flags};
            unsigned initialized=0,reset=0,actions=0,draws=0;uint32_t goalRadius=0;
            const int result=retailGroundPatrol(m,radius,tick,events,int16_t(foot),mover!=0,
                [&](int n){++draws;return retailRandom(seed,n);},[&]{++initialized;},
                [&](uint32_t r){++reset;goalRadius=r;},[&]{++actions;return respond?3:0;});
            std::cout<<result<<' '<<unsigned(m.stage)<<' '<<radius<<' '<<m.waitMask<<' '
                     <<m.deadline<<' '<<m.pending<<' '<<m.flags<<' '<<seed<<' '
                     <<initialized<<' '<<reset<<' '<<goalRadius<<' '<<actions<<' '<<draws<<'\n';
        }
        return 0;
    }
    if (argc==2 && std::string(argv[1])=="--ground-response") {
        uint32_t tick,seed,stage,mask,deadline,pending,flags,radius,events;
        int foot,blocked,mode,move,choose,valid,created; unsigned leash;
        int px,pz,gx,gz,ox,oz,tx,tz;
        while (std::cin>>tick>>seed>>stage>>mask>>deadline>>pending>>flags>>radius>>events
                >>foot>>blocked>>mode>>move>>choose>>valid>>created>>leash>>px>>pz>>gx>>gz>>ox>>oz>>tx>>tz) {
            RetailMissionState m{uint8_t(stage),mask,deadline,pending,flags};
            RetailGroundResponse response{mode,{int16_t(ox),int16_t(oz)},{int16_t(gx),int16_t(gz)}};
            RetailGroundPoint position{int16_t(px),int16_t(pz)},target{int16_t(tx),int16_t(tz)};
            unsigned scans=0,resets=0,clears=0,installs=0,validations=0;
            const int result=retailGroundMove(m,radius,tick,events,int16_t(foot),blocked!=0,mode,uint8_t(move),
                [&](int n){return retailRandom(seed,n);},[&](uint32_t){++resets;},[&]{
                    ++scans;
                    if (!choose || (mode>0 && move==1 && retailGroundDistance(target,response.goal)>=leash)) return false;
                    ++validations; if (!valid) return false;
                    ++clears; if (!created) return false;
                    ++installs; if (mode<0) response.origin=position;
                    return true;
                },[&]{
                    if (!retailGroundReturnNeeded(position,response,uint16_t(leash))) return true;
                    ++validations; if (!valid || !created) return false;
                    ++installs; return true;
                });
            std::cout<<result<<' '<<unsigned(m.stage)<<' '<<m.waitMask<<' '<<m.deadline<<' '
                     <<m.pending<<' '<<m.flags<<' '<<radius<<' '<<seed<<' '
                     <<response.origin.x<<' '<<response.origin.z<<' '
                     <<scans<<' '<<resets<<' '<<clears<<' '<<installs<<' '<<validations<<'\n';
        }
        return 0;
    }
    if (argc==2 && std::string(argv[1])=="--landing") {
        RetailFlightVector position; RetailLandingState state;
        unsigned heading,stage,events; int fx,fz,mover,fly;
        struct Host {
            uint32_t seed; int velocity,height,accept,probes=0,initialized=0,touched=0,deactivated=0,finished=0;
            std::vector<RetailFlightGoal> goals;
            uint32_t random(int n){return retailRandom(seed,n);}
            void initialize(){++initialized;}
            int velocityPercent(){return velocity;}
            bool landable(RetailFlightVector){return probes++==accept;}
            int groundHeight(RetailFlightVector){return height;}
            void touchdown(){++touched;}
            void deactivate(){++deactivated;}
            void finish(){++finished;}
            void install(RetailFlightGoal goal){goals.push_back(goal);}
        };
        while (std::cin>>position.x>>position.y>>position.z>>state.anchor.x>>state.anchor.y>>state.anchor.z
                >>heading>>fx>>fz>>events>>stage>>state.mission.waitMask>>state.angle>>state.parity>>state.canceled>>mover>>fly) {
            Host host;
            if (!(std::cin>>host.seed>>host.velocity>>host.height>>host.accept)) return 2;
            state.mission.stage=uint8_t(stage);
            const int result=retailLanding(state,events,position,uint16_t(heading),int16_t(fx),int16_t(fz),mover!=0,fly!=0,host);
            std::cout<<result<<' '<<unsigned(state.mission.stage)<<' '<<state.mission.waitMask<<' '
                     <<state.angle<<' '<<state.parity<<' '<<host.seed<<' '
                     <<state.anchor.x<<' '<<state.anchor.y<<' '<<state.anchor.z<<' '
                     <<host.probes<<' '<<host.initialized<<' '<<host.touched<<' '<<host.deactivated<<' '<<host.finished<<' '<<host.goals.size();
            for (const auto& g:host.goals) std::cout<<' '<<g.point.x<<' '<<g.point.y<<' '<<g.point.z<<' '<<g.flags<<' '<<g.radius;
            std::cout<<'\n';
        }
        return 0;
    }
    if (argc==2 && std::string(argv[1])=="--vtol-standby") {
        uint32_t tick,seed,stage,mask,deadline,pending,flags,mover,fly,air,install,land;
        while (std::cin>>tick>>seed>>stage>>mask>>deadline>>pending>>flags>>mover>>fly>>air>>install>>land) {
            RetailMissionState m{uint8_t(stage),mask,deadline,pending,flags};
            unsigned initialized=0,tried=0,landed=0;
            int result=retailVtolStandby(m,tick,mover!=0,fly!=0,air!=0,
                [&](int n){return retailRandom(seed,n);},[&]{++initialized;},
                [&]{++tried;return install!=0;},[&]{++landed;return land!=0;});
            std::cout<<result<<' '<<unsigned(m.stage)<<' '<<m.waitMask<<' '<<m.deadline<<' '
                     <<m.pending<<' '<<m.flags<<' '<<seed<<' '<<initialized<<' '<<tried<<' '<<landed<<'\n';
        }
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--park") {
        uint32_t seed,stage,mask,events,mover,present,valid,next,padding,attempts;
        int foot,tx,tz,permanent;
        while (std::cin>>seed>>stage>>mask>>events>>mover>>present>>valid>>next>>foot>>tx>>tz>>padding>>permanent>>attempts) {
            RetailMissionState m;m.stage=uint8_t(stage);m.waitMask=mask;
            unsigned cleared=0,reset=0,target=0;int outer=0,inner=0;
            const int result=retailGroundPark(m,events,mover,present,valid,next,int16_t(foot),int16_t(tx),int16_t(tz),
                padding,permanent,attempts,[&](int n){return retailRandom(seed,n);},[&]{++cleared;},
                [&](bool t,int o,int i){++reset;target=t;outer=o;inner=i;});
            std::cout<<result<<' '<<unsigned(m.stage)<<' '<<m.waitMask<<' '<<attempts<<' '<<seed<<' '
                     <<cleared<<' '<<reset<<' '<<target<<' '<<outer<<' '<<inner<<'\n';
        }
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--standby") {
        uint32_t tick, seed, stage, mask, deadline, pending, flags, mover, install;
        while (std::cin >> tick >> seed >> stage >> mask >> deadline >> pending >> flags >> mover >> install) {
            RetailMissionState m{uint8_t(stage),mask,deadline,pending,flags};
            unsigned initialized=0, tried=0;
            int result=retailStandby(m,tick,mover!=0,[&](int n) { return retailRandom(seed,n); },
                [&] { ++initialized; },[&] { ++tried; return install!=0; });
            std::cout << result << ' ' << unsigned(m.stage) << ' ' << m.waitMask << ' ' << m.deadline
                      << ' ' << m.pending << ' ' << m.flags << ' ' << seed << ' '
                      << initialized << ' ' << tried << '\n';
        }
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--ground") {
        GroundFixture f; unsigned stage;
        if (!(std::cin >> f.tick >> f.seed >> f.events >> stage >> f.state.waitMask >>
              f.state.deadline >> f.state.pending >> f.state.flags >> f.radius >>
              f.foot >> f.owner >> f.active) || stage > 2 || f.foot < 1 || f.foot > 15) return 2;
        f.state.stage = uint8_t(stage);
        retailDispatchMissions(f.tick,f.events,f);
        const auto& m=f.state;
        std::cout << unsigned(m.stage) << ' ' << m.waitMask << ' ' << m.deadline << ' '
                  << m.pending << ' ' << m.flags << ' ' << f.radius << ' '
                  << f.events << ' ' << f.seed << ' ' << f.active << ' ' << f.resets;
        for (auto n:f.bounds) std::cout << ' ' << n;
        std::cout << '\n'; return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--oracle") {
        Fixture f;
        uint32_t tick; unsigned n, actions;
        if (!(std::cin >> tick >> f.seed >> f.events >> f.live >> n >> actions) ||
            n > 8 || actions < 1 || actions > 120) return 2;
        f.states.resize(n);
        for (unsigned i = 0; i < n; ++i) {
            unsigned stage; auto& m = f.states[i];
            if (!(std::cin >> stage >> m.waitMask >> m.deadline >> m.pending >> m.flags) || stage > 255) return 2;
            m.stage = uint8_t(stage); f.queue.push_back(int(i));
        }
        for (unsigned i = 0; i < actions; ++i) {
            Action a;
            if (!(std::cin >> a.result >> a.stage >> a.mask >> a.pending >> a.events >> a.disable)) return 2;
            f.actions.push_back(a);
        }
        retailDispatchMissions(tick, f.events, f); f.print(); return 0;
    }
    // Early navigation event: reset, attach controller, schedule polling delay.
    Fixture f;
    f.seed = 1941872796;
    f.states = {{2, 0x2701, 10076, 0x3000, 0}}; f.queue = {0};
    f.actions = {{0,-1,0,0,0,false}, {1,-1,0,0,0,false}, {1,-1,0x2701,0,0,true}};
    retailDispatchMissions(10070, f.events, f);
    if (f.calls != 3 || f.states[0].stage != 2 || f.states[0].pending != 0x1000) return 1;
    // A handler that neither waits nor changes the queue must hit retail's cap.
    Fixture spin;
    spin.states = {{}}; spin.queue = {0}; spin.actions = {{2,-1,0,0,0,false}};
    retailDispatchMissions(1, spin.events, spin);
    if (spin.calls != 100 || !spin.queue.empty()) return 1;
    // Sleep adds the timer bit and wraps at 32 bits.
    RetailMissionState m; m.waitMask = 0x2000; m.sleep(0xfffffffdu, 5);
    if (m.waitMask != 0x2001 || m.deadline != 2) return 1;
    for (int active : {40,53}) {
        GroundFixture ground;
        ground.state = {2,0x2701,10076,0x3000,0x3001401};
        ground.tick=10070; ground.seed=1941872796; ground.events=0;
        ground.radius=0; ground.foot=2; ground.owner=53; ground.active=active;
        retailDispatchMissions(ground.tick,ground.events,ground);
        if (ground.state.stage!=2 || ground.state.waitMask!=0x2701 ||
            ground.state.deadline!=10078 || ground.state.pending!=0 ||
            ground.radius!=64 || ground.seed!=1747098913 || ground.resets!=1 ||
            ground.active!=(active==53 ? 0 : active) || ground.bounds!=std::vector<uint32_t>{5}) return 1;
        // With no new event, no dispatch/RNG until the timer is due.
        retailDispatchMissions(10077,ground.events,ground);
        if (ground.bounds.size()!=1 || ground.resets!=1) return 1;
        ground.tick=10078;
        retailDispatchMissions(ground.tick,ground.events,ground);
        if (ground.bounds!=std::vector<uint32_t>{5,5} || ground.resets!=1) return 1;
    }
    {
        // Exercise the real resumable worker -> notification -> mission reset
        // path. The order wakes while its cost search is still unfinished.
        PathService paths; paths.setBudget(1);
        GroundFixture ground;
        ground.state={2,0x2701,0xffffffffu,0,0};
        ground.tick=0; ground.seed=1941872796; ground.events=0;
        ground.radius=0; ground.foot=2; ground.owner=53; ground.active=53;
        ground.onReset=[&] { paths.cancel(53); };
        paths.request(53,{1,10},{18,10},20,20,{}, {},0,false,0,0,{},{},0,123);
        bool delivered=false, notified=false;
        auto score=[](int,int x,int z) { return x<0 || z<0 || x>=20 || z>=20 || x==10 ? 0 : 6; };
        auto done=[&](int,const std::vector<PathCell>&,Fixed,Fixed,bool,bool,bool,bool) { delivered=true; };
        for (;ground.tick<1000 && !notified;++ground.tick) {
            paths.tick(score,done);
            for (auto n:paths.takeNotifications()) {
                if (n.unitId!=53 || n.controller!=123) return 1;
                ground.state.pending |= uint32_t(n.events); notified=true;
            }
        }
        if (!notified || delivered || !paths.pending(53)) return 1;
        retailDispatchMissions(ground.tick,ground.events,ground);
        paths.tick(score,done);
        if (ground.resets!=1 || ground.radius!=64 || paths.pending(53) || delivered) return 1;
    }
    std::cout << "retail_mission_test: passed\n";
}
