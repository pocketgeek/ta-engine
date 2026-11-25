#pragma once

// The display VM's GET / GET_UNIT_VALUE answers (GameView::registerUnit's
// onGet): TotalA.exe's GET (0x480770), the same routine the simulation's host
// runs (sim/retailscriptget.h: World::ScriptHost::get), fed from the pinned
// render snapshot instead of live world state. The unit's words are the sim's
// own (UnitR::scriptWords, World::scriptGetWords: ACTIVATION is the script's
// activation, BUSY its BUSY, BUILD_PERCENT_LEFT the remaining work); UNIT_XZ /
// UNIT_Y / UNIT_HEIGHT read the unit the argument names; PIECE_XZ / PIECE_Y
// pose the display VM's own pieces on the unit as 0x43e060 does; GROUND_HEIGHT
// reads the map. Ids outside 1..20 answer 0 in both hosts.

#include "client/renderframe.h"
#include "sim/retailheight.h"
#include "sim/retailpiecepose.h"
#include "sim/retailscriptget.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ta::client {

// What the display host needs beyond the unit: another unit by id (the render
// snapshot), the unit's piece poses and the map's height grid.
template<class FindUnit>
struct DisplayGetHost {
    const UnitR& unit;
    FindUnit find;                                   // const UnitR*(int id)
    std::span<const ta::cob::RetailPiece> poses;     // the display VM's pieces
    const std::vector<uint8_t>* heights = nullptr;   // the map's height cells
    int mapW = 0, mapH = 0;
    std::array<int32_t,3> piecePoint(int32_t piece) const {
        std::array<int32_t,3> point = unit.worldPosition;
        // 0x43df16 -> 0x43e01d: a piece out of range is the unit origin.
        if (!unit.type || piece < 0 || size_t(piece) >= poses.size()) return point;
        const auto offset = ta::sim::retailPieceOrigin(unit.type->productionModel, poses, piece,
            unit.scriptWords.heading, unit.bodyPitch, unit.bodyRoll);
        for (size_t axis = 0; axis < 3; ++axis)
            point[axis] = int32_t(std::clamp<int64_t>(int64_t(point[axis]) + offset[axis], INT32_MIN, INT32_MAX));
        return point;
    }
    bool unitAt(int id, ta::sim::RetailGetOther& out) const {
        const UnitR* other = find(id);
        if (!other || !other->type || !other->alive()) return false;
        out = {other->worldPosition[0], other->worldPosition[1], other->worldPosition[2], other->type->modelTop};
        return true;
    }
    int groundHeight(int32_t x, int32_t z) const {
        if (!heights || heights->empty()) return 0;
        return ta::sim::retailTerrainHeight(x, z, mapW, mapH,
            [&](int cx, int cz) { return int((*heights)[size_t(cz) * size_t(mapW) + size_t(cx)]); });
    }
};

template<class FindUnit>
inline int32_t displayScriptGet(int32_t id, const std::vector<int32_t>& args, DisplayGetHost<FindUnit>& host) {
    std::array<uint32_t,4> words{};
    for (size_t i = 0; i < words.size() && i < args.size(); ++i) words[i] = uint32_t(args[i]);
    struct Queries {
        DisplayGetHost<FindUnit>& h;
        std::array<int32_t,3> piecePoint(int32_t piece) { return h.piecePoint(piece); }
        bool unit(int id, ta::sim::RetailGetOther& out) { return h.unitAt(id, out); }
        int groundHeight(int32_t x, int32_t z) { return h.groundHeight(x, z); }
    } queries{host};
    return int32_t(ta::sim::retailScriptGet(id, words, host.unit.scriptWords, queries));
}

} // namespace ta::client
