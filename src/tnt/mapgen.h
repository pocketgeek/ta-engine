#pragma once

// Random map generator. Produces a tak::tnt::Map (+ start positions) procedurally
// from a small parameter set, deterministically (integer-only) so a multiplayer
// client and the server referee build the BYTE-IDENTICAL map from the same seed --
// the generated terrain/features feed the hashed lockstep sim, so it must agree on
// every peer. The parameters ride inside the mapId string ("~gen1~<hex>"), which the
// lobby threads to every peer. A recipe is version 10, with the retail cliff/plateau,
// mountain and town kits; an id carrying any other version is malformed.

#include "tnt/tnt.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tak::hpi { class Vfs; }

namespace tak::mapgen {

// One of the retail worlds; also selects the terrain section art + feature palette.
// 0..4 match matchsetup faction ids (0=aramon 1=taros 2=veruna 3=zhon 4=creon);
// Volcano is Iron Plague's lava world (kingdom=volcano), which no side calls home.
enum MapType : uint8_t { Aramon = 0, Taros = 1, Veruna = 2, Zhon = 3, Creon = 4, Volcano = 5, kMapTypes };
// The world's name as an OTA kingdom and as its features/ and palettes/ folder.
const char* worldKey(uint8_t world);
// The world's name for menus, in capitals.
const char* worldTitle(uint8_t world);
// Lava, not water: retail blocks every lava cell (0xfffc), so nothing walks or sails it.
bool lavaWorld(uint8_t world);
// Whether a world has trees for the Forests amount, and hill sections for Hills (the
// volcano has neither; its Rocks are ruined Creon works).
bool worldHasForests(uint8_t world);
bool worldHasHills(uint8_t world);
// Whether the Hills amount can place anything: Maze is all walls and corridors, and
// Ports' land is a ring too narrow beside its kept-open routes for any hill section.
bool hillsPossible(uint8_t world, uint8_t layout);

enum Layout : uint8_t { Mainland = 0, Lakes = 1, Islands = 2, Maze, Ports, Riverlands, Jungle, Highlands, LavaFlows,
                        IndustrialZone, kLayouts };
const char* layoutName(uint8_t layout);
uint8_t themedLayout(uint8_t world);
bool automaticWater(uint8_t layout);

// Neutral mana spots scattered over open ground, as an area density. Max is
// the density of a home economy holding six spots (6 per 87x87-cell reserved home
// square, ~1 spot per 1262 cells); each lower level halves it.
enum ManaScatter : uint8_t { ScatterNone = 0, ScatterMinimum, ScatterMedium, ScatterHigh, ScatterMax, kScatterLevels };
const char* scatterName(uint8_t level);
// Jittered-grid pitch in cells for a level (0 = none): one candidate per pitch^2.
int scatterPitch(uint8_t level);

// Retail terrain kits a world can assemble (some worlds lack one; see kitAvailable).
enum Kit : uint8_t { KitPlateaus, KitMountains, KitTowns, kKits };
bool kitAvailable(uint8_t world, uint8_t kit);
// Layouts that supply their own high ground (Maze walls, Creon highlands) ignore the
// plateau amount; mountains and towns still apply.
bool automaticPlateaus(uint8_t layout);
// Whether a layout can be built on a world: a lava world has no Islands (no harbors),
// and Industrial Zone is the volcano's alone.
bool layoutAllowed(uint8_t world, uint8_t layout);
// The menus' four layout choices per world (slot 0..3): Mainland, Lakes, Islands (a
// lava world's Industrial Zone instead) and the world's themed layout.
uint8_t layoutForSlot(uint8_t world, int slot);
int slotOfLayout(uint8_t layout);

struct Params {
    uint16_t formatVer = 10;         // the recipe version, the only one an id may carry
    uint64_t seed = 1;
    std::string name;               // v5: optional ASCII map title, up to 24 characters
    uint8_t  mapType = Aramon;
    uint16_t widthCells = 256, heightCells = 256;   // multiples of 32, clamped
    uint8_t  players = 2;            // 2..8
    uint8_t  treeDensity = 128;      // 0..255 (few..lots)
    uint8_t  rockDensity = 96;       // 0..255
    uint8_t  manaDensity = 128;      // 0..255 requests 0..6 extra spots per player
    uint8_t  waterDensity = 96;      // mainland/lake intensity, not coverage; 0 = dry. Industrial
                                     // Zone: its cut-off lava pads (0 = none), each holding mana
    uint8_t  layout = Mainland;
    uint8_t  reliefDensity = 128;    // authored hill patch density; 0 = flat
    uint8_t  manaScatter = ScatterNone;   // ManaScatter level for neutral spots
    // Retail kits, 0 = none. All three at 0 leave the terrain without kits.
    uint8_t  plateauDensity = 96;    // cliff-edged plateaus (with walkable ramps)
    uint8_t  mountainDensity = 48;   // mountain ranges (raised on their own plateau)
    uint8_t  townDensity = 64;       // walled settlements / Zhon hut villages
};

struct Result {
    tak::tnt::Map map;
    int waterPercent = 0, reliefPatches = 0;
    int scatteredMana = 0;   // neutral spots placed (not part of any player's rounds; Industrial Zone dead ends too)
    int padMana = 0;         // Industrial Zone deposits on cut-off lava pads: flyers' only, outside scatteredMana
    int plateaus = 0, mountains = 0, towns = 0;   // kit placements (components/ranges/towns)
    int mountainPlateaus = 0;   // plateaus carrying a range (the rest are plain)
    int plateauMana = 0, manaPlateaus = 0;   // deposits on plateau tops, and plateaus holding one
    // v10: 0 = kits as requested; 1 = halved, 2 = dropped, because a fair map did not fit.
    int kitFallback = 0;
    // v3: one open-water harbor per island; cells suitable for naval construction.
    std::vector<std::pair<int, int>> harbors;
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

// The generator. Deterministic in the params + the install's prefab sections
// (coastlines are stamped from the retail Coast Sandy section TNTs read through
// the VFS -- same install => identical bytes on every peer; the MP data-hash
// agreement already pins the install).
Result generate(const Params& p, const hpi::Vfs& vfs);

// Prefabs consumed by either generator, included in multiplayer's data hash.
std::vector<std::string> assetPaths(const hpi::Vfs& vfs);

// The map sizes the menus offer, in sections (32 cells) per side.
constexpr int kSizeSteps[] = {8, 12, 16, 20, 24, 32, 40, 48, 56, 64};

// Clamp UI inputs to section multiples, 2..8 players, and layout-specific space: a side
// the layout cannot fit at this player count grows (the smaller side first, along
// kSizeSteps) until it can. Width and height are independent.
Params sanitize(Params p);

// Complete, balanced expansion rounds; independent of map area/player count.
inline int extraManaRounds(uint8_t density) { return (int(density)*6+127)/255; }

}  // namespace tak::mapgen
