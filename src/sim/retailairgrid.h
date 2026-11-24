#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <vector>

#include "sim/copymember.h"

namespace ta::sim {

// Retail's secondary (airborne) map occupant: map cell word +2, with each
// flyer's overlap list at unit +0x117 (count, -1 = overflow) / +0x118 (seven
// ids) and the footprint origin it was inserted at, +0x126/+0x128.
struct RetailAirOccupant {
    int16_t recordedX=-9999,recordedZ=-9999; // 507030: not inserted
    int8_t count=0;
    std::array<int,7> links{};
    // 519bd0
    bool has(int id) const {
        if (count>7 || count<=0) return false;
        for (int i=0;i<count;++i) if (links[size_t(i)]==id) return true;
        return false;
    }
    // 519c10
    void add(int self,int id) {
        if (id==self) return;
        if (count==7 || count>7 || count<-1) count=-1;
        if (count==-1) return;
        links[size_t(count++)]=id;
    }
    // 519c70: drops the LAST entry when `id` is absent. Callers test has() first.
    void remove(int self,int id) {
        if (id==self) return;
        if (count>7 || count<-1) count=-1;
        if (count==-1) return;
        for (int i=0;i<count-1;++i)
            if (links[size_t(i)]==id) { links[size_t(i)]=links[size_t(count-1)]; break; }
        --count;
    }
};

// Persistent secondary occupancy. Cells hold 0 (none), -1 (the 0xffff
// overflow sentinel) or an entity id. Stale ids of entities that stopped
// being live without a removal stay until a later write, exactly as retail's
// grid does; every reader re-checks liveness.
//
// Host: `RetailAirOccupant* state(int id)` -- live entities only (retail's
// +0x130 bit 0x1000000); `bool footprint(int id,int& x,int& z,int& fx,int& fz)`
// -- current origin +0x74/+0x76 and size +0x78/+0x7a; `bool airborne(int id)`
// -- flight mode 2; `forEachFlyer(f)` -- every live canfly entity.
class RetailAirOccupancy {
    PackedWhenKept<int> cells_;   // a keyframe of the world holds it as runs (sim/copymember.h)
    int width_=0,height_=0;
    template<class Host> static bool footprint(Host& host,int id,int& x,int& z,int& fx,int& fz) {
        return host.footprint(id,x,z,fx,fz);
    }
public:
    void reset(int width,int height) { width_=width;height_=height;cells_.assign(size_t(width)*height,0); }
    int width() const { return width_; }
    int height() const { return height_; }
    std::span<const int> cells() const { return cells_; }
    int at(int x,int z) const { return cells_[size_t(z)*width_+x]; }
    bool empty() const { return cells_.empty(); }

    // 507050(unit,1): the end-of-update pass clears the recorded footprint
    // unconditionally and resets the overlap list.
    template<class Host> void clear(int id,Host& host) {
        auto* self=host.state(id);
        if (!self) return;
        int x,z,fx,fz;
        footprint(host,id,x,z,fx,fz);
        const int rx=self->recordedX,rz=self->recordedZ;
        if (rx>=0 && rz>=0 && rx+fx<width_ && rz+fz<height_)
            for (int r=0;r<fz;++r) for (int c=0;c<fx;++c) cells_[size_t(rz+r)*width_+rx+c]=0;
        self->count=0;
    }

    // 507050(unit,0), reached from map removal 5066a0 at retirement 512ae0:
    // owned cells pass to a random overlapping list member (535cc0).
    template<class Host,class Random> void remove(int id,Host& host,Random random) {
        auto* self=host.state(id);
        if (!self) return;
        int x,z,fx,fz;
        footprint(host,id,x,z,fx,fz);
        const int rx=self->recordedX,rz=self->recordedZ;
        if (rx<0 || rz<0 || rx+fx>=width_ || rz+fz>=height_) { self->count=0; return; }
        bool overflow=self->count==-1;
        for (int r=0;r<fz;++r) for (int c=0;c<fx;++c) {
            int& cell=cells_[size_t(rz+r)*width_+rx+c];
            if (!cell) continue;
            if (cell==-1) { cell=0; overflow=true; continue; }
            if (cell!=id) continue;
            if (self->count<=0) { cell=0; continue; }
            std::array<int,7> candidates{};
            int n=0;
            for (int k=0;k<self->count;++k) {
                const int other=self->links[size_t(k)];
                const auto* state=other ? host.state(other) : nullptr;
                if (!state) continue;
                int ox,oz,ofx,ofz;
                footprint(host,other,ox,oz,ofx,ofz);
                // The candidate's RECORDED origin, unlike insertion's current one.
                if (state->recordedX>rx+c || state->recordedX+ofx-1<rx+c ||
                    state->recordedZ>rz+r || state->recordedZ+ofz-1<rz+r || n>=7) continue;
                candidates[size_t(n++)]=other;
            }
            cell=n ? candidates[size_t(random(unsigned(n)))] : 0;
        }
        if (overflow) {
            host.forEachFlyer([&](int other) {
                auto* state=host.state(other);
                if (state && state->has(id)) state->remove(other,id);
            });
        } else {
            for (int k=0;k<self->count;++k) {
                const int other=self->links[size_t(k)];
                auto* state=other ? host.state(other) : nullptr;
                if (state && state->has(id)) state->remove(other,id);
            }
        }
        self->count=0;
    }

    // 506c40: insert an airborne footprint, building reciprocal overlap lists
    // and selecting each shared cell's owner with a bounded random draw.
    template<class Host,class Random> void insert(int id,Host& host,Random random) {
        auto* self=host.state(id);
        if (!self) return;
        int x,z,fx,fz;
        footprint(host,id,x,z,fx,fz);
        if (!host.airborne(id) || x<0 || z<0 || x+fx>=width_ || z+fz>=height_) {
            self->recordedX=self->recordedZ=-9999;
            return;
        }
        for (int r=0;r<fz;++r) for (int c=0;c<fx;++c) {
            int& cell=cells_[size_t(z+r)*width_+x+c];
            if (!cell) { cell=id; continue; }
            if (cell==-1) { self->count=-1; continue; }
            const int previous=cell;
            auto* owner=host.state(previous);
            if (!owner || owner->count<=-1) { cell=-1; self->count=-1; continue; }
            if (!owner->has(id)) owner->add(previous,id);
            if (!self->has(previous)) self->add(id,previous);
            std::array<int,7> candidates{previous,id};
            int n=2;
            for (int k=0;k<owner->count;++k) {
                const int other=owner->links[size_t(k)];
                if (other==id || !other) continue;
                auto* state=host.state(other);
                if (!state) continue;
                int ox,oz,ofx,ofz;
                footprint(host,other,ox,oz,ofx,ofz);
                if (ox>x+c || ox+ofx-1<x+c || oz>z+r || oz+ofz-1<z+r) continue;
                if (n>=7) {
                    cell=-1;state->count=-1;
                    for (int candidate:candidates)
                        if (auto* listed=candidate ? host.state(candidate) : nullptr) listed->count=-1;
                    break;
                }
                if (!state->has(id)) state->add(other,id);
                if (!self->has(other)) self->add(id,other);
                candidates[size_t(n++)]=other;
            }
            if (cell!=-1) cell=candidates[size_t(random(unsigned(n)))];
        }
        self->recordedX=int16_t(x);self->recordedZ=int16_t(z);
    }
};

// Fresh single pass (all lists reset, empty grid), the projectile fixture.
struct RetailAirCollisionBody {
    int id=0,x=0,z=0,width=1,height=1;
    bool airborne=true;
};
struct RetailAirCollisionHost {
    const std::vector<RetailAirCollisionBody>& bodies;
    std::vector<RetailAirOccupant>& states;
    int index(int id) const {
        for (size_t i=0;i<bodies.size();++i) if (bodies[i].id==id) return int(i);
        return -1;
    }
    RetailAirOccupant* state(int id) { const int i=index(id); return i<0 ? nullptr : &states[size_t(i)]; }
    bool footprint(int id,int& x,int& z,int& fx,int& fz) const {
        const auto& b=bodies[size_t(index(id))];
        x=b.x;z=b.z;fx=b.width;fz=b.height;return true;
    }
    bool airborne(int id) const { return bodies[size_t(index(id))].airborne; }
    template<class F> void forEachFlyer(F f) const { for (const auto& b:bodies) f(b.id); }
};
template<class Random>
std::vector<int> retailAirCollisionGrid(int width,int height,
        const std::vector<RetailAirCollisionBody>& bodies,Random random) {
    std::vector<RetailAirOccupant> states(bodies.size());
    RetailAirCollisionHost host{bodies,states};
    RetailAirOccupancy grid;
    grid.reset(width,height);
    for (const auto& body:bodies) grid.insert(body.id,host,random);
    return {grid.cells().begin(),grid.cells().end()};
}

// 509400: may a flyer touch down with its footprint centred at (x,z)?
// Host: `int width()`, `int height()`, `uint16_t exploration(size_t coarse)`;
// `uint16_t feature(size_t cell)`, `int featureBack(size_t cell)` -- the
// linear tail offset backZ*W+backX (+0xa/+0xb), `int featureCount()`,
// `bool featureBlocking(int index)` (+0x13c bit 0x20); `bool yard(size_t)`
// (+0xd bit 0x10); `bool groundOther(size_t)` / `bool airOther(size_t)` -- a
// live entity other than the lander in word +0 / +2; `int low(size_t)` /
// `int high(size_t)` (+6 / +5); `int sea()`.
// TotalA.exe's test is 0x47e2d0 (VTOL_LandIfCan 0x40f417 / 0x40f57c), the
// same shape: the footprint-snapped origin and map bounds (0x47e2df..
// 0x47e35d), the unexplored-site pass (coarse index with footprintx on both
// axes, 0x47e363..0x47e3a8), the water limits (0x47e3ab..0x47e3f9), then per
// 13-byte cell a blocking feature (0x47e43b..0x47e4ba), cell +0xc bit 2
// (0x47e4c0; World's yard stand-in), word +0 and word +2 holding a unit other
// than the lander (0x47e4ca..0x47e4f3) and the height / slope limits
// (0x47e4f5..0x47e51b). So TA, too, refuses a site under another flyer's
// airborne word +2: airOther stays.
struct RetailLandingLimits {
    int16_t footX=1,footZ=1;
    int16_t maxWaterDepth=10000,minWaterDepth=-10000; // type +0x192/+0x194
    uint8_t maxSlope=255;                              // type +0x23c
    bool canFly=true,floater=false;                    // type +0x260 bits 0x800/0x200000
    uint8_t player=0;                                  // unit +0xfd
};
template<class Host>
bool retailLandingSiteFree(int32_t x,int32_t z,const RetailLandingLimits& limits,Host& host) {
    const int fx=limits.footX,fz=limits.footZ;
    const int16_t x0=int16_t(std::bit_cast<int32_t>(uint32_t(x)-(uint32_t(fx)<<19)+0x80000u)>>20);
    const int16_t z0=int16_t(std::bit_cast<int32_t>(uint32_t(z)-(uint32_t(fz)<<19)+0x80000u)>>20);
    const int width=host.width(),height=host.height();
    if (x0<0 || z0<0 || x0+fx>=width || z0+fz>=height) return false;
    // Retail uses footprintX for both axes of the coarse exploration index.
    const size_t coarse=size_t(((z0>>1)+(fx>>2))*(width>>1)+(fx>>2)+(x0>>1));
    if (!(host.exploration(coarse)&(1u<<(limits.player&31)))) return true;
    const int sea=host.sea();
    int lowest=sea-limits.maxWaterDepth;
    if (lowest<sea && limits.canFly && !limits.floater) lowest=sea;
    const int highest=sea-limits.minWaterDepth;
    for (int r=0;r<fz;++r) for (int c=0;c<fx;++c) {
        const size_t cell=size_t(z0+r)*width+x0+c;
        uint16_t feature=host.feature(cell);
        if (feature!=0xffff) {
            if (feature<0xfffa) {
                if (feature>=host.featureCount() || host.featureBlocking(feature)) return false;
            } else {
                if (feature!=0xfffe) return false;
                feature=host.feature(cell-size_t(host.featureBack(cell)));
                if (feature<0xfffa && host.featureBlocking(feature)) return false;
            }
        }
        if (host.yard(cell) || host.groundOther(cell) || host.airOther(cell)) return false;
        const int low=host.low(cell),high=host.high(cell);
        if (low<lowest || high>highest || high-low>limits.maxSlope) return false;
    }
    return true;
}

} // namespace ta::sim
