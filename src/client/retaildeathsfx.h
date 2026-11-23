#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace tak {

enum class RetailDeathSfxFamily : uint8_t {
    Smoke,
    DamageFlame,
    Detached
};

// KINGDOMS.icd's extended EMIT_SFX dispatcher: these codes all have distinct
// creation paths. Code 259 is deliberately absent; its native handler is a no-op.
constexpr std::optional<RetailDeathSfxFamily> retailDeathSfxFamily(int32_t code) {
    if(code==257 || code==258 || code==265)return RetailDeathSfxFamily::Smoke;
    if(code>=260 && code<=262)return RetailDeathSfxFamily::DamageFlame;
    if(code==263 || code==264)return RetailDeathSfxFamily::Detached;
    return std::nullopt;
}

// Unit-owned effect lists remain associated with the rendered unit through its
// death animation. Each snapshot carries its handoff tick; constant building
// deaths and statues retire immediately (statues never run Killed/Dying).
constexpr bool retailAttachedSfxOwnerRemoved(bool alive,int deadForTicks,
                                              int corpseStatue,int deathAnimTicks) {
    return !alive && (corpseStatue>=0 || deadForTicks>=deathAnimTicks);
}

// RenderFrame stores only the already-resolved statue bit, not the simulation's
// -1-or-feature-id field. Keep that representation boundary explicit so false
// cannot be mistaken for feature id 0.
constexpr bool retailAttachedSfxOwnerRemovedSnapshot(bool alive,int deadForTicks,
                                                       bool corpseStatue,int deathAnimTicks) {
    return !alive && (corpseStatue || deadForTicks>=deathAnimTicks);
}

constexpr uint32_t retailAttachedSfxRetirementTick(uint32_t eventTick,
                                                    int deadForTicks,
                                                    int deathAnimTicks) {
    return eventTick+uint32_t(std::max(0,deathAnimTicks-deadForTicks));
}

constexpr bool retailTickAtOrAfter(uint32_t tick,uint32_t deadline) {
    return int32_t(tick-deadline)>=0;
}

constexpr uint32_t retailEarlierTick(uint32_t a,uint32_t b) {
    return retailTickAtOrAfter(a,b) ? b : a;
}

// Native owner cleanup follows the SET_UNIT_VALUE stop request: SET26 marks
// removal for the next unit update; SET31's 1.0-second timer reaches removal
// on its 35th owner update at the 30 Hz unit cadence.
constexpr int retailOwnerVmRetirementDelayTicks(int32_t valueId) {
    return valueId==26 ? 1 : valueId==31 ? 35 : -1;
}

constexpr std::optional<uint32_t> retailOwnerVmRetirementTick(uint32_t writeTick,
                                                               int32_t valueId) {
    const int delay=retailOwnerVmRetirementDelayTicks(valueId);
    if(delay<0)return std::nullopt;
    return writeTick+uint32_t(delay);
}

// The display COB VM is paced independently from unit simulation updates, so
// mirror the native owner stop edges explicitly after either host write. SET26
// requests unit removal on the next owner update; SET31 starts the one-second
// owner timer that stops subsequent VM updates. The native host applies these
// meanings directly from the value ID, without checking the unit's death state.
constexpr bool retailOwnerVmStopsOnSetUnitValue(int32_t valueId) {
    return valueId==26 || valueId==31;
}

constexpr bool retailOwnerVmMayAdvance(bool stopRequested) {
    return !stopRequested;
}

} // namespace tak
