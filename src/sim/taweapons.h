#pragma once

// Total Annihilation weapons: every top-level section of every weapons/*.tdf, by
// ID (TotalA.exe 0x42e310 / 0x42e440; docs/retail-data.md section 3).

#include "sim/sim.h"

#include <map>
#include <string>
#include <vector>

namespace ta::hpi { class Vfs; }
namespace ta::tdf { struct Node; }

namespace ta::sim {

// The +0x111 flag bits, in retail's order.
enum TaWeaponFlag : uint32_t {
    kWLineOfSight = 0, kWBallistic, kWShellWeapon, kWBeamWeapon, kWVLaunch, kWMeteor,
    kWNoRadar, kWParalyzer, kWDropped, kWStartSmoke, kWEndSmoke, kWSoundTrigger,
    kWGuidance, kWTracks, kWUnitsOnly, kWGroundBounce, kWWaterWeapon, kWToAirWeapon,
    kWSmokeTrail, kWTurret, kWSelfProp, kWPropeller, kWNoExplode, kWBurnBlow,
    kWTwoPhase, kWCruise, kWCommandFire, kWNoAutoRange, kWStockpile, kWTargetable,
    kWInterceptor,
};

// Parse one weapon section as retail does (scales, defaults, truncations).
Weapon parseTaWeapon(const tdf::Node& node, const std::string& sectionName);

class TaWeaponTable {
public:
    // Load every weapons/*.tdf; a later section with the same ID overwrites, IDs
    // outside 0..255 are rejected (retail writes them in front of its table).
    void load(const hpi::Vfs& vfs);
    // FBI weapon1/2/3/explodeas/selfdestructas: section name (stricmp), lowest ID
    // first; empty or unknown names give slot 0 (NOWEAPON).
    const Weapon& resolve(const std::string& sectionName) const;
    const Weapon& slot(int id) const;
    bool empty() const { return byName_.empty(); }

private:
    std::vector<Weapon> slots_ = std::vector<Weapon>(256);
    std::map<std::string, int> byName_;   // lowercased section name -> lowest ID
};

}  // namespace ta::sim
