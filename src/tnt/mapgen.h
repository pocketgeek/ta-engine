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

// One of the retail worlds; also selects the terrain section art + feature palette.
// Matches matchsetup faction ids: 0=aramon 1=taros 2=veruna 3=zhon 4=creon.
enum MapType : uint8_t { Aramon = 0, Taros = 1, Veruna = 2, Zhon = 3, Creon = 4, kMapTypes };

struct Params {
    uint16_t formatVer = 2;          // v2 split doodads into tree/rock + added relief
    uint64_t seed = 1;
    uint8_t  mapType = Aramon;
    uint16_t widthCells = 256, heightCells = 256;   // multiples of 32, clamped
    uint8_t  players = 2;            // 2..8
    uint8_t  treeDensity = 128;      // 0..255 (few..lots)
    uint8_t  rockDensity = 96;       // 0..255
    uint8_t  manaDensity = 128;      // 0..255
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

// The generator. Deterministic in the params + the install's prefab sections
// (coastlines are stamped from the retail Coast Sandy section TNTs read through
// the VFS -- same install => identical bytes on every peer; the MP data-hash
// agreement already pins the install).
Result generate(const Params& p, const hpi::Vfs& vfs);

// Clamp raw UI inputs to supported ranges (even cells, 2..8 players, sane size).
Params sanitize(Params p);

}  // namespace ta::mapgen
