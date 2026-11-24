#pragma once
#include <algorithm>
#include <cstdint>

// Retail veterancy and death accounting: TotalA.exe's rules (retailTa*).
// (Kingdoms' experience routines -- experiencepoints, getLevel / setLevel and
// its kill credit -- are gone: TA's veterancy is the kill count alone.)
namespace ta::sim {

// ---- Total Annihilation (TotalA.exe) ----------------------------------------
// TA's damage record 0x489bb0(attacker, victim, amount, kind, extra) carries a
// damage kind; its handler 0x489ce0 keeps it in the victim's +0xf5 on every
// record but a repair (10), and the hitter's player in +0xf4 (10 = none, the
// value a new unit starts with, 0x485c83) only when the record names an
// attacker (0x489db4..0x489dc0). The death sender 0x4864b0 takes +0xf5 as the
// death kind (0x48afd1), so a unit dies with the kind of the record that
// killed it. The callers of 0x489bb0:
//   1 a weapon hit (0x499e37), 2 a paralyzer weapon's (weapon +0x111 bit 7;
//     0x489deb: no HP lost, a Paralyze mission instead),
//   3 a self-destruct (0x402147, the unit its own attacker) and every unit of a
//     player whose commander died under "commander dies: game ends" (0x486f94),
//   4 the old object of a change of owner (0x4886a4 / 0x4887d0, no attacker),
//   5 a builder reclaiming a unit (ReclaimUnit 0x404981, VTOL 0x414b8d),
//   6 the passengers of a dying transport (0x48680b; 3 when it died of 3),
//   7 a finished isfeature type turning into its feature (0x41b9fe / 0x486167),
//   9 a nanoframe decayed or reclaimed to nothing (0x41bc49, 0x402701),
//   10 repair (0x41bdc7), 11 deadly water (0x48af32, no attacker);
// and the death sender itself is called with 0 (0x486244) and 8 (0x4859a8,
// 0x486eb2, 0x486ef8).
namespace tadeath {
inline constexpr uint8_t kNone = 0, kWeapon = 1, kParalyze = 2, kSelfDestruct = 3, kChangeOwner = 4,
                         kReclaim = 5, kPassenger = 6, kBecomeFeature = 7, kRemoved = 8, kDecay = 9,
                         kRepair = 10, kWater = 11;
}

// World's death causes (Unit::deathType keeps the Kingdoms numbering its death
// pipeline still reads: severity, Killed/Dying, the death weapon) as TA kinds.
// A weapon's death (1-3, and Kingdoms' stone/freeze 14/15, no TA analogue) is
// TA's 1; Kingdoms' paralyzer 4 is 2; self-destruct 5 is 3; a change of owner
// 6 is 4; a passenger 8 is 6 (World passes 3 itself when its carrier died of
// 3); a wall turning feature 9 is 7; construction removed or a factory's output
// aborted 11 is 9; deadly water and a scenario's no-hitter damage 13 is 11.
// Kingdoms' mission Kill 10 and a scripted removal 0 have no TA cause: 0.
// (World passes 3 for the commander rule's sweep, TA's 0x486f10.)
inline uint8_t retailTaDeathKind(uint8_t deathType) {
    switch (deathType) {
        case 1: case 2: case 3: case 14: case 15: return tadeath::kWeapon;
        case 4: return tadeath::kParalyze;
        case 5: return tadeath::kSelfDestruct;
        case 6: return tadeath::kChangeOwner;
        case 8: return tadeath::kPassenger;
        case 9: return tadeath::kBecomeFeature;
        case 11: return tadeath::kDecay;
        case 13: return tadeath::kWater;
        case 16: return tadeath::kReclaim;   // World's Unit::kDeathReclaim
        default: return tadeath::kNone;
    }
}

// The death packet's switch (handler 0x4866d0; 0x486892, table 0x486e64, on
// kind - 1 for kinds 1..6; any other kind counts nothing):
//   1 and 6 (0x4868b3): the owner's losses word (player +0xfe) goes up when the
//     unit has an owner record; then the hitter player's kills (player +0xfc,
//     0x486906) when +0xf4 names one (not 10), the unit is finished (+0x104 ==
//     1.0) and its owner (+0xff) is not the hitter -- an owner test, no
//     alliance; then the killer unit's kill count (unit +0xb8, 0x4869ca) under
//     the same finished/owner test.
//   5 (0x486899): only when +0xf4 names a player other than the owner, then as
//     1 -- reclaiming your own unit is no loss.
//   3 (0x4869f5): a loss, unless the local player's record has the owner
//     marked out (player +0x129[owner], set at 0x45768a when a remote player's
//     final statistics arrive, packet 0x29, and then frozen by 0x457540): a
//     network statistics-sync guard a lockstep game has no use for, so always a
//     loss here. Never a kill.
//   2 and 4: nothing.
// All of it runs when the death packet is handled, at the death itself, not
// when the unit's Killed script ends. (The same block also counts the
// commander kills / commanders lost words, player +0x104 / +0x106, not shown.)
inline bool retailTaDeathCountsAsLoss(uint8_t kind, int hitterPlayer, int ownerPlayer) {
    switch (kind) {
        case tadeath::kWeapon: case tadeath::kPassenger: case tadeath::kSelfDestruct: return true;
        case tadeath::kReclaim: return hitterPlayer >= 0 && hitterPlayer != ownerPlayer;
        default: return false;
    }
}

// The kill for the hitter's player (0x4868c8..0x48690e) and, under the same
// test, for the killer unit (0x48699d..0x4869ca): kinds 1, 5 and 6, a hitter
// player recorded (+0xf4 != 10) that is not the owner, a finished victim.
inline bool retailTaKillCounts(uint8_t kind, bool finished, int hitterPlayer, int ownerPlayer) {
    if (kind != tadeath::kWeapon && kind != tadeath::kPassenger && kind != tadeath::kReclaim) return false;
    return hitterPlayer >= 0 && hitterPlayer != ownerPlayer && finished;
}

// TA's veterancy is the kill count alone (no experience points): 0x489bb0
// (0x489bf3..0x489c32) scales every damage record but a repair by the VICTIM's
// kills -- k = min(kills / 5, 5), amount = (25 - k) x amount x 4 / 100, each
// step an int32 truncated toward zero -- after the damagemodifier; 4% less per
// five kills, at most 20%. (Its shots' accuracy reads the same count: the lead
// past 5 kills, 0x48a324, and the spread, 0x49e468.)
inline int32_t retailTaVeteranDamage(int32_t amount, uint16_t victimKills) {
    const int32_t k = std::min<int32_t>(int32_t(victimKills) / 5, 5);
    return int32_t(int64_t(25 - k) * int64_t(amount) * 4 / 100);
}

} // namespace ta::sim
