#include "sim/pathsearch.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace tak::sim {

static const bool g_pathDbg = getenv("TAK_PATHDBG") != nullptr;

int pathDist(PathCell a, PathCell b) {
    return std::max(std::abs(a.x - b.x), std::abs(a.z - b.z));
}

int pathDirFromDelta(int dx, int dz) {
    // Octant, using the rotational order of the direction tables. Verified
    // against the original by emulating 0x415040 over 81 deltas: all agree.
    // The zero delta is the one case worth spelling out -- the icd answers 5,
    // not 0, and while the search should never ask for a direction to where it
    // already is, matching costs nothing.
    if (dx == 0 && dz == 0) return 5;
    if (std::abs(dx) > 2 * std::abs(dz)) return dx < 0 ? 2 : 6;
    if (std::abs(dz) > 2 * std::abs(dx)) return dz <= 0 ? 0 : 4;
    if (dx < 0) return dz <= 0 ? 1 : 3;
    return dz <= 0 ? 7 : 5;
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
        gen_ = walkGen_ = 0;
    }
    ++gen_;   // every cell is now stale, i.e. empty -- no clearing needed
    phase = Phase::Init;
    best = 0;
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
    // 4-cell cycle repeated to the 64-waypoint clamp. Retail's separate bitmap
    // at +0x2c is exactly this guard.
    if (flag_[i] & kSeen) return;
    from_[i] = uint8_t(d);
    flag_[i] = uint8_t(flag_[i] | kSeen | (score == 5 ? kScore5 : 0));
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
void PathSearch::buildRoute() {
    out.clear();
    if (!inside(goal)) return;
    std::vector<PathCell> rev;
    ++walkGen_;
    PathCell c = goal;
    int lastDir = -1;
    for (int guard = 0; guard < 8192; ++guard) {
        if (c.x == start.x && c.z == start.z) break;
        const size_t i = size_t(c.z) * size_t(w_) + size_t(c.x);
        if (!seen(i) || !(flag_[i] & kSeen)) break;
        if (walkStamp_[i] == walkGen_) break;   // belt and braces against a cycle
        walkStamp_[i] = walkGen_;
        const int d = from_[i] & 7;
        if (d != lastDir) { rev.push_back(c); lastDir = d; }
        PathCell p{c.x - kDirX[d], c.z - kDirZ[d]};
        if (!inside(p) || (p.x == c.x && p.z == c.z)) break;
        c = p;
    }
    if (rev.empty() || rev.front().x != goal.x || rev.front().z != goal.z)
        rev.insert(rev.begin(), goal);
    for (auto it = rev.rbegin(); it != rev.rend(); ++it)
        if (out.empty() || out.back().x != it->x || out.back().z != it->z)
            out.push_back(*it);
    if (out.size() > 64) out.resize(64);
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
        if (s >= kCellThreshold) {
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
            phase = Phase::Failed; return Result::Failed;
        }

        switch (phase) {
        case Phase::Init: {
            if (!inside(start) || !inside(goal)) {
                phase = Phase::Failed;
                return Result::Failed;
            }
            best = pathDist(start, goal);
            cur = start;
            org = start;
            // The goal carries the terminal flag both marches test for
            // (icd 0x414b9f / 0x414d8f test bit 0x4).
            {
                const size_t gi = size_t(goal.z) * size_t(w_) + size_t(goal.x);
                touch(gi);
                flag_[gi] |= kGoal;
            }
            if (best == 0) { phase = Phase::Done; buildRoute(); return Result::Arrived; }
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
            if (s < kCellThreshold) {
                giveUp = true;
            } else if (s == kCellOccupied) {
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
            if (atGoal(cur)) { phase = Phase::Done; buildRoute(); return Result::Arrived; }
            const int dist = pathDist(cur, goal);
            if (dist < best) best = dist;
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
            if (atGoal(org)) { phase = Phase::Done; buildRoute(); return Result::Arrived; }
            const int dist = pathDist(org, goal);
            if (dist < best) best = dist;
            break;
        }

        // State 3 (icd 0x414c25): the twin traces. Whichever cursor regains the
        // origin-to-goal line first becomes the new march origin
        // (0x414fde / 0x414ff8 both drop back into state 2).
        case Phase::Trace: {
            work += kWorkTraceStep;
            const bool okA = traceStep(score, curA, dirA, +1);
            if (okA) {
                if (atGoal(curA)) { phase = Phase::Done; buildRoute(); return Result::Arrived; }
                const int dist = pathDist(curA, goal);
                if (dist < best) best = dist;
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
                return Result::Failed;
            }
            const bool okB = traceStep(score, curB, dirB, -1);
            if (okB) {
                if (atGoal(curB)) { phase = Phase::Done; buildRoute(); return Result::Arrived; }
                const int dist = pathDist(curB, goal);
                if (dist < best) best = dist;
                if (onGoalLine(org, curB)) { org = curB; phase = Phase::CardMarch; break; }
            }
            started = true;
            if (!okA && !okB) {
                if (g_pathDbg) std::fprintf(stderr,
                    "  search gave up: both traces boxed in at A(%d,%d) B(%d,%d) visited=%d\n",
                    curA.x, curA.z, curB.x, curB.z, visited);
                phase = Phase::Failed; return Result::Failed;
            }
            break;
        }

        case Phase::Done: return Result::Arrived;
        case Phase::Failed: return Result::Failed;
        }
    }
}

// Point a pool slot at this request and start its search there.
void PathService::admit(int unitId, Entry& e, int slot) {
    e.slot = slot;
    e.cap = 0;
    slotOwner_[size_t(slot)] = unitId;
    PathSearch& ps = pool_[size_t(slot)];
    // reset() only reallocates when the map size changed, so a slot that has
    // already run a search on this map recycles its arrays for free.
    ps.reset(e.mapW, e.mapH);
    ps.unitId = unitId;
    ps.start = e.start;
    ps.goal = e.goal;
    ps.cur = e.start;
    ps.priority = e.priority;
}

void PathService::release(Entry& e) {
    if (e.slot >= 0) slotOwner_[size_t(e.slot)] = -1;
    e.slot = -1;
}

void PathService::request(int unitId, PathCell start, PathCell goal, int mapW,
                          int mapH, float goalX, float goalZ, bool priority) {
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
    // "The same search" is the same GOAL from ~the same PLACE. The start is allowed to
    // drift a cell because a unit shuffling on the spot has not invalidated anything;
    // move further than that and the old answer really is for a different question, so
    // it restarts as before. A unit that is actually travelling is making headway and
    // never reaches the retry path at all.
    if (auto it = q_.find(unitId); it != q_.end()) {
        Entry& ex = it->second;
        const int dx = ex.start.x - start.x, dz = ex.start.z - start.z;
        if (ex.goal.x == goal.x && ex.goal.z == goal.z &&
            dx >= -1 && dx <= 1 && dz >= -1 && dz <= 1) {
            if (priority) ex.priority = true;   // may still be promoted
            return;
        }
    }
    Entry& e = q_[unitId];
    const int slot = e.slot;      // keep the slot if this unit already holds one
    e.start = start;
    e.goal = goal;
    e.mapW = mapW;
    e.mapH = mapH;
    e.goalX = goalX;
    e.goalZ = goalZ;
    e.priority = priority;
    e.cap = 0;
    e.slot = -1;
    // A re-request for a unit that is already searching restarts it in place,
    // exactly as it did when every request owned its own search.
    if (slot >= 0) admit(unitId, e, slot);
}

void PathService::cancel(int unitId) {
    auto it = q_.find(unitId);
    if (it == q_.end()) return;
    release(it->second);
    q_.erase(it);
}

void PathService::tick(const std::function<int(int, int, int)>& score,
                       const std::function<void(int, const std::vector<PathCell>&,
                                                float, float)>& done) {
    if (q_.empty()) return;
    if (pool_.empty()) {
        pool_.resize(kMaxActiveSearches);
        slotOwner_.assign(kMaxActiveSearches, -1);
    }

    // ADMIT. Fill free slots from the queue, starting after the last unit id
    // admitted and wrapping, so a low unit id that re-requests every tick cannot
    // hold the pool against a higher one. Pure integer state in map order: every
    // peer admits the same requests in the same order on the same tick.
    int free = 0;
    for (int owner : slotOwner_) if (owner < 0) ++free;
    if (free > 0) {
        int admitted = 0;
        for (int pass = 0; pass < 2 && free > 0; ++pass) {
            // pass 0: ids after the cursor. pass 1: wrap to the front.
            auto it = pass == 0 ? q_.upper_bound(admitCursor_) : q_.begin();
            const auto stop = pass == 0 ? q_.end() : q_.upper_bound(admitCursor_);
            for (; it != stop && free > 0; ++it) {
                Entry& e = it->second;
                if (e.slot >= 0) continue;
                int slot = -1;
                for (size_t i = 0; i < slotOwner_.size(); ++i)
                    if (slotOwner_[i] < 0) { slot = int(i); break; }
                if (slot < 0) break;
                admit(it->first, e, slot);
                admitCursor_ = it->first;
                ++admitted;
                --free;
            }
        }
        (void)admitted;
    }

    // Count the two classes exactly as the scheduler does, then split the
    // budget: a flagged request is worth five ordinary ones (icd 0x4164fa).
    // Only ACTIVE requests count -- a queued one is doing no work this tick, and
    // counting it would hand the running searches a thinner slice for nothing.
    int a = 0, b = 0;
    for (const auto& [id, e] : q_) {
        if (e.slot < 0) continue;
        (e.priority ? b : a) += 1;
    }
    const int share = a + 5 * b;
    if (share <= 0) return;
    // share is bounded by the pool (at most 5*kMaxActiveSearches), so against a
    // budget of 12000 the quantum never bottoms out at 1 and the budget holds as
    // a real per-tick cap rather than a per-request floor.
    const int quantum = std::max(1, budget_ / share);

    std::vector<int> finished;
    for (auto& [id, e] : q_) {
        if (e.slot < 0) continue;   // queued: no scratch, no work, waits its turn
        e.cap += quantum * (e.priority ? 5 : 1);
        auto sc = [&](int cx, int cz) { return score(id, cx, cz); };
        PathSearch& ps = pool_[size_t(e.slot)];
        const PathSearch::Result r = ps.step(sc, e.cap);
        if (r == PathSearch::Result::Arrived) {
            done(id, ps.out, e.goalX, e.goalZ);
            finished.push_back(id);
        } else if (r == PathSearch::Result::Failed) {
            done(id, {}, e.goalX, e.goalZ);
            finished.push_back(id);
        }
        // Suspended: keep the entry, resume next tick with its state intact.
    }
    for (int id : finished) {
        auto it = q_.find(id);
        if (it == q_.end()) continue;
        release(it->second);
        q_.erase(it);
    }
}

}   // namespace tak::sim
