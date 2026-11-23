// Line-oriented oracle driver. Input fixtures are generated from the user's
// local retail executable by tools/re/check_cost_search.py, never committed.
#include "sim/retailcost.h"
#include "sim/retailreach.h"
#include "sim/sim.h"
#include <iostream>

namespace tak::sim {
struct RetailReplayProbe {
    static RetailCostSearch::Costs costs(const World& world,const Unit& unit) { return world.searchCosts(unit); }
};
}

static void word(uint64_t& hash, uint32_t value) {
    for (int byte = 0; byte < 4; ++byte) {
        hash ^= (value >> (byte * 8)) & 255; hash *= 1099511628211ull;
    }
}

int main(int argc, char** argv) {
    using tak::sim::RetailCostSearch;
    if (argc == 2 && std::string(argv[1]) == "--reachability") {
        int w,h,sx,sz,sw,sh,tx,tz,tw,th,mode,limited;
        while (std::cin >> w >> h >> sx >> sz >> sw >> sh >> tx >> tz >> tw >> th >> mode >> limited) {
            if (w<=0 || h<=0 || w>1024 || h>1024) return 2;
            std::vector<int> cells(size_t(w)*h);
            for (int& value:cells) std::cin >> value;
            if (!std::cin) return 2;
            std::cout << tak::sim::retailRectangleReachable(w,h,sx,sz,sw,sh,tx,tz,tw,th,
                mode!=0,limited!=0,[&](int x,int z) { return cells[size_t(z)*w+x]; }) << '\n';
        }
        return 0;
    }
    const bool resume = argc == 2 && std::string(argv[1]) == "--resume";
    const bool slices = argc == 2 && std::string(argv[1]) == "--slices";
    if (argc == 2 && (std::string(argv[1]) == "--costs" || std::string(argv[1]) == "--world-costs")) {
        const bool worldCosts=std::string(argv[1])=="--world-costs";
        int turn, foot, road, water, flags, heavy;
        while (std::cin >> turn >> foot >> road >> water >> flags >> heavy) {
            auto costs = tak::sim::retailPathCosts(uint16_t(turn), foot, road, water, uint16_t(flags), heavy != 0);
            if (worldCosts) {
                int floater,minimum,maximum;
                if (!(std::cin>>floater>>minimum>>maximum)) return 2;
                tak::sim::World world;tak::sim::UnitType type;tak::sim::Unit unit;
                type.turnRate=turn;type.footX=type.footZ=foot;
                type.roadMult=tak::sim::Fixed::raw(road);type.waterMult=tak::sim::Fixed::raw(water);
                type.floater=floater!=0;type.minWaterDepth=minimum;type.maxWaterDepth=maximum;
                unit.type=&type;unit.groundTerrainFlags=uint16_t(flags);
                costs=tak::sim::RetailReplayProbe::costs(world,unit);
            }
            for (int value : costs.turn) std::cout << value << ' ';
            std::cout << costs.ground << ' ' << costs.road << ' ' << costs.slope << ' ' << costs.traffic
                      << ' ' << costs.shortTurn << ' ' << costs.minStraight;
            if (worldCosts) std::cout << ' ' << int(costs.heavyFloater);
            std::cout << '\n';
        }
        return 0;
    }
    int width, height, start, direction, gx, gz, weight, radius, limit, spread;
    while (std::cin >> width >> height >> start >> direction >> gx >> gz >> weight >> radius >> limit >> spread) {
        if (width <= 0 || height <= 0 || width > 1024 || height > 1024 || limit < 0 || limit > 100000) return 2;
        RetailCostSearch search;
        int sliceBudget = 0, nodeLimit = 0, retry = 0;
        if (slices) std::cin >> sliceBudget >> nodeLimit >> retry;
        int tolerance = 0, processed = 0, pending = 0, laterSpread = 2, nodeCount = 0;
        if (resume) {
            std::cin >> tolerance >> processed >> pending >> laterSpread >> nodeCount;
            if (nodeCount < 0 || nodeCount > width * height) return 2;
        }
        for (int& value : search.costs.step) std::cin >> value;
        for (int& value : search.costs.turn) std::cin >> value;
        std::cin >> search.costs.ground >> search.costs.road >> search.costs.slope >> search.costs.traffic
                 >> search.costs.shortTurn >> search.costs.minStraight;
        auto distance = [&](int x, int z) { return tak::sim::retailGoalDistance(x-gx, z-gz, tolerance); };
        search.reset(width, height, start, direction, distance(start % width, start / width), weight);
        std::vector<int> grades(size_t(width) * height);
        for (size_t i = 0; i < grades.size(); ++i) {
            int flags, dir;
            std::cin >> grades[i] >> flags >> dir;
            search.cells[i].flags = uint8_t(flags); search.cells[i].direction = uint8_t(dir);
        }
        if (resume) {
            std::vector<RetailCostSearch::Node> nodes;
            for (int i = 0; i < nodeCount; ++i) {
                RetailCostSearch::Node node;
                int grade, run;
                std::cin >> node.cell >> node.cost >> node.priority >> grade >> run;
                node.gradeCost = int16_t(grade); node.straightSteps = int16_t(run);
                nodes.push_back(node);
            }
            search.restore(width, height, std::move(search.cells), std::move(nodes), processed, weight, pending != 0);
        }
        if (!std::cin) return 2;
        for (int n = 0; n < limit; ++n) {
            const auto grade = [&](int x, int z) {
                if (!resume) return grades[size_t(z*width+x)];
                int expectedX, expectedZ, grade;
                if (!(std::cin >> expectedX >> expectedZ >> grade) || x != expectedX || z != expectedZ)
                    throw std::runtime_error("retail grade-query order differs");
                return grade;
            };
            RetailCostSearch::Result result;
            if (slices) {
                auto slice = search.runSlice(grade, distance, radius, nodeLimit, sliceBudget, retry, spread);
                result = slice.result;
                std::cout << slice.work << ' ' << spread << ' ';
            } else result = search.pop(grade, distance, radius, n == 0 ? spread : laterSpread);
            uint64_t cells = 14695981039346656037ull, heap = cells;
            if (!resume)
                for (const auto& cell : search.cells) word(cells, uint32_t(cell.flags) | (uint32_t(cell.direction) << 8));
            for (int id : search.heap) {
                const auto& node = search.nodes[size_t(id)];
                for (int value : {node.cell, node.cost, node.priority, int(node.gradeCost), int(node.straightSteps)})
                    word(heap, uint32_t(value));
            }
            std::cout << search.processed << ' ' << int(result) << ' ' << search.heuristicWeight << ' '
                      << search.heap.size() << ' ';
            if (!resume) std::cout << cells << ' ';
            std::cout << heap << '\n';
            if (result != RetailCostSearch::Result::Searching || (!slices && search.empty())) break;
        }
        if (resume) {
            uint64_t cells = 14695981039346656037ull;
            for (const auto& cell : search.cells) word(cells, uint32_t(cell.flags) | (uint32_t(cell.direction) << 8));
            std::cout << "CELLS " << cells << '\n';
        }
        std::cout << "END\n";
        if (resume) { std::cin >> std::ws; return std::cin.eof() ? 0 : 2; }
    }
}
