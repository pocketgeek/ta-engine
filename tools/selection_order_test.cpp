// selection_order_test -- explicit selection orders (Cmd::Order) from
// the wire to the sim (Legion 10,000 stage 3).
//
//  1. Codec: varints and member deltas round-trip and refuse overlong, zero or
//     out-of-range encodings; orderRecordBytes is the record Writer::cmd writes; a
//     bundle mixing commands and orders round-trips, a strict read validates every
//     order whole.
//  2. packClick: a verb run becomes one order (members sorted, serials consecutive);
//     runs split on another verb or target, a repeated unit, kOrderMaxMembers;
//     other verbs pass through; the right-click move is OrderShape::Offsets.
//  3. The sim: in Retail an order is exactly its per-unit commands in increasing id
//     order (equal state hash, no stamps); in Legion every member's leg carries the
//     order's serial and its one area (kExplicitArea | serial), flyers get the
//     client's offsets, a dead or another player's member is skipped, several
//     orders in one tick keep their order (a Stop behind a Move wins, a queued leg
//     follows), a patrol's return legs keep the identity, and the serial is hashed.
//     Load and GiveUnit stay per-unit (their outcome depends on the member order,
//     which an order does not keep); for the order verbs that order changes nothing.
//  4. Delivery: the mirror's uplink lands a 10,000-unit click whole in its tick at
//     any round trip, and an ordinary command behind it is not held for its size.
#include "legion_issue_selection.h"
#include "net/orderpack.h"
#include "sim/matchsetup.h"

#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace tak::sim;
using tak::net::Cmd;
using tak::net::Command;
using tak::net::OrderShape;
using tak::net::SelectionOrder;

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", what.c_str()); ++failures; }
}

Command orderCmd(uint8_t player, uint32_t serial, const Command& tmpl, std::vector<int32_t> members,
                 OrderShape shape = OrderShape::Shared) {
    auto o = std::make_shared<SelectionOrder>();
    o->serial = serial;
    o->tmpl = tmpl;
    o->shape = shape;
    o->members = std::move(members);
    Command c;
    c.kind = Cmd::Order;
    c.player = player;
    c.order = std::move(o);
    return c;
}
Command verb(Cmd k, float x = 0, float z = 0, int target = 0, uint8_t queue = 0) {
    Command c;
    c.kind = k; c.x = x; c.z = z; c.targetId = target; c.queue = queue;
    return c;
}

// ---- 1. codec ---------------------------------------------------------------
void codec() {
    using namespace tak::net;
    for (uint32_t v : {0u, 1u, 127u, 128u, 16383u, 16384u, 0x7fffffffu, 0xffffffffu}) {
        Writer w; w.varint(v);
        Reader r(w.b.data(), w.b.size());
        check(r.varint() == v && r.ok && r.p == r.end, "varint round trip " + std::to_string(v));
    }
    {
        const uint8_t overlong[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x01};
        Reader r(overlong, sizeof overlong);
        r.varint();
        check(!r.ok, "a 6-byte varint is refused");
        const uint8_t big[] = {0xff, 0xff, 0xff, 0xff, 0x1f};
        Reader b(big, sizeof big);
        b.varint();
        check(!b.ok, "a varint past 32 bits is refused");
        const uint8_t cut[] = {0x80};
        Reader c(cut, sizeof cut);
        c.varint();
        check(!c.ok, "a truncated varint is refused");
    }
    std::mt19937 rng(5);
    for (int trial = 0; trial < 200; ++trial) {
        std::vector<int32_t> ids;
        int32_t id = int32_t(rng() % 50);
        const int n = 1 + int(rng() % 3000);
        for (int i = 0; i < n && id < INT32_MAX / 2; ++i) { ids.push_back(id); id += 1 + int32_t(rng() % (trial % 7 == 0 ? 400000 : 9)); }
        auto c = orderCmd(uint8_t(rng() % 8), 1 + rng() % kOrderMaxSerial, verb(Cmd::AttackMove, 100.5f, -20.25f), ids,
                          OrderShape::Shared);
        Writer w; w.cmd(c);
        check(w.b.size() == orderRecordBytes(*c.order), "orderRecordBytes is the written record");
        Reader r(w.b.data(), w.b.size());
        const Command d = r.cmd();
        check(r.ok && r.p == r.end && d.kind == Cmd::Order && d.order && d.player == c.player &&
                  d.order->serial == c.order->serial && d.order->members == ids && d.order->tmpl.kind == Cmd::AttackMove &&
                  d.order->tmpl.x == 100.5f && d.order->tmpl.z == -20.25f && d.order->shape == OrderShape::Shared,
              "an order record round-trips");
        if (trial == 0) {
            std::vector<int32_t> out;
            int32_t prev = -1;
            Writer m; putMembers(m, ids.data(), ids.size(), prev);
            Reader mr(m.b.data(), m.b.size());
            const uint32_t count = mr.u32();
            int32_t back = -1;
            check(getMembers(mr, count, back, out) && out == ids && back == prev, "member deltas round-trip");
        }
    }
    // A bundle mixing commands and orders, strict and live.
    {
        std::vector<Command> cmds;
        Command a = verb(Cmd::Move, 10, 20); a.player = 1; a.unitId = 7; cmds.push_back(a);
        cmds.push_back(orderCmd(1, 3, verb(Cmd::Move, 300, 400), {2, 5, 9}, OrderShape::Offsets));
        Command area; area.kind = Cmd::ReclaimArea; area.player = 2; area.unitId = 4; area.x2 = 5; area.z2 = 6; cmds.push_back(area);
        cmds.push_back(orderCmd(2, 1, verb(Cmd::Stop), {4}));
        Writer w; putBundle(w, 77, cmds, {});
        for (bool strict : {false, true}) {
            Reader r(w.b.data(), w.b.size());
            uint32_t tick = 0;
            Bundle b;
            check(getBundle(r, tick, b, strict) && tick == 77 && b.cmds.size() == 4 && b.cmds[1].order &&
                      b.cmds[1].order->members == std::vector<int32_t>{2, 5, 9} &&
                      b.cmds[1].order->shape == OrderShape::Offsets && b.cmds[2].x2 == 5 && b.cmds[3].order &&
                      b.cmds[3].order->tmpl.kind == Cmd::Stop,
                  std::string("a mixed bundle round-trips (") + (strict ? "strict" : "live") + ")");
        }
    }
    // Strict reads validate the order whole.
    auto strictOk = [](const Command& c) {
        Writer w; putBundle(w, 1, {c}, {});
        Reader r(w.b.data(), w.b.size());
        uint32_t tick = 0;
        Bundle b;
        return getBundle(r, tick, b, true);
    };
    check(strictOk(orderCmd(0, 1, verb(Cmd::Guard, 0, 0, 5), {1, 2})), "a valid order passes a strict read");
    check(!strictOk(orderCmd(0, 0, verb(Cmd::Move), {1})), "serial 0 is refused");
    check(!strictOk(orderCmd(0, kOrderMaxSerial + 1, verb(Cmd::Move), {1})), "a serial past kOrderMaxSerial is refused");
    check(!strictOk(orderCmd(0, 1, verb(Cmd::Train), {1})), "a non-order verb is refused");
    check(!strictOk(orderCmd(0, 1, verb(Cmd::Attack, 0, 0, 3), {1}, OrderShape::Offsets)), "Offsets on Attack is refused");
    check(!strictOk(orderCmd(0, 1, verb(Cmd::Move), {})), "an empty order is refused");
    check(!strictOk(orderCmd(0, 1, verb(Cmd::Move, std::numeric_limits<float>::quiet_NaN()), {1})),
          "a non-finite template is refused");
    {
        Writer w; w.cmd(orderCmd(0, 1, verb(Cmd::Move), {1, 2, 3}));
        w.b.pop_back();
        Reader r(w.b.data(), w.b.size());
        r.cmd();
        check(!r.ok, "a truncated order record is refused");
    }
    check(!validOrder([] {
              SelectionOrder o; o.serial = 1; o.tmpl = verb(Cmd::Move); o.members = {3, 2};
              return o;
          }()),
          "unsorted members are invalid");
}

// ---- 2. packClick -------------------------------------------------------------
void packing() {
    using tak::net::packClick;
    auto unit = [](Command c, int id) { c.unitId = id; return c; };
    std::vector<Command> cmds;
    for (int id : {9, 3, 7}) cmds.push_back(unit(verb(Cmd::Move, 50 + float(id), 60), id));   // offsets per unit
    uint32_t serial = 5;
    tak::net::ClickShape shape;
    shape.moveOffsets = true; shape.x = 50; shape.z = 60;
    auto out = packClick(cmds, shape, serial);
    check(out.size() == 1 && out[0].order && out[0].order->members == std::vector<int32_t>{3, 7, 9} &&
              out[0].order->shape == OrderShape::Offsets && out[0].order->tmpl.x == 50 && out[0].order->serial == 5 &&
              serial == 6 && out[0].order->tmpl.unitId == 0,
          "the right-click move is one Offsets order at the click, members sorted");
    // Without the move shape, differing points split.
    serial = 1;
    out = packClick(cmds, {}, serial);
    check(out.size() == 3 && serial == 4, "differing points without the move shape are separate orders");
    // Runs: evict (target 0) then assign (target -3); a repeated unit; a pass-through verb.
    cmds.clear();
    for (int id : {1, 2}) cmds.push_back(unit(verb(Cmd::SetSquad, 0, 0, 0), id));
    for (int id : {5, 6}) cmds.push_back(unit(verb(Cmd::SetSquad, 0, 0, -3), id));
    cmds.push_back(unit(verb(Cmd::SetSquad, 0, 0, -3), 5));   // again: a new run
    Command train; train.kind = Cmd::Train; train.unitId = 40; cmds.push_back(train);
    serial = 1;
    out = packClick(cmds, {}, serial);
    check(out.size() == 4 && out[0].order && out[0].order->tmpl.targetId == 0 && out[1].order &&
              out[1].order->members == std::vector<int32_t>{5, 6} && out[2].order && out[2].order->members == std::vector<int32_t>{5} &&
              out[3].kind == Cmd::Train && !out[3].order,
          "runs split on target and on a repeated unit; other verbs pass through");
    // A mixed click (GameView::issueClickPlan, one run per order kind): an enemy
    // right-clicked by soldiers, builders and a scout -- Attack, then the Move
    // fallback -- is two orders, the Move one Offsets order at the click.
    cmds.clear();
    for (int id : {12, 4}) cmds.push_back(unit(verb(Cmd::Attack, 0, 0, 77), id));
    for (int id : {8, 5}) cmds.push_back(unit(verb(Cmd::Move, 300 + float(id), 400), id));
    serial = 1;
    tak::net::ClickShape click;
    click.moveOffsets = true; click.x = 300; click.z = 400;
    out = packClick(cmds, click, serial);
    check(out.size() == 2 && out[0].order && out[0].order->tmpl.kind == Cmd::Attack &&
              out[0].order->tmpl.targetId == 77 && out[0].order->members == std::vector<int32_t>{4, 12} &&
              out[0].order->shape == OrderShape::Shared && out[1].order && out[1].order->tmpl.kind == Cmd::Move &&
              out[1].order->members == std::vector<int32_t>{5, 8} && out[1].order->shape == OrderShape::Offsets &&
              out[1].order->tmpl.x == 300,
          "a mixed click: Attack and the Move fallback are one order each");
    // A right-drag box (GameView::issueBoxPlan): one Attack per box enemy per
    // attacker, nearest first (the first replaces, the rest queue), then the
    // clearers' ReclaimArea. Each enemy's run packs; ReclaimArea is no order verb.
    cmds.clear();
    for (int id : {3, 6}) cmds.push_back(unit(verb(Cmd::Attack, 0, 0, 90, 0), id));
    for (int id : {3, 6}) cmds.push_back(unit(verb(Cmd::Attack, 0, 0, 91, 1), id));
    for (int id : {10, 11}) {
        Command c = unit(verb(Cmd::ReclaimArea, 100, 200), id);
        c.x2 = 300; c.z2 = 400;
        cmds.push_back(c);
    }
    serial = 1;
    out = packClick(cmds, {}, serial);
    check(out.size() == 4 && out[0].order && out[0].order->tmpl.targetId == 90 && !out[0].order->tmpl.queue &&
              out[1].order && out[1].order->tmpl.targetId == 91 && out[1].order->tmpl.queue == 1 &&
              out[1].order->members == std::vector<int32_t>{3, 6} && !out[2].order && out[2].kind == Cmd::ReclaimArea &&
              out[2].unitId == 10 && !out[3].order && out[3].unitId == 11,
          "a right-drag box: one order per enemy, queued after the first; the clears go per unit");
    // kOrderMaxMembers.
    cmds.clear();
    for (int id = 0; id < int(tak::net::kOrderMaxMembers) + 10; ++id) cmds.push_back(unit(verb(Cmd::Stop), id));
    serial = 1;
    out = packClick(cmds, {}, serial);
    check(out.size() == 2 && out[0].order->members.size() == tak::net::kOrderMaxMembers && out[1].order->members.size() == 10,
          "a run longer than kOrderMaxMembers is two orders");
}

// ---- 3. the sim -----------------------------------------------------------------
UnitType ground(int foot) {
    UnitType t{};
    t.id = t.name = "so-foot-" + std::to_string(foot);
    t.canMove = true; t.maxHp = 100; t.footX = t.footZ = foot; t.sight = 4096;
    t.maxVel = Fixed::raw(117964); t.accel = t.brake = Fixed::fromInt(10);
    t.turnRate = t.turnInPlaceRate = 2500; t.halfCellTicks = 3; t.buildTime = 1;
    return t;
}
UnitType flyer() {
    UnitType t{};
    t.id = t.name = "so-flyer";
    t.canFly = t.canMove = true; t.maxHp = 100; t.footX = t.footZ = 2; t.sight = 4096;
    t.maxVel = Fixed::fromInt(4); t.accel = t.brake = Fixed::fromInt(1); t.turnRate = 1200; t.cruiseAlt = 80;
    t.buildTime = 1; t.vtolStandby = true;
    return t;
}
const UnitType kGround = ground(2), kFlyer = flyer();

struct Field {
    World world;
    TypeRegistry reg;
    explicit Field(PathfindingMode mode) {
        world.setGameSeed(7); world.setSerialThreads(true); world.setPathService(true);
        world.setPathfindingMode(mode);
        world.setPlayerCount(2); world.setTeam(0, 0); world.setTeam(1, 1);
        world.setTerrain(std::vector<uint8_t>(size_t(256) * 256, 100), 256, 256, 64);
        world.tick(1.f / 30);
    }
    std::vector<int> block(const UnitType& t, int n, int x, int z, int perRow, int pitch, int player = 0) {
        std::vector<int> ids;
        for (int i = 0; i < n; ++i) {
            const int id = world.spawn(&t, float((x + (i % perRow) * pitch) * 16), float((z + (i / perRow) * pitch) * 16),
                                       std::nullopt, player);
            if (id <= 0) throw std::runtime_error("spawn failed");
            ids.push_back(id);
        }
        return ids;
    }
    void apply(const Command& c) { applyCommand(world, reg, c); }
    void run(int ticks) { for (int i = 0; i < ticks; ++i) world.tick(1.f / 30); }
};

std::vector<int32_t> sorted(std::vector<int> v) {
    std::sort(v.begin(), v.end());
    return {v.begin(), v.end()};
}

void retailEquivalence() {
    // An order applies its members' per-unit commands in increasing id order: Retail
    // stamps nothing, so the two worlds stay bit-identical.
    for (Cmd k : {Cmd::AttackMove, Cmd::Patrol, Cmd::Move, Cmd::Stop}) {
        Field a(PathfindingMode::Retail), b(PathfindingMode::Retail);
        const auto ids = a.block(kGround, 60, 10, 10, 10, 3);
        b.block(kGround, 60, 10, 10, 10, 3);
        const Command t = verb(k, 150 * 16, 120 * 16);
        for (int id : sorted(ids)) { Command c = t; c.unitId = id; a.apply(c); }
        b.apply(orderCmd(0, 1, t, sorted(ids)));
        a.run(120); b.run(120);
        check(a.world.stateHash() == b.world.stateHash(), "Retail: an order equals its per-unit commands (verb " +
                                                              std::to_string(int(k)) + ")");
        bool none = true;
        for (int id : ids) for (const auto& o : b.world.unit(id)->orders) none &= o.orderSerial == 0 && o.convoyTick == ConvoyTable::kNone;
        check(none && b.world.convoys().empty(), "Retail: no serials, no areas");
    }
}

void legionIdentity() {
    Field f(PathfindingMode::Legion);
    const auto foot = f.block(kGround, 200, 10, 10, 20, 3);
    const auto air = f.block(kFlyer, 8, 70, 30, 4, 3);   // 500-670 px from the centre: in reach, past 60
    std::vector<int> sel = foot;
    sel.insert(sel.end(), air.begin(), air.end());
    const float cx = 180 * 16, cz = 160 * 16;
    // Retail's flyer spread for the flyers' goals (gameview_hud.cpp right-click move,
    // sim/retailflyerspread.h), by hand: the whole-pixel centre (truncated mean), the
    // offset kept when floor(dx^2) + floor(dz^2) <= 3000 n, else the click.
    long long sx = 0, sz = 0;
    for (int id : sel) { sx += f.world.unit(id)->x.v >> 16; sz += f.world.unit(id)->z.v >> 16; }
    const double mx = double(sx / (long long)sel.size()), mz = double(sz / (long long)sel.size());
    const double reach2 = 3000.0 * double(sel.size());
    f.apply(orderCmd(0, 42, verb(Cmd::Move, cx, cz), sorted(sel), OrderShape::Offsets));
    bool stamped = true, shared = true, offsets = true, wide = false;
    for (int id : sel) {
        const auto* u = f.world.unit(id);
        if (u->orders.empty()) { stamped = false; continue; }
        const auto& o = u->orders.back();
        stamped &= o.orderSerial == 42 && o.convoyTick == (kExplicitArea | 42u);
        if (u->type->canFly) {
            const double dx = u->x.v / 65536.0 - mx, dz = u->z.v / 65536.0 - mz;
            const bool keep = std::floor(dx * dx) + std::floor(dz * dz) <= reach2;
            const double gx = keep ? cx + dx : cx, gz = keep ? cz + dz : cz;
            offsets &= std::abs(o.x.toFloat() - gx) < 0.01 && std::abs(o.z.toFloat() - gz) < 0.01;
            wide |= keep && std::abs(dx) > 60;
        } else shared &= o.x == Fixed::fromFloat(cx) && o.z == Fixed::fromFloat(cz);
    }
    check(stamped, "Legion: every member's leg carries the order's serial and area");
    check(shared, "Legion: surface members take the click");
    check(offsets && wide, "Legion: flyers take retail's flyer spread, as the client gives it");
    size_t open = 0;
    f.world.convoys().forEach([&](const ConvoyTable::Convoy& c) { open += c.player == 0; });
    check(open == 1, "Legion: the order joined the area table once");
    // The serial is hashed: the same order under another serial is another state.
    Field g(PathfindingMode::Legion);
    g.block(kGround, 200, 10, 10, 20, 3);
    g.block(kFlyer, 8, 70, 30, 4, 3);
    g.apply(orderCmd(0, 43, verb(Cmd::Move, cx, cz), sorted(sel), OrderShape::Offsets));
    check(f.world.stateHash() != g.world.stateHash(), "Legion: the serial is part of the state hash");
    Field h(PathfindingMode::Legion);
    h.block(kGround, 200, 10, 10, 20, 3);
    h.block(kFlyer, 8, 70, 30, 4, 3);
    h.apply(orderCmd(0, 42, verb(Cmd::Move, cx, cz), sorted(sel), OrderShape::Offsets));
    f.run(60); h.run(60);
    check(f.world.stateHash() == h.world.stateHash(), "Legion: the same order gives the same state");
}

void membership() {
    Field f(PathfindingMode::Legion);
    const auto mine = f.block(kGround, 30, 10, 10, 10, 3);
    const auto theirs = f.block(kGround, 5, 60, 60, 5, 3, 1);
    // A member killed before activation, and one of another player (a changed owner).
    f.world.unit(mine[3])->hp = Fixed();
    f.run(2);
    std::vector<int> sel = mine;
    sel.push_back(theirs[0]);
    sel.push_back(99999);   // never existed
    f.apply(orderCmd(0, 1, verb(Cmd::AttackMove, 150 * 16, 150 * 16), sorted(sel)));
    int ordered = 0;
    for (int id : mine)
        if (const auto* u = f.world.unit(id); u && u->alive()) ordered += !u->orders.empty();
    const auto* dead = f.world.unit(mine[3]);
    check(ordered == 29 && (!dead || !dead->alive()),
          "a dead member is skipped, the live ones ordered (" + std::to_string(ordered) + ")");
    check(f.world.unit(theirs[0])->orders.empty(), "another player's unit in an order is refused");
    // An order without a click point (Attack, Guard, Assist, ...) opens no area: its legs keep the per-unit
    // identity (an Assist's approach move would otherwise join an area at the template's (0, 0)).
    Field g(PathfindingMode::Legion);
    const auto atk = g.block(kGround, 10, 10, 10, 10, 3);
    const auto foe = g.block(kGround, 1, 100, 100, 1, 3, 1);
    g.apply(orderCmd(0, 5, verb(Cmd::Attack, 0, 0, foe[0]), sorted(atk)));
    bool none = true;
    size_t open = 0;
    for (int id : atk) for (const auto& o : g.world.unit(id)->orders) none &= o.orderSerial == 0 && o.convoyTick == ConvoyTable::kNone;
    g.world.convoys().forEach([&](const ConvoyTable::Convoy&) { ++open; });
    check(none && open == 0, "an order without a click point opens no area and stamps no serial");
}

void sameTick() {
    {
        // Move then Stop in one tick, one player: the Stop wins (bundle order).
        Field f(PathfindingMode::Legion);
        const auto ids = f.block(kGround, 40, 10, 10, 10, 3);
        f.apply(orderCmd(0, 1, verb(Cmd::Move, 200 * 16, 200 * 16), sorted(ids), OrderShape::Offsets));
        f.apply(orderCmd(0, 2, verb(Cmd::Stop), sorted(ids)));
        bool stopped = true;
        for (int id : ids) stopped &= f.world.unit(id)->orders.empty();
        check(stopped, "a Stop behind a Move in the same tick wins");
    }
    {
        // Replacement in one tick: the second Move's point and serial.
        Field f(PathfindingMode::Legion);
        const auto ids = f.block(kGround, 40, 10, 10, 10, 3);
        f.apply(orderCmd(0, 1, verb(Cmd::Move, 200 * 16, 200 * 16), sorted(ids), OrderShape::Offsets));
        f.apply(orderCmd(0, 2, verb(Cmd::Move, 100 * 16, 220 * 16), sorted(ids), OrderShape::Offsets));
        bool replaced = true;
        for (int id : ids) {
            const auto& o = f.world.unit(id)->orders;
            replaced &= o.size() == 1 && o.back().orderSerial == 2 && o.back().x == Fixed::fromInt(1600);
        }
        check(replaced, "a second Move in the same tick replaces the first");
        // Then a queued leg (Shift): its own serial and area after the current leg.
        f.apply(orderCmd(0, 3, verb(Cmd::Move, 40 * 16, 40 * 16, 0, 1), sorted(ids), OrderShape::Offsets));
        bool queued = true;
        for (int id : ids) {
            const auto& o = f.world.unit(id)->orders;
            queued &= o.size() == 2 && o[0].orderSerial == 2 && o[1].orderSerial == 3 &&
                      o[1].convoyTick == (kExplicitArea | 3u) && o[0].convoyTick == (kExplicitArea | 2u);
        }
        check(queued, "a queued order appends a leg with its own identity");
        // Ctrl (kQueueFront, C9): the order's leg goes to the head, the earlier legs
        // keep their own identities behind it; then a Shift toggle on the queued
        // leg's point takes that leg back.
        f.apply(orderCmd(0, 4, verb(Cmd::Move, 120 * 16, 40 * 16, 0, tak::net::kQueueFront | tak::net::kQueueToggle),
                         sorted(ids), OrderShape::Offsets));
        bool front = true;
        for (int id : ids) {
            std::vector<uint32_t> serials;
            for (const auto& o : f.world.unit(id)->orders) if (o.goal) serials.push_back(o.orderSerial);
            front &= serials == std::vector<uint32_t>{4, 2, 3};
        }
        check(front, "a Ctrl order goes to the head; the earlier legs keep their identities behind it");
        f.apply(orderCmd(0, 5, verb(Cmd::Move, 40 * 16, 40 * 16, 0, tak::net::kQueueAppend | tak::net::kQueueToggle),
                         sorted(ids), OrderShape::Offsets));
        bool toggled = true;
        for (int id : ids) {
            std::vector<uint32_t> serials;
            for (const auto& o : f.world.unit(id)->orders) if (o.goal) serials.push_back(o.orderSerial);
            toggled &= serials == std::vector<uint32_t>{4, 2};
        }
        check(toggled, "a Shift order on a queued leg's point takes that leg back for every member");
    }
    {
        // Two orders of one player to one point in one tick share the area; another point does not.
        Field f(PathfindingMode::Legion);
        const auto a = f.block(kGround, 20, 10, 10, 10, 3), b = f.block(kGround, 20, 50, 10, 10, 3);
        const auto c = f.block(kGround, 20, 90, 10, 10, 3);
        f.apply(orderCmd(0, 1, verb(Cmd::Move, 200 * 16, 200 * 16), sorted(a), OrderShape::Offsets));
        f.apply(orderCmd(0, 2, verb(Cmd::Move, 200 * 16, 200 * 16), sorted(b), OrderShape::Offsets));
        f.apply(orderCmd(0, 3, verb(Cmd::Move, 20 * 16, 200 * 16), sorted(c), OrderShape::Offsets));
        const auto& oa = f.world.unit(a[0])->orders.back();
        const auto& ob = f.world.unit(b[0])->orders.back();
        const auto& oc = f.world.unit(c[0])->orders.back();
        check(oa.convoyTick == ob.convoyTick && oa.convoyTick == (kExplicitArea | 1u) && ob.orderSerial == 2 &&
                  oc.convoyTick == (kExplicitArea | 3u),
              "one point: one area keyed by the first order; another point: its own");
    }
    {
        // A patrol's return legs keep the order's identity (ground and flyer).
        Field f(PathfindingMode::Legion);
        std::vector<int> ids = f.block(kGround, 10, 10, 10, 10, 3);
        const auto air = f.block(kFlyer, 4, 30, 30, 2, 3);
        ids.insert(ids.end(), air.begin(), air.end());
        f.apply(orderCmd(0, 7, verb(Cmd::Patrol, 150 * 16, 150 * 16), sorted(ids)));
        f.run(5);
        bool legs = true;
        for (int id : ids) {
            int n = 0;
            for (const auto& o : f.world.unit(id)->orders)
                if (o.patrol && o.goal) { ++n; legs &= o.orderSerial == 7 && o.convoyTick == (kExplicitArea | 7u); }
            legs &= n >= 2;
        }
        check(legs, "a patrol's outbound and return legs carry the order's identity");
    }
}

// Review R6: an order applies its members in increasing id order, not in the
// player's selection order. The verbs whose outcome depends on that order (Load:
// the transport's pickup list; GiveUnit: a recipient near its cap) are not order
// verbs and pass through packClick per unit, in issue order. For the order verbs
// the member order changes nothing: a Squad click (evict, then assign) and the
// per-unit setters over a shuffled selection give the state the per-unit commands
// give in issue order, in both modes.
void issueOrder() {
    using tak::net::packClick;
    auto unit = [](Command c, int id) { c.unitId = id; return c; };
    {
        std::vector<Command> cmds;
        for (int id : {9, 3, 7}) cmds.push_back(unit(verb(Cmd::Load, 0, 0, 40), id));
        for (int id : {8, 2}) cmds.push_back(unit(verb(Cmd::GiveUnit, 0, 0, 1), id));
        uint32_t serial = 1;
        const auto out = packClick(cmds, {}, serial);
        bool same = out.size() == cmds.size() && serial == 1;
        for (size_t i = 0; same && i < out.size(); ++i) same = !out[i].order && out[i].unitId == cmds[i].unitId;
        check(same, "Load and GiveUnit stay per-unit commands, in issue order");
        check(!tak::net::orderVerb(Cmd::Load) && !tak::net::orderVerb(Cmd::GiveUnit), "Load and GiveUnit are not order verbs");
    }
    for (PathfindingMode mode : {PathfindingMode::Retail, PathfindingMode::Legion}) {
        const std::string tag = mode == PathfindingMode::Retail ? "Retail" : "Legion";
        Field a(mode), b(mode);
        const auto ids = a.block(kGround, 40, 10, 10, 10, 3);
        b.block(kGround, 40, 10, 10, 10, 3);
        // Squad 4 already holds ids[0..9]; the player selects 25 units in a shuffled order.
        std::vector<int> sel(ids.begin() + 5, ids.begin() + 30);
        std::mt19937 rng(11);
        std::shuffle(sel.begin(), sel.end(), rng);
        for (Field* f : {&a, &b})
            for (int i = 0; i < 10; ++i) f->world.setSquad(ids[size_t(i)], 4);
        for (bool append : {false, true}) {
            // GameView::assignSquad: evict (or retype) squad members outside the selection, live
            // order; then assign the selection in selection order.
            std::vector<Command> cmds;
            const std::set<int> chosen(sel.begin(), sel.end());
            for (int id : ids)
                if (std::abs(int(a.world.unit(id)->squad)) == 4 && !chosen.count(id))
                    cmds.push_back(unit(verb(Cmd::SetSquad, 0, 0, append ? -4 : 0), id));
            for (int id : sel) cmds.push_back(unit(verb(Cmd::SetSquad, 0, 0, -4), id));
            // The per-unit stream and the setters, in issue order; the same packed.
            for (Cmd k : {Cmd::Stop, Cmd::Stance, Cmd::SetActive, Cmd::Cloak, Cmd::Destroy})
                for (int id : sel) cmds.push_back(unit(verb(k, 0, 0, 1), id));
            for (auto& c : cmds) a.apply(c);
            uint32_t serial = append ? 100 : 1;
            for (auto& c : packClick(cmds, {}, serial)) b.apply(c);
            // b's orders carry serials a's per-unit commands do not; no leg is left by these verbs.
            check(a.world.stateHash() == b.world.stateHash(),
                  tag + ": a shuffled Squad click and the setters, packed, equal the per-unit commands (append " +
                      std::to_string(append) + ")");
        }
        a.run(90); b.run(90);
        check(a.world.stateHash() == b.world.stateHash(), tag + ": and stay equal");
    }
}

// ---- 4. delivery ----------------------------------------------------------------
void delivery() {
    Field f(PathfindingMode::Legion);
    const auto ids = f.block(kGround, 10000, 2, 2, 100, 2);
    tak::scn::SelectionOrder o;
    o.verb = tak::scn::Verb::Move; o.x = 200 * 16; o.z = 200 * 16;
    for (int rt : {-1, 16, 64}) {
        const auto b = tak::scn::issueSelection(f.world, ids, o, 500, 0, rt);
        const bool whole = b.size() == 1 && b[0].tick == 500 && b[0].cmds.size() == 1 && b[0].cmds[0].order &&
                           b[0].cmds[0].order->members.size() == 10000;
        check(whole, "a 10,000-unit click lands whole in its own tick (round trip " + std::to_string(rt) + ")");
        if (whole) check(tak::net::orderRecordBytes(*b[0].cmds[0].order) <= tak::net::kOrderMaxBytes, "and fits one record");
    }
    // An ordinary command behind the big order lands with it: the order does not take command credit.
    tak::scn::Uplink link(16);
    uint32_t serial = 1;
    link.issue(tak::scn::hudItems(f.world, ids, o, 0, serial));
    Command train; train.kind = Cmd::Train; train.unitId = ids[0];
    link.issue({train});
    const auto first = link.land(500);
    check(first.size() == 2 && first[0].order && first[1].kind == Cmd::Train,
          "an ordinary command behind a 10,000-unit order lands in the same tick, after it");
    // The per-unit stream (the AI's and scripts' path) takes 157 ticks without a window.
    tak::scn::Uplink perUnit;
    perUnit.issue(tak::scn::hudCommands(f.world, ids, o));
    int ticks = 0;
    for (uint32_t t = 500; !perUnit.idle(); ++t) ticks += !perUnit.land(t).empty();
    check(ticks == 157, "per-unit commands land 64 a tick (157 ticks for 10,000)");
}
}  // namespace

int main() {
    try {
        codec();
        packing();
        retailEquivalence();
        legionIdentity();
        membership();
        sameTick();
        issueOrder();
        delivery();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("selection_order_test: all passed\n");
    return 0;
}
