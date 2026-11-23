// command_validation_test -- a player's order the sim refuses changes nothing.
//
// Every command here goes the way a client's does: serialized, through the
// server's parser (srv::parsePlayerCommands), stamped with the sender's seat,
// through a tick bundle and into applyCommand.
//
// Load: a Ctrl (front) Load rewinds the passenger's list and the carrier's
// before it issues the order (World::insertOrderFront), so the boarding rules
// (World::canLoadInto) are asked first; an enemy's or a non-transport's
// movement, mission and path are left alone, and so is the passenger's.
//
// Production: Train, RepeatTrain, Build and BuildManaArea take only a type on
// the producer's own build menu (World::canProduce), whatever its owner and
// whether or not it is builderlimited, and only from a builder. Train and
// RepeatTrain set their output down beside the producer, so they take only a
// mobile type: a structure is placed (Build). Scripts' and the retail AI
// planner's own World::train calls stay unrestricted.
//
// Synthetic types loaded through the real registry (FBI + canbuild/ tree) from
// an in-memory VFS, on a flat map. Data-independent.

#include "hpi/hpi.h"
#include "net/protocol.h"
#include "server/commands.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace tak;
using sim::Fixed;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}

constexpr uint8_t kShift = net::kQueueAppend | net::kQueueToggle;
constexpr uint8_t kCtrl = net::kQueueFront | net::kQueueToggle;

// The registry, as setupRegistry builds one: FBIs, then the canbuild/ tree.
struct Registry {
    hpi::Vfs vfs;
    sim::TypeRegistry reg;
    Registry() {
        auto files = std::make_shared<hpi::Vfs::Files>();
        auto put = [&](const std::string& path, const std::string& text) { (*files)[path] = {text.begin(), text.end()}; };
        auto fbi = [&](const char* id, const std::string& extra) {
            put(std::string("units/") + id + ".fbi",
                std::string("[UNITINFO]{\nunitname=") + id + ";\nobjectname=" + id +
                ";\nmaxdamage=300;\nturnrate=10000;\nsightdistance=400;\n" + extra + "}\n");
        };
        const std::string mover = "canmove=1;\nmaxvelocity=2;\nfootprintx=2;\nfootprintz=2;\nbuildtime=30;\nbuildcost=5;\n"
                                  "transportedsize=1;\n";
        fbi("sol", mover);
        fbi("foreign", mover);   // registered, on nobody's menu
        fbi("boat", mover + "cantransport=1;\ntransportcapacity=4;\ntransportsizecapacity=16;\ntransportsize=4;\n"
                            "transportdistance=150;\n");
        const std::string builder = mover + "builder=1;\nworkertime=100;\nbuilddistance=80;\nmogriumstorage=100000;\n";
        fbi("builder", builder + "builderlimited=1;\n");   // a Zhon-style limited conjurer
        fbi("freebuilder", builder);                        // not builderlimited
        const std::string factory = "maxvelocity=0;\nfootprintx=6;\nfootprintz=6;\nbuilder=1;\nworkertime=100;\n"
                                    "mogriumstorage=1000000;\nmaxdamage=5000;\n";
        fbi("factory", factory);
        fbi("workshop", factory);   // a mod's factory with a structure on its menu
        fbi("hut", "maxvelocity=0;\nfootprintx=2;\nfootprintz=2;\nbuildtime=3000;\nbuildcost=10;\n");
        fbi("tower", "maxvelocity=0;\nfootprintx=2;\nfootprintz=2;\nbuildtime=3000;\nbuildcost=10;\n");
        fbi("lode", "maxvelocity=0;\nfootprintx=2;\nfootprintz=2;\nbuildtime=3000;\nbuildcost=10;\n");   // onMana
        for (const char* t : {"hut", "sol", "lode"}) put(std::string("canbuild/builder/") + t + ".tdf", "[Menu]{\npriority=1;\n}\n");
        put("canbuild/freebuilder/hut.tdf", "[Menu]{\npriority=1;\n}\n");
        put("canbuild/factory/sol.tdf", "[Menu]{\npriority=1;\n}\n");
        for (const char* t : {"sol", "tower"}) put(std::string("canbuild/workshop/") + t + ".tdf", "[Menu]{\npriority=1;\n}\n");
        vfs.setMapFiles(files);
        reg.loadDir(vfs, "units");
        reg.loadBuildTree(vfs, "canbuild");
        // A passenger's top must clear the sea (canLoadInto); a model would give it one.
        if (const auto* s = reg.find("sol")) { rider = *s; rider.modelTop = 40 << 16; }
    }
    sim::UnitType rider;   // a sol with a model top, for boarding
    const sim::UnitType* operator()(const char* id) const { return std::strcmp(id, "rider") ? reg.find(id) : &rider; }
};

// Players 0 and 1 are allies, 2 the enemy.
struct Rig {
    const Registry& r;
    sim::World w;
    uint32_t tick = 0;
    explicit Rig(const Registry& reg) : r(reg) {
        w.setSerialThreads(true);
        w.setTerrain(std::vector<uint8_t>(size_t(128) * 128, 100), 128, 128, 20);
        w.setPathService(true);
        w.setPlayerCount(3);
        w.setTeam(0, 0); w.setTeam(1, 0); w.setTeam(2, 1);
        for (int p = 0; p < 3; ++p) w.player(p).mana = 1e7;
    }
    int spawn(const char* id, float x, float z, int player = 0) { return w.spawn(r(id), x, z, 0.0f, player); }
    void ticks(int n) { for (int i = 0; i < n; ++i) { w.tick(1.f / 30); ++tick; } }
    // Client -> server parser -> the sender's seat -> tick bundle -> the sim.
    void send(int player, std::vector<net::Command> cmds) {
        net::Writer batch; batch.u32(uint32_t(cmds.size()));
        for (const auto& c : cmds) batch.cmd(c);
        std::vector<net::Command> parsed;
        if (!srv::parsePlayerCommands(batch.b, parsed)) { check(false, "the server parses the client's batch"); return; }
        for (auto& c : parsed) c.player = uint8_t(player);
        deliver(parsed);
    }
    // The referee's bundle (also how a Cmd::Order reaches the sim).
    void deliver(const std::vector<net::Command>& cmds) {
        net::Writer out; net::putBundle(out, tick, cmds, {});
        net::Reader in(out.b.data(), out.b.size());
        uint32_t at = 0; net::Bundle b;
        if (!net::getBundle(in, at, b, false)) { check(false, "the bundle decodes"); return; }
        for (const auto& c : b.cmds) sim::applyCommand(w, r.reg, c);
    }
    void send(int player, net::Cmd kind, int unit, int target = 0, uint8_t queue = 0, const char* type = "",
              float x = 0, float z = 0) {
        net::Command c; c.kind = kind; c.unitId = unit; c.targetId = target; c.queue = queue; c.x = x; c.z = z;
        std::snprintf(c.type, sizeof c.type, "%s", type);
        send(player, std::vector<net::Command>{c});
    }
    int count(const char* id, int player) const {
        int n = 0;
        for (const auto& u : w.units()) n += u.alive() && u.type == r(id) && u.player == player;
        return n;
    }
};

// What a refused order must leave alone: the orders (their movement controller,
// mission and route state), production and work; and the world's state hash.
struct Snap {
    struct Leg {
        uint64_t controller; uint8_t stage; uint32_t flags, waitMask, pending; bool goal, segment, load, pickup;
        int32_t x, z; int target; const sim::UnitType* build;
        bool operator==(const Leg&) const = default;
    };
    std::vector<Leg> orders;
    size_t rally = 0, queue = 0;
    int32_t routeStamp = 0;
    const sim::UnitType* repeat = nullptr;
    int buildSite = 0, productionSite = 0, player = -1;
    bool operator==(const Snap&) const = default;
    Snap(const sim::World& w, int id) {
        const sim::Unit* u = w.unit(id);
        if (!u) return;
        for (const auto& o : u->orders)
            orders.push_back({o.controller, o.mission.stage, o.mission.flags, o.mission.waitMask, o.mission.pending,
                              o.goal, o.hasSegment, o.load, o.transportPickup, o.x.v, o.z.v, o.targetId, o.buildType});
        rally = u->rally.size(); queue = u->buildQueue.size(); routeStamp = u->routeStamp;
        repeat = u->repeatType;
        buildSite = u->buildSiteId; productionSite = u->productionSiteId; player = u->player;
    }
};

// `ids` and the whole world are as they were before `act`.
template <class F> void unchanged(Rig& g, std::vector<int> ids, F&& act, const char* what) {
    std::vector<Snap> before;
    for (int id : ids) before.emplace_back(g.w, id);
    const uint64_t hash = g.w.stateHash();
    act();
    bool same = g.w.stateHash() == hash;
    for (size_t i = 0; i < ids.size(); ++i) same &= Snap(g.w, ids[i]) == before[i];
    check(same, what);
}

// A unit under way, with a controller and a mission running.
int walker(Rig& g, const char* id, float x, float z, int player, float tx, float tz) {
    const int u = g.spawn(id, x, z, player);
    g.send(player, net::Cmd::Move, u, 0, 0, "", tx, tz);
    g.send(player, net::Cmd::Move, u, 0, kShift, "", tx, tz + 400);
    return u;
}

// ---- Load -------------------------------------------------------------------

void loadRefusals(const Registry& reg) {
    Rig g(reg);
    const int rider = walker(g, "rider", 400, 400, 0, 1500, 400);
    const int enemyBoat = walker(g, "boat", 600, 900, 2, 1500, 900);
    const int ownSol = walker(g, "sol", 600, 1200, 0, 1500, 1200);
    const int allyBoat = walker(g, "boat", 600, 1500, 1, 1500, 1500);
    const int deadBoat = g.spawn("boat", 900, 600, 0);
    const int tower = g.spawn("tower", 1600, 1600, 0);
    g.ticks(20);
    g.w.unit(deadBoat)->hp = {};
    g.ticks(1);
    check(!g.w.unit(rider)->orders.empty() && g.w.unit(rider)->orders.front().controller &&
          !g.w.unit(enemyBoat)->orders.empty() && g.w.unit(enemyBoat)->orders.front().controller,
          "load setup: the passenger and the enemy carrier are both under way");
    for (const uint8_t q : {kCtrl, uint8_t(net::kQueueFront), kShift, uint8_t(0)}) {
        const std::string mode = q == kCtrl ? "Ctrl" : q == net::kQueueFront ? "front" : q == kShift ? "Shift" : "plain";
        auto refused = [&](int target, const char* what) {
            const std::string text = mode + " Load " + what + ": both units' state unchanged";
            unchanged(g, {rider, target}, [&] { g.send(0, net::Cmd::Load, rider, target, q); }, text.c_str());
        };
        refused(enemyBoat, "into a moving enemy transport");
        refused(ownSol, "into a moving friendly non-transport");
        refused(allyBoat, "into an ally's transport");
        refused(tower, "into an own structure");
        refused(deadBoat, "into a dead transport");
        refused(99999, "naming no unit");
        refused(rider, "into itself");
    }
    // A passenger the sender does not own: nothing either way.
    unchanged(g, {enemyBoat, ownSol}, [&] { g.send(2, net::Cmd::Load, ownSol, enemyBoat, kCtrl); },
              "Ctrl Load of another player's passenger changes nothing");
    // Batched: the refused Load in one payload with a legitimate move after it.
    const Snap boatBefore(g.w, enemyBoat);
    net::Command load; load.kind = net::Cmd::Load; load.unitId = rider; load.targetId = enemyBoat; load.queue = kCtrl;
    net::Command move; move.kind = net::Cmd::Move; move.unitId = ownSol; move.x = 300; move.z = 1200;
    g.send(0, {load, move});
    check(Snap(g.w, enemyBoat) == boatBefore && g.w.unit(ownSol)->orders.size() == 1,
          "a batch: the refused Ctrl Load leaves the enemy alone, the move beside it applies");
    // Not a selection-order verb: a Cmd::Order carrying Load is refused whole.
    auto order = std::make_shared<net::SelectionOrder>();
    order->serial = 1; order->tmpl.kind = net::Cmd::Load; order->tmpl.targetId = enemyBoat; order->tmpl.queue = kCtrl;
    order->members = {rider};
    net::Command env; env.kind = net::Cmd::Order; env.player = 0; env.order = order;
    unchanged(g, {rider, enemyBoat}, [&] { g.deliver({env}); }, "a selection order carrying a Load changes nothing");
}

void loadAccepted(const Registry& reg) {
    {
        // Ctrl: the pickup and the boarding at the head of both lists, the rest behind.
        Rig g(reg);
        const int boat = walker(g, "boat", 1000, 1000, 0, 1500, 1000);
        const int p = walker(g, "rider", 1100, 1300, 0, 700, 1300);
        g.ticks(5);
        const size_t boatLegs = g.w.unit(boat)->orders.size(), riderLegs = g.w.unit(p)->orders.size();
        g.send(0, net::Cmd::Load, p, boat, kCtrl);
        const auto& bo = g.w.unit(boat)->orders;
        const auto& po = g.w.unit(p)->orders;
        check(bo.size() == boatLegs + 1 && bo.front().transportPickup && bo.front().targetId == p,
              "Ctrl Load into an own transport: its pickup goes first, its moves kept behind");
        check(po.size() == riderLegs + 1 && po.front().load && po.front().targetId == boat,
              "Ctrl Load: the passenger boards first, its moves kept behind");
        // Ctrl again on the same transport takes the queued Load back (the toggle).
        g.send(0, net::Cmd::Load, p, boat, kCtrl);
        check(std::none_of(po.begin(), po.end(), [](const sim::Order& o) { return o.load; }),
              "Ctrl Load again on the same transport takes the queued boarding back");
    }
    {
        // Shift appends; a Shift Load on a queued one whose transport can no longer
        // take it (paralysed) still takes that one back: cancelling an existing
        // order is not taking a new one.
        Rig g(reg);
        const int boat = g.spawn("boat", 1000, 1000, 0);
        const int p = walker(g, "rider", 1100, 1300, 0, 700, 1300);
        g.ticks(3);
        g.send(0, net::Cmd::Load, p, boat, kShift);
        const auto& po = g.w.unit(p)->orders;
        check(po.size() == 3 && po.back().load && po.back().targetId == boat, "Shift Load appends the boarding");
        g.w.unit(boat)->paralyzedFor = 300;
        check(!g.w.canLoadInto(p, boat), "a paralysed transport takes no new boarding");
        g.send(0, net::Cmd::Load, p, boat, kShift);
        check(po.size() == 2 && std::none_of(po.begin(), po.end(), [](const sim::Order& o) { return o.load; }),
              "Shift Load on the queued one still takes it back");
        g.w.unit(boat)->paralyzedFor = 0;
        g.send(0, net::Cmd::Load, p, boat, 0);
        g.ticks(600);
        check(g.w.unit(p)->inTransport == boat, "a plain Load still boards");
    }
}

// ---- production -------------------------------------------------------------

void restrictedBuilder(const Registry& reg) {
    Rig g(reg);
    const int b = walker(g, "builder", 400, 400, 0, 1500, 400);
    g.ticks(10);
    for (const uint8_t q : {uint8_t(0), kShift, kCtrl}) {
        unchanged(g, {b}, [&] { g.send(0, net::Cmd::Build, b, 0, q, "foreign", 600, 600); },
                  q == kCtrl ? "Ctrl Build of a type off the menu changes nothing"
                             : q ? "Shift Build of a type off the menu changes nothing"
                                 : "Build of a type off the menu changes nothing");
    }
    unchanged(g, {b}, [&] { g.send(0, net::Cmd::RepeatTrain, b, 0, 0, "foreign"); },
              "RepeatTrain of a type off the menu changes nothing (the walk goes on)");
    unchanged(g, {b}, [&] { g.send(0, net::Cmd::Train, b, 1, 0, "foreign"); },
              "Train of a type off the menu changes nothing");
    unchanged(g, {b}, [&] { g.send(0, net::Cmd::Build, b, 0, kCtrl, "nosuchtype", 600, 600); },
              "Ctrl Build of an unknown type changes nothing");
    // A structure on its menu is placed (Build); Train/RepeatTrain would make
    // it beside the builder with no placement (the HUD's Ctrl+click on a
    // structure icon starts a placement instead).
    unchanged(g, {b}, [&] { g.send(0, net::Cmd::Train, b, 1, 0, "hut"); },
              "Train of a structure on the menu changes nothing");
    unchanged(g, {b}, [&] { g.send(0, net::Cmd::RepeatTrain, b, 0, 0, "hut"); },
              "RepeatTrain of a structure on the menu changes nothing (the walk goes on)");
    g.ticks(300);
    check(g.count("foreign", 0) == 0, "no off-menu unit is produced");
    check(g.count("hut", 0) == 0, "no structure is produced beside the builder");
    // The menu still works: a conjured sol (repeat), and a hut placed.
    g.send(0, net::Cmd::RepeatTrain, b, 0, 0, "sol");
    check(g.w.unit(b)->repeatType == reg("sol"), "RepeatTrain from the menu starts repeat production");
    g.ticks(400);
    check(g.count("sol", 0) >= 1, "...and conjures the unit");
    unchanged(g, {b}, [&] { g.send(0, net::Cmd::RepeatTrain, b, 0, 0, "foreign"); },
              "a repeating producer: RepeatTrain off the menu leaves its repeat alone");
    g.send(0, net::Cmd::Stop, b);
    check(!g.w.unit(b)->repeatType && g.w.unit(b)->buildQueue.empty(), "Stop ends the conjurer's repeat");
    g.send(0, net::Cmd::BuildManaArea, b, 0, 0, "lode", 900, 900);
    check(!g.w.unit(b)->orders.empty() && g.w.unit(b)->orders.front().manaBuildArea,
          "an area build of a lodestone on the menu is taken");
    g.send(0, net::Cmd::Build, b, 0, 0, "hut", 700, 700);
    check(!g.w.unit(b)->orders.empty() && g.w.unit(b)->orders.front().buildType == reg("hut"),
          "Build from the menu queues the build");
}

void factoryProduction(const Registry& reg) {
    Rig g(reg);
    const int f = g.spawn("factory", 800, 800, 0);
    const int soldier = walker(g, "sol", 400, 1400, 0, 1500, 1400);
    const int tower = g.spawn("tower", 1400, 400, 0);
    g.ticks(5);
    unchanged(g, {f}, [&] { g.send(0, net::Cmd::Train, f, 5, 0, "foreign"); }, "factory: Train off the menu changes nothing");
    unchanged(g, {f}, [&] { g.send(0, net::Cmd::RepeatTrain, f, 0, 0, "foreign"); },
              "factory: RepeatTrain off the menu changes nothing");
    unchanged(g, {soldier}, [&] { g.send(0, net::Cmd::Train, soldier, 1, 0, "sol"); },
              "a unit that is no builder: Train changes nothing");
    unchanged(g, {soldier}, [&] { g.send(0, net::Cmd::RepeatTrain, soldier, 0, 0, "sol"); },
              "a unit that is no builder: RepeatTrain changes nothing");
    unchanged(g, {tower}, [&] { g.send(0, net::Cmd::Train, tower, 1, 0, "sol"); },
              "a structure that is no builder: Train changes nothing");
    // One batch, one refused item and one good one.
    net::Command bad; bad.kind = net::Cmd::Train; bad.unitId = f; bad.targetId = 1;
    std::snprintf(bad.type, sizeof bad.type, "foreign");
    net::Command good = bad; std::snprintf(good.type, sizeof good.type, "sol");
    g.send(0, {bad, good});
    check(g.w.unit(f)->buildQueue.size() == 1 && g.w.unit(f)->buildQueue.front() == reg("sol"),
          "a batch: only the menu's type is queued");
    g.ticks(900);
    check(g.count("foreign", 0) == 0 && g.count("sol", 0) >= 2,
          "the factory produces its menu's unit and nothing off it");
    // Repeat production and its cancel.
    g.send(0, net::Cmd::RepeatTrain, f, 0, 0, "sol");
    check(g.w.unit(f)->repeatType == reg("sol"), "factory: RepeatTrain from the menu repeats");
    // Repeating, a factory takes orders (no lock); an off-menu RepeatTrain is
    // refused, not a switch to the other type.
    unchanged(g, {f}, [&] { g.send(0, net::Cmd::RepeatTrain, f, 0, 0, "foreign"); },
              "a repeating factory: RepeatTrain off the menu leaves its repeat alone");
    g.send(0, net::Cmd::RepeatTrain, f, 0, 0, "sol");
    check(!g.w.unit(f)->repeatType && g.w.unit(f)->buildQueue.empty(), "factory: RepeatTrain again cancels it");
    // A structure on a factory's menu (a mod's): placed, not produced at its door.
    const int w = g.spawn("workshop", 1700, 900, 0);
    g.ticks(2);
    unchanged(g, {w}, [&] { g.send(0, net::Cmd::Train, w, 1, 0, "tower"); },
              "a factory: Train of a structure on its menu changes nothing");
    unchanged(g, {w}, [&] { g.send(0, net::Cmd::RepeatTrain, w, 0, 0, "tower"); },
              "a factory: RepeatTrain of a structure on its menu changes nothing");
    g.send(0, net::Cmd::Train, w, 1, 0, "sol");
    check(g.w.unit(w)->buildQueue.size() == 1 && g.w.unit(w)->buildQueue.front() == reg("sol"),
          "...and Train of the mobile type on the same menu is taken");
    // A placement needs a mobile builder: a factory's (forged) Build of its own menu's
    // unit, Ctrl included, is refused before the front insert rewinds the factory.
    g.send(0, net::Cmd::Move, w, 0, 0, "", 1900, 1300);   // a rally order to rewind
    g.ticks(2);
    for (const uint8_t q : {kCtrl, uint8_t(0)})
        unchanged(g, {w}, [&] { g.send(0, net::Cmd::Build, w, 0, q, "sol", 1900, 1100); },
                  q ? "a factory: Ctrl Build of its menu's unit changes nothing"
                    : "a factory: Build of its menu's unit changes nothing");
    // The scripts' / retail planner's API is not a player's order and stays unrestricted.
    g.w.train(f, reg("foreign"));
    check(g.w.unit(f)->buildQueue.size() == 1, "World::train itself (scripts, the planner) is not menu-bound");
}

void restrictionsAndCaps(const Registry& reg) {
    {
        Rig g(reg);
        const int f = g.spawn("factory", 800, 800, 0);
        g.w.setBuildRestrictions({"hut"});
        unchanged(g, {f}, [&] { g.send(0, net::Cmd::Train, f, 1, 0, "sol"); },
                  "a game's Use Only list still refuses a menu type");
        unchanged(g, {f}, [&] { g.send(0, net::Cmd::RepeatTrain, f, 0, 0, "sol"); },
                  "...for RepeatTrain too");
        g.w.setBuildRestrictions({}, false);
        g.send(0, net::Cmd::Train, f, 1, 0, "sol");
        check(g.w.unit(f)->buildQueue.size() == 1, "lifting it, the same Train is taken");
    }
    {
        // The unit cap stalls production rather than refusing the order (as before).
        Rig g(reg);
        const int f = g.spawn("factory", 800, 800, 0);
        g.w.setUnitCap(1);
        g.send(0, net::Cmd::Train, f, 1, 0, "sol");
        check(g.w.unit(f)->buildQueue.size() == 1, "at the unit cap a Train from the menu is still queued");
        g.ticks(600);
        check(g.count("sol", 0) == 0, "...and nothing is produced past the cap");
    }
}

void unrestrictedAndGifted(const Registry& reg) {
    Rig g(reg);
    // Not builderlimited: HelpBuild on any of its owner's sites, but construction
    // only from its menu (MobileBuild's own test, 0x519850).
    const int free = walker(g, "freebuilder", 400, 400, 0, 1500, 400);
    const int limited = walker(g, "builder", 400, 800, 0, 1500, 800);
    const int site = g.spawn("tower", 700, 1200, 0);
    g.spawn("sol", 1600, 1600, 1);   // the ally is in the game (a player with nothing is out)
    {
        auto& s = *g.w.unit(site);
        s.underConstruction = s.buildBegun = true; s.hp = Fixed::fromInt(10);
    }
    g.ticks(5);
    unchanged(g, {free}, [&] { g.send(0, net::Cmd::Build, free, 0, kCtrl, "tower", 900, 900); },
              "an unrestricted builder: Ctrl Build off its menu changes nothing");
    unchanged(g, {free}, [&] { g.send(0, net::Cmd::BuildManaArea, free, 0, 0, "lode", 900, 900); },
              "an unrestricted builder: an area build of a lodestone off its menu changes nothing");
    unchanged(g, {limited}, [&] { g.send(0, net::Cmd::Assist, limited, site, kCtrl); },
              "a builderlimited builder: Ctrl Assist on a site off its menu changes nothing");
    auto order = std::make_shared<net::SelectionOrder>();
    order->serial = 1; order->tmpl.kind = net::Cmd::Assist; order->tmpl.targetId = site; order->tmpl.queue = kCtrl;
    order->members = {limited};
    net::Command env; env.kind = net::Cmd::Order; env.player = 0; env.order = order;
    unchanged(g, {limited}, [&] { g.deliver({env}); },
              "...nor through a selection order");
    g.send(0, net::Cmd::Assist, free, site, kCtrl);
    check(!g.w.unit(free)->orders.empty() && g.w.unit(free)->orders.front().helpSite == site,
          "an unrestricted builder helps a site off its menu (HelpBuild is not menu-bound)");
    g.send(0, net::Cmd::Build, free, 0, kCtrl, "hut", 900, 900);
    check(g.w.unit(free)->orders.front().buildType == reg("hut"), "an unrestricted builder builds from its menu");

    // A factory given to an ally: its new owner trains from its menu, the old one not at all.
    const int f = g.spawn("factory", 1200, 1200, 0);
    g.send(0, net::Cmd::GiveUnit, f, 1);
    check(g.w.unit(f)->player == 1, "the factory changes hands");
    unchanged(g, {f}, [&] { g.send(0, net::Cmd::Train, f, 1, 0, "sol"); }, "the old owner can no longer train there");
    unchanged(g, {f}, [&] { g.send(1, net::Cmd::Train, f, 1, 0, "foreign"); },
              "the new owner: Train off the menu changes nothing");
    g.send(1, net::Cmd::Train, f, 1, 0, "sol");
    g.ticks(600);
    check(g.count("sol", 1) >= 1 && g.count("foreign", 1) == 0, "the new owner trains from the factory's menu");
}
} // namespace

int main() {
    const Registry reg;
    if (!reg("sol") || !reg("foreign") || !reg("factory") || reg.reg.buildable("factory") != std::vector<std::string>{"sol"} ||
        reg("builder")->buildMenu.size() != 3 || reg("workshop")->buildMenu.size() != 2 || !reg("lode")->onMana) {
        std::printf("FAIL fixture: synthetic registry did not load\n");
        return 1;
    }
    loadRefusals(reg);
    loadAccepted(reg);
    restrictedBuilder(reg);
    factoryProduction(reg);
    restrictionsAndCaps(reg);
    unrestrictedAndGifted(reg);
    std::printf(failures ? "FAIL: %d\n" : "PASS: command validation\n", failures);
    return failures ? 1 : 0;
}
