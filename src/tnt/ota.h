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
    // In WORLD units, the same space unit positions use -- NOT cells. Retail
    // writes e.g. XPos=3010 on a 210-cell (3360-unit) wide map. This was
    // documented as cells with "world px = *16", the Kingdoms convention, and a
    // reader that scaled by 16 put every start tens of thousands of units off the
    // map; setupMatch then fell back to spreading players around the map centre.
    int xpos = 0, zpos = 0;
};

struct Scenario {
    // Cartographer emits this constant; community tools vary, so it round-trips.
    std::string copyright =
        "Copyright 1998 Cavedog Entertainment. All rights reserved.";
    std::string missionName;
    std::string missionDescription;
    // Lowercase world name (archipelago/greenworld/lava/mars/metal/moon...).
    // Retail's own .ota files do NOT carry this -- it is ours, written so a map we
    // generate remembers which world's art it was built from. Absent, callers fall
    // back to the first world the install ships.
    std::string kingdom;
    int sizeW = 0, sizeH = 0;         // in Units (cells>>5); OTA "size = W x H"
    std::string useOnlyUnits;         // "<name>.tdf", or empty when unrestricted
    bool hasScenario = false;
    // TA economy inputs. These live in the GlobalHeader (tidal/solar/wind/gravity)
    // and in the per-schema block (SurfaceMetal/MohoMetal), and are what make the
    // same wind farm a power station on one map and scenery on another.
    // Defaults are the MEDIANS of the shipped maps, so a map created here plays
    // like a retail one instead of arriving becalmed and metal-less: across TA's
    // own .ota files, SurfaceMetal 5, MohoMetal 40, MaxWindSpeed 3500 and
    // TidalStrength 0 (most maps have no tide at all; the ones that do run 15-25).
    float tidalStrength = 0;
    float solarStrength = 20;
    float minWindSpeed = 0, maxWindSpeed = 3500;
    float gravity = 112;
    float surfaceMetal = 5, mohoMetal = 40;
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
