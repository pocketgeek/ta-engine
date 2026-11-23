// Legion convoys (sim/convoy.h; PLAN 3.1 A1): one click, one convoy.
//
//   convoy_test
//
// 1. The table alone: on 1e4 random order streams the indexed lookup joins
//    the convoy a scan of the whole table picks (or opens one exactly when
//    the scan finds none), and both indexes equal a rebuild every tick.
// 2. The lookup's cost: 2000 distinct per-unit points in one tick (the
//    crowdbench shape) stay within the declared convoy_tests bound (per
//    order p99 <= 16, max <= 64), and a linear scan of the open convoys --
//    the fault this bound exists to catch -- trips it.
// 3. Selections in a Legion world, each requiring every unit to carry one
//    convoyTick, twice: as the client sends them (one
//    explicit order per click, issueSelection: the order joins the table once
//    and keys its area kExplicitArea | serial) and as per-unit commands (the
//    AI's and scripts' path: hudCommands through the
//    uplink, inferred per unit): flyers listed first with saturated offsets,
//    flyers at opposite corners, an all-air selection over 64, 974 units
//    through the 512-command window (explicit: one tick at every round trip;
//    per unit: round trip 0, 8 and 16 one convoy, 24 splits, no 64-unit part
//    split), a patrol (outbound and return legs), a fight beside a patrol
//    (separate classes), and two clicks 48 px apart 4 ticks apart (two convoys).
// The World runs with TAK_LEGION_VERIFY's hook on, so every join is also
// checked against the scan and the indexes against a rebuild.
#include "legion_issue_selection.h"
#include "sim/convoy.h"
#include "sim/legion.h"
#include "sim/matchsetup.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace tak::sim;
using tak::scn::CmdBatch;
using tak::scn::SelectionOrder;
using tak::scn::Verb;

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", what.c_str()); ++failures; }
}

// ---- 1. index vs scan ------------------------------------------------------
void indexVsScan() {
    std::mt19937 rng(20261009);
    auto pick = [&](int n) { return int(rng() % uint32_t(n)); };
    uint64_t joins = 0, opened = 0, joined = 0;
    for (int stream = 0; stream < 10000; ++stream) {
        ConvoyTable t;
        uint32_t now = uint32_t(pick(5));
        // A few click centres per stream; orders sit on them (shared), at a
        // clamped offset (flyers), or anywhere near (unrelated orders).
        std::vector<std::pair<int32_t, int32_t>> clicks;
        for (int i = 0, n = 1 + pick(4); i < n; ++i)
            clicks.push_back({(200 + pick(800)) << 16, (200 + pick(800)) << 16});
        const int orders = 10 + pick(190);
        for (int i = 0; i < orders; ++i) {
            if (pick(6) == 0) now += uint32_t(pick(12));   // gaps, some past the window
            t.prune(now);
            const int player = pick(2);
            const auto cls = ConvoyClass(pick(3));
            const auto [cx, cz] = clicks[size_t(pick(int(clicks.size())))];
            const bool shared = pick(3) != 0;
            int32_t x = cx, z = cz;
            switch (std::max(0, pick(6) - 2)) {
            case 0: break;                                                     // the click
            case 1: x += (pick(2) ? 60 : -60) << 16; z += (pick(2) ? 60 : -60) << 16; break;   // saturated
            case 2: x += (pick(161) - 80) * 65536 + pick(65536); z += (pick(161) - 80) * 65536 + pick(65536); break;
            default: x += (pick(601) - 300) << 16; z += (pick(601) - 300) << 16; break;
            }
            const auto* brute = t.bruteMatch(player, cls, x, z, shared, now);
            const uint32_t want = brute ? brute->id : 0;
            const size_t before = t.size();
            const auto& got = t.join(player, cls, x, z, shared, now);
            ++joins;
            if (want) {
                ++joined;
                if (got.id != want) {
                    check(false, "stream " + std::to_string(stream) + " order " + std::to_string(i) +
                                 ": index joined " + std::to_string(got.id) + ", scan " + std::to_string(want));
                    return;
                }
            } else {
                ++opened;
                if (t.size() != before + 1) {
                    check(false, "stream " + std::to_string(stream) + " order " + std::to_string(i) +
                                 ": index joined, scan opens");
                    return;
                }
            }
            if (!t.indexesMatch(now)) {
                check(false, "stream " + std::to_string(stream) + ": indexes differ from a rebuild");
                return;
            }
        }
    }
    std::printf("index vs scan: %llu orders in 1e4 streams, %llu joined, %llu opened: identical\n",
                (unsigned long long)joins, (unsigned long long)joined, (unsigned long long)opened);
    check(joined > joins / 4 && opened > joins / 20, "the streams exercise both joining and opening");
}

// ---- 2. cost ---------------------------------------------------------------
void cost() {
    // crowdbench calls World::order once per unit with its own goal: 2000
    // shared points two cells apart, all in one tick.
    ConvoyTable t;
    uint64_t linearMax = 0, linearOver16 = 0;
    auto order = [&](int32_t x, int32_t z, bool shared) {
        // A linear scan tests every open convoy of the player and class.
        uint64_t linear = 0;
        t.forEach([&](const ConvoyTable::Convoy& c) { linear += c.player == 0 && c.cls == ConvoyClass::Move; });
        linearMax = std::max(linearMax, linear);
        linearOver16 += linear > 16;
        t.join(0, ConvoyClass::Move, x, z, shared, 100);
    };
    for (int i = 0; i < 2000; ++i) order((100 + i % 50 * 2) * 16 << 16, (100 + i / 50 * 2) * 16 << 16, true);
    const auto s = t.stats();
    std::printf("cost: %llu orders, %llu tests, max %llu, over 16: %llu, over 64: %llu; linear scan max %llu, over 16: %llu\n",
                (unsigned long long)s.orders, (unsigned long long)s.tests, (unsigned long long)s.testsMax,
                (unsigned long long)s.over16, (unsigned long long)s.over64, (unsigned long long)linearMax,
                (unsigned long long)linearOver16);
    check(s.testsMax <= 64 && s.over16 * 100 <= s.orders, "convoy_tests within p99 <= 16 and max <= 64");
    check(linearMax > 64 && linearOver16 * 100 > s.orders, "a linear scan trips the bound (the gate can fail)");
    // The worst case the bound tolerates: offset (flyer) points in the middle
    // of that crowd test every anchor within 5 cells. Over 16, under 64.
    for (int i = 0; i < 200; ++i) order((110 + i % 20 * 4) * 16 << 16, (110 + i / 20 * 4) * 16 << 16, false);
    const auto& f = t.stats();
    std::printf("cost with 200 offset points inside the crowd: max %llu, over 16: %llu of %llu\n",
                (unsigned long long)f.testsMax, (unsigned long long)(f.over16 - s.over16),
                (unsigned long long)(f.orders - s.orders));
    check(f.testsMax <= 64 && f.over64 == 0, "offset points inside a crowd of goals: max <= 64");
}

// ---- 3. selections ---------------------------------------------------------
UnitType ground(int foot) {
    UnitType t{};
    t.id = t.name = "cv-foot-" + std::to_string(foot);
    t.canMove = true; t.maxHp = 100; t.footX = t.footZ = foot; t.sight = 4096;
    t.maxVel = Fixed::raw(117964); t.accel = t.brake = Fixed::fromInt(10);
    t.turnRate = t.turnInPlaceRate = 2500; t.halfCellTicks = 3; t.buildTime = 1;
    return t;
}
UnitType flyer() {
    UnitType t{};
    t.id = t.name = "cv-flyer";
    t.canFly = t.canMove = true; t.maxHp = 100; t.footX = t.footZ = 2; t.sight = 4096;
    t.maxVel = Fixed::fromInt(4); t.accel = t.brake = Fixed::fromInt(1); t.turnRate = 1200; t.cruiseAlt = 80;
    t.buildTime = 1; t.vtolStandby = true;
    return t;
}
const UnitType kGround = ground(2), kSmall = ground(1), kFlyer = flyer();

struct Field {
    World world;
    TypeRegistry reg;
    Field() {
        world.setGameSeed(7); world.setSerialThreads(true); world.setPathService(true);
        world.setPathfindingMode(PathfindingMode::Legion);
        world.setPlayerCount(2); world.setTeam(0, 0); world.setTeam(1, 1);
        world.setTerrain(std::vector<uint8_t>(size_t(512) * 512, 100), 512, 512, 64);
        world.tick(1.f / 30);   // tick 1: the selection is clicked on a running game
    }
    std::vector<int> block(const UnitType& t, int n, int x, int z, int perRow, int pitch) {
        std::vector<int> ids;
        for (int i = 0; i < n; ++i) {
            const int id = world.spawn(&t, float((x + (i % perRow) * pitch) * 16), float((z + (i / perRow) * pitch) * 16),
                                       std::nullopt, 0);
            if (id <= 0) throw std::runtime_error("spawn failed");
            ids.push_back(id);
        }
        return ids;
    }
    // Land a click's batches tick by tick, as the sim applies them.
    void land(const std::vector<CmdBatch>& batches) {
        for (const auto& b : batches) {
            while (world.tickCount() < b.tick) world.tick(1.f / 30);
            for (const auto& c : b.cmds) applyCommand(world, reg, c);
        }
    }
    // perUnit: the per-unit command stream (hudCommands through the uplink),
    // else the client's explicit order (issueSelection, serials from 1 up).
    bool perUnit = false;
    uint32_t serial = 1;
    std::vector<CmdBatch> click(const std::vector<int>& sel, Verb verb, float x, float z, int roundTrip = -1) {
        SelectionOrder o;
        o.verb = verb; o.x = x; o.z = z;
        std::vector<CmdBatch> b;
        if (perUnit) {
            tak::scn::Uplink link(roundTrip);
            link.issue(tak::scn::hudCommands(world, sel, o));
            for (uint32_t t = world.tickCount(); !link.idle(); ++t) b.push_back({t, link.land(t)});
        } else {
            b = tak::scn::issueSelection(world, sel, o, world.tickCount(), 0, roundTrip, serial);
            for (const auto& x : b) for (const auto& c : x.cmds) if (c.order) serial = c.order->serial + 1;
        }
        land(b);
        given.clear();
        for (const auto& x : b)
            for (const auto& c : x.cmds)
                if (!c.order) given[c.unitId] = {c.x, c.z};
                else for (const int id : c.order->members) {
                    const auto& o = world.unit(id)->orders.back();
                    given[id] = {o.x.toFloat(), o.z.toFloat()};
                }
        return b;
    }
    // The point each unit was given: its command's (per unit), else its last
    // order's as the sim gave it in the activation tick (OrderShape::Offsets;
    // read before any tick, a flyer's mission snaps it later).
    std::map<int, std::pair<float, float>> given;
    std::pair<float, float> goalOf(int id) const { return given.at(id); }
    // Every stamped convoyTick on the unit's orders (one per leg).
    std::set<uint32_t> ticksOf(int id) const {
        std::set<uint32_t> out;
        for (const auto& o : world.unit(id)->orders)
            if (o.convoyTick != ConvoyTable::kNone) out.insert(o.convoyTick);
        return out;
    }
    // The selection's convoys: convoyTick -> units; a unit with no stamp or
    // two stamps is reported as kNone.
    std::map<uint32_t, int> convoysOf(const std::vector<int>& sel) const {
        std::map<uint32_t, int> out;
        for (int id : sel) {
            const auto t = ticksOf(id);
            ++out[t.size() == 1 ? *t.begin() : ConvoyTable::kNone];
        }
        return out;
    }
    size_t open(ConvoyClass cls) const {
        size_t n = 0;
        world.convoys().forEach([&](const ConvoyTable::Convoy& c) { n += c.player == 0 && c.cls == cls; });
        return n;
    }
};

std::vector<int> join(std::vector<int> a, const std::vector<int>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}
int ticksSpanned(const std::vector<CmdBatch>& b) {
    int n = 0;
    for (const auto& x : b) n += !x.cmds.empty();
    return n;
}
bool oneConvoy(const std::map<uint32_t, int>& c, size_t n, const std::string& what) {
    const bool ok = c.size() == 1 && c.begin()->first != ConvoyTable::kNone && size_t(c.begin()->second) == n;
    if (!ok) {
        std::fprintf(stderr, "  %s:", what.c_str());
        for (const auto& [t, k] : c) std::fprintf(stderr, " %d@%d", k, t == ConvoyTable::kNone ? -1 : int(t));
        std::fprintf(stderr, "\n");
    }
    return ok;
}

void selections(bool perUnit) {
    const float kX = 300 * 16, kZ = 300 * 16;
    const std::string how = perUnit ? "per-unit " : "explicit ";
    auto check = [&](bool ok, const std::string& what) { ::check(ok, how + what); };
    // An explicit order's area key is kExplicitArea | its serial; per-unit commands key by tick.
    auto keyed = [&](const std::map<uint32_t, int>& c) {
        return c.size() == 1 && explicitArea(c.begin()->first) == !perUnit;
    };
    // Flyers listed first, far from the centre on both axes (past retail's
    // flyer reach, sqrt(3000 n) = 949 px): every flyer's point is the click.
    {
        Field f;
        f.perUnit = perUnit;
        const auto fly = f.block(kFlyer, 24, 200, 200, 6, 3);
        const auto sel = join(fly, f.block(kGround, 276, 20, 20, 23, 3));
        const auto b = f.click(sel, Verb::Move, kX, kZ);
        bool saturated = true;
        for (int id : fly) saturated &= f.goalOf(id) == std::pair<float, float>{kX, kZ};
        check(saturated && ticksSpanned(b) == (perUnit ? 5 : 1), "flyers first: out of reach at the click, over 5 ticks (explicit: 1)");
        check(oneConvoy(f.convoysOf(sel), sel.size(), "flyers first") && keyed(f.convoysOf(sel)),
              "flyers first (saturated): one convoy");
    }
    // Half the flyers far up and left, half far down and right, listed first:
    // out of retail's reach, all at the click (they used to take +60/+60 and -60/-60).
    {
        Field f;
        f.perUnit = perUnit;
        const auto a = f.block(kFlyer, 12, 200, 200, 4, 3);
        const auto c = f.block(kFlyer, 12, 4, 4, 4, 3);
        const auto sel = join(join(a, c), f.block(kGround, 276, 80, 80, 23, 3));
        f.click(sel, Verb::Move, kX, kZ);
        std::set<std::pair<float, float>> offsets;
        for (int id : join(a, c)) offsets.insert({f.goalOf(id).first - kX, f.goalOf(id).second - kZ});
        check(offsets == std::set<std::pair<float, float>>{{0.f, 0.f}},
              "opposite corners: flyers out of reach at the click");
        check(oneConvoy(f.convoysOf(sel), sel.size(), "opposite corners"), "opposite corners: one convoy");
    }
    // An all-air selection over 64 (no anchor ever per unit; an explicit order anchors on its click).
    {
        Field f;
        f.perUnit = perUnit;
        const auto sel = join(f.block(kFlyer, 50, 20, 20, 10, 3), f.block(kFlyer, 50, 120, 120, 10, 3));
        const auto b = f.click(sel, Verb::Move, kX, kZ);
        check(ticksSpanned(b) == (perUnit ? 2 : 1), "all-air: over 2 ticks (explicit: 1)");
        check(oneConvoy(f.convoysOf(sel), sel.size(), "all-air"), "all-air 100 flyers: one convoy");
    }
    // 974 units through the 512-command window (an explicit order: one tick at any round trip).
    for (int rt : {-1, 0, 8, 16, 24}) {
        Field f;
        f.perUnit = perUnit;
        const auto sel = f.block(kSmall, 974, 10, 10, 60, 2);
        const auto b = f.click(sel, Verb::Move, 400 * 16, 400 * 16, rt);
        const auto c = f.convoysOf(sel);
        const std::string what = "974 round trip " + std::to_string(rt);
        if (!perUnit) {
            check(oneConvoy(c, sel.size(), what) && ticksSpanned(b) == 1, what + ": one convoy, one tick");
            continue;
        }
        if (rt < 24) {
            check(oneConvoy(c, sel.size(), what), what + ": one convoy");
            continue;
        }
        // It splits; no 64-unit part (one tick's batch) is split across convoys.
        bool partsWhole = true;
        for (const auto& x : b) {
            std::set<std::set<uint32_t>> parts;
            for (const auto& cmd : x.cmds) parts.insert(f.ticksOf(cmd.unitId));
            partsWhole &= parts.size() <= 1;
        }
        check(c.size() == 2 && !c.count(ConvoyTable::kNone) && c.begin()->second == 512, what + ": splits at the window gap (512 + 462)");
        check(partsWhole, what + ": every 64-unit part is in one convoy");
    }
    // Patrol: 100 ground and 24 flyers (flyers first): one convoy, flyer
    // outbound legs included, every return leg carries the outbound tick.
    {
        Field f;
        f.perUnit = perUnit;
        const auto sel = join(f.block(kFlyer, 24, 200, 200, 6, 3), f.block(kGround, 100, 20, 20, 10, 3));
        f.click(sel, Verb::Patrol, kX, kZ);
        check(oneConvoy(f.convoysOf(sel), sel.size(), "patrol"), "patrol: one convoy");
        bool legs = true;
        for (int id : sel) {
            int patrolLegs = 0, stamped = 0;
            for (const auto& o : f.world.unit(id)->orders)
                if (o.patrol && o.goal) {
                    ++patrolLegs;
                    stamped += o.convoyTick != ConvoyTable::kNone && (perUnit ? o.orderSerial == 0 : o.orderSerial != 0);
                }
            // A flyer's patrol mission appends its own return anchor when it
            // starts: three legs, all stamped.
            if (legs && !(patrolLegs >= 2 && stamped == patrolLegs)) {
                std::fprintf(stderr, "  unit %d (%s):", id, f.world.unit(id)->type->canFly ? "flyer" : "ground");
                for (const auto& o : f.world.unit(id)->orders)
                    std::fprintf(stderr, " [%.0f,%.0f goal=%d patrol=%d convoy=%d]", o.x.toFloat(), o.z.toFloat(),
                                 int(o.goal), int(o.patrol), o.convoyTick == ConvoyTable::kNone ? -1 : int(o.convoyTick));
                std::fprintf(stderr, "\n");
            }
            legs &= patrolLegs >= 2 && stamped == patrolLegs;
        }
        check(legs, "patrol: outbound and return legs both carry the convoy");
        check(f.open(ConvoyClass::Patrol) == 1, "patrol: one open Patrol convoy (return legs open none)");
        // A fight to the same point a tick later is a convoy of its own class.
        const auto fight = f.block(kGround, 40, 60, 60, 10, 3);
        f.world.tick(1.f / 30);
        f.click(fight, Verb::Fight, kX, kZ);
        check(oneConvoy(f.convoysOf(fight), fight.size(), "fight"), "fight: one convoy");
        check(f.open(ConvoyClass::Fight) == 1 && f.open(ConvoyClass::Patrol) == 1,
              "fight beside patrol: separate convoys per class");
    }
    // Two clicks 48 px apart, 4 ticks apart: two convoys for the ground units.
    {
        Field f;
        const auto a = f.block(kGround, 100, 20, 20, 10, 3);
        const auto c = f.block(kGround, 100, 60, 20, 10, 3);
        tak::scn::Uplink link;
        SelectionOrder oa, oc;
        oa.verb = oc.verb = Verb::Move;
        oa.x = kX; oa.z = kZ; oc.x = kX + 48; oc.z = kZ;
        const uint32_t t0 = f.world.tickCount();
        uint32_t serial = 1;
        auto items = [&](const std::vector<int>& sel, const SelectionOrder& o) {
            return perUnit ? tak::scn::hudCommands(f.world, sel, o) : tak::scn::hudItems(f.world, sel, o, 0, serial);
        };
        link.issue(items(a, oa));
        for (uint32_t t = t0; !link.idle() || t <= t0 + 4; ++t) {
            if (t == t0 + 4) link.issue(items(c, oc));
            while (f.world.tickCount() < t) f.world.tick(1.f / 30);
            for (const auto& cmd : link.land(t)) applyCommand(f.world, f.reg, cmd);
        }
        const auto ca = f.convoysOf(a), cc = f.convoysOf(c);
        check(oneConvoy(ca, a.size(), "click A") && oneConvoy(cc, c.size(), "click B"), "two clicks: each one convoy");
        check(ca.begin()->first != cc.begin()->first && f.open(ConvoyClass::Move) == 2, "two clicks 48 px apart: two convoys");
    }
    // A convoy closes: 41 ticks after the last click the table is empty.
    {
        Field f;
        f.perUnit = perUnit;
        const auto sel = f.block(kGround, 10, 20, 20, 10, 3);
        f.click(sel, Verb::Move, kX, kZ);
        check(!f.world.convoys().empty(), "an open convoy after the click");
        for (int i = 0; i < 10; ++i) f.world.tick(1.f / 30);
        check(f.world.convoys().empty(), "the table is empty once the convoy closes");
    }
    // Retail stamps nothing and keeps no table.
    {
        World w;
        w.setGameSeed(7); w.setSerialThreads(true); w.setPathService(true);
        w.setPlayerCount(2);
        w.setTerrain(std::vector<uint8_t>(size_t(128) * 128, 100), 128, 128, 64);
        const int id = w.spawn(&kGround, 320, 320, std::nullopt, 0);
        const int fl = w.spawn(&kFlyer, 400, 320, std::nullopt, 0);
        w.order(id, 900, 900, false); w.attackMove(fl, 900, 900, false); w.patrol(id, 600, 600);
        bool none = w.convoys().empty();
        for (int u : {id, fl})
            for (const auto& o : w.unit(u)->orders) none &= o.convoyTick == ConvoyTable::kNone;
        check(none, "Retail: no convoys, no stamps");
    }
}

// Several explicit orders at one point within the window share one area (decision
// A1: wall-4x50's selections on ticks 1-4), each keeping its own serial; the area
// takes the first order's key.
void sharedArea() {
    Field f;
    std::vector<std::vector<int>> sels;
    for (int k = 0; k < 4; ++k) sels.push_back(f.block(kGround, 50, 20 + 40 * k, 20, 10, 3));
    std::set<uint32_t> areas, serials;
    for (int k = 0; k < 4; ++k) {
        f.click(sels[size_t(k)], Verb::Move, 300 * 16, 300 * 16);
        f.world.tick(1.f / 30);
    }
    for (const auto& s : sels)
        for (int id : s) {
            areas.insert(f.world.unit(id)->orders.back().convoyTick);
            serials.insert(f.world.unit(id)->orders.back().orderSerial);
        }
    check(areas.size() == 1 && *areas.begin() == (kExplicitArea | 1u) && serials == std::set<uint32_t>{1, 2, 3, 4},
          "four explicit orders at one point, ticks apart: one area keyed by the first, four serials");
}
}  // namespace

int main() {
    LegionNavigator::setVerify(true);
    try {
        indexVsScan();
        cost();
        selections(false);
        selections(true);
        sharedArea();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("convoy_test: all passed\n");
    return 0;
}
