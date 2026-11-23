#include "ai/classictypes.h"

#include <algorithm>
#include <cstring>

#include "sim/sim.h"

namespace tak::ai {

void classicYardMapFlags(const std::string& yardMap, int footX, int footZ,
                         bool& manaSite, bool& lodestoneClass) {
    manaSite = lodestoneClass = false;
    // The cell characters the loader knows; every other byte is passed over.
    static constexpr const char* kCells = ".CcOoSYyfw";
    const int cells = std::max(0, footX) * std::max(0, footZ);
    size_t i = 0;
    for (int cell = 0; cell < cells && i < yardMap.size();) {
        const char c = yardMap[i];
        if (c == '\0' || !std::strchr(kCells, c)) { ++i; continue; }
        if (c == 'S') {
            manaSite = true;
            if (cell == 0) lodestoneClass = true;
        }
        ++cell;
        if (i + 1 < yardMap.size()) ++i;   // the last character fills the rest
    }
}

ClassicTypeInfo ClassicTypeInfo::from(const tak::sim::UnitType& t, const tak::sim::TypeRegistry* registry) {
    ClassicTypeInfo info;
    info.armed = t.classicFbi.weaponBlocks != 0;
    info.builder = t.isBuilder;
    info.commander = t.commander;
    info.canFly = t.canFly;
    info.floater = t.floater;
    info.minWaterDepth = t.landMinWaterDepth;
    info.bmcode = t.classicFbi.bmcode;
    info.structure = t.isStructure();
    info.isFeature = t.classicFbi.isFeature;
    if (info.bmcode == 0)
        classicYardMapFlags(t.classicFbi.yardMap, t.footX, t.footZ, info.manaSite, info.lodestoneClass);
    info.cost = t.buildCost != 0 ? t.buildCost : t.buildTime;
    info.footX = t.footX;
    info.footZ = t.footZ;
    info.buildDistance = t.buildDist;
    info.maxDamage = t.maxHp;
    info.sight = t.sight;
    info.firstWeaponRange = t.weapons.empty() ? 0 : t.weapons.front().range;
    // Menus are canbuild/<unitname>/ (a variant such as tarnecr2 has none of its own).
    info.menu = registry &&
                !registry->buildable(t.classicFbi.unitName.empty() ? t.id : t.classicFbi.unitName).empty();
    return info;
}

tak::sim::RetailBuildPriority ClassicTypeInfo::priorityInputs(int count, int completed, int32_t limit) const {
    tak::sim::RetailBuildPriority in;
    in.flags = (armed ? 0x10000u : 0u) | (builder ? 0x100u : 0u) | (isFeature ? 0x1000000u : 0u);
    in.secondaryFlags = manaSite ? 0x80000000u : 0u;
    in.count = int16_t(count);
    in.completed = int16_t(completed);
    in.weapon = minWaterDepth;
    in.desired = limit;
    in.cost = cost;
    return in;
}

}  // namespace tak::ai
