// revive_order_test -- raising the dead is an order.
//
// Retail creates a Resurrect, Reclaim or ReclaimArea task only from an order:
// the resolver 0x4de530 (mode 1, a right-click; mode 12, an area reclaim's
// child or the CLEAR button) tries resurrect, then animate, then reclaim, and
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

using namespace tak;
using sim::Fixed;

static int g_fail = 0;
static void check(bool ok, const char* what, const std::string& detail = {}) {
    std::printf("  %-66s %s%s%s\n", what, ok ? "ok" : "FAIL", detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++g_fail;
}

namespace {
struct Types {
    sim::FeatType body;
    sim::UnitType victim, ghoul, priest, animator, mason, soldier;
    Types() {
        body.name = "victim_dead"; body.resurrectable = true; body.reclaimable = true;
        body.decomposeTicks = 100000; body.energy = 10;
        auto mover = [](const char* id) {
            sim::UnitType t;
            t.id = t.name = id; t.maxHp = 100; t.canMove = true;
            t.maxVel = Fixed::fromInt(2); t.turnRate = t.turnInPlaceRate = 10000;
            t.footX = t.footZ = 2; t.buildTime = 1; t.sight = 500; t.upright = true;
            return t;
        };
        victim = mover("victim"); victim.buildTime = 100;
        ghoul = mover("ghoul"); ghoul.buildTime = 30;
        soldier = mover("soldier");
        // A caster as the shipped ones are: a mobile builder that can reclaim.
        priest = mover("priest"); priest.workerTime = 10; priest.isBuilder = true;
        priest.canReclaim = true; priest.canResurrect = true;
        animator = priest; animator.id = animator.name = "animator";
        animator.canResurrect = false; animator.canAnimate = true; animator.animateType = &ghoul;
        mason = mover("mason"); mason.workerTime = 10; mason.isBuilder = true; mason.canReclaim = true;
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
        w->player(0).mana = 100;
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
// the priest walks to it, raises it at a tenth of its health, and the body
// goes. A unit that cannot raise it keeps the order it had.
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
    int channel = -1, raisedAt = -1;
    for (int t2 = 0; t2 < 900 && raisedAt < 0; ++t2) {
        s.tick();
        if (channel < 0 && s.w->unit(priest)->reviveTarget) channel = t2;
        if (s.count(&t.victim, 0)) raisedAt = t2;
    }
    const sim::Unit* raised = nullptr;
    for (const auto& u : s.w->units()) if (u.alive() && u.type == &t.victim && u.id != s.dead) raised = &u;
    s.tick(2);
    char detail[160];
    std::snprintf(detail, sizeof detail, "channel from %d, raised at %d (%d ticks), hp %d",
                  channel, raisedAt, raisedAt - channel + 1, raised ? int(raised->hp.toFloat() + 0.5f) : -1);
    check(channel > 0 && raisedAt > channel, "the priest walks, then channels", detail);
    check(raisedAt - channel + 1 == 90 && raised && int(raised->hp.toFloat() + 0.5f) == 10,
          "...0.3 x buildtime / workertime, the unit back at a tenth of its health", detail);
    check(!s.bodyLies() && s.w->unit(priest)->orders.empty() && !s.w->unit(priest)->reviveTarget,
          "the body is gone and the order is over", detail);
    check(s.w->player(0).mana == 100, "raising cost nothing");
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
        Scene s(t, t.body, 360, 1);
        const int priest = s.w->spawn(&t.animator, 330, 300, 0, 0);
        sim::FeatType animatable = t.body; animatable.animatable = true;
        s.w->setFeatureTypes({animatable});
        bool channelled = false;
        for (int i = 0; i < 900; ++i) { s.tick(); channelled |= s.w->unit(priest)->reviveTarget != 0; }
        check(!channelled && s.bodyLies() && s.count(&t.ghoul, 0) == 0,
              "an idle animator beside an enemy body does not animate it");
    }
    {
        Scene s(t, t.body);
        const int mason = s.w->spawn(&t.mason, 330, 300, 0, 0);
        for (int i = 0; i < 900; ++i) s.tick();
        check(s.bodyLies() && s.w->unit(mason)->orders.empty() && !s.w->unit(mason)->reclaimId,
              "an idle builder beside a body leaves it");
    }
}

// The kinds (0x497100, 0x496fd0): resurrect needs canresurrect and the body's
// resurrectable, animate cananimate and the body's animatable (its own flag,
// 0x1000). Neither looks at the body's owner: the raised unit is the caster's.
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
        const int animator = s.w->spawn(&t.animator, 200, 300, 0, 0);
        s.tick(200);
        check(s.w->reviveKind(animator, s.dead) == 0, "a body that is not animatable cannot be animated");
        s.command(net::Cmd::Resurrect, animator, s.dead);
        check(s.w->unit(animator)->orders.empty(), "...and Cmd::Resurrect on it does nothing");
    }
    {
        sim::FeatType animatable = t.body; animatable.animatable = true;
        Scene s(t, animatable, 360, 1);
        const int animator = s.w->spawn(&t.animator, 200, 300, 0, 0);
        s.tick(200);
        check(s.w->reviveKind(animator, s.dead) == 2, "an animatable body is animated");
        s.command(net::Cmd::Resurrect, animator, s.dead);
        for (int i = 0; i < 900 && !s.count(&t.ghoul, 0); ++i) s.tick();
        const sim::Unit* ghoul = nullptr;
        for (const auto& u : s.w->units()) if (u.alive() && u.type == &t.ghoul) ghoul = &u;
        check(ghoul && ghoul->player == 0 && ghoul->hp == Fixed::fromInt(100) && !s.bodyLies(),
              "...rising as the caster's animatetype at full health");
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
    idleCastersAndBuildersLeaveBodies(types);
    kinds(types);
    areaReclaimRaisesFirst(types);
    newOrderEndsChannel(types);
    wire();
    if (g_fail) { std::printf("FAIL: %d\n", g_fail); return 1; }
    std::printf("PASS: revive orders\n");
    return 0;
}
