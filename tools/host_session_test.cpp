// host_session_test -- the host's powers and the live game settings across a reconnect, as
// a real MpClient sees them, against a real takserver (tools/host_session_test.py starts it
// with a short disconnect wait). Two players play a small generated map; each runs the sim
// and reports its hashes, so the referee checks that no reconnect changes the game.
//
//   guest away   the guest's link drops and the host changes the speed while it is away
//                (0.1x -> 2x, then 2x -> 0.1x with the game paused): back in, the guest's
//                gameSpeed() and room options are the server's by the first tick it missed,
//                it is told of the pause, and room().hostId is still the host's, not its own
//   host away    the host pauses and its link drops: back in under a new client id, it is
//                the host (room().hostId == myClientId(), on both clients), and its resume
//                and speed change are the game's
//   host leaves  the guest is told it holds the host's powers, and they work
#include "net/client.h"
#include "net/matchconfig.h"
#include "sim/matchsetup.h"
#include "util/u8path.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>

using namespace tak;

namespace {
int checks = 0, failures = 0;
void check(bool value, const std::string& what) {
    ++checks;
    if (!value) { ++failures; std::printf("  FAIL %s\n", what.c_str()); std::fflush(stdout); }
}

// One player: its connection, its world and how far it has played.
struct Player {
    const char* name;
    net::MpClient c;
    sim::World world;
    uint32_t tick = 0;
    // The last reseat: taken, the first tick the server sent from, and gameSpeed() when the
    // view played that tick (0 = not yet): what a reconnected view paces its play by.
    bool reseated = false;
    uint32_t reseatFrom = UINT32_MAX;
    uint8_t speedAtFrom = 0;
};

const sim::TypeRegistry* gRegistry = nullptr;
std::map<uint32_t, uint64_t> gHashes;   // tick -> the first player's hash at it

// Play every bundle that has arrived; both players must reach the same hash at every tick.
void play(Player& p) {
    net::Bundle b;
    while (p.c.takeBundle(p.tick, b)) {
        if (p.tick == p.reseatFrom) p.speedAtFrom = p.c.gameSpeed();
        for (const auto& cmd : b.cmds) sim::applyCommand(p.world, *gRegistry, cmd);
        for (const auto& ev : b.events) sim::applyEvent(p.world, ev);
        p.world.tick(1.f / net::kServerHz);
        const uint64_t h = p.world.stateHash();
        const auto [it, fresh] = gHashes.emplace(p.tick, h);
        if (!fresh && it->second != h) throw std::runtime_error(std::string(p.name) + " diverged at tick " + std::to_string(p.tick));
        if (p.tick % net::kHashPeriod == 0) p.c.sendHash(p.tick, h);
        if (p.tick % net::kProgressPeriod == 0) p.c.sendProgress(p.tick);
        ++p.tick;
    }
}

// Pump both players (playing, and reconnecting one whose link went) until `done`, or fail.
bool pump(Player& a, Player& b, double seconds, const std::function<bool()>& done, const std::string& what) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        for (Player* p : {&a, &b}) {
            if (p->c.state() == net::MpClient::State::Done) {
                if (p->c.canReconnect()) p->c.reconnect();
                continue;
            }
            p->c.poll();
            uint32_t from = 0, last = 0;
            if (p->c.takeReseated(from, last)) { p->reseated = true; p->reseatFrom = from; p->speedAtFrom = 0; }
            if (p->c.state() == net::MpClient::State::InGame) play(*p);
            if (p->c.desynced()) throw std::runtime_error(std::string(p->name) + " desynced");
        }
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!what.empty()) check(false, "timed out: " + what);   // empty: a wait meant to run out
    return false;
}

// Drop `p`'s link (no Bye, no Leave) and wait until the server waits for its seat.
void dropLink(Player& p, Player& other) {
    const int slot = p.c.room().mySlot;
    p.c.dropLink();
    p.c.poll();   // notices the link is gone (no reconnect yet: the pump would start one)
    if (!pump(other, other, 5, [&] {
            for (const auto& w : other.c.waiting()) if (w.slot == slot) return true;
            return false;
        }, std::string("the room waits for ") + p.name))
        throw std::runtime_error("no wait");
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("usage: host_session_test <port> <data-dir>");
        const uint16_t port = uint16_t(std::stoi(argv[1]));
        const auto data = hpi::mountRetailRoot(tak::u8path(argv[2]));
        sim::TypeRegistry registry;
        sim::setupRegistry(registry, data, false);
        gRegistry = &registry;
        const uint64_t dataHash = hpi::gameplayHash(data);
        // A small generated map: no map transfer, a quick world.
        const std::string map = "~gen1~0a00d20400000000000000800080000200000100000000000000";
        Player host{"host"}, guest{"guest"};
        for (Player* p : {&host, &guest}) {
            p->c.setDataHash(dataHash);
            p->c.enableRttProbe();   // a ping a second: a paused game is not a silent seat
            if (!p->c.connect("127.0.0.1", port, p->name)) throw std::runtime_error("connect");
        }
        using S = net::MpClient::State;
        pump(host, guest, 10, [&] { return host.c.state() == S::Lobby && guest.c.state() == S::Lobby; }, "both in the lobby");
        net::GameOptions o;
        o.speedUnlock = 1;
        host.c.createGame("host session", "", map, o, 2);
        pump(host, guest, 10, [&] { return host.c.room().id != 0; }, "the room is created");
        guest.c.joinGame(host.c.room().id, "");
        pump(host, guest, 10, [&] { return guest.c.room().mySlot == 1; }, "the guest is seated");
        host.c.setSlot(0, 1, 0, 0, 0, 1);
        guest.c.setSlot(1, 1, 1, 1, 1, 1);
        pump(host, guest, 10, [&] { return host.c.room().slots[1].ready && host.c.room().mapsReady; }, "the room is ready");
        host.c.startGame();
        pump(host, guest, 20, [&] { return host.c.starting() && guest.c.starting(); }, "the game starts");
        for (Player* p : {&host, &guest}) {
            sim::MatchConfig cfg = net::matchConfigFor(p->c.room().opts, p->c.room().slots, p->c.startSeed());
            cfg.vfs = &data;
            cfg.mapPath = p->c.room().mapId;
            sim::setupMatch(p->world, registry, cfg);
        }
        for (Player* p : {&host, &guest}) p->c.reportLoaded(dataHash);
        const uint32_t hostId = host.c.myClientId(), guestId = guest.c.myClientId();
        check(host.c.room().hostId == hostId && guest.c.room().hostId == hostId, "the creator is the host");
        auto played = [&](uint32_t ticks) {
            const uint32_t to = std::max(host.tick, guest.tick) + ticks;
            return pump(host, guest, 30, [&] { return host.tick >= to && guest.tick >= to; },
                        "both play " + std::to_string(ticks) + " more ticks");
        };
        auto speed = [&](Player& by, uint8_t s) {
            auto opts = by.c.room().opts;
            opts.speed = s;
            by.c.setGameOptions(opts);
        };
        auto reseated = [&](Player& p, uint32_t oldId) {
            p.reseated = false;
            pump(host, guest, 10, [&] { return p.reseated && p.c.myClientId() != oldId; },
                 std::string(p.name) + " is reseated");
        };
        // The view paces its catch-up and play by gameSpeed() from the first tick it missed.
        auto pacedAt = [&](Player& p, uint8_t want) {
            pump(host, guest, 15, [&] { return p.tick > p.reseatFrom; }, std::string(p.name) + " plays on after reseating");
            check(p.speedAtFrom == want, std::string(p.name) + " plays its first missed tick at speed " + std::to_string(want) +
                                             " (it had " + std::to_string(p.speedAtFrom) + ")");
        };
        if (!played(60)) return 1;

        // Guest away: 0.1x -> 2x.
        speed(host, 1);
        pump(host, guest, 5, [&] { return guest.c.gameSpeed() == 1; }, "the guest plays at 0.1x");
        played(3);
        dropLink(guest, host);
        speed(host, 20);
        pump(host, host, 5, [&] { return host.c.gameSpeed() == 20; }, "the host's speed change is the game's");
        reseated(guest, guestId);
        pacedAt(guest, 20);
        check(guest.c.gameSpeed() == 20 && guest.c.room().opts.speed == 20, "the guest's speed and room options are 2x");
        check(!guest.c.paused(), "the guest is not paused");
        check(guest.c.room().hostId == hostId && guest.c.room().hostId != guest.c.myClientId(),
              "a reconnected guest is not the host");
        played(90);

        // Guest away: 2x -> 0.1x, and the host pauses.
        const uint32_t guestId2 = guest.c.myClientId();
        dropLink(guest, host);
        speed(host, 1);
        host.c.setPause(true);
        pump(host, host, 5, [&] { return host.c.gameSpeed() == 1 && host.c.paused(); }, "the host slows and pauses");
        reseated(guest, guestId2);
        pump(host, guest, 5, [&] { return guest.c.gameSpeed() == 1 && guest.c.paused(); },
             "the guest learns the speed and the pause after reseating");
        const uint32_t frozen = std::max(host.tick, guest.tick);
        pump(host, guest, 1.0, [] { return false; }, "");
        check(host.tick <= frozen + 1 && guest.tick <= frozen + 1, "the game stays paused");
        host.c.setPause(false);
        pump(host, guest, 5, [&] { return !guest.c.paused(); }, "the guest hears the resume");
        pacedAt(guest, 1);
        speed(host, 20);
        played(60);

        // Host away: it pauses, its link drops, it comes back.
        host.c.setPause(true);
        pump(host, guest, 5, [&] { return guest.c.paused(); }, "the room is paused");
        dropLink(host, guest);
        reseated(host, hostId);
        const uint32_t hostId2 = host.c.myClientId();
        pump(host, guest, 5, [&] { return host.c.room().hostId == hostId2 && guest.c.room().hostId == hostId2; },
             "the reconnected host is the host again, on both clients");
        check(host.c.paused(), "the reconnected host is told the game is paused");
        host.c.setPause(false);
        if (!pump(host, guest, 5, [&] { return !guest.c.paused() && !host.c.paused(); }, "the reconnected host resumes"))
            throw std::runtime_error("the game stayed paused");
        played(30);
        speed(host, 15);
        pump(host, guest, 5, [&] { return guest.c.gameSpeed() == 15; }, "the reconnected host changes the speed");
        played(30);

        // Host leaves.
        host.c.leaveRunningGame();
        pump(guest, guest, 5, [&] { return guest.c.room().hostId == guest.c.myClientId(); },
             "the guest is told it is the host");
        speed(guest, 20);
        pump(guest, guest, 5, [&] { return guest.c.gameSpeed() == 20; }, "the new host changes the speed");
        guest.c.setPause(true);
        pump(guest, guest, 5, [&] { return guest.c.paused(); }, "the new host pauses");
        guest.c.setPause(false);
        pump(guest, guest, 5, [&] { return !guest.c.paused(); }, "the new host resumes");
        const uint32_t to = guest.tick + 30;
        pump(guest, guest, 10, [&] { return guest.tick >= to; }, "the guest plays on");
        guest.c.leaveRunningGame();
        guest.c.poll();
        std::printf("%d checks, %d failures (host played to tick %u, guest to %u)\n", checks, failures, host.tick, guest.tick);
        if (failures) return 1;
        std::printf("PASS: host powers, speed and pause survive reconnects and pass on when the host leaves\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s (%d checks, %d failures)\n", e.what(), checks, failures);
        return 1;
    }
}
