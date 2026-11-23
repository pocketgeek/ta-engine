#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace tak::sim {

struct RetailAiBuildChoice {
    uint16_t type=0;
    int32_t weight=0;
    bool special=false,sameFaction=true;
};

inline int32_t retailAiBuildWeight(bool limited,int32_t desired,int16_t count,
                                   uint8_t preference,int8_t priority) {
    if (limited && desired!=-1 && count>=desired) return 0;
    return (int32_t(preference)*int32_t(priority))/100;
}

template<class Random>
uint16_t retailAiChooseBuild(const std::vector<RetailAiBuildChoice>& choices,bool special,Random random) {
    uint32_t total=0;
    const RetailAiBuildChoice* selected=nullptr;
    for (const auto& choice:choices) {
        if (choice.weight<=0 || choice.special!=special) continue;
        total+=uint32_t(choice.weight);
        if (int32_t(random(std::bit_cast<int32_t>(total)))<choice.weight) selected=&choice;
    }
    return selected && selected->sameFaction?selected->type:0;
}

template<class Choose,class Cost,class Random,class Queue>
bool retailAiPlanFactory(float allocation,bool queued,uint16_t population,
                         Choose choose,Cost cost,Random random,Queue queue) {
    if (double(allocation)<0.7*(1.0/3.0) || queued) return false;
    const uint16_t selected=choose();
    if (!selected) return false;
    const float price=cost(selected);
    if (double(allocation)<0.7 && price>500.0f) {
        const int bound=int((double(price)-500.0)*double(0.0033333334140479565f)+2.0);
        if (random(bound)!=0 && population<uint32_t(random(30))+30u) return false;
    }
    queue(selected);return true;
}

struct RetailAiUnitClass {
    uint32_t flags=0,typeFlags=0,secondaryFlags=0;
    bool constructor=false,mover=false,mission=false;
    uint8_t missionFlags=0;
    int16_t capacity=0;
};

inline bool retailAiCategory(const RetailAiUnitClass& unit,unsigned category) {
    if (!(unit.flags&0x1000000u) || (unit.flags&0x1000u)) return false;
    const bool structure=(unit.flags&0x2000000u)!=0;
    const bool combat=!structure && !unit.constructor && (unit.flags&0x8000000u);
    switch (category) {
    case 0: return true;
    case 1: return structure;
    case 2: case 3:
        return !structure && unit.constructor && !(unit.secondaryFlags&0x40000u) && unit.mover &&
               (category==2 || !unit.mission || !(unit.missionFlags&8));
    case 4: return combat && !(unit.typeFlags&0x800u);
    case 5: return combat && (unit.typeFlags&0x800u);
    case 6: return combat && unit.mover && (unit.typeFlags&0x80000u) && unit.capacity>0;
    case 7: return !unit.constructor;
    default: return false;
    }
}

struct RetailAiCentroid {
    uint32_t x=0,z=0,count=0;
    void add(int32_t rawX,int32_t rawZ) {
        x+=uint32_t(int32_t(std::bit_cast<int16_t>(uint16_t(uint32_t(rawX)>>16))));
        z+=uint32_t(int32_t(std::bit_cast<int16_t>(uint16_t(uint32_t(rawZ)>>16))));
        ++count;
    }
    std::optional<std::pair<int32_t,int32_t>> center() const {
        if (!count) return std::nullopt;
        return std::pair{std::bit_cast<int32_t>(uint32_t(std::bit_cast<int32_t>(x)/int32_t(count))<<16),
                         std::bit_cast<int32_t>(uint32_t(std::bit_cast<int32_t>(z)/int32_t(count))<<16)};
    }
};

inline int32_t retailAiBaseRadius(int32_t footprintSum,unsigned category) {
    const uint32_t scale=category==0?14:category==1?10:category==2?7:category==3?5:1;
    uint64_t value=footprintSum>0?uint64_t(footprintSum)*scale*scale:0;
    uint64_t root=0,bit=uint64_t(1)<<62;
    while (bit>value) bit>>=2;
    while (bit) {
        if (value>=root+bit) { value-=root+bit;root=(root>>1)+bit; }
        else root>>=1;
        bit>>=2;
    }
    const uint32_t radius=uint32_t(root)/2;
    return int32_t((radius>5*scale?radius:5*scale)*16);
}

struct RetailAiGroupClock {
    bool present = false;
    uint32_t deadline = 0;
};

struct RetailAiSchedule {
    int32_t assignmentCountdown = 30;
    // The original manager has 100 slots, but dispatch visits only 0..98.
    std::array<RetailAiGroupClock,100> groups{};
};

enum class RetailAiSquadKind : uint8_t { Base, Strike, Backup, Vtol, Reserve };

struct RetailAiSquad {
    RetailAiSquadKind kind = RetailAiSquadKind::Reserve;
    uint32_t active = 0, dirty = 0;
    // Saved squad parameters at +0c..2c. Their interpretation depends on kind.
    std::array<int32_t,9> parameters{};
    std::vector<int> members;

    void setActive(uint32_t value) {
        if (active != value) { active=value; dirty=1; }
    }
};

struct RetailAiState {
    RetailAiSchedule schedule;
    std::array<RetailAiSquad,100> squads;
    std::map<int,std::pair<int16_t,int16_t>> anchors;
    bool anchorsRestored=false;
    bool initialized = false;
    uint32_t scenarioDeadline = 0;
};

struct RetailAiBaseCandidate {
    bool occupied=false;
    int32_t eligibleCount=0;
    std::optional<std::pair<int32_t,int32_t>> center;
};

// 40af70 visits base slots 1..20. Each squared fixed-point component is
// truncated separately before addition; equal distances retain the first slot.
inline unsigned retailNearestAiBase(const std::array<RetailAiBaseCandidate,20>& bases,
                                    int32_t x,int32_t z,int32_t minimum=0) {
    unsigned selected=0;
    int32_t distance=0;
    for (unsigned i=0;i<bases.size();++i) {
        const auto& base=bases[i];
        if (!base.occupied || (minimum>0 && base.eligibleCount<minimum) || !base.center) continue;
        const int64_t dx=std::bit_cast<int32_t>(uint32_t(base.center->first)-uint32_t(x));
        const int64_t dz=std::bit_cast<int32_t>(uint32_t(base.center->second)-uint32_t(z));
        const int32_t d=std::bit_cast<int32_t>(uint32_t((dx*dx)>>32)+uint32_t((dz*dz)>>32));
        if (!selected || d<distance) { selected=i+1;distance=d; }
    }
    return selected;
}

// The actual squad entry points schedule themselves before inspecting members.
// Strike squads may recruit from their backup even when initially empty.
// Nonempty bodies are host operations, not assumed to be inert.
template<class Random, class Recruit, class Plan>
void retailTickAiSquad(RetailAiSquad& squad, RetailAiGroupClock& clock,
        uint32_t tick, Random random, Recruit recruit, Plan plan) {
    switch (squad.kind) {
    case RetailAiSquadKind::Base: // 40b320
        clock.deadline=tick+uint32_t(random(30))+15u;
        if (squad.members.empty()) squad.parameters[0]=0;
        else plan(squad);
        break;
    case RetailAiSquadKind::Strike: // 40e180
        clock.deadline=tick+uint32_t(random(60))+15u;
        recruit(squad);
        if (squad.members.empty()) { squad.parameters[5]=0; squad.setActive(0); }
        else plan(squad);
        break;
    case RetailAiSquadKind::Backup: // 40ea30
        clock.deadline=tick+150u;
        if (!squad.members.empty()) plan(squad);
        break;
    case RetailAiSquadKind::Vtol: // 40eb40
        clock.deadline=tick+uint32_t(random(30))+15u;
        if (squad.members.empty()) squad.setActive(0);
        else plan(squad);
        break;
    case RetailAiSquadKind::Reserve: // 4101e0
        clock.deadline=tick+1800u;
        break;
    }
}

// 40fd80: group membership is refreshed before assignment or the first due
// group, at most once per tick. Host callbacks own membership and commands;
// the scheduler must not substitute empty groups for unsupported planners.
template<class Random, class Refresh, class Assign, class Update>
void retailTickAiSchedule(RetailAiSchedule& state, uint32_t tick, Random random,
        Refresh refresh, Assign assign, Update update) {
    bool refreshed = false;
    state.assignmentCountdown = std::bit_cast<int32_t>(uint32_t(state.assignmentCountdown)-1u);
    if (state.assignmentCountdown <= 0) {
        state.assignmentCountdown = random(7)+3;
        refresh();
        refreshed = true;
        assign();
    }
    for (unsigned i=0; i<99; ++i) {
        auto& group = state.groups[i];
        if (!group.present || group.deadline > tick) continue;
        if (!refreshed) {
            refresh();
            refreshed = true;
        }
        // Refresh/assignment can alter group state. The original reloads the
        // group object after refresh and does not repeat its deadline test.
        update(i,group);
    }
}

} // namespace tak::sim
