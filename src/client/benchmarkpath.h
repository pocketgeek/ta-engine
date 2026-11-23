#pragma once
// The pathfinders Settings -> Benchmark offers. The pick is remembered in
// Settings::benchmarkPathfinding and sent as the benchmark room's own pathfindingMode;
// the results and the stats panel name the mode the run's world really used.
#include "sim/pathmode.h"

namespace tak {

// The screen's PATHFINDING choices, left to right.
inline constexpr sim::PathfindingMode kBenchmarkPaths[2] = {sim::PathfindingMode::Retail,
                                                            sim::PathfindingMode::Legion};

constexpr const char* benchmarkPathLabel(sim::PathfindingMode mode) {
    return sim::isLegionPathfinding(mode) ? "LEGION" : "RETAIL";
}

// The screen's SPEED choices, left to right: game-speed levels (Settings::benchmarkSpeed).
// +100 (11x) runs only in a benchmark room (net::kMaxBenchmarkGameSpeed).
inline constexpr int kBenchmarkSpeeds[3] = {0, 10, 100};

constexpr const char* benchmarkSpeedLabel(int level) { return level >= 100 ? "+100" : level > 0 ? "+10" : "NORMAL"; }
// A saved or passed level as one of the choices.
constexpr int benchmarkSpeedChoice(int level) { return level >= 100 ? 100 : level > 0 ? 10 : 0; }

}  // namespace tak
