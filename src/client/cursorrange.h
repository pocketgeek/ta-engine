#pragma once

#include "sim/retailaim.h"
#include "sim/sim.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace tak::client {

enum class CursorWeaponRange { InRange, OutOfRange, Unknown };

// Native 0x4dd780's mode-3 Airstrike gate requires a WEAPON1 pointer, then
// checks the `dropped` bit on the active native slot selected by 0x519ac0.
// Local damage weapons are compressed, so translate a switchable local index
// back to its original FBI slot before testing the per-slot flag.
inline bool unitHasAirstrikeCursor(const sim::UnitType& type,int localWeaponSlot) {
    if(!type.hasPrimaryWeaponBlock)return false;
    int nativeSlot=0;
    if(type.weaponSwitching) {
        if(type.weapons.empty()) nativeSlot=std::clamp(localWeaponSlot,0,2);
        else {
            const int local=std::clamp(localWeaponSlot,0,int(type.weapons.size())-1);
            nativeSlot=type.weaponNativeSlotForLocal[size_t(local)];
        }
    }
    return nativeSlot>=0 && nativeSlot<3 && type.weaponAirstrikeCursor[size_t(nativeSlot)];
}

// Cursor eligibility uses the same native DamageCategory lookup as combat.
inline float cursorDamageVs(const sim::Weapon& weapon, const sim::UnitType& target) {
    return weapon.damageVs(&target);
}

// Retail's minrange and ordinary range checks square each 16.16 x/z delta, take
// that product's high dword, then add the two whole-pixel squared terms. Do not
// add the raw squares before shifting: the carry from the two fractional parts
// is discarded by the native code. Keeping the per-axis shifts also avoids a
// 65-bit sum at the signed-coordinate limits.
inline uint64_t cursorDistanceSquaredPixels(const std::array<int32_t, 3>& a,
                                           const std::array<int32_t, 3>& b) {
    const auto squareDelta = [](int32_t av, int32_t bv) {
        const int64_t delta = int64_t(av) - int64_t(bv);
        const uint64_t magnitude = uint64_t(delta < 0 ? -delta : delta);
        return magnitude * magnitude;
    };
    const uint64_t x2 = squareDelta(a[0], b[0]);
    const uint64_t z2 = squareDelta(a[2], b[2]);
    return (x2 >> 32) + (z2 >> 32);
}

// Native WeaponType inRange result for the classes represented by our UnitType
// data. Remote effects and dropped weapons have separate retail classes whose
// cursor callbacks have not been mapped; leave those inconclusive so they never
// produce a false TooFar glyph.
inline CursorWeaponRange cursorWeaponRange(const sim::Weapon& weapon,
        const sim::UnitType& attacker, const std::array<int32_t, 3>& attackerPosition,
        const sim::UnitType& target, const std::array<int32_t, 3>& targetPosition,
        uint8_t targetFlightGroundMode) {
    const uint64_t distance2 = cursorDistanceSquaredPixels(attackerPosition, targetPosition);
    if (weapon.minRange > 0 &&
        distance2 < uint64_t(weapon.minRange) * uint64_t(weapon.minRange))
        return CursorWeaponRange::OutOfRange;

    if (weapon.kind == sim::Weapon::Kind::Remote ||
        weapon.kind == sim::Weapon::Kind::Dropped)
        return CursorWeaponRange::Unknown;

    if (weapon.melee) {
        // MeleeWeapon::inRange ignores the authored `range`; its two footprint
        // boxes must be closer than half a 16px cell on both axes. A melee unit
        // cannot connect while the target is in retail's airborne mode 2.
        if (targetFlightGroundMode == 2) return CursorWeaponRange::OutOfRange;
        const auto absDelta = [](int32_t av, int32_t bv) {
            const int64_t d = int64_t(av) - int64_t(bv);
            return uint64_t(d < 0 ? -d : d);
        };
        constexpr uint64_t halfCell = 0x80000; // 8 px in 16.16
        const uint64_t xReach = uint64_t(attacker.footX + target.footX) * halfCell + halfCell;
        const uint64_t zReach = uint64_t(attacker.footZ + target.footZ) * halfCell + halfCell;
        return absDelta(attackerPosition[0], targetPosition[0]) < xReach &&
               absDelta(attackerPosition[2], targetPosition[2]) < zReach
                   ? CursorWeaponRange::InRange : CursorWeaponRange::OutOfRange;
    }

    if (weapon.ballistic) {
        const int32_t speed = retailAimSpeed(weapon.projVel);
        const uint16_t pitch = retailBallisticPitch(
            float(int64_t(targetPosition[0]) - attackerPosition[0]) / 65536.0f,
            float(int64_t(targetPosition[1]) - attackerPosition[1]) / 65536.0f,
            float(int64_t(targetPosition[2]) - attackerPosition[2]) / 65536.0f,
            float(speed) / 65536.0f, weapon.gravityAdj, weapon.lobPreferred);
        if (pitch == 0x8000) return CursorWeaponRange::OutOfRange;
    }

    // Base WeaponType::inRange is true for range=0 and otherwise compares the
    // 2D center distance inclusively. Guided, line-of-sight, and wandering
    // classes inherit this native check.
    if (weapon.range <= 0 ||
        distance2 <= uint64_t(weapon.range) * uint64_t(weapon.range))
        return CursorWeaponRange::InRange;
    return CursorWeaponRange::OutOfRange;
}

// What one selected attacker contributes to the cursor over an enemy, from
// retail selector 0x4dd780: default hover (mode 1) re-enters as ATTACK (mode 3)
// for a `canattack` unit, and mode 3 splits on the unit's mover (unit+8).
// A MOBILE attacker never consults weapon range -- it walks over and attacks --
// so it shows Attack (slot 1) unless its active weapon cannot hurt the target:
// damage 0x531e50 <= 0, notargetcategory (UnitDef+0x9a), a stoning/freezing
// weapon against cantbestoned/cantbefrozen (UnitDef+0x268 bits 2/4),
// toairweapon (WeaponType+0xc8 0x4000) against a target that is not airborne,
// or noairweapon (0x8000) against an airborne one (unit+0x130 low bits 2).
// An IMMOBILE attacker (no mover) applies the same immunity gates, then the
// minrange check 0x52adf0 and damage+inRange 0x52ad60. Anything else is TooFar
// (slot 3). Returns true for Attack, false for TooFar.
// tools/re/probe_cursor_enemy_hover.py runs those branches natively.
inline bool cursorCanAttackEnemy(const sim::UnitType& attacker, int weaponSlot,
        const std::array<int32_t, 3>& attackerPosition, const sim::UnitType& target,
        const std::array<int32_t, 3>& targetPosition, uint8_t targetFlightGroundMode,
        bool ignoreAirstrikeWeapons = false) {
    if (attacker.weapons.empty()) return false;
    if (ignoreAirstrikeWeapons && unitHasAirstrikeCursor(attacker, weaponSlot))
        return false;
    const int slot = attacker.weaponSwitching
        ? std::clamp(weaponSlot, 0, int(attacker.weapons.size()) - 1) : 0;
    const auto& weapon = attacker.weapons[size_t(slot)];
    const auto listed = [&](const std::vector<std::string>& cats) {
        for (const auto& c : cats)
            if (std::find(target.categories.begin(), target.categories.end(), c) !=
                target.categories.end()) return true;
        return false;
    };
    if (listed(attacker.noTargetCats) ||
        (weapon.status == sim::Weapon::Status::Stoned && target.cantBeStoned) ||
        (weapon.status == sim::Weapon::Status::Frozen && target.cantBeFrozen))
        return false;
    const bool airborne = targetFlightGroundMode == 2;
    if (!attacker.isStructure())
        return cursorDamageVs(weapon, target) > 0.0f &&
               !(weapon.toAir && !airborne) && !(weapon.noAir && airborne);
    if (cursorDamageVs(weapon, target) <= 0.0f || (weapon.noAir && airborne))
        return false;
    return cursorWeaponRange(weapon, attacker, attackerPosition, target, targetPosition,
                             targetFlightGroundMode) != CursorWeaponRange::OutOfRange;
}

} // namespace tak::client
