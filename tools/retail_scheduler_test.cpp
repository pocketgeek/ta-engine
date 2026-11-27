// Synthetic phase callbacks shared with the executable oracle's controlled
// phases. Actual slot iteration/accounting comes from RetailSearchScheduler.
#include "sim/retailscheduler.h"
#include "sim/retailcost.h"
#include <iostream>
#include <map>
#include <utility>
#include <string>
#include <vector>

using Scheduler = ta::sim::RetailSearchScheduler;
struct Fixture {
    Scheduler scheduler;
    std::array<Scheduler::Player, Scheduler::playerCount> players{};
    std::map<std::pair<int, int>, int> requests;
    struct Event { int tick, kind, player, slot; };
    std::vector<Event> events;
    int phase = 0, pops = 0, tick = 0;

    void step(int budget, int divisor) {
        ++tick;
        for (auto& p : players) p.pending = 0;
        for (auto [key, count] : requests) ++players[size_t(key.first)].pending;
        scheduler.tick(players, budget, divisor, tick,
            [&](Scheduler::Request r) { return requests.contains({r.player, r.slot}); },
            [&](Scheduler::Request r) { return requests.contains({r.player, r.slot}); },
            [&](Scheduler::Request r, bool admitted, int remaining) {
                // TotalA.exe 0x40e630 / 0x40eeaf: init (with the tracer run to
                // completion) costs 100; A* pops cost 1 each in slices of 100.
                if (admitted) phase = 0;
                if (phase == 0) {
                    events.push_back({tick, 0, r.player, r.slot});
                    phase = 1; pops = 0;
                    return Scheduler::Slice{100, false};
                }
                int work = 0;
                do {
                    for (int n = 0; n < 100; ++n) {
                        ++work;
                        if (++pops >= requests.at({r.player, r.slot})) {
                            events.push_back({tick, 2, r.player, r.slot});
                            requests.erase({r.player, r.slot});
                            return Scheduler::Slice{work, true};
                        }
                    }
                } while (work < remaining);
                return Scheduler::Slice{work, false};
            });
        if (scheduler.newAdmission) phase = 0;
    }
};

int main(int argc, char** argv) {
    if (argc>1 && std::string(argv[1])=="--admission") {
        unsigned pending; uint32_t tick,stamp;
        while (std::cin>>pending>>tick>>stamp) {
            const bool accepted=ta::sim::retailAdmitNavigator(pending!=0,tick,stamp);
            std::cout<<accepted<<' '<<stamp<<'\n';
        }
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--oracle") {
        int slots, budget, divisor, ticks, priorities, count;
        while (std::cin >> slots >> budget >> divisor >> ticks >> priorities >> count) {
            if (slots < 1 || slots > 500 || budget < 1 || budget > 20000 ||
                ticks < 1 || ticks > 300 || count < 0 || count > 10 * slots) return 2;
            Fixture f;
            for (auto& p : f.players) p.slots = slots;
            for (int n = 0; n < count; ++n) {
                int player, slot, pops;
                if (!(std::cin >> player >> slot >> pops) || player < 0 || player >= 10 ||
                    slot < 0 || slot >= slots || pops < 1) return 2;
                f.requests[{player, slot}] = pops;
                f.players[size_t(player)].enabled = true;
                f.players[size_t(player)].priority = (priorities & (1 << player)) != 0;
            }
            for (int tick = 0; tick < ticks; ++tick) {
                f.step(budget, divisor);
                const auto& s = f.scheduler;
                std::cout << "S " << s.active.player << ' ' << s.active.slot << ' '
                          << f.phase << ' ' << s.remaining << ' ' << s.playerCursor;
                for (int value : s.credit) std::cout << ' ' << value;
                for (int value : s.slotCursor) std::cout << ' ' << value;
                for (int value : s.lastWrapTick) std::cout << ' ' << value;
                std::cout << '\n';
            }
            for (const auto& e : f.events)
                std::cout << "E " << e.tick << ' ' << e.kind << ' ' << e.player << ' ' << e.slot << '\n';
            std::cout << "END\n";
        }
        return 0;
    }
    // Credits: every active player gains budget / activePlayers each tick,
    // requests or not, and keeps the turn while its credit is positive.
    {
        Fixture f;
        for (int p = 0; p < 2; ++p) f.players[size_t(p)] = {true, false, 0, 8};
        f.step(1333, 1);
        const auto& s = f.scheduler;
        if (s.credit[0] != 0 || s.credit[1] != 0 || s.visits[0] != 666 || s.visits[1] != 666 ||
            s.remaining != 0 || s.playerCursor != 1 || !f.events.empty()) return 1;
        // 666 visits of an 8-slot pool: the cursor wrapped and kept going.
        if (s.slotCursor[0] != 666 % 8) return 1;
    }
    // A search overshooting its player's credit leaves the credit negative;
    // later grants repay it before that player works again.
    {
        Fixture f;
        f.players[0] = {true, false, 0, 8};
        f.requests = {{{0, 1}, 250}};
        f.step(50, 1);   // visit slot 1 (1), admit, init (100)
        if (f.scheduler.credit[0] != -51 || f.events.size() != 1 || f.events[0].kind != 0) return 1;
        f.step(50, 1);   // -1: nothing runs
        if (f.scheduler.credit[0] != -1 || f.events.size() != 1 || f.pops != 0) return 1;
        f.step(50, 1);   // 49: one 100-pop slice
        if (f.scheduler.credit[0] != -51 || f.pops != 100) return 1;
        f.step(50, 1); f.step(50, 1);
        if (f.pops != 200) return 1;
        f.step(50, 1); f.step(50, 1);   // final 50 pops complete the search
        if (!f.requests.empty() || f.events.size() != 2 || f.events[1].kind != 2 ||
            f.scheduler.credit[0] != -1 || f.scheduler.active.player != -1) return 1;
    }
    // The running search resumes before another player's request is admitted,
    // and its slices are charged to its own player.
    {
        Fixture f;
        for (int p = 0; p < 2; ++p) f.players[size_t(p)] = {true, false, 0, 8};
        f.requests = {{{0, 1}, 1}, {{1, 1}, 1000}};
        f.step(1000, 1);
        // Player 0: visit (1), init (100), one pop (1) completes; its credit of
        // 500 keeps the turn sweeping, then player 1 admits its long search.
        if (f.events.size() < 3 || f.events[0].player != 0 || f.events[1].kind != 2 ||
            f.events[2].player != 1 || f.events[2].kind != 0) return 1;
        if (f.scheduler.active != Scheduler::Request{1, 1}) return 1;
        f.requests[{0, 2}] = 1;
        for (int i = 0; i < 4 && f.requests.contains({1, 1}); ++i) {
            f.step(1000, 1);
            if (f.scheduler.active.player == 0 && f.requests.contains({1, 1})) return 1;
        }
        if (f.requests.contains({1, 1})) return 1;
    }
    // Weights: every 150 ticks a player's slot visits / maxunits pick its
    // heuristic weight (fewer than 1 sweep 9.0, fewer than 2 4.5, else 1.5).
    for (const auto [divide, expected] : {std::pair{1, Scheduler::kWeightMid},
                                          std::pair{0, Scheduler::kWeightSlow},
                                          std::pair{2, Scheduler::kWeightFast}}) {
        Fixture f;
        f.players[0] = {true, false, 0, 8};
        for (int i = 0; i < 149; ++i) f.step(1333, 1);
        if (f.scheduler.weight[0] != Scheduler::kWeightFast) return 1;   // starts at 1.5
        const int visits = f.scheduler.visits[0];
        if (visits != 149 * 1333) return 1;
        f.scheduler.maxUnits = divide == 0 ? visits + 1 : visits / divide;
        f.step(1333, 1);
        if (f.scheduler.weight[0] != expected || f.scheduler.weight[1] != Scheduler::kWeightSlow ||
            f.scheduler.visits[0] != 1333) return 1;
    }
    // Admission gate: at most one request per 60 ticks.
    {
        uint32_t stamp = 0;
        if (ta::sim::retailAdmitNavigator(true, 59, stamp) || stamp != 0) return 1;
        if (!ta::sim::retailAdmitNavigator(true, 60, stamp) || stamp != 60) return 1;
        if (ta::sim::retailAdmitNavigator(true, 119, stamp) || stamp != 60) return 1;
        if (ta::sim::retailAdmitNavigator(false, 200, stamp) || stamp != 60) return 1;
        if (!ta::sim::retailAdmitNavigator(true, 120, stamp) || stamp != 120) return 1;
    }
    // Cancellation drops the active search and a pending admission.
    {
        Fixture f;
        f.players[0] = {true, false, 0, 8};
        f.requests = {{{0, 1}, 1000}};
        f.step(50, 1);
        if (f.scheduler.active != Scheduler::Request{0, 1}) return 1;
        f.scheduler.cancel({0, 1});
        if (f.scheduler.active.player != -1 || f.scheduler.newAdmission) return 1;
    }
    // Cost slices: 1 work per pop, the budget tested between 100-pop slices,
    // nothing extra on arrival, Exhausted once the heap empties.
    using Search = ta::sim::RetailCostSearch;
    Search search;
    const auto grade = [](int, int) { return 6; };
    const auto distance = [](int x, int z) { return ta::sim::retailGoalDistance(x - 3, z); };
    search.reset(4, 1, 0, 6, distance(0, 0));
    int spread = 4;
    auto slice = search.runSlice(grade, distance, 0, 1 << 30, 1, 0, spread);
    if (slice.result != Search::Result::Arrived || slice.work != search.processed || slice.work > 100) return 1;
    const auto far = [](int x, int z) { return ta::sim::retailGoalDistance(x - 399, z); };
    search.reset(400, 1, 0, 6, far(0, 0));
    spread = 4;
    slice = search.runSlice(grade, far, 0, 1 << 30, 1, 0, spread);
    if (slice.result != Search::Result::Searching || slice.work != 100 || spread != 2) return 1;
    slice = search.runSlice(grade, far, 0, 1 << 30, 101, 0, spread);
    if (slice.result != Search::Result::Searching || slice.work != 200) return 1;
    const auto walled = [](int x, int) { return x == 2 ? 0 : 6; };
    search.reset(4, 1, 0, 6, distance(0, 0));
    spread = 4;
    slice = search.runSlice(walled, distance, 0, 1 << 30, 1000, 0, spread);
    if (slice.result != Search::Result::Exhausted || slice.work <= 0 || slice.work != search.processed) return 1;
    slice = search.runSlice(walled, distance, 0, 1 << 30, 1000, 0, spread);
    if (slice.result != Search::Result::Exhausted || slice.work != 0) return 1;
    std::cout << "PASS: TA credits, borrowing, resume, weights, 60-tick gate, cancellation and pop slices\n";
}
