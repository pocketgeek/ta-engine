#pragma once

#include <string>
#include <vector>

namespace tak::tnt {

// A map's .ota scenario "GlobalHeader" -- metadata + start positions, the TDF
// text file Cartographer writes alongside the .tnt (writer RE'd from
// Cartographer.exe 0x41bfd0; see docs/cartographer-port.md). Placed units and
// trigger rules live in the separate binary .crt, NOT here.

struct StartPos {
    int number = 1;      // StartPos<number> (1-based)
    int xpos = 0, zpos = 0;   // in CELLS (16px); world px = *16
};

struct Scenario {
    // Cartographer emits this constant; community tools vary, so it round-trips.
    std::string copyright =
        "Copyright 1998 Cavedog Entertainment. All rights reserved.";
    std::string missionName;
    std::string missionDescription;
    std::string generatorRecipe;      // optional TAK engine extension, independent of the description
    std::string kingdom;              // lowercase world: aramon/taros/veruna/zhon
    int sizeW = 0, sizeH = 0;         // in Units (cells>>5); OTA "size = W x H"
    std::string useOnlyUnits;         // "<name>.tdf", or empty when unrestricted
    bool hasScenario = false;
    std::string mapType = "Network 1";
    std::string aiProfile = "DEFAULT";
    std::vector<StartPos> starts;

    // Parse an .ota (GlobalHeader TDF). Missing fields keep their defaults.
    static Scenario parse(const std::string& text);
    // Retail field layout (CRLF, tab indent, key order), with optional TAK
    // extensions for generator recipes and otherwise unrepresentable text.
    std::string write() const;
};

} // namespace tak::tnt
