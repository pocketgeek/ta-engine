#pragma once
#include "tnt/tnt.h"
#include "cartographer/units.h"
#include <string>
#include <atomic>
namespace tak::sim {class TypeRegistry;}
namespace tak::hpi {class Vfs;}
namespace cart {
enum class OverlayKind { Movement, Buildability, WaterDepth, Slope };
struct TerrainOverlay {
    int width=0,height=0;
    std::vector<uint8_t> rgba;
    std::string legend;
};
// Snapshot analysis using a private engine world. No simulation or map mutation.
TerrainOverlay terrainOverlay(const tak::tnt::Map& map,const tak::sim::TypeRegistry& registry,
    const tak::hpi::Vfs& vfs,OverlayKind kind,const std::string& unitType,const std::vector<PlacedUnit>& occupants={},
    const std::atomic_bool* cancel=nullptr);
}
