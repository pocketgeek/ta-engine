#pragma once
#include "cartographer/document.h"
#include "cartographer/units.h"
#include "tnt/ota.h"
#include "tnt/tnt.h"
#include <set>
namespace tak::sim { class TypeRegistry; class NavGrid; }
namespace tak::hpi { class Vfs; }
namespace cart {
struct MapIssue {
    enum class Severity { Warning, Error };
    Severity severity=Severity::Warning;
    std::string message;
    float x=-1,z=-1; // world position; negative means no navigable location
};
struct MovementRegion {int cells=0,x=0,z=0;};
// Every cell is classified by the engine's footprint-aware nav predicate.
std::vector<MovementRegion> movementRegions(const tak::sim::NavGrid& grid,int footprint);
std::vector<MapIssue> validateTerrainResources(const tak::tnt::Map& map,const tak::hpi::Vfs& vfs);
// Shared by Check Map and the rule form; warnings permit intentional scripts.
std::vector<MapIssue> validateRuleOperands(bool action,const tak::crt::Rule& rule,
    const tak::crt::Scenario& scenario,const tak::sim::TypeRegistry& registry);
// The map's AI profiles (aiprofiles.h checkAiMap: the aiprofile= name, files that do
// not travel, each file's lines), worded for Check Map. Notes (Info) stay in the
// AI Profiles window's own Check; none of these block saving.
std::vector<MapIssue> validateAiProfiles(const AiFiles& files,const std::string& aiProfile,
    const tak::sim::TypeRegistry& registry,const tak::hpi::Vfs& vfs);
// What the map's scenario loses with the CRUSADES balance on, where the roster is
// unitscb/ alone and six standard units (monelem, monmum, tarcship, tarhel, tarnecr2,
// zonspide) do not exist. Only types `standard` has and `crusades` lacks are named
// (a type neither has is validateMap's "unknown unit type"). Warnings, by severity of
// effect: a placed unit stops a CRUSADES game from starting; custom stats are skipped
// (as retail does); a rule naming one creates nothing, or counts other units instead.
// Nothing when the install has no Crusades balance (the two registries agree).
std::vector<MapIssue> validateCrusadesRoster(const tak::crt::Scenario& scenario,
    const std::vector<PlacedUnit>& units,const tak::sim::TypeRegistry& standard,
    const tak::sim::TypeRegistry& crusades);
// Observational: builds a private engine world; never modifies the document.
std::vector<MapIssue> validateMap(const tak::tnt::Map& map,
    const tak::tnt::Scenario& metadata,const tak::crt::Scenario& scenario,
    const std::vector<PlacedUnit>& units,const std::set<std::string>& useOnly,
    const tak::sim::TypeRegistry& registry,const tak::hpi::Vfs& vfs);
}
