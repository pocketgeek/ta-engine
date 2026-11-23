#pragma once

// Cartographer's placed-unit layer. Units live in the map's binary .crt, read
// and written through the SHARED tak::crt module (the same one takclient/
// takserver link). This holds the editor's working view (PlacedUnit, in map
// pixels) plus load/convert/save helpers that preserve the parts of the .crt
// the unit tool doesn't touch (custom types, trigger rules, regions).

#include "crt/crt.h"

#include <string>
#include <vector>

namespace tak::hpi { class Vfs; }
namespace tak::sim { class TypeRegistry; }

namespace cart {

struct PlacedUnit {
    std::string type;      // unit FBI name (objectName), UPPERCASE as in the .crt
    int player = 0;        // player id / start slot (0..8)
    float x = 0, z = 0;    // footprint center in map pixels
    int health = 100;      // %  0..100
    int armor = 100;       // %  0..1000
    int weapon = 100;      // %  0..1000
    int veteran = 0;       //    0..9
    float angle = 0;       // degrees 0..359
    std::string name;      // optional display name
    int vertical = 200;    // retained CRT field; native placement ignores it
    int footX = 1, footZ = 1; // coordinate conversion metadata, not serialized
};

// Parse a map's .crt in full (units + rules + regions + custom types). Empty
// Scenario (version 0) if the .crt is absent or invalid.
tak::crt::Scenario loadScenario(const tak::hpi::Vfs& vfs, const std::string& crtPath);

// The editor's working unit list, from a parsed Scenario (cells -> map pixels).
std::vector<PlacedUnit> toPlaced(const tak::crt::Scenario& s, const tak::sim::TypeRegistry* registry = nullptr);

// Serialize the map's .crt: replace `base`'s unit list with `units` (map pixels
// -> cells), keeping base's custom types, trigger rules, and regions intact.
std::vector<uint8_t> saveScenario(tak::crt::Scenario base,
                                  const std::vector<PlacedUnit>& units);

// Sorted UPPERCASE unit-type names from every units/*.fbi (for the palette/combo).
std::vector<std::string> unitTypeNames(const tak::hpi::Vfs& vfs);

// Use-only unit restriction. The map's <name>.tdf (referenced by the OTA
// `useonlyunits=<name>.tdf`) lists the allowed types, one `[TYPE] {}` section
// each. loadUseOnly returns them UPPERCASE; writeUseOnly emits the retail
// `[TYPE]\t{}` CRLF format. An empty list means "no restriction".
std::vector<std::string> loadUseOnly(const tak::hpi::Vfs& vfs, const std::string& tdfPath);
std::string writeUseOnly(const std::vector<std::string>& types);

} // namespace cart
