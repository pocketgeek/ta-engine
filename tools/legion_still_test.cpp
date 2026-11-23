// legion_still_test -- Legion's headway window (World::updateLegionStill,
// LegionHeadway::tick) on made-up motion, asset-free.
//
//   legion_still_test [CASE]
//
// A Legion ground body reports itself stopped to its scripts (legionStill:
// GET 29 and MoveRate 0, its legs stand) after a window of 8 ticks without
// NET headway, and a livelock (legionShuffle: its navigator holds it) when it
// walked or turned to and fro in that window. The motion below is the 0.8.1
// walk-in-place as the user's replay traced it (zontroll 262, zongob 183),
// the cycles around it, and the genuine slow motion that must never stand:
// a walker at a pace (a quarter of its speed), a speed-matched follower, slow
// pivots, a start from rest, a creep, a corner. On 9f677c87 the rule was per
// tick (a step of a pace, or any turn, was headway): fed the same motion it
// never stood any of the four walk-in-place cases.
#include "sim/sim.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace tak::sim;
namespace {
void check(bool ok,const std::string& message) {if(!ok)throw std::runtime_error(message);}
int32_t raw(double px) {return int32_t(std::lround(px*65536.0));}

struct Body {
    LegionHeadway w;uint8_t ticks=0;bool still=false,shuffle=false;
    int32_t x=raw(800),z=raw(800);int32_t speed;int32_t rate;int foot;
    int tick=0,firstStill=-1,firstShuffle=-1,stillTicks=0,shuffleTicks=0;
    Body(double speedPx,int turnRate,int footprint=2):speed(raw(speedPx)),rate(turnRate),foot(footprint) {}
    // One tick: a step of (dx, dz) px and a turn of `turn` BAM.
    void step(double dx,double dz,int turn=0) {
        const int32_t bx=x,bz=z;x+=raw(dx);z+=raw(dz);
        w.tick(ticks,still,shuffle,bx,bz,x,z,int16_t(turn),speed,rate,foot);
        if(still&&firstStill<0)firstStill=tick;
        if(shuffle&&firstShuffle<0)firstShuffle=tick;
        stillTicks+=still;shuffleTicks+=shuffle;++tick;
    }
};

// zontroll 262 at 17:18: 0.489 px back and forth over a cell edge every tick
// (half its 1.0 px speed), no turn.
void pingpong() {
    Body b(1.0,2200);
    for(int t=0;t<300;++t)b.step(0,t%2?-0.489:0.489);
    std::printf("pingpong first_still=%d still=%d/300 first_shuffle=%d\n",b.firstStill,b.stillTicks,b.firstShuffle);
    check(b.firstStill>=0&&b.firstStill<8,"a body stepping back and forth walked on");
    check(b.stillTicks>=292,"a body stepping back and forth walked again");
    check(b.firstShuffle>=0&&b.firstShuffle<8,"a body stepping back and forth was no livelock");
}
// A 5-tick cycle (two steps one way at half speed, three back), and a 6-tick
// turn cycle on the spot (three turns at the full rate each way).
void cycles() {
    Body a(1.0,2200);
    for(int t=0;t<300;++t)a.step(t%5<2?0.5:-1.0/3,0);
    Body d(1.4,2400);
    for(int t=0;t<300;++t)d.step(0,0,t%6<3?2400:-2400);
    std::printf("cycles five_first_still=%d five_shuffle=%d turn6_first_still=%d turn6_shuffle=%d\n",a.firstStill,a.firstShuffle,d.firstStill,d.firstShuffle);
    check(a.firstStill>=0&&a.firstStill<16&&a.stillTicks>=284,"a body in a 5-tick cycle walked on");
    check(a.firstShuffle>=0,"a 5-tick cycle was no livelock");
    check(d.firstStill>=0&&d.firstStill<16&&d.stillTicks>=284,"a body turning left and right walked on");
    check(d.firstShuffle>=0,"turning left and right was no livelock");
}
// zongob 183 at 20:39: aims that swap sides; it turns 2400 BAM one way for two
// ticks and back for two, stepping 0.16 and 0.64 px across a cell edge, while
// it creeps 0.07 px a tick down the way (its speed is 1.4 px).
void dither() {
    Body b(1.4,2400);
    const double dx[4]={0.16,0.64,-0.14,-0.66};const int turn[4]={2400,2400,-2400,-2400};
    for(int t=0;t<300;++t)b.step(dx[t%4],0.07,turn[t%4]);
    std::printf("dither first_still=%d still=%d/300 first_shuffle=%d\n",b.firstStill,b.stillTicks,b.firstShuffle);
    check(b.firstStill>=0&&b.firstStill<8&&b.stillTicks>=292,"a body turning to two swapping aims walked on");
    check(b.firstShuffle>=0&&b.firstShuffle<8,"a body turning to two swapping aims was no livelock");
}
// Genuine slow motion never stands: a walker at exactly a pace (a quarter of
// its speed), a speed-matched follower at half of it, a pivot on the spot at
// the full rate (an eighth of the speed while it faces more than 67 degrees
// away), a giant's slow pivot (1200 BAM, 0.1 px), and a giant (0.25 px/tick
// acceleration) setting off from a stand.
void slow() {
    Body walk(1.0,2200),follow(1.8,2500),pivot(1.4,2400),giant(0.8,1200),start(0.8,1200,4);
    for(int t=0;t<300;++t) {
        walk.step(0.25,0);
        follow.step(0,0.9);
        if(t<14)pivot.step(0.175,0,2400);else pivot.step(1.4,0);
        if(t<27)giant.step(0.1,0,1200);else giant.step(0.8,0);
        if(t<40)start.step(0,0);else start.step(std::min(0.8,0.25*(t-39)),0);
        if(t==40)check(start.still,"a held body was not standing");
        if(t==40)check(!start.shuffle,"a held body was a livelock");
    }
    std::printf("slow walk=%d follow=%d pivot=%d giant=%d start_still=%d\n",walk.stillTicks,follow.stillTicks,pivot.stillTicks,giant.stillTicks,start.stillTicks);
    check(walk.stillTicks==0,"a walker at a pace stood");
    check(follow.stillTicks==0,"a speed-matched follower stood");
    check(pivot.stillTicks==0,"a body pivoting on the spot stood");
    check(giant.stillTicks==0,"a slow pivot stood");
    // It stood from tick 7 to 39 and walks within four ticks of setting off.
    check(start.stillTicks>=33&&start.stillTicks<=37,"a body setting off from a stand walked late");
}
// A stop is a stand from the window's end on, never a livelock; so is a creep
// at an eighth of the speed (a shuffle that faces away), and a corner turned
// at full speed and then crept along (zonter 45 in the groupdetour fixture:
// 1.8 px east, 1.8 px north, then 0.225 px a tick north).
void stands() {
    Body stop(1.0,2200),creep(1.0,2200),corner(1.8,2500);
    for(int t=0;t<100;++t) {
        stop.step(0,0);
        creep.step(0.125,0);
        if(t==0)corner.step(1.8,0);else if(t==1)corner.step(0,-1.8);else corner.step(0,-0.225);
    }
    std::printf("stands stop_first=%d creep_first=%d shuffles=%d/%d/%d\n",stop.firstStill,creep.firstStill,stop.shuffleTicks,creep.shuffleTicks,corner.shuffleTicks);
    check(stop.firstStill==7,"a stop stood late or early");
    check(creep.firstStill>=0,"a creep walked");
    check(stop.shuffleTicks==0&&creep.shuffleTicks==0&&corner.shuffleTicks==0,"a stop, creep or corner was a livelock");
}
// The livelock lasts until the body is a body length clear of where it began
// ((foot + 1) cells, 48 px for a 2x2), however much headway a dash back
// shows, or kLock ticks: then the next to-and-fro window finds it again.
void latch() {
    Body away(1.0,2200),dash(1.0,2200),loop(1.0,2200);
    int released=-1,dashHeadway=0,dashClear=0,relatched=-1;
    for(int t=0;t<300;++t) {
        if(t<16)away.step(0,t%2?-0.489:0.489);else away.step(1.0,0);
        if(away.firstShuffle>=0&&!away.shuffle&&released<0)released=t;
        // A side-step crept for 16 ticks and undone by a dash back over 8: each
        // dash is headway (the body walks then), the cycle is not.
        if(t<16)dash.step(0,t%2?-0.489:0.489);else dash.step((t-16)%24<16?0.15:-0.3,0);
        if(dash.firstShuffle>=0&&t<dash.firstShuffle+LegionHeadway::kLock-1) {dashHeadway+=!dash.still&&t>=16;dashClear+=!dash.shuffle;}
        loop.step(0,t%2?-0.489:0.489);
        if(loop.tick>loop.firstShuffle+LegionHeadway::kLock+1&&loop.shuffle&&relatched<0)relatched=t;
    }
    std::printf("latch released=%d dash_headway=%d dash_clear=%d relatched=%d loop_shuffle=%d/300\n",released,dashHeadway,dashClear,relatched,loop.shuffleTicks);
    check(released>=16+44&&released<=16+50,"a livelock did not end a body length clear");
    check(dashHeadway>0&&dashClear==0,"a dash back ended a livelock");
    check(relatched>0,"a lasting livelock was not found again after kLock");
    check(loop.shuffleTicks>=250,"a lasting livelock held too little");
}
}

int main(int argc,char** argv) {
    const std::map<std::string_view,std::function<void()>> cases{
        {"pingpong",pingpong},{"cycles",cycles},{"dither",dither},{"slow",slow},{"stands",stands},{"latch",latch}};
    try {
        if(argc<2) {for(const auto& [name,fn]:cases)fn();}
        else {
            const auto found=cases.find(argv[1]);
            if(found==cases.end())throw std::runtime_error("unknown case");
            found->second();
        }
    } catch(const std::exception& e) {std::fprintf(stderr,"legion_still_test: %s\n",e.what());return 1;}
    std::puts("ok");
    return 0;
}
