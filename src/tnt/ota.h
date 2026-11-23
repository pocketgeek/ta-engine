#pragma once

#include <string>
#include <vector>

namespace ta::tnt {

// A map's .ota scenario "GlobalHeader" -- metadata + start positions, the TDF
// text file Cartographer writes alongside the .tnt (writer RE'd from
// Cartographer.exe 0x41bfd0; see docs/cartographer-port.md). Placed units and
// trigger rules live in the separate binary .crt, NOT here.

struct StartPos {
    int number = 1;      // StartPos<number> (1-based)
    int xpos = 0, zpos = 0;   // in CELLS (16px); world px = *16 (TA's OTA stores pixels)
};

struct Scenario {
    // Cartographer emits this constant; community tools vary, so it round-trips.
    std::string copyright =
        "Copyright 1998 Cavedog Entertainment. All rights reserved.";
    std::string missionName;
    std::string missionDescription;
    std::string generatorRecipe;      // optional TA engine extension, independent of the description
    std::string kingdom;              // the world: TA's planet= (Mars, Lush, Metal...), else TAK kingdom=
    // Total Annihilation [GlobalHeader] / [Schema 0] economy and environment.
    int tidalStrength = 0, solarStrength = 0, minWindSpeed = 0, maxWindSpeed = 2000, gravity = 112;
    int lavaWorld = 0, killMul = 50;
    int surfaceMetal = 3, mohoMetal = 40;
    int humanMetal = 1000, computerMetal = 1000, humanEnergy = 1000, computerEnergy = 1000;
    int sizeW = 0, sizeH = 0;         // in Units (cells>>5); OTA "size = W x H"
    std::string useOnlyUnits;         // "<name>.tdf", or empty when unrestricted
    bool hasScenario = false;
    std::string mapType = "Network 1";
    std::string aiProfile = "DEFAULT";
    std::vector<StartPos> starts;

    // Parse an .ota (GlobalHeader TDF). Missing fields keep their defaults.
    static Scenario parse(const std::string& text);
    // Retail field layout (CRLF, tab indent, key order), with optional TA
    // extensions for generator recipes and otherwise unrepresentable text.
    std::string write() const;
};

} // namespace ta::tnt
