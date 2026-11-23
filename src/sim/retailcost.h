#pragma once

// Resumable phase-2 kernel (0x413e70 / 0x4142c0). This is separate from the
// current PathService until its scheduler and tracer hand-off are replaced.
// Owns no world state: callers supply the grade and controller distance query.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace tak::sim {

// Scratch planes retain direction bytes between attempts. Once initialization
// owns the plane, flags never return to zero until the next reset, so that byte
// also identifies the first write without a second map-sized marker array.
// Imported/test planes use a full reset until their writes are tracked.
struct RetailSearchCellScratch {
    std::vector<uint32_t> touched;
    bool complete = false;
    size_t resetCells = 0; // observational work count, never scheduler work

    void touch(size_t index, uint8_t flags) {
        if (complete && !flags) touched.push_back(uint32_t(index));
    }
    template<class Cells>
    void reset(Cells& cells) {
        resetCells = 0;
        if (complete) {
            for (size_t index : touched) if (index < cells.size()) {
                cells[index].flags = 0; cells[index].node = -1; ++resetCells;
            }
        } else {
            for (auto& cell : cells) { cell.flags = 0; cell.node = -1; }
            resetCells = cells.size();
        }
        touched.clear(); complete = true;
    }
};

inline int retailGoalDistance(int dx, int dz, int tolerance = 0) {
    dx = std::abs(dx); dz = std::abs(dz);
    return std::max(0, 18 * std::max(dx, dz) + 7 * std::min(dx, dz) - tolerance);
}

class RetailCostSearch {
public:
    struct Cell { uint8_t flags = 0, direction = 0; int node = -1; };
    struct Node {
        int cell = 0, cost = 0, priority = 0, heapIndex = 0;
        int16_t gradeCost = 0, straightSteps = 0;
    };
    struct Costs {
        std::array<int, 8> step{16, 23, 16, 23, 16, 23, 16, 23};
        std::array<int, 8> turn{0, 80, 120, 160, 200, 160, 120, 80};
        int ground = 24, road = 8, slope = 48, traffic = 80;
        int shortTurn = 136, minStraight = 4;
        bool heavyFloater = false; // type gate also selects the attempt's initial heuristic weight
    } costs;
    enum class Result { Searching, Arrived, Exhausted };
    struct Slice { Result result; int work; };

    // 0x416668..0x4166ef: the node cap is checked BEFORE the inner loop,
    // not after each pop. A slice can cross that cap and still reach the goal.
    // Exhausted hands control back to the scheduler's retry branch. Route
    // reconstruction/delivery belongs to the caller after Arrived.
    template<class Grade, class Distance>
    Slice runSlice(Grade grade, Distance distance,
                   int goalRadius, int nodeLimit, int budget, int retry, int& spread) {
        if (empty() || processed >= nodeLimit) return {Result::Exhausted, 0};
        int work = 0;
        while (work < budget && !empty()) {
            work += 10;
            const auto result = pop(grade, distance, goalRadius, spread);
            if (result == Result::Arrived) return {result, work + 50};
            spread = 2 + int(retry > 0);
        }
        return {Result::Searching, work};
    }

    void reset(int width, int height, int startCell, int direction,
               int initialDistance, int weight = 98304, std::vector<Cell> plane = {},
               RetailSearchCellScratch tracked = {}) {
        width_ = width; height_ = height;
        // A tracer hands its existing visited/goal plane to phase 2. Do not
        // allocate and clear another map-sized plane just to throw it away.
        if (plane.empty()) cells.assign(size_t(width) * height, {});
        else {
            if (plane.size()!=size_t(width)*height)
                throw std::invalid_argument("invalid search plane size");
            cells=std::move(plane);
        }
        scratch=std::move(tracked);
        nodes.clear(); heap.clear(); free_.clear();
        processed = 0; heuristicWeight = weight; pendingRoot_ = false;
        endpoint = -1;
        auto& start=cells.at(size_t(startCell));
        scratch.touch(size_t(startCell),start.flags);
        start.flags |= 1;
        cells[size_t(startCell)].direction = uint8_t(direction & 7);
        insert({startCell, 0, weighted(initialDistance), 0, 0, 100});
    }

    // The tracer leaves goal/visited bits in this same scratch plane. Preserve
    // them before calling pop(); bits 0x18 permit even a now-blocked trace cell.
    std::vector<Cell> cells;
    RetailSearchCellScratch scratch;
    std::vector<Node> nodes;
    std::vector<int> heap;
    int processed = 0, heuristicWeight = 98304, endpoint = -1;

    // Initialization resets the logical search, while retaining the allocation
    // buffers that phase 2 will reuse. The plane/scratch move to the tracer first.
    void clearAttempt() {
        cells.clear(); scratch.touched.clear(); scratch.complete=false; scratch.resetCells=0;
        nodes.clear(); heap.clear(); free_.clear();
        costs={}; width_=height_=processed=0; heuristicWeight=98304;
        endpoint=-1; pendingRoot_=false;
    }
    size_t scratchBytes() const {
        return cells.capacity()*sizeof(Cell)+scratch.touched.capacity()*sizeof(uint32_t)+
               nodes.capacity()*sizeof(Node)+(heap.capacity()+free_.capacity())*sizeof(int);
    }

    bool empty() const { return heap.size() == size_t(pendingRoot_); }
    const Node& top() const { return nodes.at(size_t(heap.at(0))); }

    // Restore a suspended search using nodes in heap order. Node allocation
    // indices are internal: only open cells refer to them, and heap ties never
    // use those indices. Rebuild the references rather than importing pointers.
    void restore(int width, int height, std::vector<Cell> plane,
                 std::vector<Node> orderedNodes, int count, int weight, bool pendingRoot) {
        if (width <= 0 || height <= 0 || plane.size() != size_t(width) * size_t(height) ||
            count < 0 || (pendingRoot && orderedNodes.empty()))
            throw std::invalid_argument("invalid suspended search");
        width_ = width; height_ = height; cells = std::move(plane);
        scratch.touched.clear(); scratch.complete=false; scratch.resetCells=0;
        nodes = std::move(orderedNodes); heap.clear(); free_.clear();
        for (auto& cell : cells) cell.node = -1;
        for (size_t i = 0; i < nodes.size(); ++i) {
            auto& node = nodes[i];
            if (node.cell < 0 || size_t(node.cell) >= cells.size() || cells[size_t(node.cell)].node != -1)
                throw std::invalid_argument("invalid suspended heap cell");
            node.heapIndex = int(i); heap.push_back(int(i)); cells[size_t(node.cell)].node = int(i);
        }
        for (const auto& cell : cells)
            if ((cell.flags & 3) == 1 && cell.node == -1)
                throw std::invalid_argument("open cell absent from suspended heap");
        processed = count; heuristicWeight = weight; pendingRoot_ = pendingRoot; endpoint = -1;
    }

    template<class Grade, class Distance>
    Result pop(Grade grade, Distance distance,
               int goalRadius, int spread) {
        if (empty()) return Result::Exhausted;
        ++processed;
        if (processed > 100) heuristicWeight = std::min(50 * 65536, heuristicWeight + 655);
        if (pendingRoot_) { erase(heap[0]); pendingRoot_ = false; }
        const Node parent = top();
        const int parentId = heap[0];
        pendingRoot_ = true;
        Cell& origin = cells[size_t(parent.cell)];
        if (origin.flags & 4) { endpoint = parent.cell; return Result::Arrived; }
        origin.flags = uint8_t((origin.flags & ~1) | 2);
        const int heading = origin.direction;
        const int x = parent.cell % width_, z = parent.cell / width_;
        for (int offset = -spread; offset <= spread; ++offset) {
            const int relative = offset & 7, direction = (heading + relative) & 7;
            const int nx = x + dx_[direction], nz = z + dz_[direction];
            if (nx < 0 || nz < 0 || nx >= width_ || nz >= height_) continue;
            const int index = nz * width_ + nx;
            Cell& cell = cells[size_t(index)];
            const int state = cell.flags & 3;
            if (state >= 2) continue;
            int entry = 0, remaining = 0;
            if (state == 1) {
                entry = nodes[size_t(cell.node)].gradeCost;
            } else {
                scratch.touch(size_t(index),cell.flags);
                const int score = [&] {
                    if constexpr (requires { grade(nx, nz, direction); })
                        return grade(nx, nz, direction);
                    else return grade(nx, nz);
                }();
                if (score == 5) cell.flags |= 0x40;
                if (score < 4 && !(cell.flags & 0x18)) { cell.flags |= 3; continue; }
                remaining = distance(nx, nz);
                cell.flags |= uint8_t(remaining <= goalRadius ? 5 : 1);
                entry = score == 5 ? costs.traffic : score == 4 ? costs.slope :
                        score == 7 ? costs.road : costs.ground;
                entry = int16_t(entry);
            }
            int cost = parent.cost + costs.step[size_t(direction)] + costs.turn[size_t(relative)] + entry;
            if (relative && parent.straightSteps < costs.minStraight) cost += costs.shortTurn;
            const auto run = int16_t(relative ? 1 : parent.straightSteps + 1);
            if (state == 1) {
                Node& node = nodes[size_t(cell.node)];
                if (cost >= node.cost) continue;
                cell.direction = uint8_t(direction);
                node.priority += cost - node.cost;
                node.cost = cost; node.straightSteps = run;
                up(node.heapIndex);
                // A decrease-key may displace the previously expanded root.
                // Retail removes that old node immediately in this case.
                if (pendingRoot_ && nodes[size_t(parentId)].heapIndex != 0) {
                    erase(parentId); pendingRoot_ = false;
                }
            } else {
                cell.direction = uint8_t(direction);
                Node node{index, cost, cost + weighted(remaining), 0, int16_t(entry), run};
                if (pendingRoot_) {
                    // Retail reuses the root for the first newly opened child,
                    // then sifts DOWN. Pop+push has different tie behavior.
                    nodes[size_t(parentId)] = node;
                    cell.node = parentId;
                    down(0); pendingRoot_ = false;
                } else insert(node);
            }
        }
        return Result::Searching;
    }

private:
    int width_ = 0, height_ = 0;
    bool pendingRoot_ = false;
    std::vector<int> free_;
    static constexpr int dx_[8] = {0, -1, -1, -1, 0, 1, 1, 1};
    static constexpr int dz_[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
    int weighted(int distance) const { return int((int64_t(heuristicWeight) * distance) >> 16); }
    void place(int slot, int node) {
        heap[size_t(slot)] = node; nodes[size_t(node)].heapIndex = slot;
    }
    void up(int slot) {
        const int node = heap[size_t(slot)];
        while (slot > 0) {
            const int parent = (slot - 1) / 2;
            if (nodes[size_t(node)].priority >= nodes[size_t(heap[size_t(parent)])].priority) break;
            place(slot, heap[size_t(parent)]); slot = parent;
        }
        place(slot, node);
    }
    void down(int slot) {
        const int node = heap[size_t(slot)];
        for (;;) {
            int child = slot * 2 + 1;
            if (child >= int(heap.size())) break;
            if (child + 1 < int(heap.size()) && nodes[size_t(heap[size_t(child + 1)])].priority <
                                               nodes[size_t(heap[size_t(child)])].priority) ++child;
            if (nodes[size_t(heap[size_t(child)])].priority >= nodes[size_t(node)].priority) break;
            place(slot, heap[size_t(child)]); slot = child;
        }
        place(slot, node);
    }
    void insert(Node node) {
        int id;
        if (free_.empty()) { id = int(nodes.size()); nodes.push_back(node); }
        else { id = free_.back(); free_.pop_back(); nodes[size_t(id)] = node; }
        cells[size_t(node.cell)].node = id;
        const int slot = int(heap.size()); heap.push_back(id); up(slot);
    }
    void erase(int id) {
        const int slot = nodes[size_t(id)].heapIndex;
        const int last = heap.back(); heap.pop_back(); free_.push_back(id);
        if (slot < int(heap.size())) { place(slot, last); down(slot); }
    }
};

// A mover's turn rate under its terrain bits (mover +0x36): bit 11 (road)
// scales by roadmultiplier, else bit 12 (water) by watermultiplier. The
// multipliers are signed 16.16; retail truncates toward zero (_ftol) and keeps
// the low 16 bits, so a negative product wraps to a large rate. Shared by the
// cost search (0x4151ef..) and the target score's turn penalty (412a77..412ae2).
inline uint16_t retailEffectiveTurnRate(uint16_t turnRate, int32_t roadMultiplier,
                                        int32_t waterMultiplier, uint16_t movementFlags) {
    if (movementFlags & 0x800)
        return uint16_t(int64_t(turnRate) * roadMultiplier / 65536);
    if (movementFlags & 0x1000)
        return uint16_t(int64_t(turnRate) * waterMultiplier / 65536);
    return turnRate;
}

// Initialization at 0x4151ef..0x4159a5. Multipliers are signed 16.16;
// retail truncates before narrowing the effective turn rate to uint16.
inline RetailCostSearch::Costs retailPathCosts(uint16_t turnRate, int footprint,
                                               int32_t roadMultiplier, int32_t waterMultiplier,
                                               uint16_t movementFlags, bool heavySlope) {
    RetailCostSearch::Costs costs;
    costs.heavyFloater=heavySlope;
    costs.slope = heavySlope ? 320 : 48;
    if (roadMultiplier >= 81920) {
        costs.ground = int(int64_t(24) * roadMultiplier / 65536);
        costs.slope = int(int64_t(costs.slope) * roadMultiplier / 65536);
        costs.traffic = int(int64_t(80) * roadMultiplier / 65536);
    }
    const uint16_t effectiveTurn = retailEffectiveTurnRate(turnRate, roadMultiplier, waterMultiplier,
                                                           movementFlags);
    costs.minStraight = footprint + 3;
    if (effectiveTurn >= 1000) {
        costs.turn.fill(0); costs.shortTurn = 16; costs.minStraight = footprint;
    } else if (effectiveTurn > 200) {
        for (int& turn : costs.turn) turn = turn * (1000 - effectiveTurn) / 800;
        costs.shortTurn = 16 + 120 * (1000 - effectiveTurn) / 800;
        costs.minStraight = footprint + 3 * (1000 - effectiveTurn) / 800;
    }
    if (heavySlope) { costs.shortTurn *= 5; costs.minStraight *= 2; }
    return costs;
}

} // namespace tak::sim
