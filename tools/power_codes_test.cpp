// Cheat codes (sim/powercodes.h): the chat parser, and each sim code's effect
// through Cmd::PowerCode -- refused while the match has CHEAT CODES off.
#include "client/retailfog.h"
#include "net/matchconfig.h"
#include "net/protocol.h"
#include "server/commands.h"
#include "sim/matchsetup.h"
#include "sim/powercodes.h"
#include "sim/retailrng.h"
#include "sim/retailexploration.h"
#include "sim/sim.h"

#include <cstdio>
#include <stdexcept>
#include <string>

using namespace ta::sim;
using ta::net::Cmd;
using ta::net::Command;

static void check(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
static void advance(World& w, int ticks = 20) { for (int i = 0; i < ticks; ++i) w.tick(1.f / 30); }

static void parser() {
    struct Case { const char* text; PowerCode code; PowerArg arg; int value = 0; };
    const Case ok[] = {
        {"+ATM", PowerCode::Atm, PowerArg::None},
        {"+atm", PowerCode::Atm, PowerArg::None},
        {"  +Atm  ", PowerCode::Atm, PowerArg::None},
        {"\t+ atm", PowerCode::Atm, PowerArg::None},
        {"+DoubleShot", PowerCode::DoubleShot, PowerArg::None},
        {"+halfshot", PowerCode::HalfShot, PowerArg::None},
        {"+LOS", PowerCode::Los, PowerArg::None},
        {"+los on", PowerCode::Los, PowerArg::None},   // TA's LOS takes no argument (0x416d50)
        {"+Meteor", PowerCode::Meteor, PowerArg::None},
        {"+meteor 1", PowerCode::Meteor, PowerArg::On, 1},
        {"+METEOR 0", PowerCode::Meteor, PowerArg::Off},
        {"+meteor on", PowerCode::Meteor, PowerArg::Off},   // atoi("on") is 0
        {"+View 3", PowerCode::View, PowerArg::None, 3},
        {"+view", PowerCode::View, PowerArg::None, 0},
        {"+Mapping", PowerCode::Mapping, PowerArg::None},
        {"+radar", PowerCode::Radar, PowerArg::None},
        {"+MakePoster", PowerCode::MakePoster, PowerArg::None},
        {"+makeposter all", PowerCode::MakePoster, PowerArg::None},
        {"+NowISee", PowerCode::NowISee, PowerArg::None},
        {"+nowisee", PowerCode::NowISee, PowerArg::None},
        {"+atm please", PowerCode::Atm, PowerArg::None},   // other words are ignored
    };
    for (const auto& c : ok) {
        const auto in = parsePowerCode(c.text);
        check(in && in->code == c.code && in->arg == c.arg && in->value == c.value, std::string("parse: ") + c.text);
    }
    // Not a code: ordinary chat, other +words (retail's group-1 and developer
    // commands are not offered), a bare '+', a name glued to more letters.
    for (const char* text : {"ATM", "hello +ATM", "+", "+ ", "+Shadow", "+Control", "+atmx", "+los-on",
                             "++atm", "+cdstart", ""})
        check(!parsePowerCode(text), std::string("not a code: '") + text + "'");
    check(simPowerCode(PowerCode::Atm) && simPowerCode(PowerCode::NowISee) &&
          simPowerCode(PowerCode::Meteor) && !simPowerCode(PowerCode::View) &&
          !simPowerCode(PowerCode::Radar) && !simPowerCode(PowerCode::MakePoster) &&
          !simPowerCode(PowerCode::None), "sim/client split");
}

static void damageFormula() {
    check(retailDamageWithSpread(3.5f, 16384) == 3, "damage truncates");
}

static Command code(PowerCode c, int player = 0, PowerArg arg = PowerArg::None, int unit = 0) {
    Command m; m.kind = Cmd::PowerCode; m.player = uint8_t(player);
    m.targetId = int32_t(c); m.queue = uint8_t(arg); m.unitId = unit;
    return m;
}

static void simCodes() {
    TypeRegistry reg;
    UnitType eyes; eyes.id = eyes.name = "eyes"; eyes.maxHp = 100; eyes.sight = 128; eyes.sightHeight = 40;
    eyes.footX = eyes.footZ = 1;
    const auto setup = [](World& w, bool codes) {
        w.setSerialThreads(true); w.setPlayerCount(2);
        w.setTerrain(std::vector<uint8_t>(128 * 128, 0), 128, 128, 0);
        w.setPowerCodes(codes);
    };
    for (bool on : {false, true}) {
        World w; setup(w, on);
        const int mine = w.spawn(&eyes, 512, 512, {}, 0);
        const int theirs = w.spawn(&eyes, 1700, 1700, {}, 1);
        const int far = w.spawn(&eyes, 1400, 512, {}, 1);
        advance(w);
        check(!w.playerSees(0, *w.unit(far)), "an enemy out of sight is not seen");

        // ATM (TotalA.exe 0x4170c0): the viewed player's -- here the issuer's --
        // stored energy and metal each gain 1000, past the cap until the next
        // settle; the other player's do not.
        w.setStartResources(0, 40, 40); w.setStartResources(1, 40, 40);
        applyCommand(w, reg, code(PowerCode::Atm, 0));
        check(on ? w.books(0).stored[0] == 1040 && w.books(0).stored[1] == 1040
                 : w.books(0).stored[0] == 40 && w.books(0).stored[1] == 40,
              on ? "ATM adds 1000 energy and metal" : "ATM refused while off");
        check(w.books(1).stored[0] == 40 && w.books(1).stored[1] == 40, "ATM is the issuer's alone");
        // With +View 1 the issuer's ATM pays player 1 (unitId = 1 + the viewed player).
        applyCommand(w, reg, code(PowerCode::Atm, 0, PowerArg::None, 2));
        check(on ? w.books(1).stored[0] == 1040 && w.books(0).stored[0] == 1040 : w.books(1).stored[0] == 40,
              "ATM pays the viewed player");
        // MANAME was Kingdoms': not a TA code, never parsed, refused by the sim.
        check(!parsePowerCode("+maname"), "MANAME is not a power code");

        // DoubleShot / HalfShot: game flags 0x80 / 0x100, independent toggles.
        applyCommand(w, reg, code(PowerCode::DoubleShot, 1));
        check(w.doubleShot() == on && !w.halfShot(), "DoubleShot");
        applyCommand(w, reg, code(PowerCode::HalfShot, 0));
        check(w.doubleShot() == on && w.halfShot() == on, "HalfShot leaves DoubleShot alone");
        applyCommand(w, reg, code(PowerCode::DoubleShot, 0));
        check(!w.doubleShot() && w.halfShot() == on, "DoubleShot toggles off alone");
        applyCommand(w, reg, code(PowerCode::HalfShot, 0));
        check(!w.doubleShot() && !w.halfShot(), "HalfShot toggles off");

        // LOS off sees only explored cells; Mapping explores the whole map.
        applyCommand(w, reg, code(PowerCode::Los, 0));
        check(w.powerLosOff(0) == on && !w.powerLosOff(1), "LOS is the issuer's");
        check(!w.playerSees(0, *w.unit(theirs)), "LOS off still needs the cell explored");
        applyCommand(w, reg, code(PowerCode::Mapping, 0));
        check(w.powerMappingOff(0) == on, "Mapping reveals");
        check(w.playerSees(0, *w.unit(theirs)) == on, "LOS off + map revealed sees every unit");
        check(!w.playerSees(1, *w.unit(mine)), "the other player's sight is unchanged");
        applyCommand(w, reg, code(PowerCode::Los, 0));
        check(!w.powerLosOff(0) && !w.playerSees(0, *w.unit(theirs)), "LOS toggles line of sight back");
        // Mapping again: the shroud returns and exploration is forgotten, then sight
        // explores the unit's own surroundings again.
        applyCommand(w, reg, code(PowerCode::Mapping, 0));
        check(!w.powerMappingOff(0), "Mapping toggles");
        if (on) {
            const int width = 128 / 2;
            const auto& ex = w.navigationExploration();
            check(!(ex[size_t(1700 / 32) * width + 1700 / 32] & 1), "the shroud forgets exploration");
            advance(w);
            check(ex[size_t(512 / 32) * width + 512 / 32] & 1, "sight explores again after the shroud returns");
            check(!(ex[size_t(1700 / 32) * width + 1700 / 32] & 1), "unseen terrain stays shrouded");
        }
        applyCommand(w, reg, code(PowerCode::NowISee, 0));
        check(w.powerLosOff(0) == on && w.powerMappingOff(0) == on, "NowISee: line of sight and shroud off");
        check(w.playerSees(0, *w.unit(theirs)) == on && w.playerSees(0, *w.unit(far)) == on, "NowISee sees everything");
        applyCommand(w, reg, code(PowerCode::NowISee, 0));
        check(w.powerLosOff(0) == on && w.powerMappingOff(0) == on, "NowISee is not a toggle");
        // The local display plane (client/retailfog.h) of player 0 follows its codes.
        advance(w, 10);
        {
            namespace rf = ta::retailfog;
            rf::PlaneSource s;
            s.viewCounts = w.viewCounts(); s.explored = w.navigationExploration();
            s.width = w.viewPlaneW(); s.height = w.viewPlaneH(); s.viewers = 1; s.player = 0;
            s.mapRevealed = w.mapRevealedFor(0); s.losOff = w.powerLosOff(0);
            std::vector<uint8_t> plane; rf::buildPlane(s, plane);
            check((plane[size_t(1700 / 32) * size_t(s.width) + 1700 / 32] == rf::kInView) == on,
                  "NowISee lifts the display fog");
        }
        // Client-only codes never change the sim.
        const auto before = w.stateHash();
        applyCommand(w, reg, code(PowerCode::Radar, 0));
        applyCommand(w, reg, code(PowerCode::MakePoster, 0));
        check(w.stateHash() == before, "Radar and MakePoster are not sim codes");
    }
    // The hash carries the codes in a CHEAT CODES match and nothing otherwise.
    World a, b, c; setup(a, true); setup(b, true); setup(c, false);
    check(a.stateHash() == b.stateHash(), "identical worlds hash alike");
    applyCommand(b, reg, code(PowerCode::DoubleShot, 0));
    check(a.stateHash() != b.stateHash(), "the damage mode is hashed");
    applyCommand(b, reg, code(PowerCode::DoubleShot, 0));
    check(a.stateHash() == b.stateHash(), "toggled back, the hash matches again");
    applyCommand(b, reg, code(PowerCode::Los, 1));
    check(a.stateHash() != b.stateHash(), "line-of-sight bits are hashed");
    World d; setup(d, false);
    applyCommand(d, reg, code(PowerCode::DoubleShot, 0));
    check(c.stateHash() == d.stateHash(), "with CHEAT CODES off a code changes nothing");
    World e; e.setSerialThreads(true); e.setPlayerCount(2); e.setTerrain(std::vector<uint8_t>(128 * 128, 0), 128, 128, 0);
    check(c.stateHash() == e.stateHash(), "CHEAT CODES off hashes exactly like a world that never heard of it");
    // A room that starts with the map revealed starts +Mapping with the shroud off (retail
    // seeds MappingOn from Map Revealed): the first +Mapping brings the shroud back.
    World m; m.setSerialThreads(true); m.setPlayerCount(2); m.setTerrain(std::vector<uint8_t>(128 * 128, 0), 128, 128, 0);
    m.setPowerCodes(true, true);
    check(m.powerMappingOff(0) && m.powerMappingOff(1) && !m.powerLosOff(0), "map revealed: the shroud starts off, LOS on");
    applyCommand(m, reg, code(PowerCode::Mapping, 0));
    check(!m.powerMappingOff(0) && m.powerMappingOff(1), "map revealed: the first Mapping re-shrouds the issuer only");
    World off; off.setSerialThreads(true); off.setPlayerCount(2); off.setTerrain(std::vector<uint8_t>(128 * 128, 0), 128, 128, 0);
    off.setPowerCodes(false, true);
    check(!off.powerMappingOff(0) && off.stateHash() == e.stateHash(), "map revealed means nothing with CHEAT CODES off");
}

// True line of sight (TotalA.exe 0x482270, docs/retail-fog.md 2.3) and +LOSType.
static void trueSight() {
    // One ray straight east, cells 1..4, as a TABLE1 would hold it.
    // (numtables 2: r = min(sight/32, 1), TABLE r.)
    const RetailLosTable east{RetailLosRay{{1, 0}, {2, 0}, {3, 0}, {4, 0}}};
    setRetailLosTables({east, east});
    RetailSightFootprint f;
    f.trueLos = true; f.distance = 40; f.x = 2; f.z = 2; f.eyeHeight = 30;
    constexpr int W = 10, H = 5;
    std::vector<RetailExplorationHeight> h(size_t(W) * H, RetailExplorationHeight{0, 0});
    auto at = [&](int x, int z) { return h[size_t(z) * W + size_t(x)]; };
    auto cells = [&]() {
        std::vector<int> xs;
        retailTrueSightCells(f, W, H, at, [&](int x, int z) { if (z == 2) xs.push_back(x); });
        return xs;
    };
    check(cells() == std::vector<int>{2, 3, 4, 5, 6}, "flat ground: the centre and every cell of the ray");
    h[size_t(2) * W + 4] = {60, 60};     // a wall two cells out, above the eye (30)
    check(cells() == std::vector<int>{2, 3, 4}, "a wall above the eye is seen and hides what lies behind it");
    h[size_t(2) * W + 6] = {250, 250};   // a taller tower behind it shows over the wall
    check(cells() == std::vector<int>{2, 3, 4, 6}, "a tower taller than the horizon shows over the wall");
    f.distance = 20;
    check(cells() == std::vector<int>{2}, "under 32 px there is no table: the centre alone");
    // DOUBLE SIGHT (scale 2, engine option): rays to every cell on the rim of a disc
    // twice the undoubled table's radius, so on flat ground the whole disc is seen,
    // with no lattice of cells between the rays.
    {
        std::vector<RetailLosTable> eight(8, east);   // numtables 8: TABLE7 for 224..255 px
        setRetailLosTables(eight);
        RetailSightFootprint d;
        d.trueLos = true; d.scale = 2; d.distance = 2 * 230; d.x = 20; d.z = 20; d.eyeHeight = 30;
        constexpr int N = 41;
        std::vector<RetailExplorationHeight> flat(size_t(N) * N, RetailExplorationHeight{0, 0});
        auto flatAt = [&](int x, int z) { return flat[size_t(z) * N + size_t(x)]; };
        std::vector<uint8_t> seen(size_t(N) * N, 0);
        retailTrueSightCells(d, N, N, flatAt, [&](int x, int z) { seen[size_t(z) * N + size_t(x)] = 1; });
        const int R = 14;   // TABLE7 doubled
        bool full = true, outside = false;
        for (int z = 0; z < N; ++z)
            for (int x = 0; x < N; ++x) {
                const int dx = x - 20, dz = z - 20;
                const bool in = dx * dx + dz * dz <= R * R + R;
                if (in && !seen[size_t(z) * N + size_t(x)]) full = false;
                if (!in && seen[size_t(z) * N + size_t(x)]) outside = true;
            }
        check(full && !outside, "double sight: flat ground sees the whole doubled disc, no holes");
        check(retailSightRadius(d) == R, "double sight: the footprint's reach doubles");
        // A wall still hides what lies behind it.
        flat[size_t(20) * N + 23] = {120, 120};
        std::fill(seen.begin(), seen.end(), 0);
        retailTrueSightCells(d, N, N, flatAt, [&](int x, int z) { seen[size_t(z) * N + size_t(x)] = 1; });
        check(seen[size_t(20) * N + 23] && !seen[size_t(20) * N + 30], "double sight: a wall hides the ground behind it");
    }
    setRetailLosTables({});
    // Circular past the largest mask: retail clamps at 14 cells; doubled sight draws
    // its own disc, sight/32 cells.
    {
        RetailSightFootprint c;
        c.distance = 2 * 280;   // a Peewee's sight, doubled: 17 cells
        check(retailSightRadius(c) == 14, "Circular without double sight stops at the 14-cell mask");
        c.scale = 2;
        check(retailSightRadius(c) == 17, "Circular with double sight draws a 17-cell disc");
        c.distance = 2 * 200;   // within the masks: still retail's mask
        check(&retailSightMask(c) == &retailVisMaskTable()[7], "doubled sight inside the masks keeps retail's mask");
    }

    // +LOSType is retail's group 1: it acts with CHEAT CODES off too.
    TypeRegistry reg;
    for (bool codes : {false, true}) {
        World w; w.setSerialThreads(true); w.setPlayerCount(2);
        w.setTerrain(std::vector<uint8_t>(128 * 128, 0), 128, 128, 0);
        w.setPowerCodes(codes); w.setTrueLos(true);
        const auto before = w.stateHash();
        applyCommand(w, reg, code(PowerCode::LosType, 0));
        check(!w.trueLos() && w.stateHash() != before, "+LOSType toggles True to Circular, cheats or not");
        applyCommand(w, reg, code(PowerCode::LosType, 1));
        check(w.trueLos(), "and back");
    }
    check(parsePowerCode("+LOSType") && parsePowerCode("+lostype")->code == PowerCode::LosType, "parse +LOSType");
    check(alwaysPowerCode(int32_t(PowerCode::LosType)) && !alwaysPowerCode(int32_t(PowerCode::Atm)),
          "only LOSType is always on");
}

// A submerged enemy is seen only as a sonar contact (TotalA.exe 0x465ac0, 0x467440).
static void submerged() {
    UnitType eyes; eyes.id = eyes.name = "eyes"; eyes.maxHp = 100; eyes.sight = 320; eyes.footX = eyes.footZ = 1;
    UnitType sub = eyes; sub.id = sub.name = "sub"; sub.sight = 64;
    UnitType sonar = eyes; sonar.id = sonar.name = "sonar"; sonar.sonar = 400;
    UnitType jammer = eyes; jammer.id = jammer.name = "jammer"; jammer.sight = 64; jammer.sonarJam = 300;
    World w; w.setSerialThreads(true); w.setPlayerCount(2);
    w.setTerrain(std::vector<uint8_t>(128 * 128, 0), 128, 128, 50);   // sea level 50 over a flat bottom
    w.spawn(&eyes, 512, 512, {}, 0);
    const int target = w.spawn(&sub, 640, 512, {}, 1);
    auto& t = *w.unit(target);
    t.groundY = Fixed::fromInt(10);   // its top under the sea
    advance(w, 5);
    check(!w.playerSees(0, *w.unit(target)), "submerged: unseen in sight without sonar");
    w.spawn(&sonar, 500, 500, {}, 0);
    advance(w, 5);
    check(w.playerSees(0, *w.unit(target)), "submerged: seen as a sonar contact");
    w.spawn(&jammer, 900, 520, {}, 1);
    advance(w, 5);
    check(!w.playerSees(0, *w.unit(target)), "submerged: an enemy sonar jammer clears the contact");
}

// A seen enemy inside mincloakdistance keeps a cloaker from cloaking for 90 ticks
// (TotalA.exe 0x467680..0x4676e8).
static void cloakProximity() {
    UnitType eyes; eyes.id = eyes.name = "eyes"; eyes.maxHp = 100; eyes.sight = 320; eyes.footX = eyes.footZ = 1;
    UnitType cloaker = eyes; cloaker.id = cloaker.name = "cloaker"; cloaker.canCloak = true;
    cloaker.cloakCost = 10; cloaker.minCloakDist = 80;
    for (int gap : {60, 200}) {
        World w; w.setSerialThreads(true); w.setPlayerCount(2); w.setTeam(1, 1);
        w.setTerrain(std::vector<uint8_t>(128 * 128, 0), 128, 128, 0);
        const int c = w.spawn(&cloaker, 512, 512, {}, 0);
        w.spawn(&eyes, 512 + float(gap), 512, {}, 1);
        advance(w, 20);   // the visible-enemy lists rebuild on their 7-tick clock
        const bool blocked = w.unit(c)->cloakBlockedUntil > w.tickCount();
        check(blocked == (gap < 80), gap < 80 ? "an enemy inside mincloakdistance blocks the cloak"
                                              : "an enemy beyond mincloakdistance does not");
    }
}

// The meteor storm (TotalA.exe 0x437cd0..0x438070) and +Meteor.
static void meteor() {
    TypeRegistry reg;
    Weapon rock; rock.name = "METEOR";
    const auto make = [&](World& w, bool codes, bool armed) {
        w.setSerialThreads(true); w.setPlayerCount(2);
        w.setTerrain(std::vector<uint8_t>(128 * 128, 0), 128, 128, 0);
        w.setPowerCodes(codes);
        World::MeteorConfig c; c.weapon = &rock; c.radius = 300; c.density = 2; c.duration = 5; c.interval = 60;
        c.armed = armed;
        w.setMeteorStorm(c);
    };
    const auto meteors = [](const World& w) {
        int n = 0;
        for (const auto& p : w.projectiles()) n += p.taMeteor;
        return n;
    };
    {   // Armed: the first strike falls due at the hit spacing (30 / 2 = 15 ticks).
        World w; make(w, false, true);
        advance(w, 14);
        check(!w.meteorActive() && meteors(w) == 0, "no strike before the hit spacing");
        advance(w, 1);   // launched this tick (a meteor north of the map goes on its first step)
        check(w.meteorActive() && meteors(w) == 1, "an armed storm strikes");
        const auto& p = w.projectiles().back();
        check(p.fromPlayer == kNeutralPlayer && p.velocity[1] == int32_t(0xfff10000u), "a meteor falls 15 px a tick, nobody's");
        advance(w, 200);
        check(!w.meteorActive(), "a strike ends after its duration");
    }
    {   // Disarmed: nothing falls, nothing is drawn.
        World w; make(w, false, false);
        advance(w, 400);
        check(meteors(w) == 0, "a disarmed storm never strikes");
    }
    {   // +Meteor: a strike now; +Meteor 1 / 0 arm and disarm. Refused without cheats.
        for (bool on : {false, true}) {
            World w; make(w, on, false);
            applyCommand(w, reg, code(PowerCode::Meteor, 0));
            check(w.meteorActive() == on, "+Meteor starts a strike");
            applyCommand(w, reg, code(PowerCode::Meteor, 0, PowerArg::On));
            check(w.meteorArmed() == on, "+Meteor 1 arms the storm");
            applyCommand(w, reg, code(PowerCode::Meteor, 0, PowerArg::Off));
            check(!w.meteorArmed(), "+Meteor 0 disarms it");
        }
        World a, b; make(a, true, false); make(b, true, false);
        applyCommand(b, reg, code(PowerCode::Meteor, 0));
        check(a.stateHash() != b.stateHash(), "the storm is hashed");
    }
}

static void wire() {
    // The server accepts the seven sim codes and a valid argument, nothing else.
    const auto payload = [](const Command& c) {
        ta::net::Writer w; w.u32(1); w.cmd(c); return w.b;
    };
    for (int c = 1; c <= 7; ++c)
        check(ta::srv::validPlayerCommands(payload(code(PowerCode(c)))) == (c != int(PowerCode::RetiredManaMe)),
              "server accepts a sim code, refuses the retired MANAME");
    check(ta::srv::validPlayerCommands(payload(code(PowerCode::Meteor))), "Meteor travels");
    check(!ta::srv::validPlayerCommands(payload(code(PowerCode::Radar))), "Radar never travels");
    check(!ta::srv::validPlayerCommands(payload(code(PowerCode::View))), "View never travels");
    check(!ta::srv::validPlayerCommands(payload(code(PowerCode::None))), "code 0 is forged");
    Command bad = code(PowerCode::Los); bad.queue = 9;
    check(!ta::srv::validPlayerCommands(payload(bad)), "an unknown argument is forged");
    // Round trip through a strict (replay) bundle read.
    ta::net::Writer w;
    ta::net::putBundle(w, 7, {code(PowerCode::Los, 3, PowerArg::None, 42)}, {});
    ta::net::Reader r(w.b.data(), w.b.size());
    uint32_t tick = 0; ta::net::Bundle out;
    check(ta::net::getBundle(r, tick, out, true) && out.cmds.size() == 1 && out.cmds[0].kind == Cmd::PowerCode &&
          out.cmds[0].unitId == 42 && out.cmds[0].player == 3 && out.cmds[0].targetId == int32_t(PowerCode::Los),
          "a power code survives a strict bundle read");
    // The option travels in GameOptions and the replay header.
    ta::net::GameOptions o; o.powerCodes = 1;
    ta::net::Writer ow; ta::net::put(ow, o);
    ta::net::Reader orr(ow.b.data(), ow.b.size());
    ta::net::GameOptions back; ta::net::get(orr, back);
    check(orr.ok && orr.p == orr.end && back.powerCodes == 1, "GameOptions carries CHEAT CODES");
    // The replay byte carries the option (bit 0) and the room's map-revealed start (bit 1),
    // so playback seeds +Mapping as the game did.
    ta::net::SlotInfo slots[ta::net::kMaxSlots] = {};
    for (uint8_t fog : {uint8_t(0), uint8_t(1), uint8_t(2)}) {
        ta::net::GameOptions g; g.powerCodes = 1; g.fogExplored = fog;
        const auto live = ta::net::matchConfigFor(g, slots, 1, false);
        const auto h = ta::net::replayHeaderFor(g, "m", "", slots, 1);
        ta::net::GameOptions replayed; ta::net::SlotInfo rs[ta::net::kMaxSlots];
        ta::net::matchInputsFrom(h, replayed, rs);
        const auto again = ta::net::matchConfigFor(replayed, rs, 1, false);
        check(live.powerCodes && live.powerMapRevealed == (fog >= 1) && again.powerCodes &&
              again.powerMapRevealed == live.powerMapRevealed, "a replay seeds +Mapping as the game did");
    }
    ta::net::GameOptions plain; plain.fogExplored = 1;
    check(ta::net::replayHeaderFor(plain, "m", "", slots, 1).powerCodes == 0 &&
          !ta::net::matchConfigFor(plain, slots, 1, false).powerMapRevealed, "CHEAT CODES off records 0");
}

int main() {
    parser();
    damageFormula();
    simCodes();
    meteor();
    trueSight();
    submerged();
    cloakProximity();
    wire();
    std::puts("PASS: power codes -- parser, damage formula, sim effects and gating, hash, wire");
}
