#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace ta::sim {

// Kingdoms kept group records (player+0x84) whose aggregates paced and
// re-formed a group's members (its group checks); TotalA.exe keeps none, so
// they and the checks are gone. What is left: the member classes and the
// formation numbering Legion's own formations use (legionFormationGroup,
// legionStandbyCheck), the octagonal distance, and BeCarried's row flags.
enum class RetailGroupClass : uint8_t { Ground=0, Boat=1, Flyer=2 };

struct RetailGroupPoint { int32_t x=0, y=0, z=0; };

// BeCarried's row flags (descriptor 5eb69f, +0x11 at 5eb6b0): 0x24, kept
// through a clear (4) and neither moving (0x1000000) nor paced (0x2000000).
// A passenger's mission while it rides (51b5a0 -> 4d49f0 puts it in front).
inline constexpr uint32_t kRetailBeCarriedFlags = 0x24;

// 535e90: octagonal distance, max + min/4 with arithmetic shift.
constexpr int32_t retailOctDistance(int32_t dx,int32_t dz) {
    const int32_t x=dx<0?-dx:dx, z=dz<0?-dz:dz;
    return x>z ? x+(z>>2) : z+(x>>2);
}

struct RetailGroupMember {
    RetailGroupClass kind=RetailGroupClass::Ground;
    RetailGroupPoint position;   // integer world position
    int32_t area=0;              // footprint X * Z
    bool mover=true;
};

// The group a unit belongs to (unit+0xc8) and whether it is a formation
// (record +8). Retail has one record per number, written by the
// console's CreateSquad (48e170 -> 522170 -> 50b4f0 for the members, 50b700
// for +8): "CreateSquad N" rebuilds group N from the selection as a plain
// group (+8 clear), "CreateSquad N f" as a formation (+8 set), each replacing
// the other. Only a formation is ever aggregated, made active or paced; a
// plain group behaves as no group. squad is Unit::squad: +N a Ctrl group
// (plain group N), -N a formation (group N with +8). A computer player's
// units are in its AI's squads instead (aiGroup, Unit::aiGroup; the AI's
// set-squad 50b4f0 overwrites unit+0xc8), whose +8 the AI sets through
// set-active 50b700: a strike squad while it attacks (state 2), a VTOL squad
// while it has units (aiFormation, World::aiFormation).
struct RetailGroupSlot { int group=0; bool formation=false; };
constexpr RetailGroupSlot retailGroupSlot(int squad,int aiGroup=0,bool aiFormation=false) {
    if (aiGroup>0) return {aiGroup,aiFormation};
    if (squad>0) return {squad,false};
    if (squad<0) return {-squad,true};
    return {};
}

// The standbys whose group check (Standby 407779, VTOL_Standby 417527,
// VTOL_LandIfCan 416eda) Legion's counterpart stands in for.
enum class RetailStandbyCheck : uint8_t { VtolStandby, Standby, LandIfCan };

// Legion's counterpart of the standbys' group check (ours, not retail; see
// World::legionStandbyCheck): retail's tolerance, 51d1e0(unit, 1, 2) for a
// ground member, measured from the member's own Legion formation slot instead
// of the group centre. dx/dz (px) run from the slot to the member. Level 2,
// relaxed to 3 when no member of its class moves (51d1e0's empty moving
// subset); the area is the moving members' plus its own while some move (the
// standby's 0x4000000 counts the member in), else every member's.
struct LegionSlotClass { int32_t area=0, movingArea=0, moving=0; };
constexpr bool legionStandbyOutOfSlot(const LegionSlotClass& c,int32_t ownArea,int32_t dx,int32_t dz) {
    int32_t radius=c.moving>0 ? (c.movingArea+ownArea)/2 : c.area;   // level 2 halves; level 3 is the area
    radius=std::max(radius,2);
    const int32_t distance=retailOctDistance(dx,dz)/16;
    return distance*distance/4>radius;
}

} // namespace ta::sim
