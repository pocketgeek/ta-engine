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
    int xpos = 0, zpos = 0;   // in CELLS (16px); world px = *16
};

struct Scenario {
    // Cartographer emits this constant; community tools vary, so it round-trips.
    std::string copyright =
        "Copyright 1998 Cavedog Entertainment. All rights reserved.";
    std::string missionName;
    std::string missionDescription;
    std::string kingdom;              // lowercase world: aramon/taros/veruna/zhon
    int sizeW = 0, sizeH = 0;         // in Units (cells>>5); OTA "size = W x H"
    std::string useOnlyUnits;         // "<name>.tdf", or empty when unrestricted
    bool hasScenario = false;
    // TA economy inputs. These live in the GlobalHeader (tidal/solar/wind/gravity)
    // and in the per-schema block (SurfaceMetal/MohoMetal), and are what make the
    // same wind farm a power station on one map and scenery on another.
    float tidalStrength = 0;
    float solarStrength = 0;
    float minWindSpeed = 0, maxWindSpeed = 0;
    float gravity = 0;
    float surfaceMetal = 0, mohoMetal = 0;
    int   lineOfSight = 0;   // TA's LoS mode option

    std::string mapType = "Network 1";
    std::string aiProfile = "DEFAULT";
    std::vector<StartPos> starts;

    // Parse an .ota (GlobalHeader TDF). Missing fields keep their defaults.
    static Scenario parse(const std::string& text);
    // Serialize to Cartographer's exact byte layout (CRLF, tab indent, key order).
    // NOTE: still the Kingdoms key set -- the TA economy fields above are parsed
    // but not written, so a TA .ota does not round-trip yet. See docs/ta-port.md.
    std::string write() const;
};

} // namespace ta::tnt
