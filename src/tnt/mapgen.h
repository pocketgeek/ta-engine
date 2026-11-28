#pragma once

// Random map generator. Produces a ta::tnt::Map (+ start positions) procedurally
// from a small parameter set, deterministically, so a multiplayer client and the
// server referee build the BYTE-IDENTICAL map from the same seed -- the generated
// terrain/features feed the hashed lockstep sim, so it must agree on every peer.
// The parameters ride inside the mapId string ("~gen1~<hex>"), which the lobby
// threads to every peer. A recipe is version 10; an id carrying any other version
// is malformed. The maps are assembled from TA's terrain sections (mapgen_ta.cpp).

#include "tnt/tnt.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ta::hpi { class Vfs; }

namespace ta::mapgen {

// Total Annihilation's worlds, in mapgen_ta.cpp's section-folder order.
enum MapType : uint8_t { Green = 0, Mars = 1, Archipelago = 2, Metal = 3, Moon = 4, Lava = 5, kMapTypes };
// The world's name as its sections/ folder.
const char* worldKey(uint8_t world);
// The world's name for menus, in capitals.
const char* worldTitle(uint8_t world);
// Lava, not water: retail blocks every lava cell (0xfffc), so nothing walks or sails it.
bool lavaWorld(uint8_t world);

// The recipe. Every field rides in the encoded id (its layout is fixed by the recipe
// version); the TA generator reads seed, mapType, the size, players and
// reliefDensity. The rest are recipe bytes kept so every version-10 id still parses.
struct Params {
    uint16_t formatVer = 10;         // the recipe version, the only one an id may carry
    uint64_t seed = 1;
    std::string name;               // optional ASCII map title, up to 24 characters
    uint8_t  mapType = Green;
    uint16_t widthCells = 256, heightCells = 256;   // multiples of 32, clamped
    uint8_t  players = 2;            // 2..kMaxPlayers
    uint8_t  treeDensity = 128;      // TREES: woods of the world's trees; 0 = none
    uint8_t  rockDensity = 96;       // ROCKS: the world's reclaimable rocks; 0 = none
    uint8_t  manaDensity = 128;      // METAL: deposits by each start (2..6) and clusters over the map
    uint8_t  waterDensity = 96;      // (unused: the sections join at one level and never dip below it)
    uint8_t  layout = 0;             // (unused)
    uint8_t  reliefDensity = 128;    // HILLS: how much of the map is laid with sections; 0 = fewest
    uint8_t  manaScatter = 0;        // (unused)
    uint8_t  plateauDensity = 96;    // (unused)
    uint8_t  mountainDensity = 48;   // (unused)
    uint8_t  townDensity = 64;       // (unused)
};

struct Result {
    ta::tnt::Map map;
    std::vector<std::pair<int, int>> starts;   // start positions in 16px CELL coords (x, z)
};

// mapId carrier. isGeneratedMapId recognises the "~gen1~" magic; encode/decode pack
// the Params to/from the hex payload. friendlyLabel is what the UI shows.
bool        isGeneratedMapId(const std::string& id);
std::string encodeMapId(const Params& p);
// An id is well formed only with the current recipe version (Params::formatVer) and
// that version's exact payload. decodeMapId gives sane defaults for a malformed id (for
// labels and menus); parseMapId throws std::runtime_error, for every path that builds
// the map, so a malformed id is refused rather than built as some other map.
Params      decodeMapId(const std::string& id);
Params      parseMapId(const std::string& id);
std::string friendlyLabel(const Params& p);

// The generator: generateTa on the sanitized params. Deterministic in the params +
// the install's terrain sections (the MP data-hash agreement pins the install).
// Throws std::runtime_error when the data has no TA terrain sections.
Result generate(const Params& p, const hpi::Vfs& vfs);
// Total Annihilation: random maps from the world's terrain sections (mapgen_ta.cpp).
bool taSectionsAvailable(const hpi::Vfs& vfs);
Result generateTa(const Params& p, const hpi::Vfs& vfs);
const char* taPlanetFor(uint8_t world);

// The files the generator reads (terrain sections, the palette), included in
// multiplayer's data hash.
std::vector<std::string> assetPaths(const hpi::Vfs& vfs);

// The map sizes the menus offer, in sections (32 cells) per side.
constexpr int kSizeSteps[] = {8, 12, 16, 20, 24, 32, 40, 48, 56, 64};

// The most start positions a random map gets: one per player (sim::kMaxPlayers, retail's ten).
constexpr int kMaxPlayers = 10;

// Clamp UI inputs: a known world, section multiples (8..64 per side), 2..kMaxPlayers
// players, a safe name.
Params sanitize(Params p);

}  // namespace ta::mapgen
