#pragma once

// Reachability probe 0x4146e0. Shares its flags/directions with phase 2;
// returns -2 (suspended), -1 (direct route), 0 (cost search to goal), or a
// positive closest-distance threshold for a partial cost search.
#include "retailcost.h"

namespace tak::sim {
// 4161b0 and the range check in 415170. The caller supplies the current
// scheduler player's wrap timestamp, which need not be the entity's player.
inline int retailInitialPathWeight(uint32_t tick, uint32_t& lastWrapTick,
                                   int pending, int retry, bool heavyFloater,
                                   int pendingThreshold = 20) {
    if (!lastWrapTick) lastWrapTick = tick;
    const int age = int(std::min(uint32_t(300), tick - lastWrapTick)) + 30 * std::max(0, retry);
    const int load = std::max(0, pending - pendingThreshold) + (age >= 7 ? age * 5 - 35 : 0);
    const int64_t value = (heavyFloater ? 196608 : 98304) + int64_t(load) * 65536 / 10;
    return value < 65536 || value > 20 * 65536 ? 20 * 65536 : int(value);
}

// 0x415040 with its default (-1) direction hint uses a 24:10 threshold.
inline int retailSearchDirection(int x, int z) {
    if (!x && !z) return 5;
    const int64_t ax = std::abs(int64_t(x)), az = std::abs(int64_t(z));
    if (10 * ax > 24 * az) return x < 0 ? 2 : 6;
    if (10 * az > 24 * ax) return z <= 0 ? 0 : 4;
    return x < 0 ? (z <= 0 ? 1 : 3) : (z <= 0 ? 7 : 5);
}

class RetailReachability {
public:
    struct Point {
        int x = 0, z = 0;
        bool operator==(const Point&) const = default;
    };
    int width = 0, height = 0, phase = 0, best = 0, visited = 0, visitLimit = 0;
    int slopes = 0, ground = 0, roads = 0, dirA = 0, dirB = 0;
    int nearTraffic = 0, otherTraffic = 0, started = 0, work = 0;
    Point start, goal, directEnd, march, origin, a, b;
    int trafficRadius = 0;
    std::vector<RetailCostSearch::Cell> cells;
    RetailSearchCellScratch scratch;

    template<class Grade, class Distance>
    int step(Grade grade, Distance distance, int budget) {
        work = 0;
        auto mark = [&](Point p, int flags, int direction = -1) -> bool {
            const size_t index=size_t(p.z * width + p.x);
            auto& cell = cells.at(index);
            scratch.touch(index,cell.flags);
            cell.flags |= uint8_t(flags);
            if (direction >= 0) cell.direction = uint8_t(direction);
            return (cell.flags & 4) != 0;
        };
        auto closer = [&](Point p) { best = std::min(best, distance(p.x, p.z)); };
        if (phase == 0) {
            best = distance(start.x, start.z);
            if (!best) { directEnd = start; return -1; }
            if (grade(start.x, start.z, 0) < 4) return best;
            march = start;
            slopes = ground = roads = visited = 0;
            visitLimit = (width + height) * 20;
            phase = 1;
            mark(start, 0x20);
            for (int x = -3; x <= 3 && phase == 1; ++x)
                for (int z = -3; z <= 3; ++z)
                    if (grade(start.x + x, start.z + z, 0) < 4) {
                        origin = march; phase = 2; break;
                    }
        }
        while (phase == 1 && work < budget) {
            work += 8;
            if (!best) { directEnd = march; return -1; }
            const int direction = retailSearchDirection(goal.x - march.x, goal.z - march.z);
            const Point next{march.x + dx[direction], march.z + dz[direction]};
            const int score = grade(next.x, next.z, direction);
            ++visited;
            bool stop = score < 4;
            if (score == 4) { if (slopes >= 3) stop = true; else ++slopes; }
            else if (score == 7) { if (roads < 3) ++roads; else if (ground >= 3) stop = true; }
            else if (score >= 4) ++ground;
            if (stop) { origin = march; phase = 2; break; }
            march = next;
            if (score == 5) {
                if (!nearTraffic && std::abs(march.x - start.x) < trafficRadius &&
                    std::abs(march.z - start.z) < trafficRadius) nearTraffic = 1;
                else otherTraffic = 1;
            }
            if (mark(march, 0x10 | (score == 5 ? 0x40 : 0))) {
                directEnd = march; return -1;
            }
            closer(march);
        }
        while (work < budget) {
            if (visited > visitLimit) return best;
            while (phase == 2 && work < budget) {
                work += 7;
                if (!best) return 0;
                int direction = goal.x < origin.x ? 2 : goal.x > origin.x ? 6 : goal.z > origin.z ? 4 : 0;
                Point next{origin.x + dx[direction], origin.z + dz[direction]};
                int score = grade(next.x, next.z, direction);
                ++visited;
                if (score < 4) {
                    a = b = origin; dirA = dirB = (direction + 2) & 7;
                    started = 0; phase = 3; break;
                }
                origin = next;
                if (mark(origin, 8 | (score == 5 ? 0x40 : 0), direction)) return 0;
                closer(origin);
            }
            while (phase == 3 && work < budget) {
                work += 9;
                const int endA = (dirA - 3) & 7;
                dirA = (dirA - 2) & 7;
                Point next;
                int score;
                for (;;) {
                    next = {a.x + dx[dirA], a.z + dz[dirA]};
                    score = grade(next.x, next.z, dirA); ++visited;
                    if (score >= 4) break;
                    if (dirA == endA) return best;
                    dirA = (dirA + 1) & 7;
                }
                if (a == b && dirA == dirB && started) return best;
                a = next; started = 1;
                if (mark(a, 8 | (score == 5 ? 0x40 : 0), dirA)) return 0;
                if (onLine(a)) { origin = a; phase = 2; break; }
                closer(a);
                const int endB = (dirB + 3) & 7;
                dirB = (dirB + 2) & 7;
                for (;;) {
                    next = {b.x - dx[dirB], b.z - dz[dirB]};
                    score = grade(next.x, next.z, (dirB - 4) & 7); ++visited;
                    if (score >= 4) break;
                    if (dirB == endB) return best;
                    dirB = (dirB - 1) & 7;
                }
                if (a == b && dirA == dirB) return best;
                b = next;
                if (mark(b, 8 | (score == 5 ? 0x40 : 0), dirB)) return 0;
                if (onLine(b)) { origin = b; phase = 2; break; }
                closer(b);
            }
        }
        return -2;
    }

private:
    static constexpr int dx[8] = {0, -1, -1, -1, 0, 1, 1, 1};
    static constexpr int dz[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
    bool onLine(Point p) const {
        int gx = goal.x - origin.x, gz = goal.z - origin.z;
        int x = p.x - origin.x, z = p.z - origin.z;
        if (gx < 0) { gx = -gx; x = -x; }
        if (gz < 0) { gz = -gz; z = -z; }
        return (z == 0 && x > 0 && x <= gx) || (x == gx && z > 0 && z <= gz);
    }
};

struct RetailPathRoute {
    std::vector<RetailReachability::Point> points;
    int flags = 0;
};

inline RetailPathRoute retailDirectRoute(const RetailReachability& trace) {
    return {{trace.start, trace.directEnd}, trace.nearTraffic ? 1 : trace.otherTraffic ? 2 : 0};
}

// 0x414450: retain the last 64 backtracked corners in a ring, include the
// start, then reverse into travel order. Cells here are footprint origins;
// the navigator converts them to world coordinates using its footprint.
inline RetailPathRoute retailReconstructRoute(
        const std::vector<RetailCostSearch::Cell>& cells, int width,
        RetailReachability::Point start, RetailReachability::Point end,
        int partialDistance, int trafficRadius) {
    using Point = RetailReachability::Point;
    static constexpr int dx[8] = {0, -1, -1, -1, 0, 1, 1, 1};
    static constexpr int dz[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
    RetailPathRoute result;
    result.flags = partialDistance > 0 ? 4 : 0;
    auto cellAt = [&](Point p) -> const RetailCostSearch::Cell& {
        if (width <= 0 || p.x < 0 || p.x >= width || p.z < 0)
            throw std::invalid_argument("invalid route cell");
        return cells.at(size_t(p.z) * size_t(width) + size_t(p.x));
    };
    std::array<Point, 64> ring{};
    ring[0] = end;
    int count = 1, steps = 0, direction = cellAt(end).direction;
    Point cursor = end;
    while (cursor != start) {
        if (++steps > int(cells.size())) throw std::logic_error("cyclic route parent chain");
        const auto& cell = cellAt(cursor);
        if ((cell.flags & 0x40) && partialDistance == 0 && trafficRadius >= 0) {
            if (!(result.flags & 1) && std::abs(cursor.x - start.x) < trafficRadius &&
                std::abs(cursor.z - start.z) < trafficRadius) result.flags |= 1;
            else result.flags |= 2;
        }
        if (cell.direction != direction) {
            direction = cell.direction;
            ring[size_t(count++ & 63)] = cursor;
        }
        if (direction > 7) throw std::invalid_argument("invalid route direction");
        cursor.x -= dx[direction]; cursor.z -= dz[direction];
    }
    ring[size_t(count++ & 63)] = start;
    if (steps > std::abs(end.x - start.x) + std::abs(end.z - start.z))
        result.flags = (result.flags | 8) & ~3;
    for (int i = 0; i < std::min(count, 64); ++i)
        result.points.push_back(ring[size_t((count - i - 1) & 63)]);
    return result;
}

// One attempt: initialization consumes world/controller callbacks, then
// 415b10 hands the shared trace plane to phase 2. Exhaustion is handed to the
// worker below; controller notification/delivery remains the adapter's job.
class RetailSearchAttempt {
public:
    enum class Result { Searching, Complete, Retry };
    struct Slice { Result result; int work; int notification = 0; };
    RetailReachability trace;
    RetailCostSearch cost;
    RetailPathRoute route;
    int phase = 1, partialDistance = 0, nodeLimit = 0, spread = 4;
    int initialDistance = 0, heading = 0, retry = 0, weight = 98304;

    struct Parameters {
        int width = 0, height = 0;
        RetailReachability::Point start;
        int heading = 0, retry = 0, weight = 98304, trafficRadius = 0;
        RetailCostSearch::Costs costs;
    };

    // 415170 after type/weight calculation: refresh the grid before asking the
    // controller for goal cells. Controller acceptance is distinct from distance
    // zero. The scheduler charges 500 for this dispatch, even on early completion.
    template<class Prepare, class Goals, class Accepts, class Distance>
    Slice initialize(const Parameters& p, Prepare prepare, Goals goals,
                     Accepts accepts, Distance distance) {
        if (p.width <= 0 || p.height <= 0 || p.retry < 0 || p.retry > 3)
            throw std::invalid_argument("invalid search initialization");
        const bool costPlane=!cost.cells.empty();
        auto plane = costPlane ? std::move(cost.cells) : std::move(trace.cells);
        auto scratch = costPlane ? std::move(cost.scratch) : std::move(trace.scratch);
        const auto previousGoal = trace.goal;
        trace={}; cost.clearAttempt(); route.points.clear(); route.flags=0;
        phase=1; partialDistance=nodeLimit=initialDistance=0; spread=4;
        heading = p.heading; retry = p.retry;
        weight = p.weight < 65536 || p.weight > 20 * 65536 ? 20 * 65536 : p.weight;
        cost.costs = p.costs;
        trace.width = p.width; trace.height = p.height;
        trace.start = p.start; trace.goal = previousGoal; trace.trafficRadius = p.trafficRadius;
        prepare(retry == 3);
        plane.resize(size_t(p.width) * size_t(p.height));
        scratch.reset(plane);
        trace.cells = std::move(plane);
        trace.scratch = std::move(scratch);
        int64_t nearest = INT32_MAX;
        for (const auto goal : goals()) {
            if (goal.x >= 0 && goal.z >= 0 && goal.x < p.width && goal.z < p.height) {
                const size_t index=size_t(goal.z * p.width + goal.x);
                trace.scratch.touch(index,trace.cells[index].flags);
                trace.cells[index].flags |= 4;
            }
            const int64_t dx = int64_t(p.start.x) - goal.x, dz = int64_t(p.start.z) - goal.z;
            const int64_t squared = dx * dx + dz * dz;
            if (squared < nearest) { nearest = squared; trace.goal = goal; }
        }
        if (accepts(p.start.x, p.start.z)) {
            phase = 3;
            return {Result::Complete, 500, 0x1000};
        }
        initialDistance = distance(p.start.x, p.start.z);
        if (p.start.x < 0 || p.start.z < 0 || p.start.x >= p.width || p.start.z >= p.height) {
            phase = 3;
            return {Result::Complete, 500, 0x2000};
        }
        return {Result::Searching, 500};
    }

    template<class Grade, class Distance>
    Slice step(Grade grade, Distance distance, int budget,const std::function<int()>& currentHeading={}) {
        if (phase == 1) {
            partialDistance = trace.step(grade, distance, budget);
            if (partialDistance == -2) return {Result::Searching, trace.work};
            if (partialDistance == -1) {
                route = retailDirectRoute(trace); phase = 3;
                return {Result::Complete, trace.work + 30, 0x1000};
            }
            const int notification = partialDistance == 0 ? 0x1000 : 0x2000;
            if (partialDistance >= initialDistance && partialDistance != 0) {
                route = {}; phase = 3;
                return {Result::Complete, trace.work, notification};
            }
            const int startCell = trace.start.z * trace.width + trace.start.x;
            // 415cb0 reads the unit's current heading when phase 2 is seeded,
            // potentially many simulation ticks after attempt initialization.
            if (currentHeading) heading=currentHeading();
            cost.reset(trace.width, trace.height, startCell, (heading + 4096) >> 13,
                       initialDistance, weight, std::move(trace.cells),std::move(trace.scratch));
            const int count = trace.width * trace.height;
            nodeLimit = retry > 2 ? count * 2 : retry > 0 ? count / (10 / retry) : count / 20;
            spread = 4; phase = 2;
            return {Result::Searching, trace.work, notification};
        }
        if (phase == 2) {
            auto slice = cost.runSlice(grade, distance, partialDistance, nodeLimit, budget, retry, spread);
            if (slice.result == RetailCostSearch::Result::Exhausted)
                return {Result::Retry, slice.work};
            if (slice.result == RetailCostSearch::Result::Arrived) {
                route = retailReconstructRoute(cost.cells, trace.width, trace.start,
                        {cost.endpoint % trace.width, cost.endpoint / trace.width},
                        partialDistance, trace.trafficRadius);
                phase = 3;
                return {Result::Complete, slice.work};
            }
            return {Result::Searching, slice.work};
        }
        return {Result::Complete, 0};
    }
};
// Phase dispatcher supplied to RetailSearchScheduler::tick. World callbacks
// remain explicit: each retry re-reads its request and refreshes its grid.
class RetailSearchWorker {
public:
    RetailSearchAttempt attempt;
    int retry = 0;
    bool initializationPending = true;

    template<class Configure, class Prepare, class Goals, class Accepts, class Distance, class Grade>
    RetailSearchAttempt::Slice dispatch(bool admitted, int budget, Configure configure,
            Prepare prepare, Goals goals, Accepts accepts, Distance distance, Grade grade,
            const std::function<int()>& currentHeading={}) {
        using Result = RetailSearchAttempt::Result;
        if (admitted) { retry = 0; initializationPending = true; }
        if (initializationPending) {
            auto parameters = configure(retry);
            parameters.retry = retry;
            initializationPending = false;
            return attempt.initialize(parameters, prepare, goals, accepts, distance);
        }
        auto result = attempt.step(grade, distance, budget,currentHeading);
        if (result.result != Result::Retry) return result;
        if (retry < 3) {
            ++retry;
            initializationPending = true;
            return {Result::Searching, result.work};
        }
        // 416718: even final failure queries all 81 grades and controller
        // acceptances in x-major order before clearing the route.
        for (int x = -4; x <= 4; ++x)
            for (int z = -4; z <= 4; ++z) {
                const auto start = attempt.trace.start;
                (void)grade(start.x + x, start.z + z, 0);
                (void)accepts(start.x + x, start.z + z);
            }
        attempt.route = {}; attempt.phase = 3;
        return {Result::Complete, result.work};
    }
};
} // namespace tak::sim
