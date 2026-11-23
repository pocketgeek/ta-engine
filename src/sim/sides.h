#pragma once

// The game's sides, read from gamedata/sidedata.tdf ([SIDE0], [SIDE1], ...), in file
// order: the side index is the [SIDEn] number, as in TotalA.exe (retail Total
// Annihilation ships ARM and CORE). Everything that used to hard-code the Kingdoms
// factions asks this table instead. Loaded whenever a data root is mounted
// (hpi::mountRetailRoot); until then it holds retail TA's two sides so headless
// tools and tests have a sane default.

#include <string>
#include <vector>

namespace ta::hpi { class Vfs; }

namespace ta::sides {

struct Side {
    std::string name;        // ARM / CORE (display + lobby)
    std::string prefix;      // nameprefix: ARM / COR (unit ids, guis/<prefix>*.gui)
    std::string commander;   // commander= unit id (lowercased)
    std::string intGaf;      // intgaf: in-game interface art (ARMINT / CORINT)
    std::string font, fontGui;
    int energyColor = 0, metalColor = 0;
};

// Read gamedata/sidedata.tdf; keeps the previous table when the file is missing or
// names no side. Returns the number of sides.
int load(const hpi::Vfs& vfs);

const std::vector<Side>& all();
int count();
// Side i, clamped into range (an out-of-range wire value never indexes past the table).
const Side& at(int i);
// Index of the side whose name or prefix matches (case-insensitive), or -1.
int find(const std::string& nameOrPrefix);
// Lowercased prefix ("arm", "cor") -- the form unit ids and file names use.
std::string lowerPrefix(int i);
// Lowercased commander unit id of side i.
std::string commander(int i);
// Whether `unitId` is some side's commander.
bool isCommander(const std::string& unitId);

}  // namespace ta::sides
