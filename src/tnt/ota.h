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

// A unit the .ota places on the map. Campaign missions state their whole opening
// board this way -- AC01 carries 103 of these across its three difficulty schemas.
// Coordinates are WORLD units, like StartPos.
struct PlacedUnit {
    std::string unitName;      // FBI id, e.g. "ARMFAV"
    std::string ident;         // the mission script's handle for it; often empty
    int xpos = 0, ypos = 0, zpos = 0;
    int player = 0;            // .ota player number (1-based in the file)
    int healthPercent = 100;
    int angle = 0;             // heading, TA's 0..65535 turn units
    int kills = 0;             // veterancy it starts with
};

// One [Schema N] block. In a CAMPAIGN mission the schema is the DIFFICULTY --
// AC01 ships Easy / Medium / Hard, each with its own board -- not the player
// count, which is what a skirmish map's schemas vary. Both forms carry the
// economy figures, so they live here rather than on Scenario.
struct Schema {
    std::string type;          // "Easy"/"Medium"/"Hard", or "Network 1" on a skirmish map
    std::string aiProfile;     // ai/<name>.txt for this schema's opponents
    float surfaceMetal = 0, mohoMetal = 0;
    // Opening treasuries. The asymmetry is the difficulty knob: AC01 Easy gives
    // the human 1000 of each and the computer 100.
    float humanMetal = 0, humanEnergy = 0;
    float computerMetal = 0, computerEnergy = 0;
    std::vector<StartPos> starts;
    std::vector<PlacedUnit> units;
};

// A mission's objectives, as TA states them: plain GlobalHeader keys rather than
// a script or a trigger file. Surveyed across all 272 shipped .ota files; the
// count after each is how many declare it.
struct Objectives {
    bool allUnitsKilled = false;        // 163 -- kill everything hostile
    bool commanderKilled = false;       // 123 -- kill the enemy Commander
    bool destroyAllUnits = false;       //  90
    bool killAllMobileUnits = false;    //   9
    std::string allUnitsKilledOfType;   //  52 -- e.g. ARMGATE
    std::string killAllOfType;          //  40
    std::string killUnitType;           //  32
    std::string captureUnitType;        //  32
    std::string unitTypeKilled;         //  18
    std::string buildUnitType;          //   8
    bool deathTimerRunsOut = false;     //  21 -- survive the clock
    // MoveUnitToRadius=<type>, x, z, r (18) -- get a unit of `type` (ANYTYPE for
    // any) within `radius` of (x,z). AC01's is "ANYTYPE, 992, 656, 64".
    bool hasMoveToRadius = false;
    std::string moveToType;
    int moveToX = 0, moveToZ = 0, moveToRadius = 0;
    // Anything declared at all? A skirmish map declares none of these.
    bool any() const {
        return allUnitsKilled || commanderKilled || destroyAllUnits ||
               killAllMobileUnits || deathTimerRunsOut || hasMoveToRadius ||
               !allUnitsKilledOfType.empty() || !killAllOfType.empty() ||
               !killUnitType.empty() || !captureUnitType.empty() ||
               !unitTypeKilled.empty() || !buildUnitType.empty();
    }
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

    // --- campaign mission fields ---------------------------------------------
    // Present on every shipped .ota (all 272 carry the header keys); a skirmish
    // map simply leaves the mission-specific ones empty. See docs/ta-port.md.
    std::string planet;          // "Green planet"
    std::string missionHint;     // hint text file
    std::string brief;           // briefing script id
    std::string narration;       // voiceover id
    std::string glamour;         // the still shown behind the briefing
    bool  lavaWorld = false;
    int   killMul = 0;           // score weighting
    int   timeMul = 0;
    int   maxUnits = 0;          // per-player cap this mission imposes (0 = none)
    bool  waterDoesDamage = false;
    float waterDamage = 0;
    bool  noSeaLevelTrigger = false;
    int   mapping = 0;
    Objectives objectives;
    // Every [Schema N], in file order. A campaign mission's are its difficulties.
    std::vector<Schema> schemas;
    // The schema a difficulty name selects, or the first, or nullptr if none.
    const Schema* schemaFor(const std::string& type) const;
    // Does this .ota describe a campaign MISSION rather than a skirmish map?
    //
    // PLACED UNITS, and nothing else. Measured over all 272 shipped .ota files:
    // 177 place units and 95 do not, and none places units without also carrying
    // the mission metadata. The tempting tests do not work -- every one of the 272
    // carries a `brief` key, and an ordinary skirmish map declares `allUnitsKilled`
    // and `destroyAllUnits` too, those being how a normal game is won. Coast To
    // Coast reads as a mission on either of those.
    bool isMission() const {
        for (const auto& s : schemas) if (!s.units.empty()) return true;
        return false;
    }

    // Parse an .ota (GlobalHeader TDF). Missing fields keep their defaults.
    static Scenario parse(const std::string& text);
    // Serialize to Cartographer's exact byte layout (CRLF, tab indent, key order).
    // NOTE: still the Kingdoms key set -- the TA economy fields above are parsed
    // but not written, so a TA .ota does not round-trip yet. See docs/ta-port.md.
    std::string write() const;
};

} // namespace ta::tnt
