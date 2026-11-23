#pragma once

// Gameplay routes use the executable-verified RetailSearchWorker phases.
// PathSearch is retained below as a legacy comparison harness. PathService's
// admission wrapper still differs from the retail singleton scheduler; see
// docs/pathfinding-port.md for integration gaps and executable comparisons.

#include <array>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <optional>
#include <utility>
#include <vector>
#include "fixed.h"
#include "retailtrace.h"
#include "retailgoal.h"
#include "retailscheduler.h"
#include "navigationtelemetry.h"

namespace tak::sim {

// Cell grades from retail's passability query (icd 0x4139d0 -> 0x413c80 ->
// 0x4db640). Every call site in the original compares against 4, so a cell
// scoring below that is refused.
inline constexpr int kCellThreshold = 4;
inline constexpr int kCellImpassable = 0;   // terrain, or a PARKED body -- retail
                                            // grades both 0 (icd 0x509020)
inline constexpr int kCellMoving = 1;       // a body under way: icd 0x50917c. Below
                                            // the threshold like a wall, but distinct,
                                            // so grade comparisons rank it above one.
inline constexpr int kCellUnitBlocked = 2;  // the SEARCH's grade for a body it may not
                                            // pass: retail's live query bails to
                                            // 0x4db893, which returns 2 (the tracer
                                            // scores through that query, 0x4139d0 ->
                                            // 0x413c80 -> 0x4db640, not the raw map)
inline constexpr int kCellSameWay = 5;      // a body the query lets the search THROUGH:
                                            // moving, not slower than us, heading
                                            // within 90 degrees (icd 0x4db767). The
                                            // one grade the tracer marks (kScore5),
                                            // which is how retail knows a route leant
                                            // on traffic that may not part.
inline constexpr int kCellSlope = 4;        // terrain-degraded (retail's slope clamp,
                                            // 0x509318; our flat mosaic never emits it)
inline constexpr int kCellGround = 6;
inline constexpr int kCellRoad = 7;

// These are retail's 0..7 PASSABILITY grades, and units are part of them. The map the
// retail tracer searches on (the packed 4-bit array at navigator +0x348, filled through
// 0x4dfe40 and read at 0x413a67) carries these same values, refreshed as units move --
// so retail's SEARCH sees bodies and routes around a crowd, which is the entire
// go-around mechanism. The mover has no sidestep of its own. kCellOccupied (a parked
// body graded 4 = passable-but-costly) was ours, not retail's, and it is why units
// drove into crowds: a route through one cost nothing.

// The 8 compass directions in ROTATIONAL order, lifted from the tables at
// icd 0x5f304c / 0x5f3054. Index is masked & 7 throughout, so rotating the
// index by 1 turns 45 degrees and by 2 turns 90.
inline constexpr int kDirX[8] = {0, -1, -1, -1, 0, 1, 1, 1};
inline constexpr int kDirZ[8] = {-1, -1, 0, 1, 1, 1, 0, -1};

// Work charged per step, matching the original exactly: the march costs 8
// (icd 0x414844), setting up a trace costs 7 (0x414a6a), each trace step costs
// 9 (0x414c40), and the caller charges a further 30 when a waypoint comes back
// (0x415b3f).
inline constexpr int kWorkMarch = 8;
inline constexpr int kWorkTraceInit = 7;
inline constexpr int kWorkTraceStep = 9;
inline constexpr int kWorkWaypoint = 30;

// Retail's default per-frame budget (icd 0x41617b seeds both +0x221 and
// +0x225 with 0x2ee0), before the quality percentage scales it.
inline constexpr int kPathBudgetDefault = 12000;

// What a COMPLETION costs, charged against the same per-tick budget as the search
// itself. Finishing is not free: reconstruction walks the breadcrumbs and the caller
// then smooths the route, and the smoothing is superlinear in the corner count. Leaving
// it unaccounted was survivable while only one batch of searches could finish per tick;
// once freed slots are refilled within the tick, an unaccounted completion cost would
// let a tick admit many cheap searches and then pay for all of their completions at
// once -- trading a queue delay for a frame spike, which is not an improvement.
inline constexpr int kWorkCompleteBase = 64;
inline constexpr int kWorkPerCorner = 4;

struct PathCell {
    int x = 0, z = 0;
};

// One resumable search. step() burns work until the quantum runs out, then
// returns Suspended with every field intact for the next tick.
struct PathSearch {
  public:
    enum class Phase : uint8_t { Init, March, CardMarch, Trace, Done, Failed };
    enum class Result : uint8_t {
        Arrived,     // icd 0: the goal was reached
        Waypoint,    // icd -1: a point was emitted, more to do
        Suspended,   // icd -2: out of quantum, resume next tick
        Failed,      // the search gave up (closed trace, or an illegal start)
    };

    // Request.
    int unitId = 0;
    PathCell start, goal;
    int foot = 1;
    int selfId = 0;
    bool priority = false;   // five times the work share -- retail's axis is the
                             // PLAYER (+0x24e7, one bit per player slot), not the
                             // request; PathService::tick applies it per player

    // Resumable state. Field comments give the icd offset each one mirrors.
    Phase phase = Phase::Init;
    PathCell cur;                 // +0xd0 / +0xd4, the octant march cursor
    PathCell org;                 // +0xf0 / +0xf4, the cardinal march origin
    PathCell curA, curB;          // +0xf8/+0xfc and +0x100/+0x104, twin traces
    int dirA = 0, dirB = 0;       // +0x108 / +0x10c
    bool started = false;         // both cursors have taken a step
    int best = 0;                 // +0xcc, closest approach so far
    int tolCells = 0;             // traffic radius FROM START, 50 / UnitType+0x249
    bool goalCrowded = false;     // legacy name: traffic near START, not the goal
                                  // (icd 0x414563 sets unit flag bit 0)
    bool sawTraffic = false;      // another traffic cell, including a second nearby
                                  // cell after bit 0 was set (0x4145c6). The failure
                                  // report sets bit 1 from +0x50 only if +0x4c is clear.
    bool visNear = false;         // visit-time +0x4c: a score-5 cell within tolCells
                                  // of the START was MARCHED THROUGH (0x414951)
    bool visFar = false;          // visit-time +0x50: another traffic cell (0x4149ab);
                                  // these feed the FAILURE report, the walk pair
                                  // above feeds a delivered route
    PathCell bestCell{};          // the visited cell that achieved it: the endpoint of
                                  // a FAILED search's best-effort route (icd 0x415170
                                  // reconstructs on failure too -- a route toward the
                                  // nearest reachable point, which is how a unit sent
                                  // at a crowded goal walks to the crowd's edge
                                  // instead of standing on a failure)
    int nOccupied = 0;            // +0xd8
    int nGround = 0;              // +0xdc
    int nRoad = 0;                // +0xe0
    int visited = 0;              // +0xe4
    int visitLimit = 20000;       // +0xe8, set by reset() to (w+h)*20
    int work = 0;                 // +0x48, against the quantum in +0x165

    std::vector<PathCell> out;    // the finished route, in travel order

    // Per-cell scratch: retail's bitmap at +0x2c and the 4-byte record at
    // +0x1c, whose second byte is the direction the search entered the cell
    // from. That is a parent map -- it is how the route is reconstructed.
    static constexpr uint8_t kGoal = 0x4;     // icd tests bit 0x4 to stop
    static constexpr uint8_t kSeen = 0x8;
    static constexpr uint8_t kScore5 = 0x40;

    // Size the scratch to the map and clear the search. Call once per request.
    void reset(int mapW, int mapH);
    void buildRouteTo(PathCell end);
    // Legacy approximation, retained for comparison tests. Retail instead uses a
    // weighted goal heuristic, a heading-dependent neighbor fan, indexed heap
    // updates and resumable state. Its first-attempt node limit is map cells/20.
    // The former emuphase.py used an incorrect goal-distance stub; its claims
    // of Dijkstra equivalence were invalid. The corrected, separately verified
    // kernel is RetailCostSearch in retailcost.h, used by the gameplay worker.
    // Fills `out` and returns true iff it reached the goal within budget.
    bool buildDijkstraRoute(const std::function<int(int, int)>& score);

    // `score` answers retail's per-cell query for this unit. One call runs until
    // `quantum` work units are spent; call again next tick to continue.
    Result step(const std::function<int(int, int)>& score, int quantum);

private:
    int w_ = 0, h_ = 0;
    // Per-cell scratch, GENERATION-STAMPED rather than cleared. reset() used to
    // assign() both of these plus a third in buildRoute -- on a 192x192 map that
    // is ~110KB zeroed for every request, and with a request queue running every
    // tick it dominated the cost (26.2ms/tick against 19.5 baseline). Bumping a
    // counter is O(1); a cell whose stamp is stale reads as empty.
    uint32_t gen_ = 0;
    std::vector<uint32_t> stamp_;
    std::vector<uint8_t> flag_;
    std::vector<uint8_t> from_;
    std::vector<uint32_t> walkStamp_;   // buildRoute's cycle guard, same trick
    uint32_t walkGen_ = 0;
    // Dijkstra scratch, same generation-stamp trick as the tracer's. dGen_ bumps per
    // search so nothing is cleared; dDist_ is the accumulated cost, dDir_ the parent
    // direction for reconstruction.
    std::vector<uint32_t> dStamp_;
    std::vector<int32_t> dDist_;
    std::vector<uint8_t> dDir_;
    std::vector<uint8_t> dTraffic_;
    uint32_t dGen_ = 0;

    bool seen(size_t i) const { return stamp_[i] == gen_; }
    void touch(size_t i) {
        if (stamp_[i] != gen_) { stamp_[i] = gen_; flag_[i] = 0; from_[i] = 0; }
    }

    bool inside(PathCell c) const {
        return c.x >= 0 && c.z >= 0 && c.x < w_ && c.z < h_;
    }
    void mark(PathCell c, int d, int score);
    void recordTraffic(PathCell c, bool& near, bool& other) const;
    bool atGoal(PathCell c) const;
    void buildRoute();
    bool onGoalLine(PathCell org, PathCell c) const;
    bool traceStep(const std::function<int(int, int)>& score,
                   PathCell& c, int& d, int rot);
};

// The request queue and its per-tick budget scheduler (icd 0x416430).
//
// One active search is retained across ticks. Player
// shares govern admission; an active search may spend beyond its player's
// share until the GLOBAL budget is exhausted. Admission rotates players and
// scans their allocated entity slots, charging 7 even for empty slots. Init
// costs 500 and may overshoot the budget. See tools/re/emuscheduler.py, which
// executes those branches in the retail binary. RetailSearchScheduler owns
// admission and RetailSearchWorker owns resumable search phases.
//
// How many corners RECONSTRUCTION may hand back. This is NOT the navigator's
// 64-waypoint limit -- that still applies to the route actually installed (see the
// shortcut in World). The two were the same number, and that was the bug: the raw
// trace was truncated to 64 BEFORE the world smoothed it, so a trip across open
// ground lost its destination to a staircase that smoothing collapses to a single
// segment.
//
// Measured on an open-ground staircase (a goal that is neither cardinal nor 45
// degrees makes the march alternate two directions, so every step is a corner):
//
//   grid      raw corners   truncated route ended   goal        discarded
//   80x80        79           (64,32)               (79,39)      15,7 cells
//   160x160     159           (64,32)               (159,79)     95,47 cells
//   220x220     219           (64,32)               (219,109)   155,77 cells
//
// The cut is at a FIXED distance however far the goal is, and every one of those
// corners is on open ground -- so smoothing first reduces the whole trip to one
// segment and the cap never engages at all.
//
// 256 covers a full-width staircase on the largest maps we ship while still
// bounding reconstruction. Beyond it the route is genuinely partial, which
// reachedGoal already detects and handles.
inline constexpr size_t kRawRouteCap = 256;
inline constexpr int kMaxActiveSearches = 1;

// Each player's allocated entity slots, as 416430 traverses them. Retail
// (511570) gives every player a contiguous pool of maxunits records at game
// start; 5121d8 places each new unit in a CRT-ranked free slot, and a record
// stays allocated until it is retired, so a pool has holes wherever units
// died. World owns this table (its unit IDs stay global and are never reused);
// the scheduler reads it to walk slots, holes included, in retail order.
struct EntitySlotTable {
    std::array<std::vector<int32_t>,10> unitAt; // slot -> unit ID, -1 = hole
    std::vector<int32_t> slotOf;                // unit ID -> slot, -1 = none
    int slot(int unitId) const {
        return unitId>=0 && size_t(unitId)<slotOf.size() ? slotOf[size_t(unitId)] : -1;
    }
};

class PathService {
  public:
    void setTelemetry(NavigationTelemetry* observer) {
        if(observer!=telemetry_&&(telemetryTickActive_||!q_.empty()))
            throw std::logic_error("telemetry requires an empty request queue outside tick callbacks");
        telemetry_=observer;
    }
    void setBudget(int b) { budget_ = b > 0 ? b : kPathBudgetDefault; }
    int budget() const { return budget_; }
    uint64_t workSpent() const { return workSpent_; }      // observational; see workSpent_
    uint64_t completions() const { return completions_; }
    uint64_t failures() const { return failures_; }
    uint64_t requests() const { return requests_; }
    struct Diagnostics {
        uint64_t initializations=0,executionSlices=0,resetCells=0;
        uint64_t initializationNanoseconds=0,preparationNanoseconds=0,executionNanoseconds=0;
        size_t scratchBytes=0;
        // Filled by World's adapter; a standalone service has no grade cache.
        uint64_t bodySnapshotRebuilds=0;
        size_t bodySnapshotBytes=0,gradePlaneBytes=0;
    };
    // Opt-in wall-clock observations. Never read by simulation or scheduler;
    // initialization time excludes the separately measured prepare callback.
    void setProfiling(bool enabled) { profiling_=enabled; }
    bool profiling() const { return profiling_; }
    void resetDiagnostics() { diagnostics_={}; }
    Diagnostics diagnostics() const {
        auto result=diagnostics_;
        const auto& a=worker_.attempt;
        result.scratchBytes=a.cost.scratchBytes()+a.trace.cells.capacity()*sizeof(RetailCostSearch::Cell)+
            a.trace.scratch.touched.capacity()*sizeof(uint32_t)+a.route.points.capacity()*sizeof(RetailReachability::Point);
        return result;
    }
    // Allocated entity ranges include empty slots. A host binds its live slot
    // table (World does, for every match); replay hosts that restore captured
    // ID-addressed pools fix a player's range instead, which takes precedence.
    // A standalone service with neither treats IDs as slots from zero.
    void setEntitySlots(const EntitySlotTable* table) {
        if (activeId_>=0 || !q_.empty())
            throw std::logic_error("entity slots rebind requires an empty request queue");
        slots_=table;
    }
    void setEntityPool(int player,int first,int count);
    void restoreTraversal(int playerCursor,const std::array<int,10>& slots,
                          const std::array<int,10>& wraps) {
        if (activeId_>=0 || playerCursor<0 || playerCursor>=10)
            throw std::invalid_argument("invalid scheduler restore boundary");
        for (size_t i=0;i<10;++i)
            if (slots[i]<0 || (poolSize_[i] && slots[i]>=poolSize_[i]))
                throw std::invalid_argument("invalid scheduler slot cursor");
        scheduler_.playerCursor=playerCursor;
        scheduler_.slotCursor=slots; scheduler_.lastWrapTick=wraps;
    }
    struct GradeHost {
        std::function<void(int,bool)> prepare;
        std::function<int(int,int,int,int,PathCell)> score;
        std::function<void(int)> finish;
    };
    // Callbacks must outlive the service; finish also runs on cancellation.
    void setGradeHost(GradeHost host) { gradeHost_=std::move(host); }
    // Bind a copied service to the world that now holds it (World::clone), or a new
    // one to its world. A copy may carry a request queue, so unlike setEntitySlots
    // this needs none empty; observers stay with the service they were set on.
    void bindHost(const EntitySlotTable* table,GradeHost host) {
        slots_=table;gradeHost_=std::move(host);
        telemetry_=nullptr;telemetryTickActive_=false;
    }

    // Queue a search. Replaces any request already outstanding for this unit.
    void request(int unitId, PathCell start, PathCell goal, int mapW, int mapH,
                 Fixed goalX, Fixed goalZ, int player, bool priority,
                 int tolCells = 0, int heading = 0,
                 RetailCostSearch::Costs costs = {}, PathCell cellOffset = {}, int goalTolerance = 0,
                 uint64_t controller = 0, int goalRadiusSquared = 0,
                 std::optional<RetailRectGoal> rectangle = {},
                 std::optional<RetailRingGoal> ring = {});
    // Nonzero controller tokens opt into mission notifications. Phase 1 may
    // notify while phase 2 still has work; do not wait for route completion.
    // Drain after tick(), before the next mission dispatch. The caller matches
    // the token to its current controller; zero keeps the legacy route-only API.
    struct Notification { int unitId; uint64_t controller; int events; };
    std::vector<Notification> takeNotifications() { return std::exchange(notifications_, {}); }
    void cancel(int unitId,NavigationTelemetry::Cancel reason=NavigationTelemetry::Cancel::Explicit);
    bool pending(int unitId) const {
        return unitId>=0 && size_t(unitId)<pendingIds_.size() && pendingIds_[size_t(unitId)];
    }
    size_t pendingCount() const { return q_.size(); }
    void clear() {
        retireActive();
        if(telemetry_)for(const auto& [id,e]:q_) {
            (void)id;telemetry_->cancelled(e.telemetryToken,NavigationTelemetry::Cancel::Cleared);
        }
        q_.clear();
        pendingIds_.clear();
        pendingByPlayer_.fill(0);priorityByPlayer_.fill(0);
        notifications_.clear();
        scheduler_={};
        poolFirst_.fill(0); poolSize_.fill(0); fixedPool_.fill(false);
        tickNo_ = 0;
    }

    // `score(unitId, cx, cz)` answers the per-cell query for that unit's
    // movement class. `done(unitId, route, goalX, goalZ, failed, crowded)`:
    // `failed` is the navigator's partial-route bit (unit+134:4), not the
    // mission's 0x2000 failure notification. An empty failure can leave this
    // bit clear. A partial route ends at the closest approach and must not be
    // extended toward the goal; `crowded` reports kScore5-marked cells
    // within the traffic radius of the START (icd 0x414563), which selects
    // retail's faster re-ask cadence (base 10, 0x4e5226) over the normal one
    // (base 20, 0x4e5284).
    // admit checks and stamps a new request at slot selection, including when
    // that scan consumes the remaining budget. Suspended work bypasses it.
    // refresh supplies attempt inputs at initialization. At the trace-to-cost
    // transition it is called again, consuming only the current heading.
    void tick(const std::function<int(int, int, int)>& score,
              const std::function<void(int, const std::vector<PathCell>&,
                                       Fixed, Fixed, bool, bool, bool, bool)>& done,
              const std::function<void(int, PathCell&, int&, RetailCostSearch::Costs&)>& refresh = {},
              uint32_t simulationTick = 0,
              const std::function<bool(int, uint32_t)>& admit = {});

  private:
    friend struct RetailReplayProbe;
    // A queued request is just its parameters -- no per-cell scratch until it is
    // admitted and handed a pool slot.
    struct Entry {
        PathCell start, goal;
        int mapW = 0, mapH = 0;
        Fixed goalX = Fixed(), goalZ = Fixed();
        int player = 0;         // owner: the budget is split per PLAYER (icd 0x4164fa)
        int tolCells = 0;       // origin-relative traffic radius for this unit's type
        bool priority = false;
        int heading = 0, goalTolerance = 0;
        PathCell cellOffset;
        RetailCostSearch::Costs costs;
        int notification = 0;
        uint64_t controller = 0;
        int goalRadiusSquared = 0;
        std::optional<RetailRectGoal> rectangle; // footprint-origin coordinates
        std::optional<RetailRingGoal> ring;
        uint64_t telemetryToken=0;
    };
    uint64_t tickNo_ = 0;       // monotonic, integer: tells "already ran this tick" apart
    std::vector<Notification> notifications_;
    // OBSERVATIONAL ONLY -- never read by the scheduler, never hashed. Exists so a
    // benchmark can report what a crowd actually costs the pathfinder instead of
    // guessing from tick counts.
    uint64_t workSpent_ = 0;
    uint64_t completions_ = 0;
    uint64_t failures_ = 0;      // searches that gave up (visit limit / boxed in)
    uint64_t requests_ = 0;      // admissions, i.e. how often a route was asked for
    bool profiling_=false;
    Diagnostics diagnostics_;
    NavigationTelemetry* telemetry_=nullptr;
    bool telemetryTickActive_=false; // Observer lifetime guard; not simulation state.

    int budget_ = kPathBudgetDefault;
    // Requests are looked up by ID only. RetailSearchScheduler owns traversal
    // order (player and allocated entity slot), never this container's order.
    std::unordered_map<int, Entry> q_;
    // Unit id -> has an entry in q_. pending() runs for every routed unit
    // every tick; a hash probe there was a scattered cache miss per unit.
    std::vector<uint8_t> pendingIds_;
    // Derived queue counts: avoid walking thousands of scattered map nodes
    // each tick merely to reconstruct the scheduler's ten player records.
    std::array<int,10> pendingByPlayer_{},priorityByPlayer_{};
    void eraseRequest(std::unordered_map<int,Entry>::iterator it);
    RetailSearchWorker worker_;
    RetailSearchScheduler scheduler_;
    std::array<int,10> poolFirst_{},poolSize_{};
    std::array<bool,10> fixedPool_{};
    const EntitySlotTable* slots_=nullptr;
    // A unit's offset in its player's pool, or -1 when it has none.
    int slotOf(int unitId,int player) const;
    int poolCapacity(int player) const;
    int unitAtSlot(int player,int slot) const;
    int activeId_=-1;
    GradeHost gradeHost_;
    void retireActive();
};

// Legacy service's Chebyshev distance in cells. Retail's controller query is
// different: the verified point-goal metric is retailGoalDistance in retailcost.h.
int pathDist(PathCell a, PathCell b);

// Delta -> one of the 8 directions (icd 0x415040).
int pathDirFromDelta(int dx, int dz);

}   // namespace tak::sim
