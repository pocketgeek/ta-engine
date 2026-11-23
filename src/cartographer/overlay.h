#pragma once
#include "tnt/tnt.h"
#include "cartographer/units.h"
#include <string>
#include <atomic>
namespace ta::sim {class TypeRegistry;}
namespace ta::hpi {class Vfs;}
namespace cart {
enum class OverlayKind { Movement, Buildability, WaterDepth, Slope };
struct TerrainOverlay {
    int width=0,height=0;
    std::vector<uint8_t> rgba;
    std::string legend;
};
// Snapshot analysis using a private engine world. No simulation or map mutation.
TerrainOverlay terrainOverlay(const ta::tnt::Map& map,const ta::sim::TypeRegistry& registry,
    const ta::hpi::Vfs& vfs,OverlayKind kind,const std::string& unitType,const std::vector<PlacedUnit>& occupants={},
    const std::atomic_bool* cancel=nullptr);
}
