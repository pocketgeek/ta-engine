#pragma once
#include <cstdint>
namespace tak::sim {
// Stable saved/network/replay identities. Values 1-3 are reserved and invalid.
enum class PathfindingMode : uint8_t { Retail=0,Legion=4 };
// Group-planned, clearance-exact ground navigation (src/sim/legion*).
constexpr bool isLegionPathfinding(PathfindingMode mode) {return mode==PathfindingMode::Legion;}
constexpr bool validPathfindingMode(uint8_t mode) {
    return mode==uint8_t(PathfindingMode::Retail)||mode==uint8_t(PathfindingMode::Legion);
}
constexpr const char* pathfindingModeName(PathfindingMode mode) {
    switch(mode) {
    case PathfindingMode::Retail:return "Retail";
    case PathfindingMode::Legion:return "Legion";
    }
    return "Unknown";
}
constexpr PathfindingMode cyclePathfindingMode(PathfindingMode mode,int) {
    return mode==PathfindingMode::Retail?PathfindingMode::Legion:PathfindingMode::Retail;
}
}
