// Legion allies swap places, as drawn (client/swapshow.h).
//
// The sim trades two bodies' places in one tick. The display must start each
// body where it was drawn, carry it to its sim position over kSwapShowTicks
// with no jump between ticks, sidestep the two bodies to opposite sides, and
// leave ordinary walking alone.
#include "client/swapshow.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace tak;

static int fails=0;
static void check(bool ok,const std::string& what,const std::string& detail="") {
    std::printf("  [%s] %s%s\n",ok ? "PASS" : "FAIL",what.c_str(),
                detail.empty() ? "" : (" -- "+detail).c_str());
    if(!ok)++fails;
}

int main() {
    std::printf("swapshow\n");
    const float walk=1.5f;   // px per tick
    // Steps that are not swaps (a walk, a nudge of a whole cell): nothing to show.
    {
        SwapShow s;
        bool none=true;
        for(int i=0;i<30;++i) {s=swapShowStep(s,walk,0,1,false);none=none&&s.ox==0&&s.oz==0;}
        s=swapShowStep(s,16,0,1,false);none=none&&s.ox==0&&s.oz==0;
        check(none,"steps that are not swaps draw no offset");
    }
    // Two bodies 24 px apart on x swap: A jumps +24, B -24.
    {
        SwapShow a=swapShowStep({},24,0,1,true),b=swapShowStep({},-24,0,1,true);
        const float ax=24+a.ox,bx=-24+b.ox;   // drawn, each relative to its own old place
        // First tick: each moved only a little from where it was drawn (0 for both).
        check(std::fabs(ax)<=24.0f/kSwapShowTicks+0.01f&&std::fabs(bx)<=24.0f/kSwapShowTicks+0.01f,
              "the first tick moves one share of the jump",std::to_string(ax)+" "+std::to_string(bx));
        check(a.oz*b.oz<0,"the two sidestep to opposite sides",std::to_string(a.oz)+" "+std::to_string(b.oz));
        float last=ax;
        bool steady=true,done=false;
        for(int i=1;i<20;++i) {
            a=swapShowStep(a,0,0,1,false);
            const float now=24+a.ox;
            steady=steady&&std::fabs(now-last)<=24.0f/kSwapShowTicks*1.6f+0.01f;
            last=now;
            if(a.ox==0&&a.oz==0&&a.age>=kSwapShowTicks) {done=i+1==int(kSwapShowTicks);break;}
        }
        check(steady,"no tick jumps more than a share of the swap");
        check(done&&last==24,"the body reaches its sim position after kSwapShowTicks");
    }
    // A second swap while one plays out starts from where the body is drawn.
    {
        SwapShow s=swapShowStep({},24,0,1,true);
        for(int i=0;i<3;++i)s=swapShowStep(s,0,0,1,false);
        const float drawnX=24+s.ox,drawnZ=s.oz;   // in A's frame (old place 0)
        const SwapShow t=swapShowStep(s,-24,0,1,true);
        const float nowX=0+t.ox,nowZ=t.oz;
        check(std::hypot(nowX-drawnX,nowZ-drawnZ)<=24.0f/kSwapShowTicks*2,"a swap back continues from the drawn place");
    }
    // A replay seek past a swap is far past a footprint: not played out.
    check(swapShowStep({},300,0,1,true).ox==0,"a long jump is not played out");
    std::printf(fails ? "FAILED (%d)\n" : "all passed\n",fails);
    return fails ? 1 : 0;
}
