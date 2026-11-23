// Power codes (sim/powercodes.h): the chat parser, and each sim code's effect
// through Cmd::PowerCode -- refused while the match has POWER CODES off.
#include "client/retailfog.h"
#include "net/matchconfig.h"
#include "net/protocol.h"
#include "server/commands.h"
#include "sim/matchsetup.h"
#include "sim/powercodes.h"
#include "sim/retailrng.h"
#include "sim/sim.h"

#include <cstdio>
#include <stdexcept>
#include <string>

using namespace tak::sim;
using tak::net::Cmd;
using tak::net::Command;

static void check(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
static void advance(World& w, int ticks = 20) { for (int i = 0; i < ticks; ++i) w.tick(1.f / 30); }

static void parser() {
    struct Case { const char* text; PowerCode code; PowerArg arg; };
    const Case ok[] = {
        {"+ATM", PowerCode::Atm, PowerArg::None},
        {"+atm", PowerCode::Atm, PowerArg::None},
        {"  +Atm  ", PowerCode::Atm, PowerArg::None},
        {"\t+ atm", PowerCode::Atm, PowerArg::None},
        {"+maname", PowerCode::ManaMe, PowerArg::None},
        {"+MANAME", PowerCode::ManaMe, PowerArg::None},
        {"+DoubleShot", PowerCode::DoubleShot, PowerArg::None},
        {"+halfshot", PowerCode::HalfShot, PowerArg::None},
        {"+LOS", PowerCode::Los, PowerArg::None},
        {"+los on", PowerCode::Los, PowerArg::On},
        {"+LoS   OFF  ", PowerCode::Los, PowerArg::Off},
        {"+los maybe", PowerCode::Los, PowerArg::Invalid},
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
        check(in && in->code == c.code && in->arg == c.arg, std::string("parse: ") + c.text);
    }
    // Not a code: ordinary chat, other +words (retail's +View and the console
    // commands are not offered), a bare '+', a name glued to more letters.
    for (const char* text : {"ATM", "hello +ATM", "+", "+ ", "+View", "+Shadow", "+atmx", "+los-on",
                             "++atm", "+cdstart", ""})
        check(!parsePowerCode(text), std::string("not a code: '") + text + "'");
    check(simPowerCode(PowerCode::Atm) && simPowerCode(PowerCode::NowISee) &&
          !simPowerCode(PowerCode::Radar) && !simPowerCode(PowerCode::MakePoster) &&
          !simPowerCode(PowerCode::None), "sim/client split");
}

static void damageFormula() {
    // 52a330: the mode applies to the spread float before the truncation.
    check(retailDamageWithSpread(3.5f, 16384) == 3, "normal damage truncates");
    check(retailDamageWithSpread(3.5f, 16384, 1) == 7, "DoubleShot doubles before truncating (int(2s), not 2*int(s))");
    check(retailDamageWithSpread(3.5f, 16384, 2) == 1, "HalfShot halves before truncating");
    for (uint32_t roll : {0u, 1234u, 32767u})
        check(retailDamageWithSpread(100.f, roll, 1) == int((100.f + float(int((2ll * 15 * roll) / 32768ll) - 15)) * 2.f),
              "DoubleShot keeps the spread");
}

static Command code(PowerCode c, int player = 0, PowerArg arg = PowerArg::None, int unit = 0) {
    Command m; m.kind = Cmd::PowerCode; m.player = uint8_t(player);
    m.targetId = int32_t(c); m.queue = uint8_t(arg); m.unitId = unit;
    return m;
}

static void simCodes() {
    TypeRegistry reg;
    UnitType eyes; eyes.id = eyes.name = "eyes"; eyes.maxHp = 100; eyes.sight = 128; eyes.sightHeight = 40;
    eyes.footX = eyes.footZ = 1; eyes.maxMana = 250;
    UnitType mute = eyes; mute.id = mute.name = "mute"; mute.maxMana = 0;
    const auto setup = [](World& w, bool codes) {
        w.setSerialThreads(true); w.setPlayerCount(2);
        w.setTerrain(std::vector<uint8_t>(128 * 128, 0), 128, 128, 0);
        w.setPowerCodes(codes);
    };
    for (bool on : {false, true}) {
        World w; setup(w, on);
        const int mine = w.spawn(&eyes, 512, 512, {}, 0);
        const int plain = w.spawn(&mute, 540, 512, {}, 0);
        const int theirs = w.spawn(&eyes, 1700, 1700, {}, 1);
        const int far = w.spawn(&eyes, 1400, 512, {}, 1);
        w.unit(mine)->mana = 10; w.unit(theirs)->mana = 10;
        advance(w);
        check(!w.playerSees(0, *w.unit(far)), "an enemy out of sight is not seen");

        // ATM: the issuer's pool fills to its storage; the other player's does not.
        w.player(0).mana = 40; w.player(1).mana = 40;
        applyCommand(w, reg, code(PowerCode::Atm, 0));
        const double full = w.player(0).manaCapacity();
        check(on ? w.player(0).mana == full : w.player(0).mana == 40, on ? "ATM fills the pool" : "ATM refused while off");
        check(w.player(1).mana == 40, "ATM is the issuer's alone");

        // ManaMe: the issuer's own units with a pool, one command per unit.
        applyCommand(w, reg, code(PowerCode::ManaMe, 0, PowerArg::None, mine));
        applyCommand(w, reg, code(PowerCode::ManaMe, 0, PowerArg::None, theirs));   // not the issuer's
        applyCommand(w, reg, code(PowerCode::ManaMe, 0, PowerArg::None, plain));
        check(w.unit(mine)->mana == (on ? 250.0 : 10.0), on ? "ManaMe fills a selected unit" : "ManaMe refused while off");
        check(w.unit(theirs)->mana == 10.0, "ManaMe never touches another player's unit");
        check(w.unit(plain)->mana == 0.0, "ManaMe leaves a unit without a pool");

        // DoubleShot / HalfShot: one game-wide mode, each clearing the other.
        applyCommand(w, reg, code(PowerCode::DoubleShot, 1));
        check(w.damageMode() == (on ? DamageMode::Double : DamageMode::Normal), "DoubleShot");
        applyCommand(w, reg, code(PowerCode::HalfShot, 0));
        check(w.damageMode() == (on ? DamageMode::Half : DamageMode::Normal), "HalfShot clears DoubleShot");
        applyCommand(w, reg, code(PowerCode::HalfShot, 0));
        check(w.damageMode() == DamageMode::Normal, "HalfShot toggles off");
        applyCommand(w, reg, code(PowerCode::HalfShot, 0));
        applyCommand(w, reg, code(PowerCode::DoubleShot, 0));
        check(w.damageMode() == (on ? DamageMode::Double : DamageMode::Normal), "DoubleShot clears HalfShot");
        applyCommand(w, reg, code(PowerCode::DoubleShot, 0));

        // LOS Off sees only explored cells; Mapping explores the whole map.
        applyCommand(w, reg, code(PowerCode::Los, 0, PowerArg::Off));
        check(w.powerLosOff(0) == on && !w.powerLosOff(1), "LOS Off is the issuer's");
        check(!w.playerSees(0, *w.unit(theirs)), "LOS off still needs the cell explored");
        applyCommand(w, reg, code(PowerCode::Los, 0, PowerArg::Invalid));
        check(w.powerLosOff(0) == on, "an unknown LOS argument changes nothing");
        applyCommand(w, reg, code(PowerCode::Mapping, 0));
        check(w.powerMappingOff(0) == on, "Mapping reveals");
        check(w.playerSees(0, *w.unit(theirs)) == on, "LOS off + map revealed sees every unit");
        check(!w.playerSees(1, *w.unit(mine)), "the other player's sight is unchanged");
        applyCommand(w, reg, code(PowerCode::Los, 0, PowerArg::On));
        check(!w.powerLosOff(0) && !w.playerSees(0, *w.unit(theirs)), "LOS On restores line of sight");
        applyCommand(w, reg, code(PowerCode::Los, 0));
        check(w.powerLosOff(0) == on, "LOS toggles");
        applyCommand(w, reg, code(PowerCode::Los, 0));
        check(!w.powerLosOff(0), "LOS toggles back");
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
            namespace rf = tak::retailfog;
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
    // The hash carries the codes in a POWER CODES match and nothing otherwise.
    World a, b, c; setup(a, true); setup(b, true); setup(c, false);
    check(a.stateHash() == b.stateHash(), "identical worlds hash alike");
    applyCommand(b, reg, code(PowerCode::DoubleShot, 0));
    check(a.stateHash() != b.stateHash(), "the damage mode is hashed");
    applyCommand(b, reg, code(PowerCode::DoubleShot, 0));
    check(a.stateHash() == b.stateHash(), "toggled back, the hash matches again");
    applyCommand(b, reg, code(PowerCode::Los, 1, PowerArg::Off));
    check(a.stateHash() != b.stateHash(), "line-of-sight bits are hashed");
    World d; setup(d, false);
    applyCommand(d, reg, code(PowerCode::DoubleShot, 0));
    check(c.stateHash() == d.stateHash(), "with POWER CODES off a code changes nothing");
    World e; e.setSerialThreads(true); e.setPlayerCount(2); e.setTerrain(std::vector<uint8_t>(128 * 128, 0), 128, 128, 0);
    check(c.stateHash() == e.stateHash(), "POWER CODES off hashes exactly like a world that never heard of it");
    // A room that starts with the map revealed starts +Mapping with the shroud off (retail
    // seeds MappingOn from Map Revealed): the first +Mapping brings the shroud back.
    World m; m.setSerialThreads(true); m.setPlayerCount(2); m.setTerrain(std::vector<uint8_t>(128 * 128, 0), 128, 128, 0);
    m.setPowerCodes(true, true);
    check(m.powerMappingOff(0) && m.powerMappingOff(1) && !m.powerLosOff(0), "map revealed: the shroud starts off, LOS on");
    applyCommand(m, reg, code(PowerCode::Mapping, 0));
    check(!m.powerMappingOff(0) && m.powerMappingOff(1), "map revealed: the first Mapping re-shrouds the issuer only");
    World off; off.setSerialThreads(true); off.setPlayerCount(2); off.setTerrain(std::vector<uint8_t>(128 * 128, 0), 128, 128, 0);
    off.setPowerCodes(false, true);
    check(!off.powerMappingOff(0) && off.stateHash() == e.stateHash(), "map revealed means nothing with POWER CODES off");
}

static void wire() {
    // The server accepts the seven sim codes and a valid argument, nothing else.
    const auto payload = [](const Command& c) {
        tak::net::Writer w; w.u32(1); w.cmd(c); return w.b;
    };
    for (int c = 1; c <= 7; ++c)
        check(tak::srv::validPlayerCommands(payload(code(PowerCode(c)))), "server accepts a sim code");
    check(!tak::srv::validPlayerCommands(payload(code(PowerCode::Radar))), "Radar never travels");
    check(!tak::srv::validPlayerCommands(payload(code(PowerCode::None))), "code 0 is forged");
    Command bad = code(PowerCode::Los); bad.queue = 9;
    check(!tak::srv::validPlayerCommands(payload(bad)), "an unknown argument is forged");
    // Round trip through a strict (replay) bundle read.
    tak::net::Writer w;
    tak::net::putBundle(w, 7, {code(PowerCode::ManaMe, 3, PowerArg::None, 42)}, {});
    tak::net::Reader r(w.b.data(), w.b.size());
    uint32_t tick = 0; tak::net::Bundle out;
    check(tak::net::getBundle(r, tick, out, true) && out.cmds.size() == 1 && out.cmds[0].kind == Cmd::PowerCode &&
          out.cmds[0].unitId == 42 && out.cmds[0].player == 3 && out.cmds[0].targetId == int32_t(PowerCode::ManaMe),
          "a power code survives a strict bundle read");
    // The option travels in GameOptions and the replay header.
    tak::net::GameOptions o; o.powerCodes = 1;
    tak::net::Writer ow; tak::net::put(ow, o);
    tak::net::Reader orr(ow.b.data(), ow.b.size());
    tak::net::GameOptions back; tak::net::get(orr, back);
    check(orr.ok && orr.p == orr.end && back.powerCodes == 1, "GameOptions carries POWER CODES");
    // The replay byte carries the option (bit 0) and the room's map-revealed start (bit 1),
    // so playback seeds +Mapping as the game did.
    tak::net::SlotInfo slots[tak::net::kMaxSlots] = {};
    for (uint8_t fog : {uint8_t(0), uint8_t(1), uint8_t(2)}) {
        tak::net::GameOptions g; g.powerCodes = 1; g.fogExplored = fog;
        const auto live = tak::net::matchConfigFor(g, slots, 1, false);
        const auto h = tak::net::replayHeaderFor(g, "m", "", slots, 1);
        tak::net::GameOptions replayed; tak::net::SlotInfo rs[tak::net::kMaxSlots];
        tak::net::matchInputsFrom(h, replayed, rs);
        const auto again = tak::net::matchConfigFor(replayed, rs, 1, false);
        check(live.powerCodes && live.powerMapRevealed == (fog >= 1) && again.powerCodes &&
              again.powerMapRevealed == live.powerMapRevealed, "a replay seeds +Mapping as the game did");
    }
    tak::net::GameOptions plain; plain.fogExplored = 1;
    check(tak::net::replayHeaderFor(plain, "m", "", slots, 1).powerCodes == 0 &&
          !tak::net::matchConfigFor(plain, slots, 1, false).powerMapRevealed, "POWER CODES off records 0");
}

int main() {
    parser();
    damageFormula();
    simCodes();
    wire();
    std::puts("PASS: power codes -- parser, damage formula, sim effects and gating, hash, wire");
}
