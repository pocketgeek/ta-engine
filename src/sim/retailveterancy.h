#pragma once
#include <algorithm>
#include <cstdint>

// Retail veterancy (KINGDOMS.icd). A unit's experience (+0xf0) is a sum of
// experiencepoints, not a kill count, and its level is worked out from it on
// every read; tools/re/check_veterancy.py runs all three routines natively.
namespace tak::sim {

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

} // namespace tak::sim
