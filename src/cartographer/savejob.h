#pragma once
#include "cartographer/document.h"
#include <atomic>
#include <future>
#include <memory>
namespace cart {
struct SaveSnapshot {
    ta::tnt::Map map;
    ta::tnt::Scenario metadata;
    ta::crt::Scenario scenario;
    std::vector<PlacedUnit> units;
    std::set<std::string> useOnly;
    AiFiles aiFiles;
    std::filesystem::path installMaps;   // loose AI profiles are refused in (or under) it
    const ta::hpi::Vfs* vfs=nullptr; // immutable asset source; caller retains it until completion
    std::filesystem::path destination;
    bool bundle=true, rebuildMinimaps=false;
};
struct SaveProgress {
    enum Phase { Preparing, Minimap, Serializing, Writing, Finished };
    std::atomic<Phase> phase{Preparing};
    std::atomic_bool cancel{false};
};
struct SaveResult {bool ok=false;std::string error;};
// The worker owns the document snapshot and touches no live editor state or SDL
// objects. Its borrowed read-only asset source must outlive the returned future.
std::future<SaveResult> saveInBackground(SaveSnapshot snapshot,std::shared_ptr<SaveProgress> progress);
}
