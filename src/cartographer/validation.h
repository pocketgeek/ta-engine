#pragma once
#include "cartographer/document.h"
#include "cartographer/units.h"
#include "tnt/ota.h"
#include "tnt/tnt.h"
#include <set>
namespace ta::sim { class TypeRegistry; class NavGrid; }
namespace ta::hpi { class Vfs; }
namespace cart {
struct MapIssue {
    enum class Severity { Warning, Error };
    Severity severity=Severity::Warning;
    std::string message;
    float x=-1,z=-1; // world position; negative means no navigable location
};
struct MovementRegion {int cells=0,x=0,z=0;};
// Every cell is classified by the engine's footprint-aware nav predicate.
std::vector<MovementRegion> movementRegions(const ta::sim::NavGrid& grid,int footprint);
std::vector<MapIssue> validateTerrainResources(const ta::tnt::Map& map,const ta::hpi::Vfs& vfs);
// Shared by Check Map and the rule form; warnings permit intentional scripts.
std::vector<MapIssue> validateRuleOperands(bool action,const ta::crt::Rule& rule,
    const ta::crt::Scenario& scenario,const ta::sim::TypeRegistry& registry);
// The map's AI profiles (aiprofiles.h checkAiMap: the aiprofile= name, files that do
// not travel, each file's lines), worded for Check Map. Notes (Info) stay in the
// AI Profiles window's own Check; none of these block saving.
std::vector<MapIssue> validateAiProfiles(const AiFiles& files,const std::string& aiProfile,
    const ta::sim::TypeRegistry& registry,const ta::hpi::Vfs& vfs);
// Observational: builds a private engine world; never modifies the document.
std::vector<MapIssue> validateMap(const ta::tnt::Map& map,
    const ta::tnt::Scenario& metadata,const ta::crt::Scenario& scenario,
    const std::vector<PlacedUnit>& units,const std::set<std::string>& useOnly,
    const ta::sim::TypeRegistry& registry,const ta::hpi::Vfs& vfs);
}
