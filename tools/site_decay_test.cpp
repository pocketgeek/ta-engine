// site_decay_test -- an abandoned construction site decays as TotalA.exe's does.
//
// GetBuilt (0x402da0) on an unfinished site: stage 0 sleeps 300 ticks, stage 1
// 30, and stage 2 sleeps 30 after a work event or, on a timer wake with none,
// runs 0x41bcd0(site, 11) and sleeps 11. 0x41bcd0 hands 0x41ba60 the unconjure
// amount -(float)(buildtime * 11) / buildcostenergy: the remaining work c =
// clamp(rem + 11 / buildcostenergy, 0, 1), the HP word loses int(c * maxdamage)
// - int(rem * maxdamage), the SITE's metal accumulator gets buildcostmetal *
// (c - rem) as production (x0.5 / x0.7 for an easy / medium computer), and a
// site whose remaining work reaches 1 dies (kind 9) after that last refund.
// No energy comes back. A site with no GetBuilt (a change of owner) never decays.

#include "sim/sim.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace ta::sim;

static int failures = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

namespace {
struct Rig {
    std::unique_ptr<World> w = std::make_unique<World>();
    UnitType hut;
    int site = 0;
    explicit Rig(float bce, float bcm, float remaining, float mult = 1.0f) {
        w->setTerrain(std::vector<uint8_t>(64 * 64, 100), 64, 64, 20);
        hut.id = hut.name = "hut"; hut.maxHp = 1000; hut.maxVel = Fixed(); hut.footX = hut.footZ = 2;
        hut.buildTime = 2000; hut.buildCostEnergy = bce; hut.buildCostMetal = bcm;
        hut.metalStorage = 5000; hut.energyStorage = 5000;
        site = w->spawn(&hut, 300, 300, 0, 0);
        Unit& u = *w->unit(site);
        u.underConstruction = u.buildBegun = u.buildStarted = true;
        u.buildRemaining = remaining;
        u.hp = Fixed::fromInt(int32_t((1.0f - remaining) * 1000.0f));
        w->player(0).books.mult = mult;
    }
    Unit* u() { return w->unit(site); }
    bool alive() { return u() && u()->alive(); }
    void ticks(int n) { for (int i = 0; i < n; ++i) w->tick(1.0f / 30); }
};

// One 0x41ba60 decay step from `rem`, as decayConstruction takes it.
float stepRemaining(float rem, float bce, int bt = 2000) {
    const float amount = -float(double(bt * 11) / double(bce));
    const double v = double(rem) - double(amount) / double(bt);
    return v < 1.0 ? (v > 0.0 ? float(v) : 0.0f) : 1.0f;
}
}  // namespace

int main() {
    {
        // A half-built site, never worked again: nothing for 300 + 30 ticks,
        // then a step every 11.
        Rig r(3000, 600, 0.5f);
        int first = -1;
        for (int t = 1; t <= 400 && first < 0; ++t) {
            r.ticks(1);
            if (r.u()->buildRemaining != 0.5f) first = t;
        }
        check(first >= 330 && first <= 333, "the first decay comes 300 + 30 ticks after creation (got " +
              std::to_string(first) + ")");
        const float after1 = stepRemaining(0.5f, 3000);
        check(r.u()->buildRemaining == after1, "a step takes 11 / buildcostenergy of the build");
        check(r.u()->hp.floorInt() == 500 + (int32_t(0.5f * 1000) - int32_t(after1 * 1000)),
              "the HP word loses the matching hit points");
        const float refund = 600 * (after1 - 0.5f);
        check(std::fabs(r.u()->books.r[1].prod - refund) < 1e-4f && r.u()->books.r[0].prod == 0,
              "the site's metal accumulator gets buildcostmetal x the step; no energy");
        r.ticks(10);
        check(r.u()->buildRemaining == after1, "...and sleeps 11 ticks");
        r.ticks(1);
        check(r.u()->buildRemaining == stepRemaining(after1, 3000), "...then steps again");
    }
    {
        // An easy computer gets half of it (0x4fcc98), a medium one 0.7 (0x4fcc90).
        for (const auto& [mult, k] : {std::pair{0.5f, 0.5}, std::pair{0.7f, 0.7}}) {
            Rig r(3000, 600, 0.5f, mult);
            r.ticks(335);
            const float after = stepRemaining(0.5f, 3000);
            const float refund = float(double(-(600.0f * (0.5f - after))) * k);
            check(std::fabs(r.u()->books.r[1].prod - refund) < 1e-4f,
                  "a computer owner's refund is scaled by its difficulty (" + std::to_string(k) + ")");
        }
    }
    {
        // Run to the end: the site dies once nothing of it is left, refunded in full.
        Rig r(300, 600, 0.5f);
        r.w->setStartResources(0, 0, 0);
        UnitType bank; bank.id = bank.name = "bank"; bank.maxHp = 100; bank.maxVel = Fixed(); bank.metalStorage = 5000;
        r.w->spawn(&bank, 600, 600, 0, 0);   // room in the store for the refund
        int died = -1;
        for (int t = 1; t <= 3000 && died < 0; ++t) { r.ticks(1); if (!r.alive()) died = t; }
        check(died > 330, "the site decays away (dead at tick " + std::to_string(died) + ")");
        r.ticks(40);
        // Every step's refund lands at a settle while the site stands; what the
        // dying site's own accumulator holds since the last settle (up to three
        // 22-metal steps) goes with it (taSettle sums live units).
        const float back = r.w->books(0).stored[1];
        check(back >= 300.0f - 3 * 22.0f - 0.05f && back <= 300.05f && r.w->books(0).stored[0] == 0,
              "its owner got back the half built, in metal only (" + std::to_string(back) + ")");
    }
    {
        // A never worked frame (remaining 1) goes at its first step, with nothing back.
        Rig r(3000, 600, 1.0f);
        r.ticks(340);
        check(!r.alive(), "an unworked site goes at its first decay");
    }
    {
        // buildcostenergy 0: the amount is -inf and the first step takes all of it.
        Rig r(0, 600, 0.25f);
        r.ticks(329);
        check(r.alive(), "buildcostenergy 0: standing until the first step");
        r.ticks(5);
        check(!r.alive(), "...which unbuilds it at once");
    }
    {
        // No GetBuilt (a change of owner's new object): never decays.
        Rig r(3000, 600, 0.5f);
        r.u()->siteGetBuilt = false;
        r.ticks(1200);
        check(r.alive() && r.u()->buildRemaining == 0.5f, "a site without GetBuilt never decays");
    }
    {
        // Work wakes GetBuilt: each tick of work restarts the 30-tick wait.
        Rig r(3000, 600, 0.5f);
        UnitType builder; builder.id = builder.name = "builder"; builder.maxHp = 100; builder.isBuilder = true;
        builder.maxVel = Fixed::fromInt(1); builder.canMove = true; builder.workerTime = 300; builder.buildDist = 200;
        const int b = r.w->spawn(&builder, 300, 360, 0, 0);
        r.w->assist(b, r.site, false);
        r.ticks(600);
        check(!r.alive() || r.u()->buildRemaining < 0.5f, "a worked site does not decay");
    }
    std::printf("site_decay_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
