#pragma once

// Random map generator. Produces a ta::tnt::Map (+ start positions) procedurally
// from a small parameter set, deterministically (integer-only) so a multiplayer
// client and the server referee build the BYTE-IDENTICAL map from the same seed --
// the generated terrain/features feed the hashed lockstep sim, so it must agree on
// every peer. The parameters ride inside the mapId string ("~gen1~<hex>"), which the
// lobby already threads to both peers, so no net-protocol change is needed.

#include "tnt/tnt.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ta::hpi { class Vfs; }

namespace ta::mapgen {

// One of TA's terrain worlds; selects the tile art harvested from that world's
// prefab sections, its authoring sea/land levels, and its feature palette.
//
// These are the six worlds that ship SECTIONS in worlds.hpi -- the generator needs
// tiles, and a world with no section art has none to give. (TA declares many more
// worlds for FEATURES alone: urban, wetdesert, lush, slate, ice, acid, crystal and
// others.) The numbering is part of the "~gen1~" map id, so it is fixed here on
// purpose rather than discovered: a discovered order would make the same id mean
// different worlds on two installs, and both peers build the map from the id.
enum MapType : uint8_t {
    Archipelago = 0, GreenWorld = 1, Lava = 2, Mars = 3, Metal = 4, Moon = 5, kMapTypes
};

struct Params {
    uint16_t formatVer = 2;          // v2 split doodads into tree/rock + added relief
    uint64_t seed = 1;
    uint8_t  mapType = Archipelago;
    uint16_t widthCells = 256, heightCells = 256;   // multiples of 32, clamped
    uint8_t  players = 2;            // 2..8
    uint8_t  treeDensity = 128;      // 0..255 (few..lots)
    uint8_t  rockDensity = 96;       // 0..255
    uint8_t  metalDensity = 128;     // 0..255 (metal patches)
    uint8_t  waterDensity = 96;      // 0..255 (how much of the map is water)
    uint8_t  reliefDensity = 128;    // 0..255 (plateaus + ramps: 0 = flat)
};

struct Result {
    ta::tnt::Map map;
    std::vector<std::pair<int, int>> starts;   // start positions in 16px CELL coords (x, z)
};

// mapId carrier. isGeneratedMapId recognises the "~gen1~" magic; encode/decode pack
// the Params to/from the hex payload. friendlyLabel is what the UI shows.
bool        isGeneratedMapId(const std::string& id);
std::string encodeMapId(const Params& p);
Params      decodeMapId(const std::string& id);     // sane defaults on a malformed id
std::string friendlyLabel(const Params& p);

// The generator. Deterministic in the params + the install's own data: the tile
// art is harvested from the world's .sct prefab sections and the feature palette
// is read from features/<world>/, both in sorted order, so the same install
// produces identical bytes on every peer (and the MP data-hash agreement already
// pins the install). Note the tile GRAPHICS are not part of the sim hash -- only
// heights and features are -- so art that differed could never desync a match.
Result generate(const Params& p, const hpi::Vfs& vfs);

// Clamp raw UI inputs to supported ranges (even cells, 2..8 players, sane size).
Params sanitize(Params p);

// The world's name as the data spells it (sections/<name>/, features/<name>/),
// lowercased. Out-of-range yields the first world.
const char* worldName(uint8_t mapType);
// The reverse: a world name (any case) to its MapType, or Archipelago if this is
// not one of the six that ship sections.
uint8_t worldType(const std::string& name);
// That world's authoring sea level and land plateau, measured from the shipped
// maps. Exposed so the editor's blank-map path builds the same terrain the
// generator would rather than carrying its own copy of the numbers.
void worldLevels(uint8_t mapType, uint8_t& sea, uint8_t& land);

}  // namespace ta::mapgen
