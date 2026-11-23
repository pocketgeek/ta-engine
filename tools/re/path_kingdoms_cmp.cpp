// Comparison driver (tools only, not engine source): runs the engine's Retail
// search kernel (src/sim/retailtrace.h + retailcost.h, the Kingdoms port) on
// grade planes produced by tools/re/path_model.py, so path_compare.py can set
// its routes beside TotalA.exe's on identical input.
// Input per case: w h fx tol r2 heading weight turnrate sx sz gx gz, then w*h
// TA grades (0 blocked, 1 penalised, 3 clear). Output: one line of route cells.
// turnrate -1 substitutes TotalA.exe's cost constants for the Kingdoms ones.
#include "sim/retailtrace.h"
#include "sim/retailgoal.h"
#include <cstdio>
#include <vector>
using namespace ta::sim;
int main() {
    int w,h,fx,tol,r2,heading,weight,turn,sx,sz,gx,gz;
    while (std::scanf("%d %d %d %d %d %d %d %d %d %d %d %d",&w,&h,&fx,&tol,&r2,&heading,&weight,&turn,&sx,&sz,&gx,&gz)==12) {
        std::vector<int> g(size_t(w)*h);
        for (auto& v:g) { int t; std::scanf("%d",&t); v = t==0 ? 0 : t==1 ? 4 : t==2 ? 5 : 6; }
        RetailCircleGoal circle{gx,gz,tol,r2};
        auto grade=[&](int x,int z,int) { return x<0||z<0||x>=w||z>=h ? 0 : g[size_t(z)*w+x]; };
        RetailSearchWorker worker;
        auto costs=retailPathCosts(uint16_t(turn<0?0:turn),fx,65536,65536,0,false);
        if (turn<0) {   // TotalA.exe's constants (docs/retail-pathfinding.md section 3)
            costs.step={16,22,16,22,16,22,16,22}; costs.turn={0,40,60,80,100,80,60,40};
            costs.ground=0; costs.road=0; costs.slope=30; costs.traffic=0;
            costs.shortTurn=75; costs.minStraight=5;
        }
        auto configure=[&](int retry) {
            RetailSearchAttempt::Parameters p; p.width=w; p.height=h; p.start={sx,sz};
            p.heading=heading; p.retry=retry; p.weight=weight; p.trafficRadius=0; p.costs=costs; return p; };
        auto goals=[&] { std::vector<RetailReachability::Point> c; circle.enumerate([&](int x,int z){c.push_back({x,z});}); return c; };
        auto accepts=[&](int x,int z) { return circle.accepts(x,z); };
        auto distance=[&](int x,int z) { return circle.distance(x,z); };
        bool admitted=true; int note=0; long work=0; int guard=0;
        for (;;) {
            auto r=worker.dispatch(admitted,1<<30,configure,[](bool){},goals,accepts,distance,grade);
            admitted=false; work+=r.work; if (r.notification) note=r.notification;
            if (r.result==RetailSearchAttempt::Result::Complete) break;
            if (++guard>100000) break;
        }
        std::printf("%d %d %ld %d", note, worker.attempt.route.flags, work, worker.retry);
        for (auto p:worker.attempt.route.points) std::printf(" %d,%d",p.x,p.z);
        std::printf("\n");
    }
}
