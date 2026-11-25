// revive_order_test -- raising the dead is an order.
//
// Retail creates a Resurrect, Reclaim or ReclaimArea task only from an order:
// the resolver 0x4de530 (mode 1, a right-click; mode 12, an area reclaim's
// child or the CLEAR button) tries resurrect, then reclaim (Kingdoms' animate is gone), and
// nothing idle reaches those modes. Ours raised bodies on its own -- an idle
// caster took up any body within 56 px -- and an idle reclaimer cleared every
// body around it; and it had no order to raise one at all, so a right-click on
// a body with a priest destroyed it. Now Cmd::Resurrect (World::resurrect)
// walks, turns and takes the stance as RECLAIM does, then channels the revive,
// and nothing raises or clears a body unordered.

#include "net/protocol.h"
#include "server/commands.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace ta;
using sim::Fixed;

static int g_fail = 0;
static void check(bool ok, const char* what, const std::string& detail = {}) {
    std::printf("  %-66s %s%s%s\n", what, ok ? "ok" : "FAIL", detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++g_fail;
}

namespace {
struct Types {
    sim::FeatType body;
    sim::UnitType victim, priest, mason, soldier;
    Types() {
        // A TA wreck: no resurrectable key; reclaimable, and its name up to
        // the '_' names the unit it raises (TotalA.exe 0x404f36, FeatType::raises).
        body.name = "victim_dead"; body.reclaimable = true;
        body.decomposeTicks = 100000; body.energy = 10;
        auto mover = [](const char* id) {
            sim::UnitType t;
            t.id = t.name = id; t.maxHp = 100; t.canMove = true;
            t.maxVel = Fixed::fromInt(2); t.turnRate = t.turnInPlaceRate = 10000;
            t.footX = t.footZ = 2; t.buildTime = 1; t.sight = 500; t.upright = true;
            return t;
        };
        victim = mover("victim"); victim.buildTime = 3000;
        soldier = mover("soldier");
        // A caster as the shipped ones are: a mobile builder that can reclaim.
        priest = mover("priest"); priest.workerTime = 300; priest.isBuilder = true;
        priest.canReclaim = true; priest.canResurrect = true;
        mason = mover("mason"); mason.workerTime = 10; mason.isBuilder = true; mason.canReclaim = true;
        body.raises = &victim;
    }
};

struct Scene {
    std::unique_ptr<sim::World> w = std::make_unique<sim::World>();
    sim::TypeRegistry reg;   // applyCommand's; nothing here looks a type up
    int dead = 0;
    explicit Scene(const Types& t, const sim::FeatType& body, float bodyX = 360, int owner = 0) {
        w->setTerrain(std::vector<uint8_t>(64 * 64, 100), 64, 64, 20);
        w->setMapPlacementFeatures(std::vector<uint16_t>(64 * 64, 0xffff), {});
        w->setPathService(true);
        w->setFeatureTypes({body});
        w->mapCorpse(&t.victim, 0);
        
        dead = w->spawn(&t.victim, bodyX, 300, 0, owner);
        w->unit(dead)->hp = {}; w->unit(dead)->deathType = 9;
    }
    void tick(int n = 1) { for (int i = 0; i < n; ++i) w->tick(1.0f / 30); }
    // The body lies there: not raised, not reclaimed, not decayed.
    bool bodyLies() const {
        const auto* c = w->unit(dead);
        return c && !c->alive() && !c->corpseFading && c->deadFor >= 0 && c->deadFor < c->corpseUntil;
    }
    int count(const sim::UnitType* type, int player) const {
        int n = 0;
        for (const auto& u : w->units()) n += u.alive() && u.type == type && u.player == player && u.id != dead;
        return n;
    }
    void command(net::Cmd kind, int unit, int target, float x = 0, float z = 0) {
        net::Command c; c.kind = kind; c.player = 0; c.unitId = unit; c.targetId = target; c.x = x; c.z = z;
        sim::applyCommand(*w, reg, c);
    }
};
}

// A right-click on a body with a priest (the client sends Cmd::Resurrect):
// the priest walks to it and raises it -- TotalA.exe RESURRECT (0x404db0):
// the timer ftol(buildtime x 0.3 / (workertime / 30)) counts down one a tick
// (stage 4, 0x405005), the unit comes back finished with 1 HP (0x405215) and
// the priest repairs it (stage 6: REPAIRUNIT at the head of its queue). A
// unit that cannot raise it keeps the order it had.
static void orderedResurrect(const Types& t) {
    std::printf("an ordered resurrect raises the body:\n");
    Scene s(t, t.body);
    const int priest = s.w->spawn(&t.priest, 200, 300, 0, 0);
    const int soldier = s.w->spawn(&t.soldier, 200, 400, 0, 0);
    s.w->order(soldier, 600, 400, false);
    s.command(net::Cmd::Resurrect, priest, s.dead);
    s.command(net::Cmd::Resurrect, soldier, s.dead);
    const auto& o = s.w->unit(priest)->orders;
    check(o.size() == 1 && o.front().reviveKind == 1 && o.front().reclaimFeat == -s.dead,
          "Cmd::Resurrect gives the priest one RESURRECT leg on the body");
    check(s.w->unit(soldier)->orders.size() == 1 && !s.w->unit(soldier)->orders.front().reviveKind,
          "a unit that cannot raise it keeps its own order");
    int channel = -1, raisedAt = -1, raisedHp = -1, raisedId = 0;
    for (int t2 = 0; t2 < 900 && raisedAt < 0; ++t2) {
        s.tick();
        if (channel < 0 && s.w->unit(priest)->reviveTarget) channel = t2;
        for (const auto& u : s.w->units())
            if (u.alive() && u.type == &t.victim && u.id != s.dead) {
                raisedAt = t2; raisedHp = int(u.hp.toFloat() + 0.5f); raisedId = u.id;
            }
    }
    char detail[160];
    std::snprintf(detail, sizeof detail, "channel from %d, raised at %d (%d ticks), hp %d",
                  channel, raisedAt, raisedAt - channel + 1, raisedHp);
    check(channel > 0 && raisedAt > channel, "the priest walks, then channels", detail);
    // 3000 x 0.3 / (300 / 30) = 90: 90 ticks counting down, raised at the 91st.
    check(raisedAt - channel + 1 == 91 && raisedHp == 1,
          "...0.3 x buildtime / (workertime/30) ticks, the unit back with 1 HP", detail);
    const auto& after = s.w->unit(priest)->orders;
    check(!s.bodyLies() && !s.w->unit(priest)->reviveTarget && after.size() == 1 &&
          after.front().repairTarget == raisedId,
          "the body is gone and the priest repairs the raised unit", detail);
    s.tick(300);
    const sim::Unit* raised = s.w->unit(raisedId);
    check(raised && raised->hp > Fixed::fromInt(1), "...which gains health");
    check(s.w->books(0).stored[0] == 0 && s.w->books(0).stored[1] == 0 && s.w->books(0).own[0].req == 0 &&
          s.w->books(0).own[1].req == 0, "raising cost nothing");
}

// A feature -- a map wreck, or the heap a destroyed wreck leaves -- is raised
// too: Cmd::Resurrect's target is then -featureId. Its name names the unit
// (cornecro_heap raises cornecro); one that names none cannot be raised.
static void featureResurrect(const Types& t) {
    std::printf("a wreck feature is raised:\n");
    sim::FeatType heap = t.body; heap.name = "victim_heap";
    sim::FeatType rock = t.body; rock.name = "rock1"; rock.raises = nullptr;
    Scene s(t, t.body);
    s.w->setFeatureTypes({t.body, heap, rock});
    const int heapId = 22 * 64 + 30, rockId = 10 * 64 + 30;
    s.w->addFeature(heapId, 496, 368, 10, 2, 2, false, 1);   // anchor cell (30, 22)
    s.w->addFeature(rockId, 496, 176, 10, 2, 2, false, 2);
    const int priest = s.w->spawn(&t.priest, 330, 360, 0, 0);
    check(s.w->reviveKind(priest, -heapId) == 1 && s.w->reviveKind(priest, -rockId) == 0,
          "a heap naming a unit can be raised, a rock cannot");
    s.command(net::Cmd::Resurrect, priest, -heapId);
    const auto& o = s.w->unit(priest)->orders;
    check(o.size() == 1 && o.front().reviveKind == 1 && o.front().reclaimFeat == heapId,
          "Cmd::Resurrect on -featureId gives a RESURRECT leg on the feature");
    for (int i = 0; i < 900 && !s.count(&t.victim, 0); ++i) s.tick();
    const sim::Feature* f = s.w->feature(heapId);
    check(s.count(&t.victim, 0) == 1 && (!f || !f->alive), "the heap rises as the priest's unit and is gone");
}

// Nothing raises a body unordered: an idle priest beside one stands, and an
// idle builder beside one leaves it (ours cleared every body around it).
static void idleCastersAndBuildersLeaveBodies(const Types& t) {
    std::printf("idle casters and builders leave bodies alone:\n");
    {
        Scene s(t, t.body);
        const int priest = s.w->spawn(&t.priest, 330, 300, 0, 0);
        bool channelled = false;
        for (int i = 0; i < 900; ++i) { s.tick(); channelled |= s.w->unit(priest)->reviveTarget != 0; }
        check(!channelled && s.bodyLies() && s.count(&t.victim, 0) == 0 && s.w->unit(priest)->orders.empty(),
              "an idle priest beside a body does not raise it");
    }
    {
        Scene s(t, t.body);
        const int mason = s.w->spawn(&t.mason, 330, 300, 0, 0);
        for (int i = 0; i < 900; ++i) s.tick();
        check(s.bodyLies() && s.w->unit(mason)->orders.empty() && !s.w->unit(mason)->reclaimId,
              "an idle builder beside a body leaves it");
    }
}

// The kinds (0x497100; TotalA.exe 0x440043 / 0x404f36): resurrect needs
// canresurrect and a reclaimable body naming a unit; it does not look at the
// body's owner: the raised unit is the caster's.
static void kinds(const Types& t) {
    std::printf("the revive kinds:\n");
    {
        Scene s(t, t.body, 360, 1);
        const int priest = s.w->spawn(&t.priest, 200, 300, 0, 0);
        s.tick(200);
        check(s.w->reviveKind(priest, s.dead) == 1, "a priest resurrects an enemy body");
        s.command(net::Cmd::Resurrect, priest, s.dead);
        for (int i = 0; i < 900 && !s.count(&t.victim, 0); ++i) s.tick();
        check(s.count(&t.victim, 0) == 1 && s.count(&t.victim, 1) == 0, "...and it rises as the priest's");
    }
    {
        Scene s(t, t.body);
        const int mason = s.w->spawn(&t.mason, 200, 300, 0, 0);
        s.tick(200);
        check(s.w->reviveKind(mason, s.dead) == 0, "a reclaimer without canresurrect cannot raise a body");
        s.command(net::Cmd::Resurrect, mason, s.dead);
        check(s.w->unit(mason)->orders.empty(), "...and Cmd::Resurrect on it does nothing");
    }
}

// A caster's area reclaim (0x406670's child through 0x4de530 mode 12) raises
// a body it can raise before it would reclaim it; a builder that cannot
// raise it reclaims it.
static void areaReclaimRaisesFirst(const Types& t) {
    std::printf("area reclaim with a caster raises first:\n");
    {
        Scene s(t, t.body);
        const int priest = s.w->spawn(&t.priest, 200, 300, 0, 0);
        s.tick(200);
        net::Command c; c.kind = net::Cmd::ReclaimArea; c.player = 0; c.unitId = priest;
        c.x = 300; c.z = 250; c.x2 = 420; c.z2 = 350;
        sim::applyCommand(*s.w, s.reg, c);
        bool reviveLeg = false;
        for (int i = 0; i < 1200 && !s.w->unit(priest)->orders.empty(); ++i) {
            s.tick();
            for (const auto& o : s.w->unit(priest)->orders) reviveLeg |= o.reviveKind != 0;
        }
        check(reviveLeg && s.count(&t.victim, 0) == 1 && !s.bodyLies(),
              "a priest's area reclaim resurrects the body in it");
    }
    {
        Scene s(t, t.body);
        const int mason = s.w->spawn(&t.mason, 200, 300, 0, 0);
        s.tick(200);
        net::Command c; c.kind = net::Cmd::ReclaimArea; c.player = 0; c.unitId = mason;
        c.x = 300; c.z = 250; c.x2 = 420; c.z2 = 350;
        sim::applyCommand(*s.w, s.reg, c);
        for (int i = 0; i < 1200 && !s.w->unit(mason)->orders.empty(); ++i) s.tick();
        check(s.count(&t.victim, 0) == 0 && s.w->unit(s.dead)->corpseFading,
              "a mason's area reclaim reclaims it");
    }
}

// The channel belongs to its order: a new order ends it, nothing is raised
// and the body stays.
static void newOrderEndsChannel(const Types& t) {
    std::printf("a new order ends the channel:\n");
    Scene s(t, t.body);
    const int priest = s.w->spawn(&t.priest, 330, 300, 0, 0);
    s.w->resurrect(priest, s.dead, false);
    for (int i = 0; i < 600 && !s.w->unit(priest)->reviveTarget; ++i) s.tick();
    const bool started = s.w->unit(priest)->reviveTarget != 0;
    s.command(net::Cmd::Move, priest, 0, 100, 300);
    s.tick(300);
    check(started && !s.w->unit(priest)->reviveTarget && s.count(&t.victim, 0) == 0 && s.bodyLies(),
          "a move ends the revive unraised, the body kept");
}

// Cmd::Resurrect on the wire: a strict replay read accepts it, and the server
// accepts it from a client.
static void wire() {
    std::printf("Cmd::Resurrect on the wire:\n");
    net::Command c; c.kind = net::Cmd::Resurrect; c.unitId = 7; c.targetId = 9;
    net::Writer w; net::putBundle(w, 3, {c}, {});
    net::Reader r(w.b.data(), w.b.size()); uint32_t tick = 0; net::Bundle b;
    check(net::getBundle(r, tick, b, true) && b.cmds.size() == 1 &&
          b.cmds[0].kind == net::Cmd::Resurrect && b.cmds[0].targetId == 9, "a strict read takes it");
    net::Writer batch; batch.u32(1); batch.cmd(c);
    std::vector<net::Command> parsed;
    check(srv::parsePlayerCommands(batch.b, parsed) && parsed.size() == 1, "the server accepts it from a client");
}

int main() {
    const Types types;
    orderedResurrect(types);
    featureResurrect(types);
    idleCastersAndBuildersLeaveBodies(types);
    kinds(types);
    areaReclaimRaisesFirst(types);
    newOrderEndsChannel(types);
    wire();
    if (g_fail) { std::printf("FAIL: %d\n", g_fail); return 1; }
    std::printf("PASS: revive orders\n");
    return 0;
}
