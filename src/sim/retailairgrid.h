#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <vector>

#include "sim/copymember.h"

namespace ta::sim {

// A flyer's word +2 record: the footprint origin it last stamped (+0x76 /
// +0x78 as the setter left them; -9999 when it holds no cells) and the two
// overlap flags 0x47cc30 raises on a shared cell (+0x110 0x4000000 on the
// cell's holder, 0x8000000 on the flyer that did not get it).
struct RetailAirOccupant {
    int16_t recordedX=-9999,recordedZ=-9999;
    bool overlapped=false;   // +0x110 0x4000000: another flyer wants a cell it holds
    bool waiting=false;      // +0x110 0x8000000: it wants a cell another holds
};

// Persistent secondary occupancy. Cells hold 0 (none) or an entity id.
class RetailAirOccupancy {
    PackedWhenKept<int> cells_;   // a keyframe of the world holds it as runs (sim/copymember.h)
    int width_=0,height_=0;
    // One cell of a stamp (0x47cfb3..0x47d03e; the restamp's 0x47ca04..
    // 0x47ca92): an empty cell takes the flyer; an occupied one -- even one
    // holding the flyer itself, on a restamp -- keeps its holder, which is
    // marked overlapped, and the flyer is marked waiting. (A holder whose
    // owner is a remote player, +0x73 == 3, would give the cell up with the
    // flags the other way round, 0x47cfe5 / 0x47ca37: every owner is local to
    // the lockstep simulation, so the holder keeps it.)
    template<class Host> void stampCell(int id,RetailAirOccupant& self,int& cell,Host& host) {
        if (!cell) { cell=id; return; }
        if (auto* holder=host.state(cell)) holder->overlapped=true;
        self.waiting=true;
    }
public:
    void reset(int width,int height) { width_=width;height_=height;cells_.assign(size_t(width)*height,0); }
    int width() const { return width_; }
    int height() const { return height_; }
    std::span<const int> cells() const { return cells_; }
    int at(int x,int z) const { return cells_[size_t(z)*width_+x]; }
    bool empty() const { return cells_.empty(); }

    // TotalA.exe's flyer layer. The position setter 0x48a9f0 (the mover's
    // commit 0x43d6d0 -> 0x43d775, and 0x43da0e) compares the new footprint
    // origin ((x - footx << 19) + 0x80000) >> 20 and layer (+0x110 & 3) with
    // the recorded ones (+0x76 / +0x78, 0x48aa35..0x48aa56); on a change it
    // leaves the old cells (taUnstamp, 0x47d0e0) and takes the new ones
    // (taStamp, 0x47cc30).
    //
    // Host: `RetailAirOccupant* state(int id)` (a live flyer's record, or
    // null), `bool footprint(int id, int& x, int& z, int& fx, int& fz)` (its
    // current origin and size) and `forEachTouching(x, z, fx, fz, f)` -- f(id) for every live
    // flyer whose stamped footprint overlaps that rectangle (0x47e5c0's
    // sector walk, the strict overlap test 0x47e674..0x47e697), in the
    // engine's deterministic order.
    //
    // 0x47d0e0: layer 2 (0x47d23b..0x47d284) clears every cell of the recorded
    // footprint still holding this unit's index -- one another flyer holds
    // stays its; `stamped` false (the last stamp was off the map, +0x82 =
    // G+0x142b7, 0x47d0f7..0x47d0ff) clears none. Then, whatever the layer
    // (0x47d286..0x47d2ca): the unit's own waiting flag goes, and if it was
    // overlapped the flag goes too and every flyer touching the old footprint
    // is restamped (0x47e5c0 -> 0x47ed30 -> 0x47c790): a waiting one drops
    // its flag and stamps its footprint again, so it takes the cells this
    // one left.
    template<class Host> void taUnstamp(int id,RetailAirOccupant& self,bool stamped,int x,int z,int fx,int fz,
                                        Host& host) {
        if (stamped)
            for (int r=0;r<fz;++r) for (int c=0;c<fx;++c) {
                int& cell=cells_[size_t(z+r)*width_+x+c];
                if (cell==id) cell=0;
            }
        self.waiting=false;
        if (!self.overlapped) return;
        self.overlapped=false;
        if (!stamped) return;
        host.forEachTouching(x,z,fx,fz,[&](int other) { restamp(other,host); });
    }
    // 0x47c790, layer 2 (0x47c9e3..0x47cab9): only a waiting flyer; the flag
    // goes and its footprint at its current origin is stamped cell by cell.
    template<class Host> void restamp(int id,Host& host) {
        auto* self=host.state(id);
        if (!self || !self->waiting || self->recordedX<0) return;
        self->waiting=false;
        int x,z,fx,fz;
        if (!host.footprint(id,x,z,fx,fz)) return;
        for (int r=0;r<fz;++r) for (int c=0;c<fx;++c) {
            const int cx=self->recordedX+c,cz=self->recordedZ+r;
            if (cx<0 || cz<0 || cx>=width_ || cz>=height_) continue;
            stampCell(id,*self,cells_[size_t(cz)*width_+cx],host);
        }
    }
    // 0x47cc30, layer 2 (0x47cf98..0x47d042): an origin off the map stamps
    // nothing (0x47cc57..0x47cca3: x or z negative, x + footx or z + footz past
    // the last cell); else each cell as stampCell. Returns whether the
    // footprint was on the map.
    template<class Host> bool taStamp(int id,RetailAirOccupant& self,int x,int z,int fx,int fz,Host& host) {
        if (x<0 || z<0 || x+fx>=width_ || z+fz>=height_) return false;
        for (int r=0;r<fz;++r) for (int c=0;c<fx;++c)
            stampCell(id,self,cells_[size_t(z+r)*width_+x+c],host);
        return true;
    }
};

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
