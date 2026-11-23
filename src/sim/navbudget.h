#pragma once
// navbudget.h -- navigation work and byte accounting, the per-tick budgets and the deterministic
// service policy of Legion's scheduler (Legion 10,000 stage 4).
//
// Everything here counts in deterministic units -- cells touched, relaxations, registrations,
// entries -- never wall time, worker order or machine memory, so a policy that reads these numbers
// decides the same on every peer. Wall time and heap are measured by the tools, never here.
//
// The service policy (navDrr, navPromote, NavAdmit) is header-only so that navbudget_test drives
// exactly the code the navigator's scheduler runs.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tak::sim {

// Work classes. Each counts its own deterministic unit (see NavBudget for what one unit is).
enum class NavWork : uint8_t {
    FieldRelax,     // field relaxations (neighbour checks; Stats::fieldWork)
    FieldInit,      // field potential cells initialised when a build starts
    PlaneBuild,     // first (or re-) builds of a footprint-legality plane: advanceBuild's cell units
    PlaneRelabel,   // whole-plane relabels and synchronous rebuilds (cells x 2, as charged to planeDebt)
    Registration,   // registerMove: a base cost plus join candidates plus goal-resolution cells
    Awareness,      // awareScan: members gathered, candidate pairs, conflict samples, descent cells
    SlotSearch,     // destination assignment: formation / packed slot / re-choice cells
    RingBuild,      // reach-ring construction cells (buildRing)
    Connectivity,   // incremental plane refresh after a static change (refreshPlane)
    TileBuild,      // stage 6 (sparse tiles); 0 until then
    RouteSearch,    // stage 6 (hierarchical search); 0 until then
    Count
};
inline constexpr const char* kNavWorkNames[size_t(NavWork::Count)] = {
    "field_relax", "field_init", "plane_build", "plane_relabel", "registration", "awareness",
    "slot_search", "ring_build", "connectivity", "tile_build", "route_search"};

// The route consumers that state the region they need (Legion 10,000 stage 5):
// on a paused build each asks for its region (a need) and reads it through the certainty checks; one that meets a
// read a later slice could change has the route run on to done (a run-on). Observation counters per consumer.
enum class RouteNeed : uint8_t {
    Lanes,       // the passage gate, the passage lanes and the pinwheel (their chain, strip and arc)
    Awareness,   // group awareness's descent chain from the group's centroid
    SoftBlock,   // MV-05's chains from the centroid and the tail
    Slots,       // packed per-goal slots (the window round the goal)
    Ring,        // the reach ring's spots
    Entrance,    // a formation's entrance search and the slot exchange at it
    RingDirect,  // a ring member's walk to its own spot
    Count
};
inline constexpr const char* kRouteNeedNames[size_t(RouteNeed::Count)] = {
    "lanes", "awareness", "softblock", "slots", "ring", "entrance", "ringdirect"};

// Service classes of the field scheduler, highest priority first.
enum class NavClass : uint8_t {
    Waiting,        // a member stands still for want of this field (or a paused build it waits on)
    FirstRoute,     // a group's first field; its members steer by direct lines / the partial build
    AwareRepair,    // a refresh asked for by group awareness (a crossing planned round)
    StaleRefresh,   // a refresh after a static change; the members steer by the finished stale field
    Count
};
inline constexpr const char* kNavClassNames[size_t(NavClass::Count)] = {
    "waiting", "first_route", "aware_repair", "stale_refresh"};

// Deterministic bytes: element counts times fixed element sizes (the constants below, not sizeof, so
// every platform and standard library reports the same number). Capacity slack, allocator headers and
// fragmentation are NOT here: the tools measure the process heap separately (SCALE_WALL heap_kb).
struct NavBytes {
    uint64_t planes = 0, fields = 0, tiles = 0, portals = 0, queues = 0, scratch = 0, metadata = 0, inflight = 0;
    uint64_t total() const { return planes + fields + tiles + portals + queues + scratch + metadata + inflight; }
};
// Fixed element sizes for NavBytes (bytes). A tree node is charged kNode on top of its payload.
struct NavSize {
    static constexpr uint64_t kNode = 32;          // one std::map / std::set node's links and colour
    static constexpr uint64_t kCell32 = 4, kCell16 = 2, kCell8 = 1, kMemo = 8, kBox = 16, kKeyed = 8;
    static constexpr uint64_t kGroup = 768;        // a Group with its scalar fields (vectors charged apart)
    static constexpr uint64_t kField = 1200;       // a Field: scalars, its 128 bucket headers
    static constexpr uint64_t kMember = 640;       // a Member (route charged apart)
    static constexpr uint64_t kPoint = 320;        // a Point (cells, ids, queue charged apart)
    static constexpr uint64_t kCorridor = 104;     // a Field::Corridor
    static constexpr uint64_t kMover = 160;        // an awareness snapshot mover
};

// Observation-only accounting (LegionNavigator::accounting): never hashed, never read by a decision.
// `total` is cumulative per class; `tickMax` the most any one navigator tick window (one tick() to the
// next: the tick, then the commands applied before the next one) spent per class; `tickAll` the
// most of every class together in one window; `bytes` the current NavBytes and `bytesPeak` its largest
// total so far (sampled when accounting() is read).
struct NavAccounting {
    std::array<uint64_t, size_t(NavWork::Count)> total{}, tickMax{};
    uint64_t tickAll = 0;
    NavBytes bytes;
    uint64_t bytesPeak = 0;
    // The scheduler's service record (stage 4): the longest a field job asking for work waited for a
    // slice while in a class (ticks since its last slice or since it entered the class, whichever is
    // later, by the class it was served in) and in any class (serviceGapAny), the longest a start request
    // waited (ticks from its request to its field starting, by its own class), job-ticks spent in a class
    // above the job's own (age promotion), capacity refusals the fit scan stepped over, reservations (a
    // refused request old enough that nothing may overtake it), registrations that waited for the budget
    // and for their plane, and the longest such wait (ticks).
    std::array<uint64_t, size_t(NavClass::Count)> serviceGapMax{}, requestAgeMax{};
    uint64_t serviceGapAny = 0;
    uint64_t promotions = 0, fitSkips = 0, reservations = 0;
    // Start requests listed in a class above their own (request-ticks: age promotion of requests), the
    // most start requests listed in one tick (the queue the request ages are measured against), and
    // starts admitted after the room scan had stopped (a reservation or the fit-scan bound): only by
    // sharing a field that already exists, which takes no room; and every start request admitted (a new
    // build or a share: the admission rate the queue drains at).
    uint64_t requestPromotions = 0, requestsMax = 0, stoppedShares = 0, admissions = 0;
    // Evictions of a shared field (dropped from every holder at once), and the most builds of its kind in
    // flight (asking for work) just after a new build was admitted while more than half the byte allowance
    // was in use -- first builds and refreshes; the per-kind cap (kFirstInflight / kRefreshInflight) keeps
    // them within it.
    uint64_t sharedEvictions = 0, firstInflightMax = 0, refreshInflightMax = 0;
    // The most field cells the start requests listed in one tick would need together (their windows): the
    // queue, in memory, a request's wait is measured against (tools/legion_sched_load_test.py).
    uint64_t requestCellsMax = 0;
    // The longest run of consecutive ticks in which a start request was refused for room and no new build
    // started (admission liveness under memory pressure: every finished field is a victim one tenure after
    // it starts, so this stays within kFieldTenure + kStillScan + the longest build).
    uint64_t admitGapMax = 0;
    uint64_t regDeferred = 0, regWaitMax = 0, planeWaits = 0;
    // Synchronous stages that ran over their budget unit (reported, never hidden): whole-plane
    // relabels/rebuilds done in one call, the largest such call (units), and the largest single
    // field initialisation (cells).
    uint64_t syncPlaneCalls = 0, syncPlaneMax = 0, initMax = 0;
    // Route demand (stage 5): the most demand entries one paused build held at once, and the demands that
    // could not be represented (dropped from a full list: a subscriber whose need the build then never saw).
    uint64_t demandPeak = 0, demandDropped = 0;
    // Stated regions (stage 5): needs raised on a paused build, and routes made to run on to done, per consumer.
    std::array<uint64_t, size_t(RouteNeed::Count)> routeNeeds{}, routeRunOns{};
    // Stage 6 (hierarchical routes): corridor fields built, corridors widened for a member outside them, plans
    // that fell back to the window because the search did not reach every member's region within its bound.
    uint64_t corridors = 0, corridorWidens = 0, routeFallbacks = 0;
    // Tile chains: chains built, shared tiles built and found in the cache, tiles evicted, the cache's largest size
    // (bytes), chain regions left off (nothing to lead into, or a parent left off) and regions left off because
    // their sector was relabelled since the plan.
    uint64_t chains = 0, tilesBuilt = 0, tileHits = 0, tileEvictions = 0, tileBytesPeak = 0, chainSkips = 0, chainAbandons = 0;
    uint64_t chainRepairs = 0;   // halo regions attached because a member stepped into one
    uint64_t tileCrossHits = 0;  // tile hits by a route to other goals than the route that built the tile
    // ---- Stage 7 (static navigation apart from traffic; each stays 0 until the step that builds its mechanism) ----
    // Static changes: routes a closure reached (refreshed at once) and routes only improvements reached (refresh
    // deferred), deferred refreshes promoted by time and by a member's need, trapped members kept trapped through a
    // change that could not reopen their component (and re-registered by one that could).
    uint64_t routesClosed = 0, routesOpened = 0, improveRefreshes = 0, improveDemands = 0, trappedKept = 0, trappedReopened = 0;
    uint64_t chainResolves = 0;   // chain regions re-resolved on new geometry (their dirty tile rebuilt, the route kept)
    uint64_t planRepairs = 0;     // chain refreshes that repaired the plan instead of searching again
    // Traffic: sector traffic counts recomputed and region steps the coarse search charged for traffic; members stamped
    // as standing jams (soft to other orders); traffic corrections (tiles whose key holds the route's traffic view,
    // apart from the static terrain tiles) built and found in their own cache.
    uint64_t trafficSectors = 0, trafficCharges = 0;
    uint64_t jamStamps = 0;
    uint64_t correctionsBuilt = 0, correctionHits = 0;
    // ---- Stage 8 (chokepoints, subgroups, arrivals) ----
    // Chokepoints: opening records made (a narrow crossing of a sector boundary, bounded on both sides; legion.cpp
    // Opening), recounts of their entrants and the member-entrants counted, the largest queue delay any opening had
    // (cells), fields built with queue charges and the charges they carried, re-plans because an opening's delay
    // moved away from the charge a route was built with, and region-search steps charged for a queue.
    uint64_t openings = 0, openingScans = 0, openingEntrants = 0, openingDelayMax = 0;
    uint64_t chargedFields = 0, openingCharges = 0, openingReplans = 0, openingSearchCharges = 0;
    uint64_t openingSplits = 0, splitMembers = 0;   // subgroups split off a big group for another passage, and their members
    // Admission at narrow passages: bodies admitted, member-updates spent waiting, turns of a passage's direction
    // and closings (the other way waited kAdmitGreen ticks).
    uint64_t passAdmits = 0, admitWaits = 0, admitTurns = 0, admitClosings = 0;
    uint64_t sharedAreas = 0;   // member registrations that joined an earlier order's destination area (one crowd)
    uint64_t groupEntrances = 0;   // entrances found for a point's further groups (their own plane, field and way in)
    // ---- Route requests on large maps (docs/legion-pathfinding.md "Hierarchical routes and tiles", "Static changes
    // and routes") ----
    // Plans made for a group whose bounded window had failed, where the whole component's window is large; plans
    // over the seeds' own cells (every member cut off from the goals); window builds charged for their buried seeds'
    // tiles, and those charged for the window after all (a step opened before the first slice).
    uint64_t fullPlans = 0, seedPlans = 0, seedBoundStarts = 0, seedBoundRestores = 0;
    // Members trapped because their static component holds none of their group's goals, trapped members that went
    // on as the member they were when an improvement joined their component to the goals again, and member-ticks
    // spent standing off a touched corridor or chain (the refresh they wait for is a Waiting job).
    uint64_t cutTraps = 0, trappedResumed = 0, awaitedTicks = 0;
};

// Per-tick budgets (constants: the same on every peer). The reference cost per unit was measured on an
// Intel Core Ultra 9 275HX (build-o2).
struct NavBudget {
    // Field service: relaxations + initialisation + ring cells per tick. A quarter of it is the most
    // the refresh classes (AwareRepair + StaleRefresh) may take, so first routes keep most of it.
    static constexpr uint64_t kFieldQuota = 384'000;
    static constexpr uint64_t kRefreshQuota = kFieldQuota / 4;
    // One field cell initialised costs 1/kInitDiv of a relaxation unit (a 4-byte fill against a
    // neighbour test with soft and corridor reads).
    static constexpr uint64_t kInitDiv = 16;
    // Deficit round robin: the slice a job may spend per visit (a multiple of the paused-build
    // slice, so a paused build stops at the same points however its work is cut), and the window: how
    // many of a class's oldest jobs share the quota at once (about as many as one tick's quota gives a
    // slice each; the refresh classes as many as the refresh allowance does).
    static constexpr int64_t kSlice = 32768;
    static constexpr size_t kWindow = 12, kRefreshWindow = 3;
    // New builds in flight (asking for work), by kind over every class, besides each class's own window,
    // while more than half the field byte allowance is in use: a first build sits in the Waiting or the
    // FirstRoute window, a refresh in the AwareRepair or the StaleRefresh one, so each kind may fill the two
    // windows it can be served in -- and no more, whichever class its jobs move to (at most one window's
    // worth of them then waits unserved, holding memory others need).
    static constexpr size_t kFirstInflight = 2 * kWindow, kRefreshInflight = 2 * kRefreshWindow;
    // Age promotion: a job (or start request) that has gone this many ticks without service moves up
    // one class, so no class starves another for longer than (classes - 1) x kAgeStep.
    static constexpr uint32_t kAgeStep = 150;
    // Bounded fit scan: start requests refused for capacity that the admission steps over per tick --
    // kWaitFitScan in the Waiting class (members stand still for these: every one that fits should
    // start), kFitScan in the others -- and the age at which a refused request reserves: nothing after
    // it may start until it does.
    static constexpr int kFitScan = 8, kWaitFitScan = 256;
    static constexpr uint32_t kReserveAge = 150;
    // Field memory: deterministic bytes (potentials + in-flight frontiers) every live field may use
    // together. A fixed allowance, not a number of whole maps, with a floor of two whole-map fields
    // so that no single request can be larger than the allowance on any map.
    static constexpr uint64_t kFieldBytes = uint64_t(128) << 20;
    // Registration: units per tick (kRegBase per registration plus its join candidates and its goal
    // resolution cells). Registrations requested between ticks (orders) wait in a FIFO drained at
    // the start of the next navigator tick; one requested during a tick runs at once while the budget
    // lasts and the FIFO holds nothing it could overtake.
    static constexpr uint64_t kRegBase = 64;
    static constexpr uint64_t kRegBudget = 4096 * kRegBase;
    // Connectivity: first plane builds a registration waits for (advanceBuild units per tick).
    static constexpr uint64_t kPlaneBuildBudget = 1'048'576;
    // Awareness: candidate pairs + conflict samples + descent cells per tick; a scan that needs more
    // resumes next tick on the same snapshot.
    static constexpr uint64_t kAwareBudget = 262'144;
};

// The class a job serves in after `age` ticks without service (age promotion).
constexpr NavClass navPromote(NavClass base, uint32_t age) {
    const int c = int(base) - int(age / NavBudget::kAgeStep);
    return NavClass(c < 0 ? 0 : c);
}

// One visit's outcome: work spent, and whether the job stays in this tick's ring (it still has work
// and could use more). A visit that spends nothing must return keep = false.
struct NavVisit { uint64_t spent = 0; bool keep = false; };

// A class's round-robin position, kept across ticks (hashed by the navigator): the key of the last job
// served, and whether its visit was cut short by the budget with deficit left (it resumes first, without
// a new slice: one slice per job per round, as deficit round robin requires).
struct NavCursor {
    uint64_t key = 0;
    bool resume = false;
};

// Deficit round robin over a bounded window of a class's jobs. `jobs` are in ascending key order (the key
// is the job's start serial, so the oldest first); the window holds the oldest `window` jobs that still
// want work in this call, and a job that leaves it (finished, paused, or out of work) is replaced by the
// next oldest. Each round visits the window from the cursor (the first key above it, or the cut-short job
// itself), so no job of the window is always the one the end of a tick reaches; a visit adds `slice` to
// the job's deficit (capped at two slices; nothing on a resumed visit) and lets it spend up to that. It
// ends when `budget` (and `cap`, a second limit the same work also spends, e.g. the refresh allowance;
// nullptr: none) is spent or no job wants work. The window bounds how many jobs share the quota at once,
// so each still finishes promptly (a refresh is worth nothing until it is done, and the next awareness
// re-plan discards one still building); jobs behind it wait their turn oldest first, and age promotion
// (navPromote) bounds how long that is.
// Job: { uint64_t key; int64_t* deficit; ... }. serve(Job&, allowance) -> NavVisit.
template <class Job, class Serve>
void navDrr(const std::vector<Job>& jobs, size_t window, NavCursor& cursor, uint64_t& budget, uint64_t* cap,
            Serve&& serve, int64_t slice = NavBudget::kSlice) {
    auto room = [&] { return budget > 0 && (!cap || *cap > 0); };
    std::vector<Job> ring, kept;
    size_t next = 0;
    while (room()) {
        while (ring.size() < window && next < jobs.size()) ring.push_back(jobs[next++]);
        if (ring.empty()) return;
        size_t start = 0;
        while (start < ring.size() && (cursor.resume ? ring[start].key < cursor.key : ring[start].key <= cursor.key)) ++start;
        if (start == ring.size()) start = 0;
        const bool resuming = cursor.resume && ring[start].key == cursor.key;
        cursor.resume = false;
        kept.clear();
        for (size_t n = 0; n < ring.size(); ++n) {
            Job& j = ring[(start + n) % ring.size()];
            if (!room()) { kept.push_back(j); continue; }
            int64_t& d = *j.deficit;
            if (!(resuming && n == 0)) d = std::min<int64_t>(d + slice, 2 * slice);
            if (d <= 0) { kept.push_back(j); continue; }
            uint64_t allow = std::min<uint64_t>(uint64_t(d), budget);
            if (cap) allow = std::min(allow, *cap);
            const NavVisit v = serve(j, allow);
            budget -= std::min(budget, v.spent);
            if (cap) *cap -= std::min(*cap, v.spent);
            d -= int64_t(v.spent);
            if (v.spent) cursor = {j.key, d > 0 && v.keep && !room()};
            if (v.keep && v.spent) kept.push_back(j);
            else d = 0;   // left the window: an idle job carries no credit (DRR)
        }
        std::sort(kept.begin(), kept.end(), [](const Job& a, const Job& b) { return a.key < b.key; });
        ring.swap(kept);
    }
}

// The bounded fit scan of one tick's admission: start requests are tried class by class, oldest first;
// one refused for capacity is stepped over -- at most kWaitFitScan times a tick in the Waiting class
// (their members stand still: every one that fits should start), kFitScan in each other class -- unless
// it has been refused for kReserveAge ticks: then it reserves, and nothing after it takes new room this
// tick, in its class or any later one, so the capacity that frees next is its (no request is overtaken for
// ever). Behind a stopped scan (the bound or a reservation) a request may still start by sharing a field
// that already exists: that takes no room, so it delays nobody (serveFields: shareOnly).
// (Letting the Waiting class's smaller requests start round a Waiting reservation was measured too: a few
// percent more arrivals in the overloaded many-squad rows, but a large waiting request could then be
// overtaken without end.) `blockedSince` is the request's own (hashed) stamp, kNever when clear. Call
// nextClass() between classes.
struct NavAdmit {
    static constexpr uint32_t kNever = ~0u;
    int skips = 0;
    bool waiting = true;       // the class being admitted is the Waiting class
    bool classStopped = false; // the room scan of this class ended (its bound, or a reservation)
    bool reserved = false;     // a request reserved this tick: the rest of the tick takes no new room
    // The room scan of this class has ended: its remaining requests (and, after a reservation, every
    // later class's) may still start, but only by sharing a field that already exists (no new room).
    bool stopped() const { return classStopped; }
    void reserve() { reserved = classStopped = true; }
    // A refusal; true when this class's scan must stop.
    bool refuse(uint32_t& blockedSince, uint32_t now) {
        if (blockedSince == kNever) blockedSince = now;
        if (now - blockedSince >= NavBudget::kReserveAge) { reserve(); return true; }
        if (++skips >= (waiting ? NavBudget::kWaitFitScan : NavBudget::kFitScan)) { classStopped = true; return true; }
        return false;
    }
    // The next class: a fresh scan bound, stopped outright if a request reserved.
    void nextClass() { waiting = false; skips = 0; classStopped = reserved; }
    static void admitted(uint32_t& blockedSince) { blockedSince = kNever; }
};

}  // namespace tak::sim
