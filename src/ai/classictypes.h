#pragma once

// Per-type facts the retail AI reads, derived on the Classic brain's side from a
// UnitType and its raw FBI words (UnitType::classicFbi), and the adapter that
// feeds them to the shared priority template sim::retailBuildPriority (0x410810).
// docs/classic-ai-plan.md §1.1 / §1.3 / §4.3.

#include <cstdint>
#include <string>

#include "sim/retailplayer.h"

namespace tak::sim { class TypeRegistry; struct UnitType; }

namespace tak::ai {

struct ClassicTypeInfo {
    bool armed = false;       // any [WEAPONn] block, damage or not (type+0x260 0x10000)
    bool builder = false;     // FBI builder= (type+0x260 0x100)
    bool commander = false;   // FBI commander= (the Monarch)
    bool canFly = false, floater = false;
    // The movement class's minwaterdepth, else the FBI's, default -10000
    // (type+0x194). >= 0 means a water unit: the priority's x3.
    int16_t minWaterDepth = -10000;
    int32_t bmcode = 0;       // FBI bmcode=, retail's default 0
    bool structure = false;   // isStructure() (maxvelocity <= 0)
    bool isFeature = false;   // FBI isfeature= (type+0x260 0x1000000)
    // The yardmap, read only for bmcode 0: an 'S' cell anywhere makes the type a
    // mana site (type+0x264 bit 31: lodestones, divine lodestones), and an 'S'
    // FIRST cell puts it in the lodestone draw class (yardmap[0] & 0x80).
    bool manaSite = false;
    bool lodestoneClass = false;
    float cost = 0;           // buildcost, else buildtime (type+0x20e)
    int footX = 1, footZ = 1;
    int32_t buildDistance = 0, maxDamage = 0, sight = 0;
    bool menu = false;        // has a canbuild menu (type+0x132)
    int32_t firstWeaponRange = 0;   // the first weapon's range (type+0x1aa -> +0x90), 0 without one

    static ClassicTypeInfo from(const tak::sim::UnitType& type, const tak::sim::TypeRegistry* registry);

    // sim::RetailBuildPriority names its inputs after other readings of the same
    // retail fields; this is what each one holds here:
    //   flags 0x10000 = armed, 0x100 = FBI builder ("mobile" in the template),
    //   0x1000000 = isfeature; secondaryFlags bit 31 = mana site
    //   ("resourceProducer"); `weapon` = minwaterdepth; `desired` = the profile
    //   limit; count/completed = the owner's units of the type, all / finished.
    tak::sim::RetailBuildPriority priorityInputs(int count, int completed, int32_t limit) const;
};

// Retail's yardmap walk (FBI loader 0x4c0f2c..0x4c101f) over footX*footZ cells:
// a known cell character fills one cell and is repeated while it is the last
// character, an unknown one (a space) is skipped. Sets the two 'S' facts.
void classicYardMapFlags(const std::string& yardMap, int footX, int footZ,
                         bool& manaSite, bool& lodestoneClass);

}  // namespace tak::ai
