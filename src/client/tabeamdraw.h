#pragma once

// TotalA.exe's projectile draw (0x49be60) for the line-drawn classes: a
// beam (rendertype 0, 0x49bf3a..0x49c097) and lightning (rendertype 7,
// 0x49c48c..0x49c727). Both draw between the shot's head (+0x04) and its
// tail (+0x10, sim::Projectile::taTail) with the line routine 0x4be950, in
// palette indices taken through the game's colour map (game +0xdcb, built by
// taGuiColourMap). Points are projected the game's way: x, z - (y >> 1)
// (whole px, the high words), before the camera offset.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace ta {

// 0x4ac7d0: each guipal.pal entry's nearest entry of the game palette
// (palettes\palette.pal), by the summed |dR| + |dG| + |dB|, the first on a
// tie. The weapons' color / color2 index this map (0x49bf5c, 0x49bf7b).
inline std::array<uint8_t,256> taGuiColourMap(const uint8_t (&gui)[256][4],const uint8_t (&game)[256][4]) {
    std::array<uint8_t,256> map{};
    for(int i=0;i<256;++i) {
        int best=9999999,index=0;   // 0x98967f
        for(int j=0;j<256;++j) {
            const int d=std::abs(int(gui[i][2])-int(game[j][2]))+std::abs(int(gui[i][0])-int(game[j][0]))+
                        std::abs(int(gui[i][1])-int(game[j][1]));
            if(d<best) {best=d;index=j;}
        }
        map[size_t(i)]=uint8_t(index);
    }
    return map;
}

struct TaShotLine { int x1=0,y1=0,x2=0,y2=0; uint8_t colour=0; };

inline std::array<int,2> taShotScreenPoint(const std::array<int32_t,3>& p) {
    const auto whole=[](int32_t v){return int(int16_t(uint16_t(uint32_t(v)>>16)));};
    return {whole(p[0]),whole(p[2])-(whole(p[1])>>1)};
}

// Rendertype 0. With a color2 (the raw TDF byte, 0x49bfd2) a second line in
// the mapped color2 goes under the first, a pixel off its major axis: one
// row up for a mostly horizontal beam (0x49c002), and for a mostly vertical
// one from a column left of its top end to a column right of its bottom
// end (0x49c023; the ends are ordered first, 0x49bff2 / 0x49c013). Then the
// beam itself in the mapped color (0x49c04d).
inline std::vector<TaShotLine> taBeamLines(const std::array<int32_t,3>& head,const std::array<int32_t,3>& tail,
                                           uint8_t colour,uint8_t colour2Raw,uint8_t colour2) {
    auto [x1,y1]=taShotScreenPoint(head);
    auto [x2,y2]=taShotScreenPoint(tail);
    std::vector<TaShotLine> lines;
    if(colour2Raw) {
        if(std::abs(x1-x2)>std::abs(y1-y2)) {
            if(x1>x2) {std::swap(x1,x2);std::swap(y1,y2);}
            lines.push_back({x1,y1-1,x2,y2-1,colour2});
        } else {
            if(y1>y2) {std::swap(x1,x2);std::swap(y1,y2);}
            lines.push_back({x1-1,y1,x2+1,y2,colour2});
        }
    }
    lines.push_back({x1,y1,x2,y2,colour});
    return lines;
}

// Rendertype 7: two jagged strands from the tail toward the head, in the
// mapped color. The head-to-tail length L (x87 sqrt, _ftol) gives a count
// n = (L << 16) / 5.0 px (__alldiv, 0x49c510); each strand takes n >> 16
// steps of d * 65536 / n (5 px) from the tail, every step's point jittered
// by rand() * 11 / 32768 - 5 whole px on x, y and z in turn (0x49c619..
// 0x49c6a8), each segment joining the last jittered point to the next.
// `rand` is the CRT's (0x4e4870): drawing only, it never reaches the sim.
// The game seeds that stream with the clock at start (srand(time(0)),
// 0x49e8bb / 0x4971ae) and its other callers share it; the client keeps one
// stream per process, seeded the same way (GameView::crtRand_).
template<class Rand>
std::vector<TaShotLine> taLightningLines(const std::array<int32_t,3>& head,const std::array<int32_t,3>& tail,
                                         uint8_t colour,Rand&& rand) {
    std::vector<TaShotLine> lines;
    std::array<int32_t,3> d{};
    for(size_t axis=0;axis<3;++axis)d[axis]=int32_t(uint32_t(head[axis])-uint32_t(tail[axis]));
    const double length=std::sqrt(double(d[0])*d[0]+double(d[1])*d[1]+double(d[2])*d[2]);
    const int64_t whole=length<2147483648.0 ? int64_t(int32_t(length)) : int64_t(INT32_MIN);
    const int64_t n=int32_t(uint32_t(uint64_t((whole*65536)/0x50000)));
    if(n==0)return lines;
    std::array<int32_t,3> step{};
    for(size_t axis=0;axis<3;++axis)step[axis]=int32_t((int64_t(d[axis])*65536)/n);
    const int segments=int(int16_t(uint16_t(uint32_t(n)>>16)));
    for(int strand=0;strand<2;++strand) {
        std::array<int32_t,3> base=tail,last=tail;
        for(int k=0;k<segments;++k) {
            for(size_t axis=0;axis<3;++axis)base[axis]=int32_t(uint32_t(base[axis])+uint32_t(step[axis]));
            auto jittered=base;
            for(size_t axis=0;axis<3;++axis) {
                const int32_t r=int32_t((int64_t(rand())*11)/0x8000)-5;
                const uint16_t high=uint16_t(uint16_t(uint32_t(jittered[axis])>>16)+uint16_t(r));
                jittered[axis]=int32_t((uint32_t(jittered[axis])&0xffffu)|(uint32_t(high)<<16));
            }
            const auto a=taShotScreenPoint(last),b=taShotScreenPoint(jittered);
            lines.push_back({a[0],a[1],b[0],b[1],colour});
            last=jittered;
        }
    }
    return lines;
}

// The CRT's rand (0x4e4870): x = x * 214013 + 2531011, (x >> 16) & 0x7fff.
struct TaCrtRand {
    uint32_t state=1;
    int operator()() {state=state*214013u+2531011u;return int((state>>16)&0x7fffu);}
};

} // namespace ta
