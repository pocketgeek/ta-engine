#include "sim/pathsearch.h"
#include "sim/retailtrace.h"
#include "util/devenv.h"

#include <bit>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace tak::sim {

static const bool g_pathDbg = tak::devEnv("TAK_PATHDBG") != nullptr;

int pathDist(PathCell a, PathCell b) {
    return std::max(std::abs(a.x - b.x), std::abs(a.z - b.z));
}

int pathDirFromDelta(int dx, int dz) {
    return retailSearchDirection(dx, dz);
}

// Cardinal direction toward a delta (icd 0x414ae9): x dominates, ties go north.
static int cardinalToward(int dx, int dz) {
    if (dx < 0) return 2;
    if (dx > 0) return 6;
    return dz <= 0 ? 0 : 4;
}

void PathSearch::reset(int mapW, int mapH) {
    const bool resized = (w_ != mapW || h_ != mapH);
    w_ = mapW;
    h_ = mapH;
    // icd 0x414797: (width + height) * 20. Retail abandons a search that wanders
    // this far and leaves the unit on its straight segment, so long or tangled
    // routes legitimately fail rather than costing unbounded work.
    visitLimit = (mapW + mapH) * 20;
    const size_t n = size_t(w_) * size_t(h_);
    if (resized || stamp_.size() != n) {
        stamp_.assign(n, 0);
        walkStamp_.assign(n, 0);
        flag_.assign(n, 0);
        from_.assign(n, 0);
        dStamp_.assign(n, 0);
        dDist_.assign(n, 0);
        dDir_.assign(n, 0);
        dTraffic_.assign(n, 0);
        gen_ = walkGen_ = dGen_ = 0;
    }
    ++gen_;   // every cell is now stale, i.e. empty -- no clearing needed
    phase = Phase::Init;
    best = 0;
    goalCrowded = false;
    sawTraffic = false;
    visNear = false;
    visFar = false;
    visited = 0;
    work = 0;
    nOccupied = nGround = nRoad = 0;
    out.clear();
}

// Lay a breadcrumb: remember how we entered `c`, and flag it seen. Mirrors
// icd 0x414b57..0x414b9d -- the bitmap write plus the 4-byte per-cell record
// whose second byte is the incoming direction.
void PathSearch::mark(PathCell c, int d, int score) {
    if (!inside(c)) return;
    const size_t i = size_t(c.z) * size_t(w_) + size_t(c.x);
    touch(i);
    // FIRST VISIT WINS. Overwriting the incoming direction on a revisit turns
    // the parent map into a graph with cycles, and the backtrack then walks a
    // little loop for ever instead of reaching the start -- observed as a
    // 4-cell cycle repeated to the 64-waypoint clamp. This is a legacy port
    // guard, not retail's bitmap behavior: the verified probe in retailtrace.h
    // overwrites directions and hands the plane to a separate cost search.
    if (flag_[i] & kSeen) return;
    from_[i] = uint8_t(d);
    flag_[i] = uint8_t(flag_[i] | kSeen | (score == 5 ? kScore5 : 0));
    // RETAIL ACCUMULATES THE CROWD EVIDENCE AT VISIT TIME, right here in the
    // march (icd 0x414951-0x4149ab): the first score-5 cell within the type's
    // origin-relative radius latches +0x4c, any other latches +0x50. The failure report
    // (0x414450 called with a nonzero arg from the -1 branch of the step runner,
    // 0x415b53) converts these to the navigator's outcome bits, +0x4c first --
    // emulated to confirm: {4c=1}->bit0, {50=1}->bit1, {both}->bit0, {none}->0.
    // The reconstruction walk recomputes the same pair over BREADCRUMBS for a
    // delivered route; visNear/visFar here serve the failure report.
    if (score == 5) recordTraffic(c, visNear, visFar);
}

void PathSearch::recordTraffic(PathCell c, bool& near, bool& other) const {
    // Both visit-time 414951 and reconstruction 414563 compare with +30/+32:
    // the search origin. Once near is set, another traffic cell sets bit 1,
    // even if it too is near. The traversal order is therefore significant.
    if (!near && tolCells > 0 &&
        std::max(std::abs(c.x - start.x), std::abs(c.z - start.z)) < tolCells)
        near = true;
    else other = true;
}

bool PathSearch::atGoal(PathCell c) const {
    if (!inside(c)) return false;
    const size_t i = size_t(c.z) * size_t(w_) + size_t(c.x);
    return seen(i) && (flag_[i] & kGoal) != 0;
}

// Walk the breadcrumbs back from the goal and hand out the corners in travel
// order (icd 0x414450 does the same over its own 0x218-byte buffer). Only
// direction CHANGES become waypoints -- a straight run needs no intermediate
// points -- and the navigator caps the list at 64 (0x4e4ea0).
static bool stepLegal(const std::function<int(int, int)>& score, PathCell c, int d);

void PathSearch::buildRoute() { buildRouteTo(goal); }

// Legacy Dijkstra approximation, not retail-equivalent. The earlier goal-distance
// emulation stub was wrong. Retail's verified weighted, resumable replacement is
// in retailcost.h; this service still needs the scheduler/tracer hand-off port.
bool PathSearch::buildDijkstraRoute(const std::function<int(int, int)>& score) {
    out.clear();
    if (!inside(start) || !inside(goal)) return false;

    // The exact per-grade cell cost from init (emuphase.py): the grade the score()
    // callback returns (retail's 0..7) maps to the entry cost. Grades below the
    // threshold are impassable and never entered.
    auto gradeCost = [](int g) -> int {
        switch (g) {
            case kCellRoad:   return 8;    // 7, +0xc4
            case kCellGround: return 24;   // 6, +0xc0
            case kCellSlope:  return 48;   // 4, +0xbc
            case kCellSameWay:return 80;   // 5, +0xc8
            default:          return -1;   // 0..3: impassable
        }
    };
    // Step cost by direction: cardinal 16, diagonal 23 (retail +0x90). Turn cost by
    // the change in heading from the parent's incoming direction, |d| in 0..4 mapping
    // to 0/80/120/160/200 (retail +0x70). Directions are the tracer's rotational
    // order (kDirX/kDirZ), so a delta of 1 is 45 degrees.
    static const int kStepCost[8] = {16, 23, 16, 23, 16, 23, 16, 23};
    static const int kTurnCost[5] = {0, 80, 120, 160, 200};

    const size_t goalI = size_t(goal.z) * size_t(w_) + size_t(goal.x);
    if (score(start.x, start.z) < kCellThreshold) return false;

    ++dGen_;
    // Binary min-heap of (cost, cell, dir): cost primary, cell index tie-break so
    // ties resolve identically on every machine.
    struct Node { int32_t cost; int32_t cell; uint8_t dir; };
    std::vector<Node> heap;
    auto less = [](const Node& a, const Node& b) {
        return a.cost != b.cost ? a.cost > b.cost : a.cell > b.cell;  // min-heap
    };
    auto relax = [&](int32_t cell, int32_t cost, uint8_t dir, bool traffic) {
        if (dStamp_[size_t(cell)] == dGen_ && dDist_[size_t(cell)] <= cost) return;
        dStamp_[size_t(cell)] = dGen_;
        dDist_[size_t(cell)] = cost;
        dDir_[size_t(cell)] = dir;
        dTraffic_[size_t(cell)] = uint8_t(traffic);
        heap.push_back({cost, cell, dir});
        std::push_heap(heap.begin(), heap.end(), less);
    };
    const int32_t startCell = int32_t(start.z * w_ + start.x);
    relax(startCell, 0, 0xff, false);   // 0xff = no incoming direction

    // Node budget: retail bounds phase 2 at map cells / 10 (icd 0x415c47). Beyond it
    // the scheduler abandons phase 2 and falls back -- for us, to the tracer route.
    const int budget = std::max(64, (w_ * h_) / 10);
    int expanded = 0;
    bool reached = false;

    while (!heap.empty()) {
        std::pop_heap(heap.begin(), heap.end(), less);
        const Node top = heap.back();
        heap.pop_back();
        // Stale heap entry (a cheaper path to this cell already popped): skip.
        if (dStamp_[size_t(top.cell)] != dGen_ || dDist_[size_t(top.cell)] != top.cost)
            continue;
        if (size_t(top.cell) == goalI) { reached = true; break; }
        if (++expanded > budget) break;

        const int cx = top.cell % w_, cz = top.cell / w_;
        for (int d = 0; d < 8; ++d) {
            const int nx = cx + kDirX[d], nz = cz + kDirZ[d];
            if (nx < 0 || nz < 0 || nx >= w_ || nz >= h_) continue;
            const int grade = score(nx, nz);
            const int gc = gradeCost(grade);
            if (gc < 0) continue;                       // impassable
            // NO corner-cut guard here (unlike the tracer's stepLegal): retail's cost
            // function (0x413e70) grades only the destination cell, so a diagonal may
            // round a wall corner. Validated against an OBSERVED retail route: on a
            // wall-detour the search reaches the goal at cost 1084, byte-for-byte the
            // cost of the route retail's own search produced on the same grid. The
            // mover's per-cell solidity handles the corner the diagonal rounds.
            // Turn cost from the parent's incoming heading. The start (dir 0xff) pays
            // none; otherwise the |rotational difference|, folded to 0..4.
            int turn = 0;
            if (top.dir != 0xff) {
                int diff = (d - int(top.dir)) & 7;
                if (diff > 4) diff = 8 - diff;
                turn = kTurnCost[diff];
            }
            const int32_t nc = top.cost + kStepCost[d] + turn + gc;
            relax(int32_t(nz * w_ + nx), nc, uint8_t(d), grade == kCellSameWay);
        }
    }
    if (!reached) return false;

    // Reconstruct start->goal from the parent-direction map, emitting a waypoint only
    // at each change of direction (retail's route is direction-change corners), and
    // capping at retail's 64 waypoints (icd 0x4e4ea0).
    std::vector<PathCell> rev;
    int c = int(goalI);
    int lastDir = -1;
    for (int guard = 0; guard < w_ * h_ + 8; ++guard) {
        const int cx = c % w_, cz = c / w_;
        if (cx == start.x && cz == start.z) break;
        const int d = dDir_[size_t(c)];
        if (d == 0xff) break;
        if (dTraffic_[size_t(c)])
            recordTraffic({cx, cz}, goalCrowded, sawTraffic);
        if (d != lastDir) { rev.push_back({cx, cz}); lastDir = d; }
        c = (cz - kDirZ[d]) * w_ + (cx - kDirX[d]);
    }
    out.assign(rev.rbegin(), rev.rend());
    if (out.size() > 64) out.resize(64);
    return true;
}

// Reconstruct the breadcrumb walk back from `end`. Retail reconstructs on FAILURE
// too (0x415170), from the closest-approach cell -- the best-effort route that walks
// a unit to the reachable edge of wherever it was sent, rather than leaving it to
// steer a straight line into the first wall.
void PathSearch::buildRouteTo(PathCell end) {
    out.clear();
    if (!inside(end)) return;
    std::vector<PathCell> rev;
    ++walkGen_;
    PathCell c = end;
    int lastDir = -1;
    for (int guard = 0; guard < 8192; ++guard) {
        if (c.x == start.x && c.z == start.z) break;
        const size_t i = size_t(c.z) * size_t(w_) + size_t(c.x);
        if (!seen(i) || !(flag_[i] & kSeen)) break;
        if (walkStamp_[i] == walkGen_) break;   // belt and braces against a cycle
        walkStamp_[i] = walkGen_;
        const int d = from_[i] & 7;
        // Match retail's backtracking order and origin-relative traffic flags.
        if (flag_[i] & kScore5)
            recordTraffic(c, goalCrowded, sawTraffic);
        if (d != lastDir) { rev.push_back(c); lastDir = d; }
        PathCell p{c.x - kDirX[d], c.z - kDirZ[d]};
        if (!inside(p) || (p.x == c.x && p.z == c.z)) break;
        c = p;
    }
    // The first backtracked point is already `end`. A failed search may end
    // at bestCell, short of an unreachable goal. Appending the requested goal
    // fabricated a final leg through the obstacle; with no progress it even
    // fabricated a route from a boxed-in start. Only emit traced points.
    for (auto it = rev.rbegin(); it != rev.rend(); ++it)
        if (out.empty() || out.back().x != it->x || out.back().z != it->z)
            out.push_back(*it);
    if (out.size() > kRawRouteCap) out.resize(kRawRouteCap);
}

// Has `c` landed back on the straight line from `org` to the goal? Retail
// normalises the signs so the goal delta is positive, then accepts a cell that
// is either along the first leg or at the far x with z in range
// (icd 0x414dc4..0x414df4). This is the M-line re-crossing test that tells a
// bug algorithm it has finished going around.
bool PathSearch::onGoalLine(PathCell org, PathCell c) const {
    int gx = goal.x - org.x, gz = goal.z - org.z;
    int cx = c.x - org.x, cz = c.z - org.z;
    if (gx < 0) { gx = -gx; cx = -cx; }
    if (gz < 0) { gz = -gz; cz = -cz; }
    if (cz == 0 && cx > 0 && cx <= gx) return true;
    if (cx == gx && cz > 0 && cz <= gz) return true;
    return false;
}

// Is a step from `c` in direction `d` one the MOVER will actually make?
//
// For a cardinal step, the destination cell is the whole question. For a DIAGONAL, the
// body passes between the two orthogonal neighbours, and the movers refuse to cut that
// corner -- as does segmentFits, which validates the shortcut afterwards. The search used to
// ask only about the destination, so it could plan a diagonal squeeze between two blocked
// cells: the mover then stalls at the corner it will not cut, and the shortcut validation
// rejects the very connection the search had just committed to. Planning a move that two
// later stages both refuse is worse than planning a longer one.
static bool stepLegal(const std::function<int(int, int)>& score, PathCell c, int d) {
    if (kDirX[d] == 0 || kDirZ[d] == 0) return true;   // cardinal: nothing to squeeze past
    return score(c.x + kDirX[d], c.z) >= kCellThreshold &&
           score(c.x, c.z + kDirZ[d]) >= kCellThreshold;
}

// One step of a boundary trace. `rot` is +1 for the cursor sweeping one way
// round the obstacle and -1 for its twin: retail runs BOTH at once (icd
// 0x414c52 rotates by -2/-3, 0x414e23 by +2/+3) and takes whichever regains
// the goal line first, which is what lets it round a wall from either end.
bool PathSearch::traceStep(const std::function<int(int, int)>& score,
                           PathCell& c, int& d, int rot) {
    const int limit = (d + 3 * rot) & 7;
    int probe = (d + 2 * rot) & 7;
    for (;;) {
        const PathCell n{c.x + kDirX[probe], c.z + kDirZ[probe]};
        const int s = score(n.x, n.z);
        ++visited;
        if (s >= kCellThreshold && stepLegal(score, c, probe)) {
            c = n;
            d = probe;
            mark(c, probe, s);
            return true;
        }
        probe = (probe - rot) & 7;
        if (probe == limit) return false;   // swept the circle: boxed in
    }
}



PathSearch::Result PathSearch::step(const std::function<int(int, int)>& score,
                                    int quantum) {
    if (w_ <= 0) return Result::Failed;     // reset() was never called
    for (;;) {
        if (work >= quantum) return Result::Suspended;   // icd 0x415028, -2
        if (visited > visitLimit) {
            if (g_pathDbg) std::fprintf(stderr,
                "  search gave up: visitLimit, phase=%d visited=%d work=%d best=%d cur=(%d,%d)\n",
                int(phase), visited, work, best, cur.x, cur.z);
            phase = Phase::Failed;
            buildRouteTo(bestCell);
            return Result::Failed;
        }

        switch (phase) {
case Phase::Init: {
            if (!inside(start) || !inside(goal)) {
                phase = Phase::Failed;
                return Result::Failed;
            }
            best = pathDist(start, goal);
            bestCell = start;
            cur = start;
            org = start;
            // The goal carries the terminal flag both marches test for
            // (icd 0x414b9f / 0x414d8f test bit 0x4).
            {
                const size_t gi = size_t(goal.z) * size_t(w_) + size_t(goal.x);
                touch(gi);
                flag_[gi] |= kGoal;
            }
if (best == 0) { phase = Phase::Done;
                if (!buildDijkstraRoute(score)) buildRoute();
                return Result::Arrived; }
            if (score(start.x, start.z) < kCellThreshold) {
                phase = Phase::Failed;
                return Result::Failed;
            }
            phase = Phase::March;
            break;
        }

        // State 1 (icd 0x414833): the OCTANT march, run once from the true
        // start. It counts what it crosses and gives up on the third occupied
        // cell; ordinary ground is counted but only capped once the road run
        // has also reached 3 (0x4148d7..0x4148fa).
        case Phase::March: {
            work += kWorkMarch;
            const int d = pathDirFromDelta(goal.x - cur.x, goal.z - cur.z);
            const PathCell n{cur.x + kDirX[d], cur.z + kDirZ[d]};
            ++visited;
            const int s = score(n.x, n.z);
            bool giveUp = false;
            if (s < kCellThreshold || !stepLegal(score, cur, d)) {
                giveUp = true;
            } else if (s == kCellSlope) {
                if (nOccupied >= 3) giveUp = true; else ++nOccupied;
            } else if (s == kCellRoad) {
                if (nRoad >= 3) { if (nGround >= 3) giveUp = true; }
                else ++nRoad;
            } else {
                ++nGround;
            }
            if (giveUp) { org = cur; phase = Phase::CardMarch; break; }
            cur = n;
            mark(cur, d, s);
            if (atGoal(cur)) { phase = Phase::Done;
                if (!buildDijkstraRoute(score)) buildRoute();
                return Result::Arrived; }
            const int dist = pathDist(cur, goal);
            if (dist < best) { best = dist; bestCell = cur; }
            break;
        }

        // State 2 (icd 0x414a6a): the CARDINAL march from the current origin,
        // laying breadcrumbs, until something blocks it.
        case Phase::CardMarch: {
            work += kWorkTraceInit;
            const int d = cardinalToward(goal.x - org.x, goal.z - org.z);
            const PathCell n{org.x + kDirX[d], org.z + kDirZ[d]};
            const int s = score(n.x, n.z);
            ++visited;
            if (s < kCellThreshold) {          // blocked -> start both traces
                curA = org; curB = org;
                // Seed so each cursor's FIRST probe lands on the direction that
                // was just refused, then its own rule takes over. Emulating the
                // original over this exact case shows both cursors probing the
                // blocked heading first (6, then 7,0 one way and 5 the other);
                // seeding both with the blocked direction instead makes them
                // open at blocked+-2 and skip straight past it, which is how a
                // two-step detour turned into a long wander.
                dirA = (d - 2) & 7;            // probes from (dirA+2) == d
                dirB = (d + 2) & 7;            // probes from (dirB-2) == d
                started = false;
                phase = Phase::Trace;
                break;
            }
            org = n;
            mark(org, d, s);
            if (atGoal(org)) { phase = Phase::Done;
                if (!buildDijkstraRoute(score)) buildRoute();
                return Result::Arrived; }
            const int dist = pathDist(org, goal);
            if (dist < best) { best = dist; bestCell = org; }
            break;
        }

        // State 3 (icd 0x414c25): the twin traces. Whichever cursor regains the
        // origin-to-goal line first becomes the new march origin
        // (0x414fde / 0x414ff8 both drop back into state 2).
        case Phase::Trace: {
            work += kWorkTraceStep;
            const bool okA = traceStep(score, curA, dirA, +1);
            if (okA) {
                if (atGoal(curA)) { phase = Phase::Done;
                    if (!buildDijkstraRoute(score)) buildRoute();
                    return Result::Arrived; }
                const int dist = pathDist(curA, goal);
                if (dist < best) { best = dist; bestCell = curA; }
                if (onGoalLine(org, curA)) { org = curA; phase = Phase::CardMarch; break; }
            }
            // The traces give up when they MEET (icd 0x414ec7 compares cursor B's
            // cell and direction against cursor A's): two cursors going opposite
            // ways round the same outline can only coincide if the outline is
            // closed, so there is no way through. Comparing each cursor with its
            // own start instead -- which is what I had -- lets a trace wander most
            // of the map before it notices, which is what burned the visit limit.
            if (started && curA.x == curB.x && curA.z == curB.z && dirA == dirB) {
                phase = Phase::Failed;
                buildRouteTo(bestCell);
                return Result::Failed;
            }
            const bool okB = traceStep(score, curB, dirB, -1);
            if (okB) {
                if (atGoal(curB)) { phase = Phase::Done;
                    if (!buildDijkstraRoute(score)) buildRoute();
                    return Result::Arrived; }
                const int dist = pathDist(curB, goal);
                if (dist < best) { best = dist; bestCell = curB; }
                if (onGoalLine(org, curB)) { org = curB; phase = Phase::CardMarch; break; }
            }
            started = true;
            if (!okA && !okB) {
                if (g_pathDbg) std::fprintf(stderr,
                    "  search gave up: both traces boxed in at A(%d,%d) B(%d,%d) visited=%d\n",
                    curA.x, curA.z, curB.x, curB.z, visited);
                phase = Phase::Failed;
                buildRouteTo(bestCell);
                return Result::Failed;
            }
            break;
        }

        case Phase::Done: return Result::Arrived;
        case Phase::Failed: return Result::Failed;
        }
    }
}

void PathService::setEntityPool(int player,int first,int count) {
    if (player<0 || player>=10 || first<0 || count<1 || int64_t(first)+count>INT32_MAX)
        throw std::invalid_argument("invalid allocated entity range");
    if (scheduler_.active.player==player &&
        (poolFirst_[size_t(player)]!=first || poolSize_[size_t(player)]!=count))
        throw std::logic_error("cannot relocate an active entity pool");
    poolFirst_[size_t(player)]=first; poolSize_[size_t(player)]=count;
    fixedPool_[size_t(player)]=true;
}

int PathService::slotOf(int unitId,int player) const {
    const size_t owner=size_t(player);
    if (fixedPool_[owner] || !slots_) {
        const int slot=unitId-poolFirst_[owner];
        return slot>=0 && slot<poolSize_[owner] ? slot : -1;
    }
    const int slot=slots_->slot(unitId);
    return slot>=0 && size_t(slot)<slots_->unitAt[owner].size() &&
        slots_->unitAt[owner][size_t(slot)]==unitId ? slot : -1;
}

int PathService::poolCapacity(int player) const {
    const size_t owner=size_t(player);
    return fixedPool_[owner] || !slots_ ? poolSize_[owner] : int(slots_->unitAt[owner].size());
}

int PathService::unitAtSlot(int player,int slot) const {
    const size_t owner=size_t(player);
    if (fixedPool_[owner] || !slots_) return poolFirst_[owner]+slot;
    const auto& pool=slots_->unitAt[owner];
    return slot>=0 && size_t(slot)<pool.size() ? pool[size_t(slot)] : -1;
}

void PathService::retireActive() {
    if (activeId_>=0 && gradeHost_.finish) gradeHost_.finish(activeId_);
    activeId_=-1;
    scheduler_.cancel(scheduler_.active);
}

void PathService::request(int unitId, PathCell start, PathCell goal, int mapW,
                          int mapH, Fixed goalX, Fixed goalZ, int player, bool priority,
                          int tolCells, int heading, RetailCostSearch::Costs costs, PathCell cellOffset, int goalTolerance,
                          uint64_t controller, int goalRadiusSquared,
                          std::optional<RetailRectGoal> rectangle,std::optional<RetailRingGoal> ring) {
    if (player<0 || player>=10 || unitId<0 || unitId==INT32_MAX)
        throw std::invalid_argument("invalid search owner");
    const size_t owner=size_t(player);
    if (!fixedPool_[owner] && !slots_) poolSize_[owner]=std::max(poolSize_[owner],unitId+1);
    if (slotOf(unitId,player)<0)
        throw std::invalid_argument("search owner outside allocated entity range");
    // Coordinates alone do not identify an order. A replacement controller
    // cannot inherit the old controller's search or undelivered events.
    if (auto it = q_.find(unitId); it != q_.end() &&
        (it->second.controller != controller || it->second.player != player))
        cancel(unitId,NavigationTelemetry::Cancel::Replaced);
    // A RE-REQUEST FOR THE SAME SEARCH LETS IT RUN. Everything below restarts the
    // search from scratch (cap = 0), which is right when the question changed and
    // ruinous when it did not: the sim re-asks every kPathRetryTicks (120 ticks, 4s)
    // for any unit not making headway, while a saturated queue takes far longer than
    // that to reach it. Measured at 3300 units: 1445 pending against 12 slots, average
    // wait 490 ticks (~16s), so a starved unit reset its own search about four times
    // before it could ever have finished -- 19,985 requests yielded 996 completions.
    // It never got a route, and with no local obstacle avoidance in the steering,
    // waiting for a route means walking straight at the goal into whatever is between.
    //
    // "The same search" is the same controller and DESTINATION; where the unit
    // now stands does not enter into it. Retail's retry (4e5268) calls
    // 4e4f50(1), which returns at 4e4f68 once the navigator's pending bit
    // (+0x114 & 2) is set: queued or running, the request is left alone
    // however far the unit has moved, and 416430 reads the start only when it
    // admits the request (refresh). Only setDestination (4e54e0) cancels
    // (415f30) and resubmits, which here is a new controller or destination.
    // This used to restart past a one-cell drift, retiring a running worker
    // that retail keeps. A rectangle or ring controller's destination is its
    // shape; the anchor cell is projected from the unit and moves with it.
    if (auto it = q_.find(unitId); it != q_.end()) {
        Entry& ex = it->second;
        const bool shaped = rectangle || ring;
        if ((shaped || (ex.goal.x == goal.x && ex.goal.z == goal.z)) &&
            ex.cellOffset.x == cellOffset.x && ex.cellOffset.z == cellOffset.z &&
            ex.goalTolerance == goalTolerance && ex.goalRadiusSquared == goalRadiusSquared &&
            ex.rectangle == rectangle && ex.ring == ring) {
            // Keep the search's PROGRESS (cap, slot) but take the new request's exact
            // destination. goalX/goalZ never enter the search -- it works in cells --
            // they are carried through to done(), which installs them as the order's
            // point. Holding the old pair meant a fresh move order to a different spot
            // inside the SAME 16px cell silently walked to where the previous order
            // had pointed.
            ex.goalX = goalX;
            ex.goalZ = goalZ;
            // A request not yet admitted has not read its start. Hosts that
            // pass no refresh get the current one here instead; a running
            // attempt keeps the start it was admitted with.
            if (activeId_ != unitId && !(scheduler_.active==
                    RetailSearchScheduler::Request{ex.player,slotOf(unitId,ex.player)})) {
                ex.start = start; ex.heading = heading; ex.costs = costs;
            }
            if (priority && !ex.priority) {
                ex.priority=true;++priorityByPlayer_[owner];
            }
            return;
        }
    }
    if (auto it=q_.find(unitId); it!=q_.end() && scheduler_.active==
        RetailSearchScheduler::Request{it->second.player,slotOf(unitId,it->second.player)})
        retireActive();
    auto [entry,inserted]=q_.try_emplace(unitId);
    Entry& e=entry->second;
    if (inserted && unitId>=0) {
        if (size_t(unitId)>=pendingIds_.size()) pendingIds_.resize(size_t(unitId)+1,0);
        pendingIds_[size_t(unitId)]=1;
    }
    if (!inserted) {
        if(telemetry_)telemetry_->cancelled(e.telemetryToken,NavigationTelemetry::Cancel::Replaced);
        --pendingByPlayer_[size_t(e.player)];
        priorityByPlayer_[size_t(e.player)]-=e.priority;
    }
    ++pendingByPlayer_[owner];priorityByPlayer_[owner]+=priority;
    e.start = start;
    e.goal = goal;
    e.mapW = mapW;
    e.mapH = mapH;
    e.goalX = goalX;
    e.goalZ = goalZ;
    e.player = player;
    e.tolCells = tolCells;
    e.priority = priority;
    e.heading = heading;
    e.cellOffset = cellOffset;
    e.goalTolerance = goalTolerance;
    e.costs = costs;
    e.notification = 0;
    e.controller = controller;
    e.goalRadiusSquared = goalRadiusSquared;
    e.rectangle = rectangle;
    e.ring = ring;
    e.telemetryToken=telemetry_?telemetry_->requested(unitId):0;
}

void PathService::eraseRequest(std::unordered_map<int,Entry>::iterator it) {
    --pendingByPlayer_[size_t(it->second.player)];
    priorityByPlayer_[size_t(it->second.player)]-=it->second.priority;
    if (it->first>=0 && size_t(it->first)<pendingIds_.size()) pendingIds_[size_t(it->first)]=0;
    q_.erase(it);
}

void PathService::cancel(int unitId,NavigationTelemetry::Cancel reason) {
    std::erase_if(notifications_, [=](const Notification& n) { return n.unitId == unitId; });
    auto it = q_.find(unitId);
    if (it == q_.end()) return;
    if(telemetry_)telemetry_->cancelled(it->second.telemetryToken,reason);
    if (scheduler_.active==RetailSearchScheduler::Request{
        it->second.player,slotOf(unitId,it->second.player)}) retireActive();
    // A finish callback may queue other requests and rehash the lookup table.
    eraseRequest(q_.find(unitId));
}

void PathService::tick(const std::function<int(int,int,int)>& score,
        const std::function<void(int,const std::vector<PathCell>&,Fixed,Fixed,bool,bool,bool,bool)>& done,
        const std::function<void(int,PathCell&,int&,RetailCostSearch::Costs&)>& refresh,
        uint32_t simulationTick, const std::function<bool(int,uint32_t)>& admit) {
    ++tickNo_;
    if (q_.empty()) return;
    // Completed entries leave q_ before their delivery callbacks run. Keep the
    // observer fixed across the whole dispatch, including those outstanding
    // tokens, and restore the guard if a callback throws or ticks recursively.
    struct TelemetryTickScope {
        bool& active;
        bool previous;
        explicit TelemetryTickScope(bool& value):active(value),previous(value){active=true;}
        ~TelemetryTickScope(){active=previous;}
    } telemetryScope(telemetryTickActive_);
    const uint32_t now=simulationTick ? simulationTick : uint32_t(tickNo_);
    std::array<RetailSearchScheduler::Player,10> players{};
    for (size_t i=0;i<players.size();++i) {
        auto& p=players[i];
        p.enabled=pendingByPlayer_[i]!=0;p.priority=priorityByPlayer_[i]!=0;
        p.pending=pendingByPlayer_[i];p.slots=p.enabled ? poolCapacity(int(i)) : 0;
    }
    auto lookup=[&](RetailSearchScheduler::Request r) {
        const int id=unitAtSlot(r.player,r.slot);
        if (id<0) return q_.end();
        auto it=q_.find(id);
        return it!=q_.end() && it->second.player==r.player ? it : q_.end();
    };
    struct Finished { int id; std::vector<PathCell> route; Fixed x,z; bool failed,crowded,traffic,detour; uint64_t telemetryToken;bool telemetryFailed; };
    std::vector<Finished> finished;
    scheduler_.tick(players,budget_,1,int(now),
        [&](auto r) { return lookup(r)!=q_.end(); },
        [&](auto r) {
            workSpent_+=7;
            const auto it=lookup(r);
            return it!=q_.end() && (!admit || admit(it->first,now));
        },
        [&](auto r,bool admitted,int remaining) {
            auto it=lookup(r);
            const int id=it->first;
            auto& e=it->second;
            if (admitted) { activeId_=id; e.notification=0; ++requests_; }
            const RetailCircleGoal circle{e.goal.x-e.cellOffset.x,e.goal.z-e.cellOffset.z,
                                           e.goalTolerance,e.goalRadiusSquared};
            const bool initializing=admitted || worker_.initializationPending;
            std::chrono::steady_clock::time_point profileStart;
            uint64_t preparationNanoseconds=0;
            if (profiling_) profileStart=std::chrono::steady_clock::now();
            auto result=worker_.dispatch(admitted,remaining,[&](int retry) {
                if (refresh) refresh(id,e.start,e.heading,e.costs);
                RetailSearchAttempt::Parameters p;
                p.width=e.mapW; p.height=e.mapH;
                p.start={e.start.x-e.cellOffset.x,e.start.z-e.cellOffset.z};
                p.heading=e.heading; p.costs=e.costs; p.trafficRadius=e.tolCells;
                p.retry=retry;
                auto& last=scheduler_.lastWrapTick[size_t(scheduler_.playerCursor)];
                uint32_t wrap=uint32_t(last);
                p.weight=retailInitialPathWeight(now,wrap,players[size_t(e.player)].pending,retry,e.costs.heavyFloater);
                last=int(wrap);
                return p;
            },[&](bool last) {
                if (gradeHost_.prepare) {
                    if (profiling_) {
                        const auto start=std::chrono::steady_clock::now();
                        gradeHost_.prepare(id,last);
                        preparationNanoseconds+=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now()-start).count());
                    } else gradeHost_.prepare(id,last);
                }
            },[&] {
                std::vector<RetailReachability::Point> cells;
                auto emit=[&](int x,int z) { cells.push_back({x,z}); };
                if (e.rectangle) e.rectangle->enumerate(emit);
                else if (e.ring) e.ring->enumerate(emit); else circle.enumerate(emit);
                return cells;
            },[&](int x,int z) {
                return e.rectangle ? e.rectangle->accepts(x,z) : e.ring ? e.ring->accepts(x,z) : circle.accepts(x,z);
            },[&](int x,int z) {
                return e.rectangle ? e.rectangle->distance(x,z) : e.ring ? e.ring->distance(x,z) : circle.distance(x,z);
            },[&](int x,int z,int) {
                const int cx=x+e.cellOffset.x,cz=z+e.cellOffset.z;
                return gradeHost_.score ? gradeHost_.score(id,cx,cz,worker_.retry,e.start) : score(id,cx,cz);
            },[&] {
                // The start and cost profile remain those of this attempt.
                // Only orientation is sampled again at the phase transition.
                auto start=e.start;auto costs=e.costs;int heading=e.heading;
                if (refresh) refresh(id,start,heading,costs);
                return heading;
            });
            if (profiling_) {
                const uint64_t elapsed=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now()-profileStart).count());
                if (initializing) {
                    ++diagnostics_.initializations;
                    diagnostics_.resetCells+=worker_.attempt.trace.scratch.resetCells;
                    diagnostics_.initializationNanoseconds+=elapsed-preparationNanoseconds;
                    diagnostics_.preparationNanoseconds+=preparationNanoseconds;
                } else {
                    ++diagnostics_.executionSlices;
                    diagnostics_.executionNanoseconds+=elapsed;
                }
            }
            workSpent_+=uint64_t(result.work);
            if (result.notification) {
                e.notification=result.notification;
                if (e.controller) notifications_.push_back({id,e.controller,result.notification});
            }
            const bool complete=result.result==RetailSearchAttempt::Result::Complete;
            if (complete) {
                const auto& route=worker_.attempt.route;
                const bool partial=(route.flags&4)!=0;
                if (e.notification==0x2000 || partial) ++failures_; else ++completions_;
                std::vector<PathCell> points;
                for (auto p:route.points) points.push_back({p.x+e.cellOffset.x,p.z+e.cellOffset.z});
                finished.push_back({id,std::move(points),e.goalX,e.goalZ,partial,
                                    (route.flags&1)!=0,(route.flags&2)!=0,(route.flags&8)!=0,
                                    e.telemetryToken,e.notification==0x2000||partial});
                if (gradeHost_.finish) gradeHost_.finish(id);
                activeId_=-1;
                --players[size_t(e.player)].pending;
                // Entry references survive callback insertions; iterators do not.
                eraseRequest(q_.find(id));
            }
            return RetailSearchScheduler::Slice{result.work,complete};
        });
    // Delivery can cancel or replace other orders. Release the active queue
    // traversal first so callbacks never invalidate its current entry.
    for (const auto& f:finished) {
        if(telemetry_)telemetry_->delivered(f.telemetryToken,f.telemetryFailed,f.route.size());
        done(f.id,f.route,f.x,f.z,f.failed,f.crowded,f.traffic,f.detour);
    }
}

}   // namespace tak::sim
