// The TA settle arithmetic (sim/taeconomy.h) against docs/retail-economy.md:
// shared payment fractions, debt carried and stalling, overflow waste, payNow.
#include "sim/taeconomy.h"

#include <cmath>
#include <cstdio>

using namespace ta::sim::econ;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int main() {
    // Enough in store: everything is paid, nothing carries.
    {
        PlayerBooks p; p.stored[E] = 100; p.stored[M] = 100; p.cap[E] = p.cap[M] = 1000;
        UnitBooks a, b;
        CHECK(requestBoth(a, 30, 10));
        CHECK(requestBoth(b, 20, 10));
        Sums s; s.add(a.r); s.add(b.r);
        Fractions f = pay(p, s);
        roll(a.r, f); roll(b.r, f);
        CHECK(near(p.stored[E], 50) && near(p.stored[M], 80));
        CHECK(!a.indebted() && !b.indebted());
        CHECK(near(p.expense[E], 50));
    }
    // Short: both get the same fraction; the unpaid part becomes debt and stalls.
    {
        PlayerBooks p; p.stored[E] = 50; p.stored[M] = 1000; p.cap[E] = p.cap[M] = 1000;
        UnitBooks a, b;
        requestBoth(a, 60, 0);
        requestBoth(b, 40, 0);
        Sums s; s.add(a.r); s.add(b.r);
        Fractions f = pay(p, s);
        CHECK(near(f.fN[E], 0.5f));
        roll(a.r, f); roll(b.r, f);
        CHECK(near(a.r[E].debt, 30) && near(b.r[E].debt, 20));
        CHECK(near(p.stored[E], 0));
        // In debt: a new request counts but is not pooled; the unit must not act.
        CHECK(!requestBoth(a, 6, 0));
        CHECK(near(a.r[E].req, 6) && near(a.r[E].newReq, 0));
        // Next second, income clears old debt first.
        Sums s2; s2.add(a.r); s2.add(b.r); s2.prod[E] = 100;
        Fractions f2 = pay(p, s2);
        CHECK(near(f2.fD[E], 1));
        roll(a.r, f2); roll(b.r, f2);
        CHECK(!a.indebted() && !b.indebted());
        CHECK(near(p.stored[E], 50));
    }
    // Overflow above the cap is wasted.
    {
        PlayerBooks p; p.stored[M] = 990; p.cap[M] = 1000;
        Sums s; s.prod[M] = 25;
        pay(p, s);
        CHECK(near(p.stored[M], 1000) && near(float(p.wasted[M]), 15));
        // The lifetime totals (the result plate): produced gains the second's whole
        // production (0x401a0d), wasted only the overflow (0x401b21).
        CHECK(p.produced[M] == 25.0 && p.produced[E] == 0.0);
        Sums s2; s2.prod[M] = 7;
        pay(p, s2);
        CHECK(p.produced[M] == 32.0 && near(float(p.wasted[M]), 22));
    }
    // The plate's Score (0x41ddbe): trunc(tick/60 * timemul) + trunc(kills * killmul), >= 0.
    {
        CHECK(plateScore(9000, 12, 50, 0) == 600);
        CHECK(plateScore(9059, 0, 50, 1) == 150);    // 9059/60 = 150, an integer division first
        CHECK(plateScore(36000, 3, 10, -1) == 0);    // -600 + 30 is floored at 0
        CHECK(plateScore(600, 3, 10, -1) == 20);
    }
    // payNow takes from storage at once, or refuses without touching it.
    {
        PlayerBooks p; p.stored[E] = 10;
        Acc a;
        CHECK(!payNow(p, a, E, 20) && near(p.stored[E], 10));
        CHECK(payNow(p, a, E, 4) && near(p.stored[E], 6) && near(a.req, 4));
    }
    // The MSVC LCG (wind interval): first draws from seed 1.
    {
        MsvcRand r; r.s = 1;
        CHECK(r.next() == 41 && r.next() == 18467 && r.next() == 6334);
    }
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("ta_economy_test: ok");
    return 0;
}
