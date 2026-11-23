#pragma once
#include <cstdint>

// Retail deaths no weapon causes (KINGDOMS.icd); tools/re/check_kill_credit.py
// runs them natively.
namespace tak::sim {

// Deadly water, the unit update's 51e0d5..51e14c: once a second (the game
// tick a multiple of 30), when the map has waterdoesdamage (+0xd41) and a
// nonzero waterdamage (+0xd45), a unit whose height's whole part (+0x6e, a
// signed word of the 16.16 +0x6c) is at or below sea level (the byte at game
// +0x19ef8) and whose type is not canhover (UnitDef +0x260 & 0x1000) takes
// the whole waterdamage through 51a140, with no hitter and damage kind 13.
// Flight, a ship's hull or an amphibian's depth exempt nothing: only the
// height counts. 51a1d4 hands the amount on as a word.
inline uint16_t retailDeadlyWaterDamage(uint32_t tick, int32_t height, int seaLevel, bool canHover,
                                        bool enabled, int32_t amount) {
    if (!enabled || amount == 0 || tick % 30 != 0) return 0;
    if (int16_t(uint32_t(height) >> 16) > int16_t(uint8_t(seaLevel)) || canHover) return 0;
    return uint16_t(amount);
}

// 51a16e..51a19a: a hit (not a heal, kind 12) on a unit whose activation
// byte has bit 1 (+0x114 & 2), below 30000, is scaled by the type's
// damagemodifier (16.16): a 64-bit product shifted back 16 (5d3dc0, an
// arithmetic shift). The damage packet, so the HP drop, the hit response and
// HitByWeapon, carries the result. Deadly water (kind 13) is scaled too.
inline int32_t retailDamageModifier(int32_t damage, uint8_t kind, bool activated, int32_t modifier) {
    if (kind == 12 || !activated || damage >= 30000) return damage;
    return int32_t((int64_t(damage) * int64_t(modifier)) >> 16);
}

// The 16-bit HP word (+0x10c) for World's whole hit points, which do not wrap
// (hurt clamps them; see retail-engine.md, "A dying unit takes no more hits").
inline int16_t retailHpWord(int32_t whole) {
    return int16_t(whole < -32768 ? -32768 : whole > 32767 ? 32767 : whole);
}

// The unit update's HP sample (51e07c..51e0cf), once a second (the game tick
// a multiple of 30): the HP word (+0x10c) times 100 over the type's
// maxdamage (+0x1be), an unsigned 32-bit division, then 0 below 0 and 100
// above 100 (signed compares). The new value goes to +0x110 and the old one
// to +0x111. A dying unit's negative HP wraps to a large quotient, so it
// reads 100 (0 over a maxdamage of 1). (E: tools/re/check_kill_credit.py.)
inline uint8_t retailHpSample(int16_t hp, int32_t maxDamage) {
    if (maxDamage == 0) return 0;   // retail faults; no shipped type has none
    const int32_t value = int32_t(uint32_t(int32_t(hp) * 100) / uint32_t(maxDamage));
    return uint8_t(value < 0 ? 0 : value > 100 ? 100 : value);
}

// 512610's severity (51267f..512739), Killed's first argument and the death
// packet's +9: 0 for kinds 0, 6-11, 14 and 15, 1 for self-destruct (5), and
// for the others (1-4, 12, 13) 0 while the HP word is above 0; otherwise the
// overkill (-HP x 100 over maxdamage, unsigned) plus the sample of a second
// before (+0x111), halved toward zero and kept in 1..100. Only that last case
// runs Killed. (E: tools/re/check_kill_credit.py.)
inline uint8_t retailDeathSeverity(uint8_t kind, int16_t hp, int32_t maxDamage, uint8_t previousSample) {
    switch (kind) {
    case 0: case 6: case 7: case 8: case 9: case 10: case 11: case 14: case 15: return 0;
    case 5: return 1;
    default: break;
    }
    if (hp > 0 || maxDamage == 0) return 0;
    const int32_t overkill = int32_t(uint32_t(-int32_t(hp) * 100) / uint32_t(maxDamage));
    const int32_t severity = (overkill + int32_t(previousSample)) / 2;
    return uint8_t(severity < 1 ? 1 : severity > 100 ? 100 : severity);
}

// A corpse on land, as the feature update 4959c0 (495aaf..495af1) runs it
// after 513027..513034 loaded its decomposition +0x3c (the feature type's
// +0x130: decomposetime x 30, truncated, low 16 bits): each update counts
// +0x3c down; the update that empties it starts +0x40 at 1 and every update
// from that one on adds 1 to it (the body sinks into the ground meanwhile),
// and the one that takes it past 60 removes the feature (496380). The
// placement's own tick runs the first update, so a corpse with a
// decomposition D goes D + 58 updates after it; with none (0) it rests for
// good (494a80). A land corpse has no velocity, so this closed form is the
// whole record; retailfeaturesink.h steps the same +0x3c / +0x40 for a water
// grave a reclaim wake reloads. (E: tools/re/check_corpse_decay.py, which
// also requires the two to agree.)
inline int32_t retailCorpseDecayLife(int32_t decompose) {
    return decompose > 0 ? decompose + 58 : -1;
}

// A reclaim wake on a decaying corpse (406615 / VTOL 420ba4) reloads +0x3c
// with the decomposition D and leaves +0x40 alone. `remaining` is the updates
// left until the removal, counted as retailCorpseDecayLife does: 59 or more
// while +0x3c still counts (it holds remaining - 58), fewer once +0x40 runs
// (the removal comes when it passes 60 whatever +0x3c holds). The reload
// starts the count again unless +0x40 runs out first: when +0x3c empties
// before or on the removal update, +0x40 starts again at 1. Returns the
// updates left after the wake. (E: tools/re/check_corpse_decay.py.)
inline int32_t retailCorpseDecayHold(int32_t remaining, int32_t decompose) {
    if (decompose <= 0) return remaining >= 59 ? -1 : remaining;
    if (remaining >= 59 || decompose - 1 <= remaining) return decompose + 58;
    return remaining;
}


} // namespace tak::sim
