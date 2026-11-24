// Absurd / Absurdly Defensive AI seats earn double energy AND metal: the seat's
// income multiplier (ai::incomeMultFor -> MatchSlot::manaMult -> PlayerBooks::mult)
// scales every production credit of the TA economy settle (sim/taeconomy.cpp).
// Retail's own computer levels keep TotalA.exe's x0.5 / x0.7 / x1.0 (retail-ai.md 2).
#include "ai/ai.h"
#include "sim/sim.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ta;
static int failures = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    failures += !ok;
}

int main() {
    using ai::Difficulty;
    check(ai::incomeMultFor(Difficulty::Absurd) == 2.0f, "Absurd seat: income x2");
    check(ai::incomeMultFor(Difficulty::AbsurdlyDefensive) == 2.0f, "Absurdly Defensive seat: income x2");
    check(ai::incomeMultFor(Difficulty::Hard) == 1.0f && ai::incomeMultFor(Difficulty::Normal) == 1.0f,
          "Hard / Normal seats: income x1");
    check(ai::incomeMultFor(Difficulty::RetailEasy) == 0.5f && ai::incomeMultFor(Difficulty::Classic) == 0.7f &&
          ai::incomeMultFor(Difficulty::RetailHard) == 1.0f,
          "retail Easy / Medium / Hard seats: x0.5 / x0.7 / x1.0");

    // Two identical players, each with one producer making energy and metal; player 1
    // carries Absurd's multiplier. Over the same settles it produces twice as much of both.
    sim::UnitType gen;
    gen.id = "gen"; gen.maxHp = 100; gen.footX = gen.footZ = 2;
    gen.energyMake = 20; gen.metalMake = 1.5f;
    sim::World w;
    w.setTerrain(std::vector<uint8_t>(64 * 64, 100), 64, 64, 20);
    w.setPlayerCount(2);
    for (int p = 0; p < 2; ++p) w.setStartResources(p, 0, 0);
    w.player(1).books.mult = ai::incomeMultFor(Difficulty::Absurd);
    w.spawn(&gen, 300, 300, 0.0f, 0);
    w.spawn(&gen, 700, 700, 0.0f, 1);
    for (int i = 0; i < 30 * 10; ++i) w.tick(1.f / 30.f);
    const auto& a = w.books(0);
    const auto& b = w.books(1);
    std::printf("produced: normal E %.1f M %.1f, absurd E %.1f M %.1f\n",
                a.produced[0], a.produced[1], b.produced[0], b.produced[1]);
    check(a.produced[0] > 0 && a.produced[1] > 0, "the producer makes energy and metal");
    check(std::fabs(b.produced[0] - 2 * a.produced[0]) < 1e-3, "Absurd seat produces double energy");
    check(std::fabs(b.produced[1] - 2 * a.produced[1]) < 1e-3, "Absurd seat produces double metal");
    std::printf(failures ? "FAILED (%d)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
