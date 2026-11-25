#pragma once

// TotalA.exe's COB GET (the unit script host's vtable +0x44, 0x480770): ids
// 1..20 through the jump table at 0x480ac4 (id - 1 up to 0x13, 0x480781); 0, a
// negative id and every id past 20 return 0 (0x480abb), so Kingdoms' 27..46
// (heading, terrain, animation speeds, veteran level, standing order) read 0.
// One routine for both hosts: the simulation's (World::ScriptHost::get) and
// the client's display VM (client/displayget.h, from the render snapshot), so
// the two answer alike. The host supplies the unit's own words and three
// queries:
//   piecePoint(piece)  -> the piece's world point (0x43e060; the origin when
//                         the piece is out of range)
//   unit(id, out)      -> false for no live unit; else its +0x6a/+0x72 x/z,
//                         +0x6e y and its type's +0x16e height
//   groundHeight(x, z) -> 0x485070's terrain height in whole px (-1 off the map)

#include "sim/fixed.h"
#include "sim/retailconstruction.h"
#include "sim/retailmotion.h"

#include <array>
#include <bit>
#include <cstdint>
#include <utility>

namespace ta::sim {

// The unit's words GET reads directly.
struct RetailGetUnit {
    bool activated = false;   // +0x10e bit 0 (ACTIVATION, 0x480794)
    bool ready = false;       // +0x10f bit 0 (INBUILDSTANCE, 0x4807ef)
    bool busy = false;        // +0x10f bit 1 (BUSY, 0x4807ff)
    bool yardOpen = false;    // +0x10f bit 2 (YARD_OPEN, 0x480a83)
    bool buggerOff = false;   // +0x10f bit 3 (BUGGER_OFF, 0x480a96)
    bool armored = false;     // +0x10e bit 1 (ARMORED, 0x480aa9; SET 20 0x480be3)
    uint32_t moveState = 0;   // +0x110 >> 18 & 3 (STANDINGMOVEORDERS, 0x4807a4)
    uint32_t fireState = 0;   // +0x110 >> 20 & 3 (STANDINGFIREORDERS, 0x4807b7)
    int32_t hp = 0;           // the signed HP word +0x108
    int32_t maxHp = 1;        // type +0x1fa
    float remaining = 0;      // +0x104, the work left (0 finished)
    uint16_t heading = 0;     // the retail heading word
};

struct RetailGetOther { int32_t x = 0, y = 0, z = 0, height = 0; };

// A packed XZ word is x's high word over z's: (x & 0xffff0000) + (z >> 16);
// unpacking adds 0x10000 to x when the z half is negative (0x480965).
inline std::pair<int32_t,int32_t> retailUnpackXZ(uint32_t value) {
    uint32_t x=value&0xffff0000u;
    const int32_t z=std::bit_cast<int32_t>(value<<16);
    if (z<0) x+=0x10000u;
    return {std::bit_cast<int32_t>(x),z};
}
inline uint32_t retailPackXZ(int32_t x,int32_t z) {
    return (uint32_t(x)&0xffff0000u)+uint32_t(z>>16);
}

template<class Host>
uint32_t retailScriptGet(int id,const std::array<uint32_t,4>& args,const RetailGetUnit& u,Host& host) {
    switch (id) {
    case 1: return u.activated;
    case 2: return u.moveState;
    case 3: return u.fireState;
    case 4: return uint32_t(int32_t(int16_t(u.hp))*100/(u.maxHp>0 ? u.maxHp : 1));   // 0x4807ca
    case 5: return u.ready;
    case 6: return u.busy;
    case 7: case 8: {
        // PIECE_XZ / PIECE_Y (0x480811 / 0x48083f).
        const std::array<int32_t,3> point=host.piecePoint(std::bit_cast<int32_t>(args[0]));
        return id==7 ? retailPackXZ(point[0],point[2]) : uint32_t(point[1]);
    }
    case 9: case 10: case 11: {
        // UNIT_XZ / UNIT_Y / UNIT_HEIGHT (0x480868 / 0x4808c2 / 0x48090f): the
        // unit the argument names (0 is none).
        RetailGetOther other;
        if (!args[0] || !host.unit(int(args[0]),other)) return 0;
        if (id==9) return retailPackXZ(other.x,other.z);
        return uint32_t(id==10 ? other.y : other.height);
    }
    case 12: {
        // XZ_ATAN (0x480965): 0x4b715a's atan2(x, z) less the heading.
        const auto [x,z]=retailUnpackXZ(args[0]);
        return uint16_t(uint16_t(retailDirection(Fixed::raw(x),Fixed::raw(z)).v)-u.heading);
    }
    case 13: case 15: {
        // XZ_HYPOT / HYPOT (0x480994 / 0x4809e5): hypot of the two words, truncated.
        int64_t a,b;
        if (id==13) { const auto [x,z]=retailUnpackXZ(args[0]);a=x;b=z; }
        else { a=std::bit_cast<int32_t>(args[0]);b=std::bit_cast<int32_t>(args[1]); }
        return uint32_t(isqrt64(uint64_t(a*a)+uint64_t(b*b)));
    }
    case 14:   // ATAN (0x4809c7): 0x4b715a's atan2(a, b), a BAM word.
        return uint16_t(retailDirection(Fixed::raw(std::bit_cast<int32_t>(args[0])),
                                        Fixed::raw(std::bit_cast<int32_t>(args[1]))).v);
    case 16: {
        // GROUND_HEIGHT (0x480a0d): the terrain under the point, in 16.16.
        const auto [x,z]=retailUnpackXZ(args[0]);
        return uint32_t(host.groundHeight(x,z))<<16;
    }
    case 17: return retailConstructionPercent(u.remaining);   // BUILD_PERCENT_LEFT (0x480a44)
    case 18: return u.yardOpen;
    case 19: return u.buggerOff;
    case 20: return u.armored;   // ARMORED (0x480aa9: +0x10e bit 1, SET 20's 0x48b090(2))
    default: return 0;
    }
}

} // namespace ta::sim
