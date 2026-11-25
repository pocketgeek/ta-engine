#pragma once

// A surface carrier's own TransportPickup / TransportDrop, for the engine-built
// carriers of the transport fixtures (TA's armtship / armthovr scripts cut to
// their rules). TotalA.exe has no script-less unit: GROUND_PICKUP and
// GROUND_UNLOAD only call the script (0x40680f, 0x4069bf), which decides with
// BoomCalc whether the boom reaches, holds BUSY while it works and attaches or
// drops through ATTACH_UNIT / DROP_UNIT (0x481340, 0x4813b0).
//
// Two pieces: `base` (piece 0, the turret at the hull's origin) and `link`
// (piece 1, its child, where a dropped passenger rides). Both scripts measure
// the boom as armtship's BoomCalc does, get XZ_HYPOT(get PIECE_XZ(base) - xz),
// against `reach` px; out of reach they do nothing. TransportPickup(unit):
// BUSY, `workMs`, the attach to no piece (-1), BUSY off. TransportDrop(unit,
// xz): BUSY, the boom turned (XZ_ATAN) and run out (XZ_HYPOT) at once, the
// passenger on the link, `workMs`, DROP_UNIT, the boom back, BUSY off.

#include "cob/cob.h"
#include "sim/sim.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace ta::testfixture {

inline std::shared_ptr<const cob::File> carrierScript(int reach,int workMs=500) {
    constexpr uint32_t push=0x10021001,local=0x10021002,sub=0x10032000,getArgs=0x10043000,
        lessEqual=0x10052000,jumpIfFalse=0x10066000,set=0x10082000,sleep=0x10013000,
        attach=0x10083000,drop=0x10084000,ret=0x10065000,moveNow=0x1000b000,turnNow=0x1000c000,
        createLocal=0x10022000;
    auto f=std::make_shared<cob::File>();
    f->pieces={"base","link"};
    auto& c=f->code;
    const auto get=[&](uint32_t id,auto&& firstArg) {   // get ID(first, 0, 0, 0)
        c.insert(c.end(),{push,id});firstArg();
        c.insert(c.end(),{push,0,push,0,push,0,getArgs});
    };
    const auto pieceXZ=[&] { get(7,[&]{c.insert(c.end(),{push,0});}); };
    const auto pickupOffset=[&] {   // get UNIT_XZ(unit) - get PIECE_XZ(base)
        get(9,[&]{c.insert(c.end(),{local,0});});pieceXZ();c.push_back(sub);
    };
    const auto dropOffset=[&] {     // get PIECE_XZ(base) - xz
        pieceXZ();c.insert(c.end(),{local,1,sub});
    };
    const auto busy=[&](uint32_t on) { c.insert(c.end(),{push,6,push,on,set}); };
    const auto reachTest=[&](auto&& offset) -> size_t {   // returns the jump's operand slot
        get(13,offset);
        c.insert(c.end(),{push,uint32_t(reach)<<16,lessEqual,jumpIfFalse,0});
        return c.size()-1;
    };
    // TransportPickup(unit)
    f->scripts.push_back({"TransportPickup",uint32_t(c.size())});
    size_t out=reachTest(pickupOffset);
    busy(1);
    c.insert(c.end(),{push,uint32_t(workMs),sleep});
    c.insert(c.end(),{local,0,push,0xffffffffu,push,0,attach});
    busy(0);
    c[out]=uint32_t(c.size());
    c.insert(c.end(),{push,0,ret});
    // TransportDrop(unit, xz)
    f->scripts.push_back({"TransportDrop",uint32_t(c.size())});
    // The engine exposes one argument word (0x4b0a70, count 1); the point in
    // the second is kept as a local before anything is pushed over it.
    c.push_back(createLocal);
    out=reachTest(dropOffset);
    busy(1);
    get(12,dropOffset);c.insert(c.end(),{turnNow,0,1});    // turn base to y-axis XZ_ATAN(offset) now
    get(13,dropOffset);c.insert(c.end(),{moveNow,1,2});    // move link to z-axis XZ_HYPOT(offset) now
    c.insert(c.end(),{local,0,push,1,push,0,attach});        // the passenger rides the link
    c.insert(c.end(),{push,uint32_t(workMs),sleep});
    c.insert(c.end(),{local,0,drop});
    c.insert(c.end(),{push,0,moveNow,1,2,push,0,turnNow,0,1});
    busy(0);
    c[out]=uint32_t(c.size());
    c.insert(c.end(),{push,0,ret});
    return f;
}

// Give an engine-built carrier type the script and its two-piece model.
inline void giveCarrierScript(sim::UnitType& t,int reach,int workMs=500) {
    t.productionScript=carrierScript(reach,workMs);
    t.productionModel.clear();
    sim::RetailModelPiece base;base.scriptPiece=0;
    sim::RetailModelPiece link;link.parent=0;link.scriptPiece=1;
    t.productionModel={base,link};
}

} // namespace ta::testfixture
