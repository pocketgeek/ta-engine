#pragma once

// Retail's pathfinder, ported from KINGDOMS.icd by static analysis.
//
// It is NOT an A*. There is no open list, no priority queue and no cost-to-goal
// ordering anywhere in the original. It marches straight at the goal and, when
// something blocks it, traces the obstacle's outline from BOTH sides at once
// until one of them regains the origin-to-goal line -- a "bug" algorithm. See
// docs/retail-engine.md for the full disassembly notes; addresses in the
// comments below are the icd entry points the code mirrors.
//
// Everything here is integer arithmetic on 16px cells, and the per-frame work
// budget is an integer too (retail's default is 12000 units), so a match runs
// identically on every peer. No timers, no floats: safe for lockstep.
//
// Three phases, mirroring the original's state machine at +0x60:
//   1  OCTANT march from the true start, counting the quality of what it
//      crosses and giving up on the third occupied cell (icd 0x414833).
//   2  CARDINAL march from the current origin, laying a breadcrumb in every
//      cell it enters, until something blocks it (0x414a6a).
//   3  TWIN boundary traces, counter-rotating, running at the same time; the
//      first one to regain the straight line from the origin to the goal
//      becomes the new origin and hands back to phase 2 (0x414c25, with the
//      M-line test at 0x414dc4 and the hand-offs at 0x414fde / 0x414ff8).
// Reaching a cell flagged as the goal ends the search, and the route is
// reconstructed by walking the breadcrumbs back (0x414450).
//
// This IS the sim's pathfinder. PathService below owns the queue, the bounded pool of
// concurrent searches and the per-tick budget; World::requestPath feeds it and installs
// finished routes as order legs (replaceLeg). The header used to end "NOT WIRED INTO
// THE SIM YET", which was true only until it was.

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace tak::sim {

// Cell grades from retail's passability query (icd 0x4139d0 -> 0x413c80 ->
// 0x4db640). Every call site in the original compares against 4, so a cell
// scoring below that is refused.
inline constexpr int kCellThreshold = 4;
inline constexpr int kCellImpassable = 0;   // terrain, or a body that blocks
inline constexpr int kCellOccupied = 4;     // a parked body: passable, costly
inline constexpr int kCellGround = 6;
inline constexpr int kCellRoad = 7;

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
    enum class Phase : uint8_t { Init, March, CardMarch, Trace, Done, Failed, AStar };
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
    bool priority = false;   // retail's +0x24e7 flag: five times the work share
    // Use the bounded A* instead of the boundary tracer.
    //
    // The tracer is cheap and right for ordinary trips, and it stays the default. It is a
    // BUG ALGORITHM though: it walks an obstacle's outline until it regains the goal
    // line, which in terrain that keeps interrupting that line means following every wall
    // it meets. Measured on a six-baffle serpentine, 12 units, 240s:
    //
    //   straight line          2550px
    //   shortest legal path    3565px   (x1.40 -- the maze itself forces only this)
    //   what units travelled  15909px   (x6.24) = x4.46 of the best possible route
    //
    // and it cost 3.5M work against ~300k for the same crowd on open ground, with only
    // 2 failures in 710 searches. So this is not a failure-to-find problem -- the tracer
    // succeeds, and returns a route four and a half times longer than necessary, then the
    // unit re-asks and gets another one.
    bool useAStar = false;

    // Resumable state. Field comments give the icd offset each one mirrors.
    Phase phase = Phase::Init;
    PathCell cur;                 // +0xd0 / +0xd4, the octant march cursor
    PathCell org;                 // +0xf0 / +0xf4, the cardinal march origin
    PathCell curA, curB;          // +0xf8/+0xfc and +0x100/+0x104, twin traces
    int dirA = 0, dirB = 0;       // +0x108 / +0x10c
    bool started = false;         // both cursors have taken a step
    int best = 0;                 // +0xcc, closest approach so far
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
    // A* scratch, stamped by the same generation trick so it costs nothing to reset.
    std::vector<uint32_t> gStamp_;      // generation for gScore_
    std::vector<int32_t> gScore_;       // cost from the start, in 10/14 units
    std::vector<int32_t> open_;         // binary heap of cell indices
    std::vector<int32_t> openF_;        // f-score parallel to open_ (heap key)
    void aStarPush(int32_t cell, int32_t f);
    int32_t aStarPop();                 // lowest f; ties broken by cell index
    void buildAStarRoute();
    uint32_t walkGen_ = 0;

    bool seen(size_t i) const { return stamp_[i] == gen_; }
    void touch(size_t i) {
        if (stamp_[i] != gen_) { stamp_[i] = gen_; flag_[i] = 0; from_[i] = 0; }
    }

    bool inside(PathCell c) const {
        return c.x >= 0 && c.z >= 0 && c.x < w_ && c.z < h_;
    }
    void mark(PathCell c, int d, int score);
    bool atGoal(PathCell c) const;
    void buildRoute();
    bool onGoalLine(PathCell org, PathCell c) const;
    bool traceStep(const std::function<int(int, int)>& score,
                   PathCell& c, int& d, int rot);
};

// The request queue and its per-tick budget scheduler (icd 0x416430).
//
// Retail counts every pending request across all players, splits one integer
// work budget between them, and gives each a slice; flagged requests get five
// times the share. Each search then runs until its slice is spent and resumes
// next tick. Requests are keyed by unit id and visited in that order, so the
// whole thing is deterministic and safe to run inside the lockstep sim.
//
// We depart from retail in ONE way: at most kMaxActiveSearches run at a time,
// and the rest wait their turn. Two reasons, and neither costs throughput.
//
// MEMORY. The per-cell scratch is the expensive part of a PathSearch -- two
// uint32 stamp arrays and two byte arrays, 10 bytes per map cell. That is
// 360 KiB per search on a 192x192 map (measured), so one-search-per-pending-
// request makes footprint a product of map area and how many units happen to be
// ordered at once: ~176 MB for 500 pending, on the referee as well as every
// client. Only the active searches now own scratch, and they own it in a POOL
// that is reused, so a completed request hands its arrays to the next one
// instead of freeing and reallocating them.
//
// BUDGET. `quantum = max(1, budget / share)` is not a cap: once share exceeds
// the budget every request still gets 1, so total work per tick grows without
// limit as requests pile up. Bounding the active set bounds `share`, which
// makes the budget mean what it says.
//
// Throughput is unaffected because the budget is fixed either way: running 100
// searches at 1/100th speed each and running them 12 at a time finish the whole
// set at the same tick. Bounding only changes the ORDER -- and it improves the
// early latencies, since a search that gets a real slice finishes in a tick or
// two instead of all of them crawling together.
//
// Admission rotates (admitCursor_) rather than always starting at the lowest
// unit id, so a busy low-numbered unit cannot starve a high-numbered one. The
// cursor is an integer advanced in map order: deterministic, like the rest.
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
inline constexpr int kMaxActiveSearches = 12;

class PathService {
  public:
    void setBudget(int b) { budget_ = b > 0 ? b : kPathBudgetDefault; }
    int budget() const { return budget_; }
    uint64_t workSpent() const { return workSpent_; }      // observational; see workSpent_
    uint64_t completions() const { return completions_; }
    uint64_t failures() const { return failures_; }
    uint64_t requests() const { return requests_; }

    // Queue a search. Replaces any request already outstanding for this unit.
    void request(int unitId, PathCell start, PathCell goal, int mapW, int mapH,
                 float goalX, float goalZ, bool priority, bool useAStar = false);
    void cancel(int unitId);
    bool pending(int unitId) const { return q_.find(unitId) != q_.end(); }
    size_t pendingCount() const { return q_.size(); }
    void clear() {
        q_.clear();
        slotOwner_.assign(slotOwner_.size(), -1);   // hand every slot back
        admitCursor_ = -1;
        tickNo_ = 0;
    }

    // `score(unitId, cx, cz)` answers the per-cell query for that unit's
    // movement class. `done(unitId, route, goalX, goalZ)` receives a finished
    // route in travel order -- empty if the search failed.
    void tick(const std::function<int(int, int, int)>& score,
              const std::function<void(int, const std::vector<PathCell>&,
                                       float, float)>& done);

  private:
    // A queued request is just its parameters -- no per-cell scratch until it is
    // admitted and handed a pool slot.
    struct Entry {
        PathCell start, goal;
        int mapW = 0, mapH = 0;
        float goalX = 0, goalZ = 0;
        bool priority = false;
        int slot = -1;          // index into pool_, or -1 while queued
        int cap = 0;            // icd +0x165: grows by the quantum each tick
        bool useAStar = false;  // this request wants the bounded planner
        uint64_t ranAt = 0;     // tick this entry last got a slice (see the refill loop)
    };
    uint64_t tickNo_ = 0;       // monotonic, integer: tells "already ran this tick" apart
    // OBSERVATIONAL ONLY -- never read by the scheduler, never hashed. Exists so a
    // benchmark can report what a crowd actually costs the pathfinder instead of
    // guessing from tick counts.
    uint64_t workSpent_ = 0;
    uint64_t completions_ = 0;
    uint64_t failures_ = 0;      // searches that gave up (visit limit / boxed in)
    uint64_t requests_ = 0;      // admissions, i.e. how often a route was asked for

    int budget_ = kPathBudgetDefault;
    std::map<int, Entry> q_;    // unit id order: deterministic
    std::vector<PathSearch> pool_;    // the only owners of per-cell scratch
    std::vector<int> slotOwner_;      // unit id in each slot, -1 = free
    int admitCursor_ = -1;            // admit starting after this unit id

    void admit(int unitId, Entry& e, int slot);
    void release(Entry& e);
};

// Chebyshev distance in cells. Retail's 0x413e50 returns a distance the search
// only ever compares, so the metric that matters is the 8-connected one its
// own stepping uses.
int pathDist(PathCell a, PathCell b);

// Delta -> one of the 8 directions (icd 0x415040).
int pathDirFromDelta(int dx, int dz);

}   // namespace tak::sim
