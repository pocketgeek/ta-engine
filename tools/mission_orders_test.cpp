// mission_orders_test -- the campaign order strings (InitialMission= and the god
// script's SetMission) read the way retail's parser 0x513fb0 reads them.
//
// Each clause is cut at a comma and scanned on its own, so no clause reads into
// the next; and the letters mean what they mean in retail:
//   o A       2 offensive / 1 defensive / 0 passive (0x5198a0); o A B the fields
//   b TYPE N  a factory builds N (BuildingBuild); b TYPE N X Y a mobile builder
//             builds TYPE at X,Y (MobileBuild) -- neither spawns anything free
//   i NAME    start aboard the transport whose Ident= is NAME (51b4f0)
//   g NAME / wa NAME / a NAME  the placed unit NAME (Ident=, else Unitname)
//   wa        hold until the watched unit is HIT (WaitForAttack 4017a0)
//   w F N     a wait that a visible enemy within N cells ends (Wait 401480)
//   v F       a fixed veterancy multiplier (unit +0xf4): rank 10, kills can't move it
//   a TYPE    AttackUType: every unit of TYPE, one after another, until none
//   a X Y     the click's armed Attack on the ground at X*16,Y*16 (SUPPRESS)
//   s / d     no owner change / a self-destruct queued in its place
//   m/ma/p/u  the point X*16,Y*16 (16.16, X*65536 chopped first), no half cell
//
// And where units come into a mission, and the triggers that see them:
//   [units]   XPos*16 with no half cell; a building snapped by 5085c0 so XPos is
//             its centre cell; Angle the raw retail heading word (514b40)
//   Create    the type's footprint corner at the cell, birth heading (4d35c0)
//   SetTrigger  cells of the footprint origin, inclusive; circle max+min/4;
//             once per unit per region; an armed slot is not re-armed (4d33a0)
//   UnitCreated once per unit that enters play, with its owner byte (4d33a0)
//
// Synthetic types from a scratch FBI tree, on a flat map. With a retail install
// (`mission_orders_test --data <dir>`) it checks shipped missions instead: takx04's
// Flying Builder builds its shipyard when the lodestone JIM is attacked rather than
// conjuring it at start, its keeps queue their waves, and takx07's soldiers start
// aboard their transports.

#include "ai/classicbrain.h"
#include "sim/mission.h"
#include "sim/sim.h"
#include "sim/matchsetup.h"
#include "sim/footprint.h"
#include "sim/retailcampaignconditions.h"
#include "hpi/hpi.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

using namespace tak;
using sim::Fixed;
using sim::Order;
using sim::ScriptedOrder;
using sim::Unit;
using sim::World;

static int g_fail = 0;
static void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("  %-70s %s%s%s\n", what.c_str(), ok ? "ok" : "FAIL",
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++g_fail;
}

static void tickN(World& w, int n) { for (int i = 0; i < n; ++i) w.tick(1.0f / 30.0f); }

static float cell(float c) { return c * 16.0f + 8.0f; }   // a cell's centre in world px

static const ScriptedOrder* scripted(const World& w, const Order& o) { return w.scriptedOrder(o); }
static bool isKind(const World& w, const Order& o, ScriptedOrder::Kind k) {
    const ScriptedOrder* s = scripted(w, o);
    return s && s->kind == k;
}

// Kill `id` as a weapon would: no hp left, the last hit from `killer`.
static void kill(World& w, int id, int killer) {
    Unit* u = w.unit(id);
    const Unit* k = w.unit(killer);
    u->hp = Fixed(); u->deathType = 1;
    u->lastHitBy = killer; u->lastHitPlayer = k ? k->player : -1;
}

// ---- the scratch data --------------------------------------------------------

struct Fixture {
    std::filesystem::path root;
    hpi::Vfs vfs;
    sim::TypeRegistry reg;
    Fixture() {
        root = std::filesystem::temp_directory_path() /
               ("tak-mission-orders-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root / "units");
        std::filesystem::create_directories(root / "canbuild" / "builder");
        std::filesystem::create_directories(root / "canbuild" / "fort");
        std::filesystem::create_directories(root / "canbuild" / "gunner");
        auto fbi = [&](const char* name, const std::string& body, const std::string& weapon = {}) {
            std::ofstream f(root / "units" / (std::string(name) + ".fbi"));
            f << "[UNITINFO]\n{\nunitname=" << name << ";\nobjectname=" << name << ";\n" << body << "}\n" << weapon;
        };
        const std::string mobile = "canmove=1;\nmaxvelocity=1.5;\nturnrate=2000;\nturninplacerate=2000;\n"
                                   "footprintx=2;\nfootprintz=2;\nsightdistance=200;\nmaxdamage=500;\n";
        const std::string melee = "[WEAPON1]\n{\nname=Blade;\ntype=Melee;\nrange=30;\nreloadtime=1;\n"
                                  "[DAMAGE]\n{\ndefault=100;\n}\n}\n";
        const auto ranged = [](const char* extra) {
            return std::string("[WEAPON1]\n{\nname=Bow;\nrange=400;\nreloadtime=1;\nvelocity=400;\n") + extra +
                   "[DAMAGE]\n{\ndefault=50;\n}\n}\n";
        };
        fbi("soldier", mobile + "buildtime=60;\nbuildcost=10;\n", melee);
        fbi("archer", mobile, ranged(""));
        fbi("flak", mobile, ranged("toairweapon=1;\n"));
        fbi("tower", "footprintx=2;\nfootprintz=2;\nmaxdamage=2000;\nsightdistance=400;\n", ranged(""));
        // An armed factory (no shipped one has a [WEAPON1]; a mod's could).
        fbi("fort", "footprintx=4;\nfootprintz=4;\nmaxdamage=5000;\nsightdistance=400;\nbuilder=1;\nworkertime=10;\n",
            ranged(""));
        // A mobile one with a build menu and no bmcode (0, a building's): the
        // load strips its canattack as a factory's (4c18d7..4c18ff).
        fbi("gunner", mobile + "sightdistance=600;\nbuilder=1;\nworkertime=10;\n", ranged(""));
        const std::string flyer = mobile + "canfly=1;\ncruisealt=80;\nacceleration=0.2;\nbrakerate=0.2;\n";
        fbi("hawk", flyer, ranged(""));
        fbi("hoverer", flyer + "hoverattack=1;\n", ranged(""));
        fbi("bomber", flyer, ranged("dropped=1;\n"));
        fbi("target", mobile);
        fbi("stubborn", mobile + "unitstandorders=0;\n", melee);
        fbi("plain", mobile + "noveteran=1;\n", melee);
        fbi("builder", mobile + "builder=1;\nworkertime=10;\nbuilddistance=60;\n");
        fbi("wagon", "canmove=1;\nmaxvelocity=2;\nturnrate=4000;\nturninplacerate=4000;\nfootprintx=3;\n"
                     "footprintz=3;\nmaxdamage=800;\ncantransport=1;\ntransportcapacity=4;\ntransportsize=4;\n"
                     "transportsizecapacity=16;\ntransportdistance=150;\n");
        fbi("keep", "footprintx=4;\nfootprintz=4;\nmaxdamage=5000;\nbuilder=1;\nworkertime=10;\n");
        fbi("hut", "footprintx=2;\nfootprintz=2;\nmaxdamage=1000;\nbuildtime=60;\nbuildcost=10;\n");
        std::ofstream(root / "canbuild" / "builder" / "hut.tdf") << "[Menu]\n{\nPriority=1;\n}\n";
        std::ofstream(root / "canbuild" / "fort" / "soldier.tdf") << "[Menu]\n{\nPriority=1;\n}\n";
        std::ofstream(root / "canbuild" / "gunner" / "hut.tdf") << "[Menu]\n{\nPriority=1;\n}\n";
        vfs.addLayer(hpi::MountSet(root));
        reg.loadDir(vfs, "units");
        reg.loadBuildTree(vfs, "canbuild");
    }
    ~Fixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    const sim::UnitType* t(const char* name) const { return reg.find(name); }
};

static std::unique_ptr<World> makeWorld() {
    auto w = std::make_unique<World>();
    w->setTerrain(std::vector<uint8_t>(size_t(128) * 128, 100), 128, 128, 20);
    w->setPathService(true);
    w->player(0).team = 0; w->player(1).team = 1;
    w->player(0).mana = 1e6; w->player(1).mana = 1e6;
    return w;
}

// One placed unit of a mission: its type, cell, owner, Ident= and InitialMission=.
struct Placed { const char* type; int cx, cz, player; const char* ident; const char* orders; int id = 0; };

// A GlobalHeader with a [Map Data] section, as every shipped mission has: its
// order points are X*16 (retail's scenario +0xdc5 is 16; 1 without the section).
static tdf::Node mapDataHeader() {
    tdf::Node header;
    header.children["map data"];
    header.childOrder.push_back("map data");
    return header;
}

// The MakeSelectable a parse appends (514a7d) at the end of a unit's queue.
// The checks of the other clauses look at the queue the clauses themselves
// made, so startMission takes it off unless asked to keep it (selectable()).
static void dropMakeSelectable(World& w, int id) {
    Unit* u = w.unit(id);
    if (!u || u->orders.empty() || !isKind(w, u->orders.back(), ScriptedOrder::Kind::MakeSelectable)) return;
    u->orders.pop_back();
    u->selectable = true;
}

// Spawn `placed` in file order and start a mission whose only script is their
// InitialMission strings.
static std::unique_ptr<sim::MissionScript> startMission(Fixture& f, World& w, std::vector<Placed>& placed,
                                                        const tdf::Node& header = mapDataHeader(),
                                                        bool keepSelectable = false) {
    std::vector<sim::MissionScript::Placement> table;
    std::vector<std::pair<int, std::string>> orders;
    for (auto& p : placed) {
        p.id = w.spawn(f.t(p.type), cell(float(p.cx)), cell(float(p.cz)), 0.0f, p.player);
        table.push_back({p.ident, p.type, p.id});
        if (p.orders && *p.orders) orders.push_back({p.id, p.orders});
    }
    auto m = std::make_unique<sim::MissionScript>(std::vector<uint8_t>{}, header, f.reg, 0, "orders fixture");
    m->setInitialOrders(std::move(orders));
    m->setPlacements(std::move(table));
    m->start(w);
    if (!keepSelectable) for (const auto& p : placed) dropMakeSelectable(w, p.id);
    return m;
}

// A god script whose Start runs `SetMission <orders>` on `unitId` (MAP_COMMAND 0, one argument).
static std::vector<uint8_t> setMissionCob(int unitId, const std::string& orders) {
    const std::string name = "SetMission " + orders;
    const std::vector<uint32_t> code = {0x10021001, uint32_t(unitId), 0x10073000, 0, 1, 0x10024000,
                                        0x10021001, 0, 0x10065000};
    std::vector<uint8_t> cob(size_t(160 + name.size() + 1 + code.size() * 4));
    auto put = [&](size_t offset, uint32_t value) { std::memcpy(cob.data() + offset, &value, 4); };
    const size_t codeAt = 160 + ((name.size() + 4) & ~size_t(3));
    cob.resize(codeAt + code.size() * 4);
    put(0, 6); put(4, 1); put(12, uint32_t(code.size())); put(24, 52); put(28, 56);
    put(36, uint32_t(codeAt)); put(44, 60); put(48, 1); put(52, 0); put(56, 72); put(60, 80);
    std::memcpy(cob.data() + 72, "Start", 6);
    std::memcpy(cob.data() + 80, name.c_str(), name.size() + 1);
    for (size_t i = 0; i < code.size(); ++i) put(codeAt + i * 4, code[i]);
    return cob;
}

// ---- 1. clauses end at commas -------------------------------------------------

static void clauses(Fixture& f) {
    std::printf("a clause ends at its comma:\n");
    auto w = makeWorld();
    std::vector<Placed> p = {
        {"soldier", 20, 20, 0, "", "ma, a target"},
        {"keep", 40, 20, 0, "", "w 900 40 b soldier 1"},
        {"target", 90, 90, 1, "", ""},
    };
    auto m = startMission(f, *w, p);
    const Unit* s = w->unit(p[0].id);
    check(s->orders.size() == 1 && isKind(*w, s->orders.front(), ScriptedOrder::Kind::AttackType),
          "\"ma\" with no point does nothing; \"a target\" still reads",
          std::to_string(s->orders.size()) + " order(s)");
    const Unit* k = w->unit(p[1].id);
    const ScriptedOrder* wait = k->orders.size() == 1 ? scripted(*w, k->orders.front()) : nullptr;
    check(wait && wait->kind == ScriptedOrder::Kind::WaitNear && wait->radius == 640 && wait->count == 27000,
          "\"w 900 40 b soldier 1\" is one wait of 900 s within 40 cells",
          std::to_string(k->orders.size()) + " order(s)");
    const size_t before = w->units().size();
    tickN(*w, 90);
    check(w->units().size() == before && w->unit(p[1].id)->buildQueue.empty(),
          "and the \"b soldier 1\" in its tail neither spawns nor builds");
}

// ---- 2. o: the standing orders ------------------------------------------------

static void standing(Fixture& f) {
    std::printf("o A / o A B:\n");
    auto w = makeWorld();
    std::vector<Placed> p = {
        {"soldier", 10, 10, 0, "", "o 2"},
        {"soldier", 14, 10, 0, "", "o 1"},
        {"soldier", 18, 10, 0, "", "o 0"},
        {"soldier", 22, 10, 0, "", "o 1 2"},
        {"stubborn", 26, 10, 0, "", "o 0"},
        {"soldier", 30, 10, 0, "", "o 3"},
    };
    auto m = startMission(f, *w, p);
    auto fields = [&](int i) {
        const Unit* u = w->unit(p[size_t(i)].id);
        return std::to_string(u->standingOrder) + "/" + std::to_string(u->moveState) + "/" + std::to_string(u->fireState);
    };
    check(fields(0) == "2/1/2", "o 2 is Offensive: maneuver, fire at will (it was Passive)", fields(0));
    check(fields(1) == "1/0/2", "o 1 is Defensive: hold position, fire at will", fields(1));
    check(fields(2) == "0/0/0", "o 0 is Passive: hold position, hold fire (it was Offensive)", fields(2));
    const std::string spawned = fields(5);
    check(fields(3) == spawned.substr(0, 1) + "/1/2", "o 1 2 sets the move and fire fields, not the shown order",
          fields(3));
    check(fields(4) == "0/0/0", "a type without stance buttons still takes a script's o", fields(4));
    // The soldier type sets no standingunitorder: retail's parser stores its
    // composite as 2 (4c00a7), and o 3 leaves it there.
    check(w->unit(p[5].id)->standingOrder == w->unit(p[5].id)->type->defaultStandingOrder &&
          w->unit(p[5].id)->standingOrder == 2, "o 3 is out of range and does nothing", fields(5));
}

// ---- 3. b: real build orders ----------------------------------------------------

static void builds(Fixture& f) {
    std::printf("b TYPE N (a factory) / b TYPE N X Y (a builder):\n");
    auto w = makeWorld();
    std::vector<Placed> p = {
        {"keep", 30, 30, 0, "", "b soldier 2, w 1, b soldier 1"},
        {"builder", 50, 30, 0, "", "b hut 0 60 30"},
        {"builder", 50, 40, 0, "", "b soldier 0 60 40, b hut 0 60 44"},
    };
    auto m = startMission(f, *w, p);
    const Unit* b = w->unit(p[1].id);
    const Order* build = b->orders.empty() ? nullptr : &b->orders.front();
    check(build && build->buildType == f.t("hut"), "the builder gets a build order, not a hut out of nowhere");
    if (build)
        check(sim::footprintOrigin(build->buildX, 2) == 59 && sim::footprintOrigin(build->buildZ, 2) == 29,
              "at retail's site: X*16 snapped to the 2x2 footprint (origin 59,29)",
              std::to_string(sim::footprintOrigin(build->buildX, 2)) + "," +
              std::to_string(sim::footprintOrigin(build->buildZ, 2)));
    const Unit* c = w->unit(p[2].id);
    check(c->orders.size() == 1 && c->orders.front().buildType == f.t("hut"),
          "a type off the builder's menu is refused (519850); the next clause still reads");
    const Unit* keep = w->unit(p[0].id);
    check(keep->orders.size() == 3 && isKind(*w, keep->orders[0], ScriptedOrder::Kind::Train) &&
          keep->orders[1].wait > 0 && isKind(*w, keep->orders[2], ScriptedOrder::Kind::Train),
          "the keep queues build 2, wait, build 1", std::to_string(keep->orders.size()) + " order(s)");
    tickN(*w, 2);
    check(w->unit(p[0].id)->buildQueue.size() == 2, "the first build puts 2 soldiers in the queue, not 3",
          std::to_string(w->unit(p[0].id)->buildQueue.size()));
    auto soldiers = [&] {
        int n = 0;
        for (const auto& u : w->units()) if (u.alive() && u.type == f.t("soldier") && !u.underConstruction) ++n;
        return n;
    };
    check(soldiers() == 0, "nobody arrives free at the start");
    int first = -1, third = -1;
    for (int t = 0; t < 3000 && third < 0; ++t) {
        tickN(*w, 1);
        if (first < 0 && soldiers() == 2) first = t;
        if (soldiers() == 3) third = t;
    }
    check(first > 0 && third > first + 30, "it builds two, waits, then builds the third",
          "two at tick " + std::to_string(first) + ", three at " + std::to_string(third));
    check(w->unit(p[0].id)->orders.empty() && w->unit(p[0].id)->buildQueue.empty(), "and its script is spent");
    bool hut = false;
    for (const auto& u : w->units()) hut = hut || (u.type == f.t("hut") && u.alive());
    check(hut, "the builder walks there and builds the hut");
}

// ---- 3b. the point of m / ma / p / u ------------------------------------------------

static void points(Fixture& f) {
    std::printf("m / ma / p / u X Y: the point X*16 (16.16, chopped), no half cell:\n");
    // 0x513fb0: ftol(X * 65536.0) * scenario +0xdc5, emulated (tools/re/check_mission_points.py).
    auto raw = [](const Fixed& v) { return std::to_string(v.v); };
    // A ground move keeps the point it was given as its click point; its leg may
    // already carry a route's waypoint. An unload keeps it as the order's point.
    auto px = [](const Order* o) { return o->unload ? o->x : o->clickX; };
    auto pz = [](const Order* o) { return o->unload ? o->z : o->clickZ; };
    auto at = [&](const Order* o, int32_t x, int32_t z) { return o && px(o).v == x && pz(o).v == z; };
    auto shown = [&](const Order* o) { return o ? raw(px(o)) + "," + raw(pz(o)) : std::string("none"); };
    {
        auto w = makeWorld();
        std::vector<Placed> p = {
            {"soldier", 20, 20, 0, "", "m 30 40"},
            {"soldier", 24, 20, 0, "", "ma 10.5 3.25"},
            {"soldier", 28, 20, 0, "", "p 7.3 9.99 4"},
            {"wagon", 40, 20, 0, "", "u 45 30"},
        };
        auto m = startMission(f, *w, p);
        auto last = [&](int i) -> const Order* {
            const Unit* u = w->unit(p[size_t(i)].id);
            return u && !u->orders.empty() ? &u->orders.back() : nullptr;
        };
        const Order* mv = last(0);
        check(at(mv, 480 << 16, 640 << 16), "m 30 40 goes to pixel 480,640 (it was the cell centre 488,648)", shown(mv));
        const Order* fight = last(1);
        check(at(fight, 168 << 16, 52 << 16), "ma 10.5 3.25 keeps the fraction: pixel 168,52", shown(fight));
        // 514120: both push the kind-2 move; ma passes -1 where m passes 0, the
        // move's response mode +0x52, as the fight-move key does (E,
        // tools/re/check_mission_move_fight.py).
        check(fight && fight->attackMove && fight->groundResponse.mode == -1 && mv && !mv->attackMove &&
              mv->groundResponse.mode == 0, "ma is the move with response mode -1 (the fight-move), m mode 0");
        // float 7.3 * 65536 = 478412.79 -> 478412, x16; 9.99 -> 654704, x16.
        const Order* patrol = last(2);
        check(at(patrol, 478412 * 16, 654704 * 16) && patrol->patrol,
              "p 7.3 9.99 chops X*65536 before the x16 (7654592,10475264 raw)", shown(patrol));
        const Unit* wagon = w->unit(p[3].id);
        const Order* drop = nullptr;
        for (const Order& o : wagon->orders) if (o.unload) drop = &o;
        check(at(drop, 720 << 16, 480 << 16), "u 45 30 unloads at pixel 720,480", shown(drop));
    }
    {
        // A scenario without [Map Data] keeps +0xdc5 at 1 (4c8748): the point is X pixels.
        auto w = makeWorld();
        std::vector<Placed> p = {{"soldier", 20, 20, 0, "", "m 30 40"}};
        auto m = startMission(f, *w, p, tdf::Node{});
        const Unit* u = w->unit(p[0].id);
        const Order* mv = u->orders.empty() ? nullptr : &u->orders.back();
        check(at(mv, 30 << 16, 40 << 16), "without [Map Data] m 30 40 is pixel 30,40", shown(mv));
    }
    check(sim::MissionScript::orderPoint(-0.3f, 16) == float(-19660 * 16) / 65536.0f,
          "a negative fraction chops toward zero (-0.3 -> -19660 raw, x16)");
}

// ---- 4. i / u: troops start aboard ------------------------------------------------

static void aboard(Fixture& f) {
    std::printf("i NAME / u X Y:\n");
    auto w = makeWorld();
    std::vector<Placed> p = {
        {"wagon", 20, 20, 0, "beta", ""},
        {"wagon", 20, 30, 0, "alpha", "u 45 30"},
        {"soldier", 60, 60, 0, "", "i alpha, m 50 24"},
        {"soldier", 64, 60, 0, "", "i wagon"},
        {"soldier", 68, 60, 0, "", "i nobody, m 70 70"},
    };
    auto m = startMission(f, *w, p);
    const Unit* a = w->unit(p[2].id);
    check(a->inTransport == p[1].id, "i alpha boards the wagon whose Ident= is alpha");
    check(a->orders.size() == 1 && a->orders.front().goal, "and its later move waits aboard");
    check(w->unit(p[3].id)->inTransport == p[0].id, "i wagon falls back to the first record named wagon");
    check(!w->unit(p[4].id)->embarked() && w->unit(p[4].id)->orders.size() == 1,
          "a NAME nothing answers to does nothing");
    check(!w->unit(p[1].id)->orders.empty() && w->unit(p[1].id)->orders.back().unload,
          "the wagon's unload is queued though it was empty when it was read");
    bool out = false;
    for (int t = 0; t < 4000 && !out; ++t) { tickN(*w, 1); out = !w->unit(p[2].id)->embarked(); }
    check(out, "the wagon unloads it");
    for (int t = 0; t < 2000; ++t) {
        tickN(*w, 1);
        const Unit* u = w->unit(p[2].id);
        if (std::hypot(u->x.toFloat() - cell(50), u->z.toFloat() - cell(24)) < 40) break;
    }
    const Unit* u = w->unit(p[2].id);
    check(std::hypot(u->x.toFloat() - cell(50), u->z.toFloat() - cell(24)) < 40,
          "then its own move runs", std::to_string(int(u->x.toFloat())) + "," + std::to_string(int(u->z.toFloat())));
}

// ---- 5. g / wa NAME / wa: names and the WaitForAttack -------------------------------

static void names(Fixture& f) {
    std::printf("g NAME / wa NAME / wa:\n");
    auto w = makeWorld();
    std::vector<Placed> p = {
        {"soldier", 20, 20, 0, "boss", ""},
        {"soldier", 24, 20, 0, "", "i boss"},
        {"soldier", 28, 20, 0, "", "g boss"},
        {"target", 30, 40, 0, "bait", ""},
        {"target", 40, 40, 0, "", "wa bait, m 40 50"},   // unarmed: they never pick a fight
        {"target", 44, 40, 0, "", "wa, m 44 50"},
        {"target", 48, 40, 0, "", "wa nobody, m 48 50"},
        {"target", 52, 40, 0, "", "w 1, wa, m 52 50"},
        {"target", 46, 44, 1, "", ""},           // an enemy in sight that never strikes
    };
    auto m = startMission(f, *w, p);
    const Unit* g = w->unit(p[2].id);
    check(!g->orders.empty() && g->orders.front().guard && g->orders.front().targetId == p[0].id,
          "g boss guards the unit whose Ident= is boss, not one an i clause named");
    check(!w->unit(p[6].id)->orders.empty() && !w->unit(p[6].id)->orders.front().waitAttack,
          "wa with a NAME nothing answers to is dropped");
    w->unit(p[7].id)->attackedTick = w->tickCount() + 1;   // hit during its first wait
    tickN(*w, 20);
    auto waiting = [&](int i) { const Unit* u = w->unit(p[size_t(i)].id); return !u->orders.empty() && u->orders.front().waitAttack; };
    check(waiting(4) && waiting(5), "an enemy in sight does not end a wa (it used to)");
    tickN(*w, 30);
    check(!waiting(7), "a hit that came before the wa's turn ends it when it comes up");
    // Strike the bait: only the wa watching it ends.
    const int striker = w->spawn(f.t("soldier"), cell(31), cell(41), 0.0f, 1);
    w->attack(striker, p[3].id, false);
    bool released = false;
    for (int t = 0; t < 600 && !released; ++t) { tickN(*w, 1); released = !waiting(4); }
    check(released && waiting(5), "wa bait ends when bait is hit; wa (itself) holds on");
    // The watched unit dying ends it too.
    std::vector<Placed> q = {
        {"target", 80, 80, 0, "ward", ""},
        {"target", 84, 80, 0, "", "wa ward, m 84 90"},
    };
    auto w2 = makeWorld();
    auto m2 = startMission(f, *w2, q);
    tickN(*w2, 10);
    check(!w2->unit(q[1].id)->orders.empty() && w2->unit(q[1].id)->orders.front().waitAttack, "wa ward holds");
    kill(*w2, q[0].id, 0);
    tickN(*w2, 3);
    check(!w2->unit(q[1].id)->orders.empty() && !w2->unit(q[1].id)->orders.front().waitAttack,
          "and ends when ward dies");
}

// ---- 6. w F N -------------------------------------------------------------------------

static void waitNear(Fixture& f) {
    std::printf("w F N:\n");
    auto w = makeWorld();
    std::vector<Placed> p = {
        {"target", 30, 30, 0, "", "w 100 5, m 30 40"},
        {"target", 60, 30, 0, "", "w 100 1, m 60 40"},
        {"target", 33, 30, 1, "", ""},            // 48 px away: inside 5 cells
        {"target", 63, 30, 1, "", ""},            // 48 px away: outside 1 cell
    };
    auto m = startMission(f, *w, p);
    tickN(*w, 60);
    auto waiting = [&](int i) { const Unit* u = w->unit(p[size_t(i)].id); return !u->orders.empty() && u->orders.front().wait > 0; };
    check(!waiting(0), "a visible enemy within N cells ends the wait early (N was ignored)");
    check(waiting(1), "one beyond N cells does not");
}

// ---- 7. v F ----------------------------------------------------------------------------

static void veterancy(Fixture& f) {
    std::printf("v F:\n");
    auto w = makeWorld();
    std::vector<Placed> p = {
        {"soldier", 20, 20, 0, "", "v 2.5"},
        {"plain", 24, 20, 0, "", "v 3"},
        {"target", 21, 20, 1, "", ""},
    };
    auto m = startMission(f, *w, p);
    const Unit* v = w->unit(p[0].id);
    check(std::fabs(v->vetMul() - 2.5f) < 1e-6f && v->veteran == 10, "v 2.5: multiplier 2.5, rank 10",
          std::to_string(v->vetMul()) + ", rank " + std::to_string(v->veteran));
    check(w->unit(p[1].id)->vetMul() == 1.0f && w->unit(p[1].id)->veteran == 0, "a noveteran type ignores it");
    kill(*w, p[2].id, p[0].id);
    tickN(*w, 2);
    check(w->unit(p[0].id)->veteran == 10 && w->unit(p[0].id)->vetMul() == 2.5f, "a kill moves neither");
}

// ---- 8. a TYPE / a NAME / a X Y ---------------------------------------------------------

static void attacks(Fixture& f) {
    std::printf("a TYPE / a NAME / a X Y:\n");
    auto w = makeWorld();
    std::vector<Placed> p = {
        {"soldier", 60, 60, 0, "", "o 0, a target"},   // holds fire: only the order strikes
        {"target", 62, 60, 0, "", ""},            // its own player's: never a mark
        {"target", 70, 60, 1, "", ""},
        {"target", 60, 95, 1, "", ""},
        {"target", 20, 60, 1, "chief", ""},
        {"soldier", 90, 90, 0, "", "a chief"},
        {"archer", 100, 20, 0, "", "a 110 20"},
    };
    auto m = startMission(f, *w, p);
    const Unit* chief = w->unit(p[5].id);
    check(!chief->orders.empty() && chief->orders.front().targetId == p[4].id,
          "a chief attacks the unit whose Ident= is chief (it was read as a type)");
    const Unit* ground = w->unit(p[6].id);
    check(ground->orders.size() == 1 && ground->orders.back().suppress &&
          ground->orders.back().attackPointX.toFloat() == 1760.0f && ground->orders.back().attackPointZ.toFloat() == 320.0f,
          "a X Y fires at the point X*16, Y*16 (SUPPRESS; it walked into reach, half a cell off)",
          std::to_string(ground->orders.size()) + " order(s)");
    std::vector<int> struck;
    for (int t = 0; t < 4000 && struck.size() < 3; ++t) {
        tickN(*w, 1);
        const Unit* s = w->unit(p[0].id);
        if (!s->orders.empty() && s->orders.front().targetId && !s->orders.front().autoTarget) {
            const int mark = s->orders.front().targetId;
            struck.push_back(mark);
            kill(*w, mark, p[0].id);
            tickN(*w, 2);
        }
    }
    check(struck.size() == 3 && struck[0] == p[2].id, "a target attacks the nearest enemy target first",
          std::to_string(struck.size()) + " struck");
    check(std::find(struck.begin(), struck.end(), p[1].id) == struck.end(),
          "never one its own player owns");
    tickN(*w, 120);
    check(w->unit(p[0].id)->orders.empty() && w->unit(p[1].id)->alive(), "and stops when none is left");
}

// ---- 8b. a X Y is the click's armed Attack on the ground ---------------------------------
// 5143b5 asks 4de530 kind 3 with the point, as the click does, and pushes what it
// gives queued; when it gives nothing the clause does nothing and the next reads.

static void attackPoint(Fixture& f) {
    std::printf("a X Y (fire at a point):\n");
    auto w = makeWorld();
    std::vector<Placed> p = {
        {"tower", 40, 40, 0, "", "a 45 45"},
        {"flak", 20, 60, 0, "", "a 5 5, m 9 9"},
        {"soldier", 60, 20, 0, "", "a 62 20"},
        {"archer", 80, 80, 0, "", "a target, a 85 85"},
        {"archer", 30, 100, 0, "", "a 35 100"},
        {"target", 120, 120, 1, "", ""},
    };
    auto m = startMission(f, *w, p);
    const Unit* tower = w->unit(p[0].id);
    check(tower->orders.size() == 1 && tower->orders.front().suppress &&
          tower->orders.front().attackPointX.toFloat() == 720.0f,
          "a tower with a ranged weapon gets SUPPRESS (it got nothing)", std::to_string(tower->orders.size()) + " order(s)");
    const Unit* flak = w->unit(p[1].id);
    check(flak->orders.size() == 1 && !flak->orders.front().suppress && !flak->orders.front().attackMove &&
          std::fabs(flak->orders.front().x.toFloat() - cell(9)) <= 8.0f &&     // (a 2x2 mover's goal snaps
          std::fabs(flak->orders.front().z.toFloat() - cell(9)) <= 8.0f,       // to a cell corner)
          "a toairweapon gets no order, and the move after it still goes in",
          std::to_string(flak->orders.size()) + " order(s), the first at " +
          (flak->orders.empty() ? std::string("-") : std::to_string(flak->orders.front().x.toFloat()) + "," +
                                                     std::to_string(flak->orders.front().z.toFloat())));
    check(w->unit(p[2].id)->orders.empty(), "a melee weapon off a blocker gets no order");
    const Unit* both = w->unit(p[3].id);
    check(both->orders.size() == 2 && isKind(*w, both->orders[0], ScriptedOrder::Kind::AttackType) &&
          both->orders[1].suppress && both->orders[1].attackPointZ.toFloat() == 1360.0f,
          "a TYPE, a X Y: AttackType first, SUPPRESS behind it", std::to_string(both->orders.size()) + " order(s)");
    const Unit* gun = w->unit(p[4].id);
    const Fixed x0 = gun->x, z0 = gun->z;
    int shots = 0, towerShots = 0;
    for (int t = 0; t < 300; ++t) {
        tickN(*w, 1);
        if (w->unit(p[4].id)->firedWeapons) ++shots;
        if (w->unit(p[0].id)->firedWeapons) ++towerShots;
    }
    gun = w->unit(p[4].id);
    check(shots > 0 && gun->x == x0 && gun->z == z0 && gun->orders.size() == 1 && gun->orders.front().suppress,
          "in reach it fires at the point from where it stands, and keeps the order",
          std::to_string(shots) + " shot tick(s)");
    check(towerShots > 0, "and so does the tower", std::to_string(towerShots) + " shot tick(s)");
    // 4c18d7..4c18ff: an armed type with bmcode 0 and a canbuild menu loses
    // canattack at load, and the resolver (4de62d) then gives no order.
    {
        auto w2 = makeWorld();
        std::vector<Placed> q = {
            {"fort", 60, 60, 0, "", "a 70 60, m 64 70"},
            {"target", 120, 120, 1, "", ""},
        };
        auto m2 = startMission(f, *w2, q);
        const Unit* fort = w2->unit(q[0].id);
        check(fort->type->producesUnits() && !fort->type->weapons.empty() &&
              World::suppressRule(*fort->type, 0) == World::SuppressRule::None &&
              !w2->suppressAccepts(q[0].id, 70 * 16.0f, 60 * 16.0f),
              "an armed factory with a build menu gets no fire at a point (canattack cleared at load)");
        check(std::none_of(fort->orders.begin(), fort->orders.end(), [](const Order& o) { return o.suppress; }),
              "and a X Y puts no SUPPRESS on it", std::to_string(fort->orders.size()) + " order(s)");
        check(World::suppressRule(*f.t("tower"), 0) == World::SuppressRule::Fire,
              "an armed building with no build menu still fires at a point");
    }
    const auto seen = ai::ClassicBrain::firstMissionOf(*w, *gun);
    check(seen == std::pair<uint8_t, uint32_t>{1, 0x1000410},
          "the Retail AI reads it as SUPPRESS (row 0x5eb8b8: class 1, 0x1000410; it read a move)",
          std::to_string(seen.first) + " " + std::to_string(seen.second));
}

// ---- 8b'. m / p / u take X*16, Y*16 --------------------------------------------------------
// 514120, 514307 and 5141d3 scale the point as 5143b5 does: no half cell (E,
// tools/re/check_mission_attack_point.py).

static void clausePoints(Fixture& f) {
    std::printf("m X Y / p X Y / u X Y (the point):\n");
    auto w = makeWorld();
    std::vector<Placed> p = {
        {"target", 20, 20, 0, "", "m 50 40"},
        {"target", 30, 20, 0, "", "p 60 41"},
        {"wagon", 40, 20, 0, "", "u 70 42"},
    };
    auto m = startMission(f, *w, p);
    const Unit* mover = w->unit(p[0].id);
    check(!mover->orders.empty() && mover->orders.back().clickX.toFloat() == 800.0f &&
              mover->orders.back().clickZ.toFloat() == 640.0f,
          "m 50 40 moves to 800, 640 (it took the cell centre, 808, 648)",
          mover->orders.empty() ? "no order" : std::to_string(mover->orders.back().clickX.toFloat()));
    const Unit* patrol = w->unit(p[1].id);
    check(!patrol->orders.empty() && patrol->orders.back().patrol && patrol->orders.back().x.toFloat() == 960.0f &&
              patrol->orders.back().z.toFloat() == 656.0f,
          "p 60 41 patrols to 960, 656",
          patrol->orders.empty() ? "no order" : std::to_string(patrol->orders.back().x.toFloat()));
    const Unit* wagon = w->unit(p[2].id);
    check(!wagon->orders.empty() && wagon->orders.back().unload && wagon->orders.back().x.toFloat() == 1120.0f &&
              wagon->orders.back().z.toFloat() == 672.0f,
          "u 70 42 unloads at 1120, 672",
          wagon->orders.empty() ? "no order" : std::to_string(wagon->orders.back().x.toFloat()));
}

// ---- 8c. a X Y on a flyer: its air attack at the point -----------------------------------
// 4de770..4de7e3 gives AIRSTRIKE for a `dropped` weapon, AIRTOGROUNDHOVER for a
// hoverattack type, else AIRTOGROUND; the Retail AI reads that mission's class
// and flags (E, tools/re/check_air_point_mission.py), not a move's.

static void airPoint(Fixture& f) {
    std::printf("a X Y (a flyer):\n");
    auto w = makeWorld();
    std::vector<Placed> p = {
        {"bomber", 20, 20, 0, "", "a 40 40"},
        {"hoverer", 30, 20, 0, "", "a 40 40"},
        {"hawk", 40, 20, 0, "", "a 40 40"},
        {"target", 120, 120, 1, "", ""},
    };
    auto m = startMission(f, *w, p);
    const struct { int i; uint8_t kind; std::pair<uint8_t, uint32_t> mission; const char* what; } rows[] = {
        {0, Order::kAirStrike, {2, 0x1000400}, "a dropped weapon: AIRSTRIKE, read as class 2, 0x1000400 (it read a move)"},
        {1, Order::kAirHover, {1, 0x1000000}, "a hoverattack type: AIRTOGROUNDHOVER, class 1, 0x1000000"},
        {2, Order::kAirRun, {1, 0x1000000}, "else AIRTOGROUND, class 1, 0x1000000"},
    };
    for (const auto& r : rows) {
        const Unit* u = w->unit(p[size_t(r.i)].id);
        const auto seen = ai::ClassicBrain::firstMissionOf(*w, *u);
        check(u->orders.size() == 1 && u->orders.front().airPoint == r.kind &&
              u->orders.front().attackPointX.toFloat() == 640.0f && seen == r.mission, r.what,
              std::to_string(u->orders.size()) + " order(s), " + std::to_string(seen.first) + " " +
              std::to_string(seen.second));
    }
}

// ---- 8d. a flyer's attack on a unit and its patrol, as the Retail AI reads them -------------
// The resolver's unit branch (4de8b0..4de9ea) gives a flyer AIRTOAIR at a
// flyer, AIRSTRIKE on a `dropped` weapon, else AIRTOGROUNDHOVER / AIRTOGROUND;
// each keeps the target bit (0x1000200), AIRSTRIKE its 0x400 with the order's
// point (2, 0x1000600). A flyer's patrol is VTOL_PATROL (0x1000412, 0x4000 on
// the anchored leg). E, tools/re/check_air_point_mission.py.

static void airUnit(Fixture& f) {
    std::printf("a flyer at a unit, a flyer's patrol (the Retail AI's reading):\n");
    auto w = makeWorld();
    const int bomber = w->spawn(f.t("bomber"), 300, 300, 0, 0);
    const int hoverer = w->spawn(f.t("hoverer"), 340, 300, 0, 0);
    const int hawk = w->spawn(f.t("hawk"), 380, 300, 0, 0);
    const int dogfighter = w->spawn(f.t("hawk"), 420, 300, 0, 0);
    const int patroller = w->spawn(f.t("hawk"), 460, 300, 0, 0);
    const int walker = w->spawn(f.t("archer"), 500, 300, 0, 0);
    const int target = w->spawn(f.t("target"), 1200, 1200, 0, 1);
    const int enemyFlyer = w->spawn(f.t("hawk"), 1300, 1200, 0, 1);
    for (int id : {bomber, hoverer, hawk, walker}) w->attack(id, target, false);
    w->attack(dogfighter, enemyFlyer, false);
    w->patrol(patroller, 900, 300);
    const struct { int id; std::pair<uint8_t, uint32_t> mission; const char* what; } rows[] = {
        {bomber, {2, 0x1000600}, "a dropped weapon on a unit: AIRSTRIKE, class 2, 0x1000600 (it read 1, 0x1000280)"},
        {hoverer, {1, 0x1000200}, "a hoverattack type on a unit: AIRTOGROUNDHOVER, class 1, 0x1000200"},
        {hawk, {1, 0x1000200}, "else on a unit: AIRTOGROUND, class 1, 0x1000200"},
        {dogfighter, {1, 0x1000200}, "on a flyer: AIRTOAIR, class 1, 0x1000200"},
        {patroller, {7, 0x1004412}, "a flyer's patrol: VTOL_PATROL, class 7, 0x1004412 (it read Patrol's 0x3000410)"},
        {walker, {1, 0x1000280}, "a walker on a unit stays Attack_Chase, class 1, 0x1000280"},
    };
    for (const auto& r : rows) {
        const auto seen = ai::ClassicBrain::firstMissionOf(*w, *w->unit(r.id));
        char got[32]; std::snprintf(got, sizeof got, "%u %#x", unsigned(seen.first), unsigned(seen.second));
        check(seen == r.mission, r.what, got);
    }
}

// ---- 8e. an armed type stripped of canattack attacks no unit ---------------------------------
// 4c18d7..4c18ff clears canattack for a type with a weapon, bmcode 0 and a
// canbuild menu; the resolver's attack mode (4de62d) then gives no mission,
// for a click, a raw command, a mission's a NAME and an automatic attack
// (4d7de0) alike. A factory's attack on a unit stays a rally for its output
// (the rally rule).

static void strippedAttack(Fixture& f) {
    std::printf("an armed type stripped of canattack (a NAME, Cmd::Attack, automatic):\n");
    auto w = makeWorld();
    std::vector<Placed> p = {
        {"gunner", 60, 60, 0, "", "a foe"},
        {"target", 66, 60, 1, "foe", ""},
        {"fort", 80, 80, 0, "", ""},
        {"archer", 90, 60, 0, "", "a foe"},
    };
    auto m = startMission(f, *w, p);
    const auto attacking = [&](int id) {
        const auto& o = w->unit(id)->orders;
        return std::any_of(o.begin(), o.end(), [](const Order& q) { return q.targetId != 0; });
    };
    check(World::attackStrippedAtLoad(*f.t("gunner")) && World::attackStrippedAtLoad(*f.t("fort")) &&
              !World::attackStrippedAtLoad(*f.t("archer")) && !World::attackStrippedAtLoad(*f.t("tower")),
          "a weapon, bmcode 0 and a canbuild menu: canattack stripped at load");
    check(!attacking(p[0].id), "a foe puts no attack on it",
          std::to_string(w->unit(p[0].id)->orders.size()) + " order(s)");
    check(attacking(p[3].id), "and still does on an archer");
    net::Command c;
    c.kind = net::Cmd::Attack; c.player = 0; c.unitId = p[0].id; c.targetId = p[1].id;
    sim::applyCommand(*w, f.reg, c);
    check(!attacking(p[0].id), "Cmd::Attack gives it none either");
    c.unitId = p[2].id;
    sim::applyCommand(*w, f.reg, c);
    const Unit* fort = w->unit(p[2].id);
    check(fort->orders.empty() && fort->rally.size() == 1 && fort->rally.front().targetId == p[1].id,
          "a factory's attack on a unit is still its output's rally",
          std::to_string(fort->orders.size()) + " order(s), " + std::to_string(fort->rally.size()) + " rally");
    bool automatic = false;
    for (int t = 0; t < 300; ++t) { tickN(*w, 1); automatic = automatic || attacking(p[0].id); }
    check(!automatic, "nor does its scan give it one with an enemy in sight");
}

// ---- 9. s and d ----------------------------------------------------------------------------

static void selectAndDestruct(Fixture& f) {
    std::printf("s / d:\n");
    auto w = makeWorld();
    std::vector<Placed> p = {
        {"soldier", 20, 20, 1, "", "s, m 20 30"},
        {"soldier", 30, 20, 0, "", "w 1, d"},
    };
    auto m = startMission(f, *w, p);
    check(w->unit(p[0].id)->player == 1, "s leaves an enemy unit its owner's (it was handed to the player)");
    tickN(*w, 15);
    check(w->unit(p[1].id)->alive(), "d waits its turn behind w 1 (it used to kill at once)");
    tickN(*w, 40);
    const Unit* d = w->unit(p[1].id);
    check(!d->alive() && d->deathType == Unit::kDeathSelfDestruct, "then self-destructs");
}

// ---- 9b. the selectable bit ---------------------------------------------------------------
// 513fb0's end (514a7d): a parse that pushed a mission clears +0x130 & 0x20 and,
// unless a patrol, a given `a X Y`, `s` or `d` was among the clauses, appends
// MakeSelectable (401450), which sets it again when it comes up.

static bool makesSelectable(const World& w, const Unit& u) {
    return !u.orders.empty() && isKind(w, u.orders.back(), ScriptedOrder::Kind::MakeSelectable);
}

// The table tools/re/check_mission_selectable.py compares with retail's parser:
// each order string with the lookups answering yes (1) or no (0).
struct SelectableCase { const char* label; int answer; const char* subject; const char* orders; };
static const SelectableCase kSelectableCases[] = {
    // "yes": a mover / the named unit exists / the type is known; "no": the
    // resolver gives nothing (a building for m, u on a non-transport, a melee
    // unit's a X Y) or the name or type does not resolve.
    {"m 5 7", 1, "soldier", "m 5 7"}, {"m 5 7", 0, "tower", "m 5 7"},
    {"ma 5 7", 1, "soldier", "ma 5 7"}, {"ma 5 7", 0, "tower", "ma 5 7"},
    {"u 5 7", 1, "wagon", "u 5 7"}, {"u 5 7", 0, "soldier", "u 5 7"},
    {"p 5 7", 1, "soldier", "p 5 7"}, {"p 5 7", 0, "tower", "p 5 7"},
    {"p 5 7 9", 1, "soldier", "p 5 7 9"}, {"p 5 7 9", 0, "tower", "p 5 7 9"},
    {"a 5 7", 1, "archer", "a 5 7"}, {"a 5 7", 0, "soldier", "a 5 7"},
    {"a NAMEU", 1, "soldier", "a FOE"}, {"a NAMEU", 0, "soldier", "a NOBODY"},
    {"g NAMEU", 1, "soldier", "g FRIEND"}, {"g NAMEU", 0, "soldier", "g NOBODY"},
    {"w 30", 1, "soldier", "w 30"}, {"w 30", 0, "soldier", "w 30"},
    {"w 60 20", 1, "soldier", "w 60 20"}, {"w 60 20", 0, "soldier", "w 60 20"},
    {"wa", 1, "soldier", "wa"}, {"wa", 0, "soldier", "wa"},
    {"wa NAMEU", 1, "soldier", "wa FRIEND"}, {"wa NAMEU", 0, "soldier", "wa NOBODY"},
    {"b ARATYPE 2", 1, "fort", "b soldier 2"}, {"b ARATYPE 2", 0, "fort", "b nosuchtype 2"},
    {"b ARATYPE 1 5 7", 1, "builder", "b hut 1 60 60"}, {"b ARATYPE 1 5 7", 0, "builder", "b nosuchtype 1 60 60"},
    {"s", 1, "soldier", "s"}, {"s", 0, "soldier", "s"},
    {"d", 1, "soldier", "d"}, {"d", 0, "soldier", "d"},
    {"i NAMEU", 1, "soldier", "i CART"}, {"i NAMEU", 0, "soldier", "i NOBODY"},
    {"v 1.5", 1, "soldier", "v 1.5"}, {"v 1.5", 0, "soldier", "v 1.5"},
    {"o 2", 1, "soldier", "o 2"}, {"o 2", 0, "soldier", "o 2"},
    {"o 1 2", 1, "soldier", "o 1 2"}, {"o 1 2", 0, "soldier", "o 1 2"},
    {"c", 1, "soldier", "c"}, {"c", 0, "soldier", "c"},
    {"w 30, m 5 7", 1, "soldier", "w 30, m 5 7"}, {"w 30, m 5 7", 0, "tower", "w 30, m 5 7"},
    {"m 5 7, p 9 9", 1, "soldier", "m 5 7, p 9 9"}, {"m 5 7, p 9 9", 0, "tower", "m 5 7, p 9 9"},
    {"s, m 5 7", 1, "soldier", "s, m 5 7"}, {"s, m 5 7", 0, "tower", "s, m 5 7"},
    {"m 5 7, s", 1, "soldier", "m 5 7, s"}, {"m 5 7, s", 0, "tower", "m 5 7, s"},
    {"i NAMEU, m 5 7", 1, "soldier", "i CART, m 5 7"}, {"i NAMEU, m 5 7", 0, "tower", "i NOBODY, m 5 7"},
    {"v 1.0, o 2", 1, "soldier", "v 1.0, o 2"}, {"v 1.0, o 2", 0, "soldier", "v 1.0, o 2"},
    {"w 30, d", 1, "soldier", "w 30, d"}, {"w 30, d", 0, "soldier", "w 30, d"},
    {"a 5 7, m 9 9", 1, "archer", "a 5 7, m 9 9"}, {"a 5 7, m 9 9", 0, "hut", "a 5 7, m 9 9"},
    {"o 2, wa, ", 1, "soldier", "o 2, wa, "}, {"o 2, wa, ", 0, "soldier", "o 2, wa, "},
    {"c, w 30", 1, "soldier", "c, w 30"}, {"c, w 30", 0, "soldier", "c, w 30"},
};

// One case: the subject, plus FOE (an enemy), FRIEND (an own soldier) and
// CART (an own wagon) to name. A building's `m` is a rally only when it
// produces (ours counts that as retail's QMove push), so the "no" rows use a
// tower or a hut, which produce nothing (and a hut has no weapon for `a X Y`).
static void selectableRow(Fixture& f, const SelectableCase& c) {
    auto w = makeWorld();
    std::vector<Placed> p = {
        {c.subject, 20, 20, 0, "SUBJECT", c.orders},
        {"target", 40, 40, 1, "FOE", ""},
        {"soldier", 24, 20, 0, "FRIEND", ""},
        {"wagon", 16, 24, 0, "CART", ""},
    };
    auto m = startMission(f, *w, p, mapDataHeader(), true);
    const Unit* u = w->unit(p[0].id);
    // The parse's own MakeSelectable: those in the queue less the `s` clauses'.
    int made = 0, own = 0;
    for (const Order& o : u->orders) made += isKind(*w, o, ScriptedOrder::Kind::MakeSelectable);
    for (const char* q = c.orders; *q; ++q)
        if ((q == c.orders || q[-1] == ',' || q[-1] == ' ') && (*q == 's' || *q == 'S') &&
            (!q[1] || q[1] == ',' || q[1] == ' ')) ++own;
    std::printf("'%s' %d selectable=%d tail=%d\n", c.label, c.answer, int(u->selectable), int(made > own));
}

static void selectable(Fixture& f) {
    std::printf("the selectable bit:\n");
    auto w = makeWorld();
    std::vector<Placed> p = {
        {"soldier", 20, 20, 0, "", "w 1, m 24 20"},
        {"soldier", 20, 30, 0, "", "w 1, p 24 30"},
        {"soldier", 20, 40, 0, "", "s, w 1, m 24 40"},
        {"soldier", 20, 50, 0, "", "o 2, v 1.5, c"},
        {"soldier", 20, 60, 0, "", "w 1, d"},
    };
    auto m = startMission(f, *w, p, mapDataHeader(), true);
    const Unit* moved = w->unit(p[0].id);
    check(!moved->selectable && makesSelectable(*w, *moved) && moved->orders.size() == 3,
          "w 1, m: not selectable, MakeSelectable queued behind the move");
    check(!w->unit(p[1].id)->selectable && !makesSelectable(*w, *w->unit(p[1].id)),
          "a patrol holds MakeSelectable back");
    check(!w->unit(p[2].id)->selectable && !makesSelectable(*w, *w->unit(p[2].id)),
          "s, ...: the s is the only MakeSelectable, at the front");
    check(w->unit(p[3].id)->selectable && w->unit(p[3].id)->orders.empty(), "o, v and c push nothing: selectable");
    check(!w->unit(p[4].id)->selectable && w->unit(p[4].id)->orders.size() == 2, "w 1, d: no MakeSelectable");
    tickN(*w, 2);
    check(w->unit(p[2].id)->selectable, "the leading s comes up at once");
    tickN(*w, 300);
    check(w->unit(p[0].id)->selectable && w->unit(p[0].id)->orders.empty(),
          "the waited move done, MakeSelectable comes up: selectable again");
    check(!w->unit(p[1].id)->selectable, "the patroller never is");
    // The retail AI reads the bit (its assignment 40f262, member loops 40b3fd,
    // 40bc75, 40bd38, ready counts 40a7f7): the patroller is no candidate.
    {
        ai::ClassicBrain brain(0, 1);
        const auto units = brain.squadUnits(*w);
        auto flags = [&](int id) {
            for (const auto& c : units) if (c.id == id) return c.flags;
            return 0u;
        };
        check(!(flags(p[1].id) & ai::ClassicSquadUnit::kSelectable) && (flags(p[0].id) & ai::ClassicSquadUnit::kSelectable),
              "the retail AI sees the patroller unselectable, the mover selectable again");
    }
    // A change of owner makes a new object: selectable again (511b21).
    check(w->scenarioTransfer(p[1].id, 1) && w->unit(p[1].id)->selectable, "a unit that changes sides is selectable");
    // Move_Ground's fallback (402ece): an unselectable unit on a move does not
    // answer its last attacker; the same unit selectable does.
    for (int scripted = 0; scripted < 2; ++scripted) {
        auto v = makeWorld();
        const int a = v->spawn(f.t("archer"), cell(20), cell(20), 0.0f, 0);
        const int e = v->spawn(f.t("target"), cell(30), cell(20), 0.0f, 1);
        Unit* u = v->unit(a);
        u->lastHitBy = e; u->lastHitPlayer = 1;
        u->selectable = scripted == 0;
        const int found = v->retailMoveFallback(*u);
        check(scripted ? found == 0 : found == e,
              scripted ? "a scripted (unselectable) mover does not answer its attacker (402ece)"
                       : "the same mover selectable answers it");
    }
}

// ---- 10. a hold keeps its place --------------------------------------------------------------
// The leg behind a scripted hold must not start: a route installed for it replaces
// every order up to it (replaceLeg), the hold included.

static void holds(Fixture& f) {
    std::printf("a hold stays in front of the orders behind it:\n");
    auto w = makeWorld();
    std::vector<Placed> p = {
        {"builder", 20, 20, 0, "", "w 10, b hut 0 60 60"},
        {"builder", 20, 40, 0, "", "wa, b hut 0 60 80"},
        {"soldier", 20, 50, 0, "", "o 0, a target, m 90 50"},
        {"target", 120, 120, 1, "", ""},
    };
    auto m = startMission(f, *w, p);
    tickN(*w, 90);
    auto front = [&](int i) -> const Order* { const Unit* u = w->unit(p[size_t(i)].id); return u->orders.empty() ? nullptr : &u->orders.front(); };
    check(front(0) && front(0)->wait > 0 && w->unit(p[0].id)->orders.size() == 2,
          "a builder's wait is not replaced by the route to its site");
    check(front(1) && front(1)->waitAttack && w->unit(p[1].id)->orders.size() == 2,
          "nor its wa");
    const Unit* s = w->unit(p[2].id);
    check(s->orders.size() == 3 && isKind(*w, s->orders[1], ScriptedOrder::Kind::AttackType) && s->orders[0].targetId == p[3].id,
          "a target puts its attack in front and stays behind it, the move last",
          std::to_string(s->orders.size()) + " order(s)");
}

// ---- 11. SetMission ---------------------------------------------------------------------------

static void setMission(Fixture& f) {
    std::printf("SetMission:\n");
    auto w = makeWorld();
    const int unit = w->spawn(f.t("soldier"), cell(20), cell(20), 0.0f, 0);
    w->spawn(f.t("soldier"), cell(24), cell(20), 0.0f, 0);
    w->orderWait(unit, 100, false);
    sim::MissionScript m(setMissionCob(unit, "a target, wa bait"), tdf::Node{}, f.reg, 0, "setmission fixture");
    m.start(*w);
    m.step(*w, 1.0f / 30.0f);
    const Unit* u = w->unit(unit);
    check(u->orders.size() == 3 && u->orders[0].wait > 0 && isKind(*w, u->orders[1], ScriptedOrder::Kind::AttackType),
          "it queues behind what the unit has; its NAMEs resolve to nothing",
          std::to_string(u->orders.size()) + " order(s)");
    // It parses through 513fb0 as an InitialMission does (4d37f6): the attack
    // pushed, so the unit is not selectable and MakeSelectable follows.
    check(!u->selectable && u->orders.size() == 3 && isKind(*w, u->orders[2], ScriptedOrder::Kind::MakeSelectable),
          "and its pushed order clears the selectable bit, MakeSelectable behind it");
}

// ---- 12. a long mission does not run out of order slots -----------------------------------------
// Order::scripted is a 16-bit index; takx24's script hands out ~250 a minute, so
// the table would fill in about four hours and every later order would be lost.

static void tableCompacts(Fixture& f) {
    std::printf("a long mission keeps getting its orders:\n");
    auto w = makeWorld();
    const int keeper = w->spawn(f.t("soldier"), cell(20), cell(20), 0.0f, 0);
    const int busy = w->spawn(f.t("soldier"), cell(30), cell(20), 0.0f, 0);
    const int watched = w->spawn(f.t("target"), cell(40), cell(20), 0.0f, 1);
    w->orderWaitAttack(keeper, true, watched);
    for (int i = 0; i < 70000; ++i) w->orderAttackType(busy, f.t("target"), false);
    const Unit* k = w->unit(keeper);
    const Unit* b = w->unit(busy);
    const ScriptedOrder* hold = k->orders.size() == 1 ? scripted(*w, k->orders.front()) : nullptr;
    check(hold && hold->kind == ScriptedOrder::Kind::WaitAttack && hold->watched == watched,
          "an order given before the table filled keeps its state");
    check(b->orders.size() == 1 && isKind(*w, b->orders.front(), ScriptedOrder::Kind::AttackType),
          "and the 70000th order still arrives", std::to_string(b->orders.size()) + " order(s)");
}

// ---- 13. where placed and created units stand ------------------------------------------------------
// Emulated values (tools/re/check_mission_regions.py): retail 514b40 / 5085c0 for
// an .ota record, 4d35c0 for Create.

static sim::UnitType sized(int fx, int fz, int bmcode) {
    sim::UnitType t;
    t.footX = fx; t.footZ = fz; t.buildMovementCode = uint8_t(bmcode);
    return t;
}

static void placement() {
    std::printf("placed and created units:\n");
    auto px = [](int fx, int fz, int bm, int x, int z) {
        const auto p = sim::MissionScript::placedPoint(sized(fx, fz, bm), x, z);
        return std::to_string(p.x.v / 65536.0) + "," + std::to_string(p.z.v / 65536.0);
    };
    auto placedAt = [](int fx, int fz, int bm, int x, int z, int ex, int ez) {
        const auto p = sim::MissionScript::placedPoint(sized(fx, fz, bm), x, z);
        return p.x == Fixed::fromInt(ex) && p.z == Fixed::fromInt(ez);
    };
    check(placedAt(2, 2, 0, 10, 10, 160, 160), "a 2x2 building at XPos 10 stands at 160 (it was 168)", px(2, 2, 0, 10, 10));
    check(placedAt(3, 3, 0, 10, 10, 168, 168), "a 3x3 building at XPos 10 at 168", px(3, 3, 0, 10, 10));
    check(placedAt(4, 4, 0, 1, 1, 16, 16), "a 4x4 building at XPos 1 at 16", px(4, 4, 0, 1, 1));
    check(placedAt(5, 5, 0, 0, 0, 8, 8), "a 5x5 building at XPos 0 at 8", px(5, 5, 0, 0, 0));
    check(placedAt(1, 4, 0, 3, 20, 56, 320), "a 1x4 building at ZPos 20 at 320", px(1, 4, 0, 3, 20));
    check(placedAt(2, 3, 2, 10, 10, 160, 168), "a bmcode 2 is a building too", px(2, 3, 2, 10, 10));
    bool mobiles = true;
    for (int n = 1; n <= 5; ++n) for (int x : {0, 1, 7, 10, 20}) mobiles = mobiles && placedAt(n, n, 1, x, x, x * 16, x * 16);
    check(mobiles, "a mobile (bmcode 1) of any size stands at XPos*16 (it was +8)");
    check(sim::MissionScript::placedHeading(180) == 0x8000 && sim::MissionScript::placedHeading(0) == 0 &&
          sim::MissionScript::placedHeading(359) == 0xff49 && sim::MissionScript::placedHeading(90) == 0x4000 &&
          sim::MissionScript::placedHeading(-90) == 0xc000,
          "Angle is the raw heading word (A<<16)/360: 180 -> 0x8000, 359 -> 0xff49");
    check(sim::retailHeadingToPort(sim::MissionScript::placedHeading(180)).v == 0,
          "so Angle 180 faces the way an unturned unit does (it faced the other way)");
    auto createdAt = [](int fx, int fz, int bm, int x, int z, int ex, int ez) {
        const auto p = sim::MissionScript::createPoint(sized(fx, fz, bm), x, z);
        return p.x == Fixed::fromInt(ex) && p.z == Fixed::fromInt(ez);
    };
    check(createdAt(2, 2, 1, 10, 20, 176, 336), "Create puts a 2x2 mobile's corner at 10,20: 176,336 (it was 168,328)");
    check(createdAt(5, 1, 0, 0, 0, 40, 8), "and a 5x1 building's at 0,0: 40,8");
    check(createdAt(2, 2, 0, 3, -1, 64, 0), "a building's too, a negative cell included");
}

// A placed unit's Angle against the headings World gives its own units: 511990's
// birth heading 0x8000 is Angle 180, and the ground kernel 4d9ad0 turns a unit
// walking east to 0xc000 (Angle 270) and one walking west to 0x4000 (Angle 90), all
// E (tools/re/check_mission_facing.py).
static void facings(Fixture& f) {
    std::printf("Angle against the birth heading and the walking ones:\n");
    using sim::MissionScript;
    using sim::retailHeadingToPort;
    auto hex = [](uint16_t v) { char s[8]; std::snprintf(s, sizeof s, "%#06x", v); return std::string(s); };
    {
        auto w = makeWorld();
        const Unit* u = w->unit(w->spawn(f.t("soldier"), cell(20), cell(20), std::nullopt, 0));
        check(u->heading == retailHeadingToPort(MissionScript::placedHeading(180)),
              "a freshly built unit faces as Angle 180 does", hex(sim::portHeadingToRetail(u->heading)));
    }
    // Placed with the Angle as matchsetup places it (514d02's word over the birth
    // heading), a unit sent due east / west must need no turn: it walks the line
    // without leaving it and keeps that heading. With the wrong word it would
    // turn first and its turning arc would take it off the line.
    for (const auto& [angle, dx, name] : {std::tuple{270, 1, "east"}, std::tuple{90, -1, "west"}}) {
        auto w = makeWorld();
        const int id = w->spawn(f.t("soldier"), cell(64), 1040.0f, std::nullopt, 0);
        w->unit(id)->heading = retailHeadingToPort(MissionScript::placedHeading(angle));
        w->unit(id)->tickStartHeadingBam = uint16_t(w->unit(id)->heading.v);
        w->order(id, cell(float(64 + dx * 40)), 1040.0f, false);
        tickN(*w, 60);
        const Unit* u = w->unit(id);
        const bool along = (dx > 0 ? u->x > Fixed::fromFloat(cell(66)) : u->x < Fixed::fromFloat(cell(62))) &&
                           u->z == Fixed::fromInt(1040);
        check(along && u->speed > Fixed() && u->heading == retailHeadingToPort(MissionScript::placedHeading(angle)),
              std::string("a unit placed at Angle ") + std::to_string(angle) + " walks due " + name +
              " without turning", hex(sim::portHeadingToRetail(u->heading)) + " at " +
              std::to_string(u->x.toFloat()) + "," + std::to_string(u->z.toFloat()));
    }
    // And the other way round: Angle 90 sent east turns and leaves the line.
    {
        auto w = makeWorld();
        const int id = w->spawn(f.t("soldier"), cell(64), 1040.0f, std::nullopt, 0);
        w->unit(id)->heading = retailHeadingToPort(MissionScript::placedHeading(90));
        w->order(id, cell(104), 1040.0f, false);
        tickN(*w, 60);
        check(w->unit(id)->z != Fixed::fromInt(1040), "(a unit facing west sent east turns, off the line)");
    }
}

// ---- 14. trigger regions and UnitCreated -----------------------------------------------------------

// A cob from scripts and a name table (MAP_COMMAND indexes the names).
static std::vector<uint8_t> buildCob(const std::vector<std::pair<std::string, std::vector<uint32_t>>>& scripts,
                                     const std::vector<std::string>& names) {
    const size_t n = scripts.size(), m = names.size();
    const size_t entries = 52, scriptNames = entries + n * 4, nameTable = scriptNames + n * 4;
    // Header + both tables sized up front: growing a 52-byte vector here made GCC 16's
    // -O2 -Warray-bounds misread resize()'s default-append as a write past the old buffer.
    std::vector<uint8_t> cob(nameTable + m * 4);
    auto put = [&](size_t offset, uint32_t value) {
        if (cob.size() < offset + 4) cob.resize(offset + 4);
        std::memcpy(cob.data() + offset, &value, 4);
    };
    auto text = [&](const std::string& str) {
        const size_t at = cob.size();
        cob.resize(at + ((str.size() + 4) & ~size_t(3)));
        std::memcpy(cob.data() + at, str.c_str(), str.size() + 1);
        return uint32_t(at);
    };
    std::vector<uint32_t> code;
    for (size_t i = 0; i < n; ++i) {
        put(entries + i * 4, uint32_t(code.size()));
        code.insert(code.end(), scripts[i].second.begin(), scripts[i].second.end());
        put(scriptNames + i * 4, text(scripts[i].first));
    }
    for (size_t i = 0; i < m; ++i) put(nameTable + i * 4, text(names[i]));
    const size_t codeAt = cob.size();
    for (size_t i = 0; i < code.size(); ++i) put(codeAt + i * 4, code[i]);
    put(0, 6); put(4, uint32_t(n)); put(12, uint32_t(code.size())); put(24, uint32_t(entries));
    put(28, uint32_t(scriptNames)); put(36, uint32_t(codeAt)); put(44, uint32_t(nameTable)); put(48, uint32_t(m));
    return cob;
}

// TriggerHit(region, unit, owner) counts into "hits" and keeps the last call;
// UnitCreated(unit, owner) counts into "created" and sums the owners.
static std::vector<uint8_t> recorderCob() {
    enum : uint32_t { Const = 0x10021001, Local = 0x10021002, Pop = 0x10024000, Add = 0x10031000,
                      Map = 0x10073000, Return = 0x10065000 };
    const std::vector<std::string> names = {"ReadValue hits", "WriteValue hits", "WriteValue region",
        "WriteValue unit", "WriteValue owner", "ReadValue created", "WriteValue created",
        "ReadValue owners", "WriteValue owners", "ReadValue destroyed", "WriteValue destroyed",
        "WriteValue dunit"};
    const std::vector<uint32_t> hit = {Map, 0, 0, Const, 1, Add, Map, 1, 1, Pop,
        Local, 0, Map, 2, 1, Pop, Local, 1, Map, 3, 1, Pop, Local, 2, Map, 4, 1, Pop, Const, 0, Return};
    const std::vector<uint32_t> created = {Map, 5, 0, Const, 1, Add, Map, 6, 1, Pop,
        Map, 7, 0, Local, 1, Add, Map, 8, 1, Pop, Const, 0, Return};
    const std::vector<uint32_t> destroyed = {Map, 9, 0, Const, 1, Add, Map, 10, 1, Pop,
        Local, 0, Map, 11, 1, Pop, Const, 0, Return};
    return buildCob({{"TriggerHit", hit}, {"UnitCreated", created}, {"UnitDestroyed", destroyed}}, names);
}

static void creates(Fixture& f) {
    std::printf("Create:\n");
    auto w = makeWorld();
    sim::MissionScript m(std::vector<uint8_t>{}, tdf::Node{}, f.reg, 0, "create fixture");
    m.start(*w);
    const int id = m.command(*w, "Create soldier", {1, 10, 20});
    const Unit* u = w->unit(id);
    check(u && u->x == Fixed::fromInt(176) && u->z == Fixed::fromInt(336) && u->player == 1,
          "Create soldier 1 10 20 puts the 2x2's corner at cell 10,20 (it was its centre)",
          u ? std::to_string(u->x.toFloat()) + "," + std::to_string(u->z.toFloat()) : "none");
    const int keep = m.command(*w, "Create keep", {0, 3, 4});
    check(w->unit(keep) && w->unit(keep)->x == Fixed::fromInt(80) && w->unit(keep)->z == Fixed::fromInt(96),
          "and a 4x4 keep's at 3,4: 80,96");
    const size_t before = w->units().size();
    check(m.command(*w, "Create soldier", {0, 1, 2, 3}) == 0 && m.command(*w, "Create soldier", {5}) == 0 &&
          w->units().size() == before, "4 args or 1 make nothing");
}


static void regions(Fixture& f) {
    std::printf("trigger regions and UnitCreated:\n");
    using sim::retailTriggerRect;
    using sim::retailTriggerCircle;
    check(retailTriggerRect(10, 20, 10, 20, 12, 22) && retailTriggerRect(12, 22, 10, 20, 12, 22),
          "a rectangle holds both corner cells");
    check(!retailTriggerRect(9, 20, 10, 20, 12, 22) && !retailTriggerRect(13, 22, 10, 20, 12, 22) &&
          !retailTriggerRect(11, 23, 10, 20, 12, 22) && !retailTriggerRect(11, 19, 10, 20, 12, 22),
          "and nothing outside them");
    check(!retailTriggerRect(11, 21, 12, 22, 10, 20), "an inverted rectangle never hits");
    check(retailTriggerCircle(34, 30, 30, 30, 4) && retailTriggerCircle(26, 27, 30, 30, 4) &&
          retailTriggerCircle(30, 26, 30, 30, 4) && !retailTriggerCircle(35, 30, 30, 30, 4) &&
          retailTriggerCircle(33, 33, 30, 30, 4) && retailTriggerCircle(34, 31, 30, 30, 4),
          "a circle measures max + min/4 cells: 3,3 and 4,1 are in at r 4 (not by Euclid)");

    auto w = makeWorld();
    // A 2x2 unit's footprint-origin cell o is at pixel o*16+16.
    auto at = [](int cell) { return float(cell * 16 + 16); };
    const int a = w->spawn(f.t("soldier"), at(11), at(21), 0.0f, 0);
    const int b = w->spawn(f.t("soldier"), at(50), at(50), 0.0f, 1);
    const int c = w->spawn(f.t("soldier"), at(26), at(27), 0.0f, 1);
    // .ota Player1 is slot 1 and Player2 slot 0, so slot 0's owner byte is 1.
    sim::MissionScript m(recorderCob(), tdf::Node{}, f.reg, 0, "regions fixture", {-1, 1, 0});
    m.start(*w);
    check(m.command(*w, "SetTrigger", {3, 10, 20, 12, 22}) == 3, "SetTrigger returns the region");
    check(m.command(*w, "SetTrigger", {5, 30, 30, 4}) == 5, "4 args make a circle");
    auto step = [&](int n = 2) { for (int i = 0; i < n; ++i) m.step(*w, 1.0f / 30.0f); };
    step();
    check(m.value("created") == 3 && m.value("owners") == 1 + 0 + 0,
          "UnitCreated once for each unit there, with its owner byte (it never came for them)",
          std::to_string(m.value("created")) + " created, owners " + std::to_string(m.value("owners")));
    check(m.value("hits") == 2, "a unit on the rectangle's corner cell and one in the circle fire",
          std::to_string(m.value("hits")) + " hits");
    step(4);
    check(m.value("hits") == 2 && m.value("created") == 3, "a unit still inside does not fire again");
    w->unit(a)->x = Fixed::fromFloat(at(40)); step();
    w->unit(a)->x = Fixed::fromFloat(at(11)); step();
    check(m.value("hits") == 2, "nor one that left and came back (it did)");
    check(m.command(*w, "SetTrigger", {3, 0, 0, 100, 100}) == 0, "an armed region is not set again");
    step();
    check(m.value("hits") == 2, "so the unit at 50,50 is not in it", std::to_string(m.value("hits")) + " hits");
    m.command(*w, "RemoveTrigger", {3});
    m.command(*w, "SetTrigger", {3, 10, 20, 12, 22});
    step();
    check(m.value("hits") == 3 && m.value("unit") == a && m.value("region") == 3 && m.value("owner") == 1,
          "RemoveTrigger and arming again fires it once more, with the owner byte",
          std::to_string(m.value("hits")) + " hits, owner " + std::to_string(m.value("owner")));
    const int d = w->spawn(f.t("soldier"), at(11), at(20), 0.0f, 0);
    w->unit(d)->underConstruction = true;
    step();
    check(m.value("created") == 3 && m.value("hits") == 3, "a unit under construction is not seen");
    w->unit(d)->underConstruction = false;
    step();
    check(m.value("created") == 4 && m.value("hits") == 4, "and is once it is finished");
    w->unit(b)->hp = Fixed();
    m.command(*w, "SetTrigger", {6, 45, 45, 55, 55});
    step();
    check(m.value("hits") == 4, "a dying unit is not seen");
    int armed = 0;
    for (int id = 0; id < 16; ++id) armed += m.command(*w, "SetTrigger", {id, 0, 0, 1, 1}) == id ? 1 : 0;
    check(armed == 13 && m.command(*w, "SetTrigger", {16, 0, 0, 1, 1}) == 0,
          "16 regions at most (3, 5 and 6 were armed)", std::to_string(armed) + " more armed");
    (void)c;

    // 51d3e0 checks owner 0's units before owner 1's, whatever the spawn
    // order: slot 0 (owner byte 1) spawns first here, so its hit runs last.
    auto v = makeWorld();
    const int first = v->spawn(f.t("soldier"), at(11), at(21), 0.0f, 0);
    const int second = v->spawn(f.t("soldier"), at(12), at(21), 0.0f, 1);
    sim::MissionScript order(recorderCob(), tdf::Node{}, f.reg, 0, "order fixture", {-1, 1, 0});
    order.start(*v);
    order.command(*v, "SetTrigger", {3, 10, 20, 12, 22});
    for (int i = 0; i < 2; ++i) order.step(*v, 1.0f / 30.0f);
    check(order.value("hits") == 2 && order.value("unit") == first && order.value("owner") == 1,
          "the sweep goes owner by owner: owner 0's hit runs before owner 1's (it went in spawn order)",
          std::to_string(order.value("hits")) + " hits, last unit " + std::to_string(order.value("unit")) +
          " (owner-1 unit " + std::to_string(first) + ", owner-0 unit " + std::to_string(second) + ")");

    // A unit aboard a transport counts: 4dad30 commits the carrier's attach point
    // as its position and 51b3b0 its footprint-origin cell from its OWN footprint,
    // and 4d33a0 tests nothing about being carried (E,
    // tools/re/check_mission_cargo_regions.py). The 3x3 wagon at 184,344 has
    // origin cell 10,20; the 2x2 soldier aboard it 11,21.
    auto ride = makeWorld();
    const int wagon = ride->spawn(f.t("wagon"), 184, 600, 0.0f, 0);
    const int rider = ride->spawn(f.t("soldier"), at(60), at(60), 0.0f, 0);
    sim::MissionScript cargo(recorderCob(), tdf::Node{}, f.reg, 0, "cargo fixture", {-1, 1, 0});
    cargo.start(*ride);
    cargo.command(*ride, "SetTrigger", {3, 11, 21, 11, 21});
    check(ride->embarkInto(rider, wagon), "the soldier boards the wagon");
    for (int i = 0; i < 2; ++i) cargo.step(*ride, 1.0f / 30.0f);
    check(cargo.value("hits") == 0, "aboard, away from the region, it does not fire");
    ride->unit(wagon)->z = Fixed::fromInt(344);
    tickN(*ride, 1);
    for (int i = 0; i < 2; ++i) cargo.step(*ride, 1.0f / 30.0f);
    check(cargo.value("hits") == 1 && cargo.value("unit") == rider && ride->unit(rider)->embarked(),
          "carried into it, the passenger fires at its own cell under the carrier (ours skipped it)",
          std::to_string(cargo.value("hits")) + " hits, last unit " + std::to_string(cargo.value("unit")));

    // A change of owner (514da0) makes a new object, whose trigger latch +0xd4
    // starts at 0 (511990, E: tools/re/check_change_owner.py): the sweep fires
    // UnitCreated with the new owner byte and the region it stands in again.
    // The old object retires with kind 6, which starts UnitDestroyed for it.
    auto turn = makeWorld();
    const int convert = turn->spawn(f.t("soldier"), at(11), at(21), 0.0f, 0);
    turn->spawn(f.t("soldier"), at(60), at(60), 0.0f, 1);
    auto owned = std::make_unique<sim::MissionScript>(recorderCob(), tdf::Node{}, f.reg, 0, "owner fixture",
                                                      std::vector<int>{-1, 1, 0});
    sim::MissionScript& mission = *owned;
    turn->setMission(std::move(owned));
    mission.command(*turn, "SetTrigger", {3, 10, 20, 12, 22});
    tickN(*turn, 2);
    check(mission.value("created") == 2 && mission.value("hits") == 1 && mission.value("owners") == 1,
          "before the change: each unit created once, the soldier in the region hit once",
          std::to_string(mission.value("created")) + " created, " + std::to_string(mission.value("hits")) + " hits");
    mission.command(*turn, "Capture", {convert, 1});
    tickN(*turn, 2);
    check(turn->unit(convert)->player == 1 && mission.value("created") == 3 && mission.value("owners") == 1 &&
          mission.value("hits") == 2 && mission.value("owner") == 0 && mission.value("unit") == convert,
          "the captured soldier is created again for its new owner (byte 0) and its region fires again",
          std::to_string(mission.value("created")) + " created, " + std::to_string(mission.value("hits")) +
          " hits, owner " + std::to_string(mission.value("owner")));
    check(mission.value("destroyed") == 1 && mission.value("dunit") == convert,
          "and the old object's retirement starts UnitDestroyed",
          std::to_string(mission.value("destroyed")) + " destroyed");
}

// ---- shipped missions -----------------------------------------------------------------------------

static void shipped(const char* data) {
    hpi::Vfs vfs = hpi::mountRetailRoot(data);
    sim::TypeRegistry reg;
    sim::setupRegistry(reg, vfs, false);
    const auto* arafly = reg.find("arafly"), *arassh = reg.find("arassh"), *aralode = reg.find("aralode");
    const auto* arakeep = reg.find("arakeep"), *araat = reg.find("araat");
    {
        // 4c18d7..4c18ff strips canattack from a type with a weapon, bmcode 0
        // and a canbuild menu: none shipped is one (merged units and unitscb).
        std::string stripped;
        for (const auto& [name, t] : reg.types()) if (World::attackStrippedAtLoad(t)) stripped += " " + name;
        check(stripped.empty(), "no shipped type loses canattack at load", stripped);
    }
    std::printf("takx04 (Flying Builder, keeps, towers):\n");
    {
        World w;
        int human = 0;
        if (!sim::setupMission(w, reg, vfs, "takx04_ph", human)) { check(false, "takx04_ph loads"); return; }
        tickN(w, 2);
        int flyer = 0, jim = 0, keep = 0;
        std::vector<int> towers;
        for (const auto& u : w.units()) {
            if (u.type == arafly) flyer = u.id;
            if (u.type == aralode && u.x.toFloat() == 231 * 16 && u.z.toFloat() == 222 * 16) jim = u.id;
            if (u.type == arakeep) keep = u.id;
            if (u.type == araat) towers.push_back(u.id);
        }
        check(flyer && jim && keep && towers.size() >= 2, "finds the flyer, the lodestone JIM, the keep and the towers");
        if (!flyer || !jim || !keep) return;
        // [units] records: unit71 arafly 14,60 (2x2 mobile), unit109 aralode JIM
        // 231,222 (2x2 building), unit110 arassh JOE 176,243 (4x4), all Angle 180.
        auto standsAt = [&](const sim::UnitType* t, int x, int z) {
            for (const auto& u : w.units())
                if (u.type == t && u.x == Fixed::fromInt(x) && u.z == Fixed::fromInt(z)) return u.heading.v == 0;
            return false;
        };
        check(standsAt(arafly, 14 * 16, 60 * 16), "a placed Flying Builder (mobile) stands at XPos*16, facing Angle 180");
        check(standsAt(aralode, 231 * 16, 222 * 16), "the 2x2 lodestone JIM at 231*16 (it was +8)");
        check(standsAt(arassh, 176 * 16, 243 * 16), "the 4x4 shipyard JOE at 176*16");
        const Unit* fl = w.unit(flyer);
        const ScriptedOrder* wa = fl->orders.empty() ? nullptr : w.scriptedOrder(fl->orders.front());
        check(wa && wa->kind == ScriptedOrder::Kind::WaitAttack && wa->watched == jim,
              "the Flying Builder waits on JIM (wa JIM)");
        tickN(w, 300);
        int shipyards = 0;   // (two more are placed elsewhere: JOE and ZAP)
        for (const auto& u : w.units())
            if (u.type == arassh && u.alive() &&
                std::hypot(u.x.toFloat() - 370 * 16.0f, u.z.toFloat() - 225 * 16.0f) < 160) ++shipyards;
        check(shipyards == 0, "no shipyard appears at 370,225 at the start (it was conjured free)");
        check(!w.unit(flyer)->orders.empty() && w.unit(flyer)->orders.front().waitAttack, "it is still waiting");
        w.unit(jim)->attackedTick = w.tickCount() + 1;     // JIM is attacked
        tickN(w, 3);
        fl = w.unit(flyer);
        const Order* build = fl->orders.empty() ? nullptr : &fl->orders.front();
        check(build && build->buildType == arassh, "once JIM is attacked it sets off to build the shipyard");
        if (build && arassh)
            check(sim::footprintOrigin(build->buildX, arassh->footX) == 370 - arassh->footX / 2 &&
                  sim::footprintOrigin(build->buildZ, arassh->footZ) == 225 - arassh->footZ / 2,
                  "at 370,225", std::to_string(sim::footprintOrigin(build->buildX, arassh->footX)) + "," +
                                std::to_string(sim::footprintOrigin(build->buildZ, arassh->footZ)));
        const Unit* k = w.unit(keep);
        // Its last order is the parse's MakeSelectable (514a7d): the keep is
        // not selectable until it has built them.
        const ScriptedOrder* first = k->orders.size() == 4 ? w.scriptedOrder(k->orders[0]) : nullptr;
        check(first && first->kind == ScriptedOrder::Kind::WaitNear && first->radius == 80 &&
              w.scriptedOrder(k->orders[1]) && w.scriptedOrder(k->orders[1])->kind == ScriptedOrder::Kind::Train &&
              w.scriptedOrder(k->orders[1])->count == 15 && k->buildQueue.empty() && !k->selectable &&
              isKind(w, k->orders[3], ScriptedOrder::Kind::MakeSelectable),
              "the keep waits (5 cells) before it builds 15 swordsmen, then 20 archers",
              std::to_string(k->orders.size()) + " order(s)");
        const Unit* tower = w.unit(towers.front());
        check(tower->standingOrder == 0 && tower->fireState == 0, "the towers' o 0 is Passive",
              std::to_string(tower->standingOrder) + "/" + std::to_string(tower->fireState));
    }
    std::printf("takx07 (troops aboard):\n");
    {
        World w;
        int human = 0;
        if (!sim::setupMission(w, reg, vfs, "takx07_ph", human)) { check(false, "takx07_ph loads"); return; }
        tickN(w, 2);
        int aboard = 0, carriers = 0;
        for (const auto& u : w.units()) {
            if (u.embarked()) ++aboard;
            if (u.type && u.type->canTransport && !u.cargo.empty()) ++carriers;
        }
        check(aboard >= 95 && carriers >= 2, "the i clauses put the troops in their transports",
              std::to_string(aboard) + " aboard " + std::to_string(carriers) + " carriers");
        tickN(w, 300);
        check(true, "and the mission runs on");
    }
    std::printf("takmission41 (mortars bombard a point):\n");
    {
        World w;
        int human = 0;
        if (!sim::setupMission(w, reg, vfs, "takmission41_dh", human)) { check(false, "takmission41_dh loads"); return; }
        tickN(w, 2);
        const auto* vermort = reg.find("vermort");
        std::vector<int> mortars;
        for (const auto& u : w.units())
            if (u.type == vermort && !u.orders.empty() && u.orders.front().suppress) mortars.push_back(u.id);
        check(mortars.size() == 3, "the three Mortars' a X Y is SUPPRESS (they did nothing)",
              std::to_string(mortars.size()) + " on SUPPRESS");
        int fired = 0;
        for (int t = 0; t < 900 && !fired; ++t) {
            tickN(w, 1);
            for (int id : mortars)
                if (const Unit* u = w.unit(id); u && u->firedWeapons) ++fired;
        }
        check(fired > 0, "and they fire at it");
    }
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--points") {
        // tools/re/check_mission_points.py: "X scale" per line -> orderPoint's 16.16 raw.
        char text[64];
        int scale = 0;
        while (std::scanf("%63s %d", text, &scale) == 2)
            std::printf("%d\n", Fixed::fromFloat(sim::MissionScript::orderPoint(std::strtof(text, nullptr), scale)).v);
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--regions") {
        // tools/re/check_mission_regions.py: one query per line, one answer each.
        //   place FX FZ BMCODE X Z -> placedPoint raw x z     angle A -> placedHeading
        //   create FX FZ X Z -> createPoint raw x z           origin P FX -> footprintOrigin(raw P)
        //   rect X1 Z1 X2 Z2 OX OZ / circle CX CZ R OX OZ -> 1 hit, 0 miss
        char verb[16];
        while (std::scanf("%15s", verb) == 1) {
            const std::string v = verb;
            int n[6] = {};
            if (v == "place" && std::scanf("%d %d %d %d %d", &n[0], &n[1], &n[2], &n[3], &n[4]) == 5) {
                const auto at = sim::MissionScript::placedPoint(sized(n[0], n[1], n[2]), n[3], n[4]);
                std::printf("%d %d\n", at.x.v, at.z.v);
            } else if (v == "create" && std::scanf("%d %d %d %d", &n[0], &n[1], &n[2], &n[3]) == 4) {
                const auto at = sim::MissionScript::createPoint(sized(n[0], n[1], 0), n[2], n[3]);
                std::printf("%d %d\n", at.x.v, at.z.v);
            } else if (v == "angle" && std::scanf("%d", &n[0]) == 1) {
                std::printf("%d\n", sim::MissionScript::placedHeading(n[0]));
            } else if (v == "origin" && std::scanf("%d %d", &n[0], &n[1]) == 2) {
                std::printf("%d\n", sim::footprintOrigin(Fixed::raw(n[0]), n[1]));
            } else if (v == "rect" && std::scanf("%d %d %d %d %d %d", &n[0], &n[1], &n[2], &n[3], &n[4], &n[5]) == 6) {
                std::printf("%d\n", sim::retailTriggerRect(n[4], n[5], n[0], n[1], n[2], n[3]) ? 1 : 0);
            } else if (v == "circle" && std::scanf("%d %d %d %d %d", &n[0], &n[1], &n[2], &n[3], &n[4]) == 5) {
                std::printf("%d\n", sim::retailTriggerCircle(n[3], n[4], n[0], n[1], n[2]) ? 1 : 0);
            } else {
                std::printf("bad query %s\n", verb);
                return 2;
            }
        }
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--selectable") {
        // tools/re/check_mission_selectable.py: the port's side of its table.
        Fixture f;
        for (const auto& c : kSelectableCases) selectableRow(f, c);
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--data") {
        std::printf("mission_orders_test (shipped missions)\n");
        shipped(argv[2]);
    } else {
        std::printf("mission_orders_test\n");
        Fixture f;
        if (!f.t("soldier") || !f.t("keep") || !f.t("builder") || !f.t("wagon")) {
            std::printf("fixture types failed to load\n");
            return 2;
        }
        clauses(f);
        standing(f);
        builds(f);
        points(f);
        aboard(f);
        names(f);
        waitNear(f);
        veterancy(f);
        attacks(f);
        attackPoint(f);
        clausePoints(f);
        airPoint(f);
        airUnit(f);
        strippedAttack(f);
        selectAndDestruct(f);
        selectable(f);
        holds(f);
        setMission(f);
        tableCompacts(f);
        placement();
        facings(f);
        creates(f);
        regions(f);
    }
    std::printf(g_fail ? "mission_orders_test: %d FAILED\n" : "mission_orders_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
