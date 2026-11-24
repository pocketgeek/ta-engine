#pragma once
#include <algorithm>
#include <cstdint>

// Retail veterancy and death accounting. The Kingdoms routines (KINGDOMS.icd)
// come first: a unit's experience (+0xf0) is a sum of experiencepoints, not a
// kill count, and its level is worked out from it on every read;
// tools/re/check_veterancy.py runs all three routines natively. TotalA.exe's
// rules follow them (retailTa*): what World counts now.
namespace ta::sim {

// The kill credit of the death routine 512ae0: the switch at 512b71 on the
// death kind (+0x116 >> 4) reaches the credit at 512c9c only for kinds 1, 2,
// 3, 7, 8, 14 and 15 (among them a normal, fire or explosion blow, stone and
// freeze); self-destruct (5), a paralyzer (4) and the rest credit nobody. Then the victim must
// be finished (+0x108 == 0) and its owner (+0xfd) differ from the player of
// the unit that hit it last (+0xfc): an ally's kill counts, the owner's own
// does not. Nothing about the killer is tested, so a building or a noveteran
// unit gains experience too (getLevel keeps the noveteran one at 0).
inline bool retailKillCreditsExperience(uint8_t deathType, bool finished, int hitterPlayer, int victimPlayer) {
    constexpr uint16_t kinds = 1u << 1 | 1u << 2 | 1u << 3 | 1u << 7 | 1u << 8 | 1u << 14 | 1u << 15;
    return deathType < 16 && (kinds >> deathType & 1) && finished && hitterPlayer != victimPlayer;
}

// The same switch case counts the kill and the score for the hitter's PLAYER
// (512bd5..512c63), not for a killer unit: +0xfc must name one (10 is none),
// the victim be finished and its owner (+0xfd) differ from +0xfc -- the same
// owner test, no alliance -- and then the player's kill count (record +0xa0)
// goes up and, unless the scenario took the score over (game +0x3070 & 4),
// its score gains the victim type's experiencepoints. A dead or converted
// killer still counts: only the player recorded at the hit is read.
// The owner's losses word (record +0xa2): 512ae0 adds 1 only in the switch
// cases of the credit kinds (512bce) and of self-destruct and a no-hitter
// hit, kinds 5 and 13 (512d23), whoever hit it and finished or not (table
// 512ec4, S). A feature conversion (9), a change of owner (6), a mission
// Kill or a player's army ended (10), a factory's aborted output or a site
// unconjured away (11) and a scripted removal (0) are no loss.
inline bool retailDeathCountsAsLoss(uint8_t deathType) {
    constexpr uint16_t kinds = 1u << 1 | 1u << 2 | 1u << 3 | 1u << 5 | 1u << 7 | 1u << 8 | 1u << 13 |
                               1u << 14 | 1u << 15;
    return deathType < 16 && (kinds >> deathType & 1);
}

inline bool retailKillCountsForPlayer(uint8_t deathType, bool finished, int hitterPlayer, int victimPlayer) {
    return hitterPlayer >= 0 && retailKillCreditsExperience(deathType, finished, hitterPlayer, victimPlayer);
}

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

// 512ce0: the killer gains the VICTIM type's experiencepoints (+0x1c2), a
// 32-bit add that wraps.
inline int32_t retailAddExperience(int32_t experience, int32_t points) {
    return int32_t(uint32_t(experience) + uint32_t(points));
}

// getLevel 519310: noveteran (+0x268 & 0x10) is 0, a mission's `v F`
// multiplier (+0xf4) is 10, else the experience over the unit's OWN type's
// experiencepoints (idiv, toward zero), capped at 10 and not floored. Retail
// faults on a zero divisor (no shipped type has one); this answers 0.
inline int retailVeteranLevel(int32_t experience, int32_t points, bool noVeteran, float multiplier) {
    if (noVeteran) return 0;
    if (multiplier != 0.0f) return 10;
    if (points == 0) return 0;
    return int(std::min<int64_t>(int64_t(experience) / int64_t(points), 10));
}

// setLevel 519370 (an authored veteran level: placement 4cd335, a type's
// scenario stats 4cd5f4): unchanged for noveteran or a `v F` multiplier, else
// (level + 0.5) * experiencepoints, truncated by _ftol 5d3d54 (a 64-bit
// fistp, low dword kept). The half level keeps getLevel at `level` exactly.
inline int32_t retailExperienceForLevel(int32_t experience, int level, int32_t points, bool noVeteran,
                                        float multiplier) {
    if (noVeteran || multiplier != 0.0f) return experience;
    const double value = (double(level) + 0.5) * double(points);   // exact below 2^53
    if (!(value > -9223372036854775808.0 && value < 9223372036854775808.0)) return 0;   // fistp's indefinite
    return int32_t(uint32_t(uint64_t(int64_t(value))));            // the conversion truncates, as _ftol
}

} // namespace ta::sim
