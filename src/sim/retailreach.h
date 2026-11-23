#pragma once
#include <algorithm>
#include <cstdlib>
#include <utility>
#include <vector>

namespace tak::sim {

// Rectangle connectivity (4e1570). Two wall followers attempt to rejoin the
// horizontal-then-vertical direct route. A failed long traversal ends the
// query; short failures retry the remaining source perimeter in reverse order.
// This deliberately differs from exhaustive flood fill.
template<class Grade>
bool retailRectangleReachable(int width,int height,int sx,int sz,int sw,int sh,
                              int tx,int tz,int tw,int th,bool staticOnly,
                              bool limited,Grade grade) {
    using Point=std::pair<int,int>;
    auto inside=[](Point p,int x,int z,int w,int h) {
        return p.first>=x && p.first<x+w && p.second>=z && p.second<z+h;
    };
    auto target=[&](Point p) { return inside(p,tx,tz,tw,th); };
    auto passable=[&](Point p) {
        if (p.first<0 || p.first>=width || p.second<0 || p.second>=height) return false;
        const int value=grade(p.first,p.second);
        return staticOnly ? !(value&8) : (value&7)>=4;
    };
    if (target({sx,sz}) || inside({tx,tz},sx,sz,sw,sh)) return true;
    std::vector<Point> starts;
    for (int x=sx;x<sx+sw;++x) for (int z=sz;z<sz+sh;++z)
        if ((x==sx || x==sx+sw-1 || z==sz || z==sz+sh-1) && passable({x,z}))
            starts.emplace_back(x,z);
    auto step=[](Point p,int direction,int sign=1) -> Point {
        constexpr int dx[]={0,-1,-1,-1,0,1,1,1};
        constexpr int dz[]={-1,-1,0,1,1,1,0,-1};
        return {p.first+sign*dx[direction],p.second+sign*dz[direction]};
    };
    const int budget=3*(std::abs(sx-tx)+std::abs(sz-tz));
    for (auto start=starts.rbegin();start!=starts.rend();++start) {
        Point current=*start;
        int steps=0;
        bool failed=false;
        while (!failed) {
            if (target(current)) return true;
            const int direction=tx<current.first ? 2 : tx>current.first ? 6 : tz>current.second ? 4 : 0;
            const Point candidate=step(current,direction);
            if (passable(candidate)) {
                if (target(candidate)) return true;
                ++steps;current=candidate;
                if (limited && steps>budget) return false;
                continue;
            }
            Point left=current,right=current;
            int ld=(direction+2)&7,rd=ld;
            bool moved=false;
            auto rejoin=[&](Point p) {
                int dx=tx-current.first,dz=tz-current.second;
                int cx=p.first-current.first,cz=p.second-current.second;
                if (dx<0) { dx=-dx;cx=-cx; }
                if (dz<0) { dz=-dz;cz=-cz; }
                return (cz==0 && cx>0 && cx<=dx) || (cx==dx && cz>0 && cz<=dz);
            };
            for (;;) {
                int stop=(ld-3)&7;ld=(ld-2)&7;
                while (!passable(step(left,ld))) {
                    if (ld==stop) { failed=true;break; }
                    ld=(ld+1)&7;
                }
                if (failed || (left==right && ld==rd && moved)) { failed=true;break; }
                left=step(left,ld);++steps;moved=true;
                if (limited && steps>budget) return false;
                if (target(left)) return true;
                if (rejoin(left)) { current=left;break; }
                stop=(rd+3)&7;rd=(rd+2)&7;
                while (!passable(step(right,rd,-1))) {
                    if (rd==stop) { failed=true;break; }
                    rd=(rd-1)&7;
                }
                if (failed || (left==right && ld==rd)) { failed=true;break; }
                right=step(right,rd,-1);++steps;
                if (limited && steps>budget) return false;
                if (target(right)) return true;
                if (rejoin(right)) { current=right;break; }
            }
        }
        if (steps>=3*(sw+sh)) return false;
    }
    return false;
}

} // namespace tak::sim
