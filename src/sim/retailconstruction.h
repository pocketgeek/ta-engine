#pragma once

#include <cstdint>
#include <optional>
#include "retailmission.h"
#include "fixed.h"

namespace ta::sim {

// 405756..405809: failed MobileBuild paths may still leave the builder close
// enough to place the site. Each footprint diagonal is truncated separately.
// Retail subtracts in 32 bits and reads the distance as a signed 16-bit word
// of whole pixels (movsx at 4057ec), so 32768 px or more reads as negative and
// "within reach". No retail map has such a distance; a map from 1449 cells a
// side has (its diagonal). Ours: the differences and the distance are taken
// whole, and a far site is far. Identical below 32768 px.
inline bool retailBuildWithinReach(Fixed x,Fixed z,Fixed gx,Fixed gz,
        int16_t fx,int16_t fz,int16_t siteFx,int16_t siteFz,uint16_t reach) {
    const int64_t dx=int64_t(x.v)-int64_t(gx.v);
    const int64_t dz=int64_t(z.v)-int64_t(gz.v);
    const int distance=int(isqrt64(uint64_t(dx*dx)+uint64_t(dz*dz))>>16);
    const auto edge=[](int16_t width,int16_t height) {
        return int(isqrt64(uint64_t(int64_t(width)*width+int64_t(height)*height)*64));
    };
    return distance-edge(fx,fz)-edge(siteFx,siteFz)<=reach;
}

struct RetailConstructionResources {
    float stored=0,allocation=1,requested=0,produced=0;
    double totalProduced=0;
    float capacity=1,capacityOverride=0;
    double excess=0;

    // 51d837: income and demand are accumulated by the host in unit-slot
    // order. Retail uses this decimal tick scale, not exact 1/30.
    void beginTick(float income,float storage,float demand) {
        const double gain=double(capacityOverride>0 ? 0.f : income)*0.03333333;
        stored=float(double(stored)+gain);
        totalProduced+=gain;
        produced=float(double(produced)+gain);
        capacity=storage>0 ? storage : 1.f;
        const double required=double(demand)*0.03333333;
        allocation=required>double(stored) ? float(double(stored)/required) : 1.f;
    }

    // 401270 clamps only after unit work, immediately before history rolls.
    void finishTick() {
        if (capacityOverride>0) capacity=capacityOverride;
        if (stored<0) { excess+=double(stored);stored=0; }
        if (stored>capacity) {
            excess+=double(stored)-double(capacity);stored=capacity;
        }
    }
};

struct RetailConstructionProgress {
    float remaining=1;
    uint16_t hp=0;
    uint32_t events=0,flags=0;
};

// COB BUILD_PERCENT_LEFT (50d1ab): unfinished work maps to 1..100.
inline uint32_t retailConstructionPercent(float remaining) {
    return remaining==0 ? 0u : uint32_t(1-int32_t(double(remaining)*-99.0));
}

struct RetailConstructionType {
    float inverseTime=0,cost=0;
    uint32_t maxHp=0;
};

// 4023fd..402490: GetBuilt while work remains. Completion, weapon hosts and
// unconjure accounting are separate operations; the mission owns their timing.
template<class StopWeapons,class Decay>
int retailGetBuiltWaiting(RetailMissionState& mission,uint32_t tick,uint32_t events,
                          StopWeapons stopWeapons,Decay decay) {
    switch (mission.stage) {
    case 0:
        stopWeapons();mission.sleep(tick,300);mission.waitMask|=0x10000000u;return 1;
    case 1:
        mission.sleep(tick,30);mission.waitMask|=0x10000000u;return 1;
    case 2:
        if (events&0x10000000u) mission.sleep(tick,30);
        else if (events&1) { mission.sleep(tick,1);decay(); }
        mission.waitMask|=0x10000000u;return 2;
    default:return 7;
    }
}

// One dispatch (4d8450) of an unfinished site's GetBuilt (402220) when the
// site's own mission is its only one: `worked` is the unit's 0x10000000 event
// at dispatch time (429af0 sets it for any work >= 0, paid or not). A fresh
// mission sleeps 300 ticks, a work event wakes it to sleep 30 more, and from
// then on every work event restarts the 30 while a timer wake with no work
// decays the site once and sleeps one tick. True when this dispatch decays.
inline bool retailSiteTimer(RetailMissionState& mission,uint32_t tick,bool worked) {
    struct Host {
        RetailMissionState& m;uint32_t tick;bool decay=false;
        bool enabled() const { return true; }
        RetailMissionState* head() { return &m; }
        bool hasNext(const RetailMissionState&) const { return false; }
        void idle() {}
        uint32_t random(int) { return 0; }
        void remove(RetailMissionState&) {}
        void rotate(RetailMissionState&) {}
        void clear() {}
        int handle(RetailMissionState& state,uint32_t events) {
            return retailGetBuiltWaiting(state,tick,events,[]{},[&]{ decay=true; });
        }
    } host{mission,tick};
    uint32_t events=worked ? 0x10000000u : 0u;
    retailDispatchMissions(tick,events,host);
    return host.decay;
}

// 429d70(site, 1): the decay is 429af0(site, site, 1 * -0.5f), unconjure work
// done by the site on itself, so the refund goes to the SITE's owner. Each
// call takes 0.5 / buildtime of the whole build off the site and pays back
// float(that * buildcost), at full value whatever the owner's allocation.
// 429af0 removes the site, refunding nothing, once its remaining work would
// reach the whole build (remaining + progress >= 1).
struct RetailSiteDecay { double progress=0; float refund=0; };
inline RetailSiteDecay retailSiteDecay(float inverseTime,float cost) {
    const double progress=0.5*double(inverseTime);
    return {progress,float(progress*double(cost))};
}

struct RetailConstructionSite {
    RetailConstructionProgress progress;
    RetailConstructionType type;
    std::optional<RetailMissionState> mission;
    int builder=0;
};

struct RetailConstructionJob {
    int target=0;
    bool flying=false;
    uint32_t flyingOwnerFlags=0;
    int32_t flyingSlowSpeed=0,flyingFastSpeed=0;
    uint16_t flyingBuildDistance=0;
    float workerTime=0;
    bool working=false;
    uint32_t activityDeadline=0;
    std::optional<uint16_t> turnHeading; // inserted Turn mission before build-stance wait
    RetailMissionState mission;
};

// 429990's completion eligibility and notifications. The host performs the
// returned attachment, activation and network effects in that order.
struct RetailConstructionCompletion {
    bool builderPresent=false,forced=false,sameUnit=false,allied=false;
    uint32_t builderFlags=0,builderTypeFlags=0,builderSecondaryFlags=0,siteTypeFlags=0;
    bool ownerPresent=false,attached=false;
    uint8_t ownerRole=0,deathType=0;
};
struct RetailConstructionCompletionEffects {
    bool applied=false,detach=false,activate=false,notify=false;
    uint8_t deathType=0;
};
inline RetailConstructionCompletionEffects retailCompleteConstruction(
        RetailConstructionProgress& site,const RetailConstructionCompletion& input) {
    RetailConstructionCompletionEffects effects;effects.deathType=input.deathType;
    const bool builderAllocated=input.builderPresent && (input.builderFlags&0x1000000u);
    if ((!input.forced && !builderAllocated) || !(site.flags&0x1000000u)) return effects;
    if (builderAllocated) {
        const bool constructionAllowed=(input.builderTypeFlags&0x100u) &&
            (!(input.builderSecondaryFlags&0x2000000u) || input.allied);
        if (!constructionAllowed && !(input.builderSecondaryFlags&0x20001000u) && !input.sameUnit)
            return effects;
    }
    effects.applied=true;site.remaining=0;site.flags|=0x800u;
    const bool controlled=input.ownerPresent && (input.ownerRole==1 || input.ownerRole==2);
    effects.detach=controlled && !(site.flags&0x2000000u) && input.attached;
    effects.activate=(input.siteTypeFlags&0x40000u)!=0;
    if (input.siteTypeFlags&0x1000000u) { effects.deathType=9;site.flags|=0x1000u; }
    effects.notify=controlled && !input.forced;
    return effects;
}

// 5d3d54 (_ftol): truncate through a 64-bit fistp and keep the low word. Out of
// range stores the integer indefinite, whose low word is zero.
inline uint32_t retailFtol(double x) {
    if (!(x>-9223372036854775808.0 && x<9223372036854775808.0)) return 0;
    return uint32_t(uint64_t(int64_t(x)));
}

// 429af0: work is the builder's per-update contribution. Negative work is
// unconjuring. Requested consumption is recorded BEFORE affordability scaling.
// Completion/death callbacks remain with the host; flags and return behavior
// are independent of whether the caller chooses to display build particles.
enum class RetailConstructionResult { Idle, Worked, Completed, Removed };
inline RetailConstructionResult retailConstructionWork(
        RetailConstructionProgress& site,const RetailConstructionType& type,
        RetailConstructionResources& resources,float work) {
    if (site.remaining==0) return RetailConstructionResult::Idle;
    double amount;
    if (work>=0) {
        site.events|=0x10000000u;
        amount=double(resources.allocation)*double(work);
    } else amount=-double(work);
    if (amount<=0) return RetailConstructionResult::Idle;
    double progress=amount*double(type.inverseTime);
    float cost=float(progress*double(type.cost));
    if (work>0) {
        resources.requested=float(double(resources.requested)+
            double(type.inverseTime)*double(type.cost)*double(work));
        if (cost>resources.stored && cost>0) {
            const double fraction=resources.stored>0 ? double(resources.stored)/double(cost) : 0;
            cost=float(fraction*double(cost));
            progress*=fraction;
        }
        site.remaining=float(double(site.remaining)-progress);
        resources.stored=float(double(resources.stored)-double(cost));
        if (site.remaining<=0) {
            site.remaining=0;site.hp=uint16_t(type.maxHp);
            return RetailConstructionResult::Completed;
        }
        // 429c93 stores _ftol's low word: a lethal hit (51a3e0) can leave remaining
        // above 1, so the product may be negative (UB through an unsigned cast).
        site.hp=uint16_t(retailFtol((1.0-double(site.remaining))*double(type.maxHp)));
        site.flags|=0x800u;
        return RetailConstructionResult::Worked;
    }
    const double remaining=double(site.remaining)+progress;
    site.remaining=float(remaining);
    if (remaining>=1) { site.hp=0;return RetailConstructionResult::Removed; }
    site.hp=uint16_t(retailFtol((1.0-remaining)*double(type.maxHp)));
    site.flags|=0x800u;
    resources.stored=float(double(resources.stored)+double(cost));
    resources.totalProduced+=double(cost);
    resources.produced=float(double(resources.produced)+double(cost));
    return RetailConstructionResult::Idle;
}

// 4bff28..4bff79: the parser reads a nonpositive buildtime as 100 and keeps its
// reciprocal at UnitType+0x216, which every build and repair multiplies by.
inline float retailInverseBuildTime(float buildTime) {
    return float(1.0/double(buildTime>0 ? buildTime : 100.0f));
}

// SELF-REPAIR, 51e1a8 -> 429d90 (unpaid). healtime is the unit's own worker time,
// not seconds per hit point: every eighth tick (the caller's cadence and gates: a
// finished, living unit below full health whose healtime is nonzero) it puts
// healtime * 0.26666664 -- eight thirtieths of a second of that work -- into itself
// as a fraction of its type's build, so it regains maxdamage * healtime / buildtime
// hit points a second. 429d90 turns the fraction into hit points as a 16.16 product
// of maxdamage, adds the unit's carried remainder (+0x10e: bits 8..15 of the last
// sum; bits 0..7 are dropped every call) and returns the whole points it hands to
// 51a140 as damage type 12.
inline uint16_t retailSelfRepairPoints(uint8_t& carry,float healTime,float inverseTime,
                                       uint32_t maxHp) {
    const float work=float(double(healTime)*0.26666664);
    const double progress=double(work)*double(inverseTime);
    const uint32_t sum=(uint32_t(carry)<<8)+retailFtol(double(maxHp)*progress*65536.0);
    carry=uint8_t(sum>>8);
    return uint16_t(sum>>16);
}

// 51a2f8..51a319, damage type 12: the points are added to the signed 16-bit hit
// points as an unsigned word, and anything at or past maxdamage becomes maxdamage.
inline uint16_t retailAddHitPoints(uint16_t hp,uint16_t points,uint32_t maxHp) {
    const uint32_t sum=uint32_t(points)+uint32_t(int32_t(int16_t(hp)));
    return uint16_t(sum>=maxHp ? maxHp : sum);
}

} // namespace ta::sim
