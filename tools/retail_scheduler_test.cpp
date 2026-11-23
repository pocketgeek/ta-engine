// Synthetic phase callbacks shared with the executable oracle's controlled
// phases. Actual slot iteration/accounting comes from RetailSearchScheduler.
#include "sim/retailscheduler.h"
#include "sim/retailcost.h"
#include <iostream>
#include <map>
#include <string>
#include <vector>

using Scheduler = tak::sim::RetailSearchScheduler;
struct Fixture {
    Scheduler scheduler;
    std::array<Scheduler::Player, 10> players{};
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
                if (admitted) phase = 0;
                if (phase == 0) {
                    events.push_back({tick, 0, r.player, r.slot});
                    phase = 1; pops = 0;
                    return Scheduler::Slice{500, false};
                }
                if (phase == 1) {
                    events.push_back({tick, 1, r.player, r.slot});
                    phase = 2;
                    return Scheduler::Slice{30, false};
                }
                int work = 0;
                do {
                    work += 10;
                    if (++pops >= requests.at({r.player, r.slot})) {
                        events.push_back({tick, 2, r.player, r.slot});
                        requests.erase({r.player, r.slot});
                        return Scheduler::Slice{work + 50, true};
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
            const bool accepted=tak::sim::retailAdmitNavigator(pending!=0,tick,stamp);
            std::cout<<accepted<<' '<<stamp<<'\n';
        }
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--oracle") {
        int slots, budget, divisor, ticks, priorities, count;
        while (std::cin >> slots >> budget >> divisor >> ticks >> priorities >> count) {
            if (slots < 1 || slots > 500 || budget < 1 || budget > 12000 ||
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
                for (int value : s.shares) std::cout << ' ' << value;
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
    // Resume the singleton before admitting another player's short request.
    Fixture f;
    for (int p = 0; p < 2; ++p) f.players[size_t(p)] = {true, false, 0, 8};
    f.requests = {{{0, 1}, 1}, {{1, 1}, 2000}};
    f.step(12000, 1);
    if (f.scheduler.active != Scheduler::Request{1, 1} || f.scheduler.shares[1] >= 0 || f.events.size() != 2) return 1;
    f.step(12000, 1);
    if (!f.requests.empty() || f.events.size() != 6 || f.events[2].player != 1 || f.events[5].player != 0) return 1;
    // Scan can exhaust the budget before initialization; admission survives.
    Fixture small;
    small.players[0] = {true, false, 0, 8};
    small.requests = {{{0, 1}, 1}};
    small.step(7, 1);
    if (!small.scheduler.newAdmission || !small.events.empty()) return 1;
    small.step(7, 1);
    if (small.phase != 1 || small.scheduler.remaining != -493) return 1;
    small.scheduler.cancel({0, 1});
    if (small.scheduler.active.player != -1 || small.scheduler.newAdmission) return 1;
    // An arrival beyond the node cap is legal within the same slice. On the
    // next slice, the cap is enforced before another node can be expanded.
    using Search = tak::sim::RetailCostSearch;
    Search search;
    const auto grade = [](int, int) { return 6; };
    const auto distance = [](int x, int z) { return tak::sim::retailGoalDistance(x - 3, z); };
    search.reset(4, 1, 0, 6, distance(0, 0));
    int spread = 4;
    auto slice = search.runSlice(grade, distance, 0, 1, 1000, 0, spread);
    if (slice.result != Search::Result::Arrived || search.processed != 4 || slice.work != 90) return 1;
    search.reset(4, 1, 0, 6, distance(0, 0));
    spread = 4;
    slice = search.runSlice(grade, distance, 0, 1, 1, 1, spread);
    if (slice.result != Search::Result::Searching || slice.work != 10 || spread != 3) return 1;
    slice = search.runSlice(grade, distance, 0, 1, 1000, 1, spread);
    if (slice.result != Search::Result::Exhausted || slice.work != 0 || search.processed != 1) return 1;
    std::cout << "PASS: singleton resume, borrowing, admission suspension, cancellation and cost-slice limits\n";
}
