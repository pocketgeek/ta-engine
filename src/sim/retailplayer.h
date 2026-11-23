#pragma once

#include <cstdint>
#include <algorithm>
#include <array>
#include <vector>
#include <optional>
#include <string>
#include "retailconstruction.h"
#include "retailconstructionparticles.h"

namespace tak::sim {

struct UnitType;

// 401270 rolls thirty samples; 401330/401350 deliberately average only the
// oldest twenty-nine. The newest sample does not affect this tick's pressure.
// samples[i] = {produced, requested}: the resource object's +0xc and +0x10.
// `usage` is the mana REQUESTED this tick, not what was paid: retail
// construction and repair (429af0, 429d90) add the whole full-rate request to
// +0x10 before the allocation scales the work or the store caps the payment,
// so a short economy requests more than it earns. Both readers use it: the
// HUD's -N (401350) and the AI's shortfall test in 410810 (401330 - 401350).
struct RetailResourceHistory {
    std::array<std::array<float,2>,30> samples{};
    float income=0,usage=0;
    void advance() {
        for (size_t i=1;i<samples.size();++i) samples[i-1]=samples[i];
        samples.back()={income,usage}; income=usage=0;
    }
    bool shortfall(float stored) const {
        double produced=0,used=0;
        for (size_t i=0;i<29;++i) { produced+=samples[i][0]; used+=samples[i][1]; }
        const float averageIncome=float(produced*double(1.034482717514038f));
        const double net=double(averageIncome)-used*double(1.034482717514038f);
        return net<-0.01 || (net<0.01 && double(stored)<0.01);
    }
};

struct RetailPlayerCacheClock {
    bool enabled=false;
    uint32_t lastRefresh=0;
};

// One catalogue entry in 410810. The host supplies current counts, resource
// pressure and the player's desired composition; these are not timeless type
// properties. Kept separate from the clock until World supplies those inputs.
// What the fields hold in retail (docs/classic-ai-plan.md §1.3): flags is type
// +0x260 (0x10000 any weapon block, 0x100 the FBI builder flag -- "mobile" below
// -- and 0x1000000 isfeature); secondaryFlags is +0x264 (bit 31 a yardmap 'S'
// cell, a mana site -- "resourceProducer" below); `weapon` is the movement
// class minwaterdepth (+0x194, >= 0 for water units); `desired` is the AI
// profile limit. ai::ClassicTypeInfo::priorityInputs fills them by those names.
struct RetailBuildPriority {
    uint32_t flags=0,secondaryFlags=0;
    int16_t count=0,completed=0,weapon=-1;
    int32_t desired=-1;
    float cost=0;
};

struct RetailBuildCacheEntry {
    const UnitType* type=nullptr;
    RetailBuildPriority inputs;
    uint8_t priority=0;
    bool hasEconomy=false;
    float income=0,storage=0;
    struct Construction {
        RetailConstructionType type;
        float workerTime=0;
        RetailConstructionEmitter emitter;
    };
    std::optional<Construction> construction;
};

struct RetailBuildCache {
    struct PlannerType {
        std::vector<uint16_t> choices;
        std::string faction;
        uint8_t weight=0;
        bool special=false;
    };
    struct Planner {
        bool limited=false;
        std::vector<PlannerType> types;
    };
    std::optional<Planner> planner;
    RetailResourceHistory resources;
    // Retail catalogue order is gameplay state: each entry may draw RNG.
    // Slot zero is excluded; it is the catalogue sentinel.
    std::vector<RetailBuildCacheEntry> entries;
};

template<class Random>
uint8_t retailBuildPriority(const RetailBuildPriority& type,uint16_t population,
                           float resourceRatio,bool shortfall,Random random) {
    int score=(type.flags&0x10000u) ? 21 : 1;
    const bool mobile=(type.flags&0x100u)!=0;
    const bool resourceProducer=(type.secondaryFlags&0x80000000u)!=0;
    if (mobile && type.completed==type.count) {
        if (type.desired<=2) score+=15/(shortfall ? 3 : 1);
        else if (type.count<=type.desired/4) score+=30/(shortfall ? 2 : 1);
        else if (type.count<=type.desired/2) score+=10/(shortfall ? 2 : 1);
        else score+=6/(shortfall ? 2 : 1);
    }
    if (resourceProducer && resourceRatio<0.01f) score+=30;
    if (resourceProducer && resourceRatio<0.3f) score+=30;
    if (type.cost>500.0f) {
        // Preserve the intermediate product before conversion to integer.
        // The oracle explicitly selects 53-bit x87 precision.
        const int penalty=int((double(type.cost)-500.0)*double(0.01f));
        auto reduce=[&] { score-=int(random(std::min(score,penalty))); };
        if (population<int(random(30))+10) {
            reduce(); reduce(); if (!mobile) reduce();
        } else if (population<int(random(40))+40) {
            reduce(); if (!mobile) reduce();
        } else if (population<int(random(80))+80) {
            score-=int(random(std::min(score,penalty)))/2;
        }
    }
    if (type.count==0) score*=4;
    else if (type.count==1) score*=2;
    if (type.weapon>=0) score*=3;
    if (type.flags&0x1000000u) score=0;
    if (resourceProducer && (resourceRatio>=0.3f || !shortfall || type.completed<type.count)) score=0;
    return uint8_t(std::min(score,100));
}

// 411a90: player unit/catalogue bookkeeping (including human players).
// Unsigned last+7 comparison is intentional, including its wrap behavior.
// Returns whether the periodic availability-cache rebuild was selected.
// The World host answers availability/count queries from current state rather
// than retaining retail's caches, but this clock still advances the shared RNG.
template<class Random>
bool retailTickPlayerCache(RetailPlayerCacheClock& clock,uint32_t tick,Random random) {
    if (!clock.enabled || tick<clock.lastRefresh+7u) return false;
    clock.lastRefresh=tick;
    return random(30)==0;
}

} // namespace tak::sim
