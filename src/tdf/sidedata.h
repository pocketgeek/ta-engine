#pragma once

#include <map>
#include <string>
#include <vector>

namespace ta::hpi { class Vfs; }

namespace ta::tdf {

// A typed view over TA's `gamedata/SIDEDATA.TDF` -- the same shape of thing
// src/tnt/ota.h is for a map's .ota.
//
// This one file carries four things Kingdoms kept in four different places:
//
//   * The SIDES themselves ([SIDE0] ARM, [SIDE1] CORE) and, crucially, each
//     one's starting `commander=`. Kingdoms had no equivalent -- its five
//     MONARCHS were a hardcoded table in the engine -- so reading this is what
//     lets the side roster come from the data instead of from source.
//   * The BUILD TREE, under [CANBUILD] as one block per builder with
//     `canbuild1..N=UNIT` in menu order. Kingdoms instead used a directory of
//     `canbuild/<builder>/<buildable>.tdf` marker files.
//   * The HUD PANEL LAYOUT: exact pixel rects for the metal and energy bars,
//     their numbers and caps, the production/consumption readouts, the unit
//     info footer and the reload bars. This is what a faithful two-resource
//     panel is drawn from.
//   * Per-side cosmetics: the interface GAF, fonts, and the palette indices the
//     two resource bars are drawn in.

// A screen region as SIDEDATA states it. Note these are NOT all rectangles:
// several entries (ENERGYNUM, METAL0, the UNIT*MAKE/USE pairs) repeat a
// coordinate or give an x2/y2 smaller than x1/y1, because retail reads them as
// a text ANCHOR rather than a box. Stored verbatim; interpreting them is the
// drawing code's business.
struct PanelRect {
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    int width() const { return x2 - x1; }
    int height() const { return y2 - y1; }
};

struct Side {
    std::string name;        // "ARM" / "CORE"
    std::string namePrefix;  // "ARM" / "COR" -- the unit-id prefix, which is NOT
                             // the same string as `name` for CORE
    std::string commander;   // lowercased unit id of this side's commander
    std::string intGaf;      // interface GAF (ARMINT / CORINT)
    std::string font, fontGui;
    int energyColor = 0, metalColor = 0;   // palette indices for the two bars
    // Panel regions by their uppercase SIDEDATA name (ENERGYBAR, METALNUM, ...).
    std::map<std::string, PanelRect> panels;

    const PanelRect* panel(const std::string& name) const;
};

struct SideData {
    std::vector<Side> sides;
    // Builder unit id (lowercase) -> the units it can build, in menu order
    // (lowercase). Empty for a builder SIDEDATA does not list.
    std::map<std::string, std::vector<std::string>> canBuild;

    const Side* side(const std::string& name) const;       // by name, case-insensitive
    const Side* sideByPrefix(const std::string& p) const;  // by unit-id prefix

    // Read gamedata/SIDEDATA.TDF through the VFS. Returns an empty SideData if
    // the file is absent or unparseable rather than throwing: callers treat "no
    // sides" as "this is not a TA install", which is a better error than an
    // exception out of a loader.
    static SideData load(const hpi::Vfs& vfs);
    // Parse from text (for tests, and for an override that supplies its own).
    static SideData parse(const std::string& text, const std::string& origin = "<memory>");
};

} // namespace ta::tdf
