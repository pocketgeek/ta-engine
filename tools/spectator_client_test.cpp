// spectator_client_test -- what a real MpClient makes of ALLOW SPECTATORS, against a
// scripted loopback server (no game data, no taserver; the server's own enforcement
// is tools/server_spectators_test.py).
//
//  - GameList carries the room's rule, so the browser can show NO WATCH.
//  - A refused Spectate leaves the reason in joinError() for the browser to show, and
//    leaves the client in the lobby, not watching.
//  - The refusal also drops the pending spectate. Before 258 it stayed set, so the next
//    ordinary JoinGame's GameStarting came up as a SPECTATOR start: the player got no
//    control of the slot the server had just seated them in.
//  - GameOver (the server ended the watched game) is taken in a room and by a watcher
//    whose GameStarting is still to come, names the game's length and winner, and is
//    forgotten when the client leaves or asks for another room; outside a room it is
//    not this client's.
#include "net/client.h"
#include "net/netcompat.h"

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

using namespace ta;

namespace {
int checks = 0, failures = 0;
void check(bool value, const char* what) {
    ++checks;
    if (!value) { ++failures; std::printf("  FAIL %s\n", what); }
}

struct Server {
    int listener = -1; uint16_t port = 0; net::Conn conn;
    Server() {
        std::string error; listener = net::listenOn(0, error, true);
        if (listener < 0) throw std::runtime_error(error);
        sockaddr_storage address{}; socklen_t n = sizeof address;
        if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &n)) throw std::runtime_error("getsockname");
        port = address.ss_family == AF_INET ? ntohs(reinterpret_cast<sockaddr_in*>(&address)->sin_port)
                                            : ntohs(reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);
    }
    ~Server() { if (listener >= 0) net::sockClose(listener); }
    void accept(net::MpClient& client) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            client.poll();
            sockaddr_storage address{}; socklen_t n = sizeof address;
            int fd = int(::accept(listener, reinterpret_cast<sockaddr*>(&address), &n));
            if (fd >= 0) { net::setupSocket(fd); conn = net::Conn(fd); return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw std::runtime_error("timed out accepting the client");
    }
    // The next request the client sends (keepalives answered and skipped).
    net::Frame receive(net::MpClient& client) {
        for (int i = 0; i < 2000; ++i) {
            client.poll();
            if (!conn.recv()) throw std::runtime_error("server receive");
            net::Frame f;
            while (conn.poll(f)) {
                if (f.kind == net::Msg::Ping) { conn.send(net::Msg::Pong); conn.flushWrite(); continue; }
                if (f.kind == net::Msg::Pong) continue;
                return f;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw std::runtime_error("timed out waiting for a client request");
    }
    // Answer the client's next Ping (the one it sends behind a LeaveGame: until that Pong
    // it drops everything of a room, MpClient::takes).
    void answerPing(net::MpClient& client) {
        for (int i = 0; i < 2000; ++i) {
            client.poll();
            if (!conn.recv()) throw std::runtime_error("server receive");
            net::Frame f;
            while (conn.poll(f))
                if (f.kind == net::Msg::Ping) { conn.send(net::Msg::Pong); conn.flushWrite(); return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw std::runtime_error("timed out waiting for the client's ping");
    }
    // Send, then a Ping: the client's Pong proves it processed the message first.
    void send(net::Msg kind, const net::Writer& w, net::MpClient& client) {
        conn.send(kind, w); conn.send(net::Msg::Ping);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            conn.flushWrite(); client.poll();
            if (!conn.recv()) throw std::runtime_error("server receive");
            net::Frame f;
            while (conn.poll(f)) if (f.kind == net::Msg::Pong) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw std::runtime_error("timed out waiting for the client to process a message");
    }
};

// A GameStarting as Server::writeSlots + sendGameStarting compose it. A non-empty
// mission keeps the client from waiting for a map package.
net::Writer gameStarting(uint32_t room, uint8_t slot, uint8_t allowSpectators) {
    net::RoomState r; r.id = room; r.name = "room"; r.mapId = "Inner Circle"; r.mission = "mission";
    r.opts.allowSpectators = allowSpectators; r.hostId = 1; r.mapsReady = true; r.capacity = 2; r.roomRev = 1;
    for (int i = 0; i < net::kMaxSlots; ++i) { r.slots[i].type = i < 2 ? 1 : 3; r.slots[i].color = uint8_t(i); }
    net::Writer w; net::put(w, r);
    w.u8(slot); w.u32(7); w.u64(42); w.u32(0);
    return w;
}
}  // namespace

int main() {
    try {
        Server server; net::MpClient client;
        check(client.connect("127.0.0.1", server.port, "Watcher"), "connect");
        server.accept(client);
        check(server.receive(client).kind == net::Msg::Hello, "hello");
        { net::Writer w; w.u32(5); w.str("Watcher"); server.send(net::Msg::Welcome, w, client); }
        check(client.state() == net::MpClient::State::Lobby, "in the lobby");

        // The browser's list: one running room with spectators off, one with them on.
        client.listGames();
        check(server.receive(client).kind == net::Msg::ListGames, "list request");
        {
            net::Writer w; w.u32(2);
            for (uint32_t id : {11u, 12u}) {
                w.u32(id); w.str(id == 11 ? "closed" : "open"); w.str("Inner Circle");
                w.u8(2); w.u8(2); w.u8(1); w.u8(0); w.u32(30); w.u8(id == 11 ? 0 : 1);
            }
            server.send(net::Msg::GameList, w, client);
        }
        const auto& games = client.games();
        check(games.size() == 2 && games[0].allowSpectators == 0 && games[1].allowSpectators == 1,
              "GameList carries each room's ALLOW SPECTATORS");

        // WATCH the closed room: refused, with the reason kept for the browser.
        client.spectate(11, "");
        check(server.receive(client).kind == net::Msg::Spectate, "spectate request");
        { net::Writer w; w.u8(0); w.u8(0); w.str("spectators are not allowed in this game"); w.u32(0);
          server.send(net::Msg::JoinResult, w, client); }
        check(client.joinError() == "spectators are not allowed in this game", "refusal reason kept");
        check(client.state() == net::MpClient::State::Lobby && !client.isSpectator(), "refused watcher stays in the lobby");

        // Then an ordinary join is seated -- and must start as a PLAYER.
        client.joinGame(12, "");
        check(client.joinError().empty(), "a new request clears the old refusal");
        check(server.receive(client).kind == net::Msg::JoinGame, "join request");
        { net::Writer w; w.u8(1); w.u8(1); w.str(""); w.u32(3); server.send(net::Msg::JoinResult, w, client); }
        check(client.state() == net::MpClient::State::InRoom && client.room().mySlot == 1, "seated in slot 1");
        check(client.room().roomRev == 3, "the JoinResult's room revision is kept for the first READY");
        server.send(net::Msg::GameStarting, gameStarting(12, 1, 1), client);
        check(client.starting(), "game starting");
        check(!client.isSpectator(), "a refused watch turned the next seated start into a spectator start");
        check(!client.isCatchUp(), "a seated fresh start reads as a spectate catch-up");
        check(client.room().mySlot == 1 && client.room().opts.allowSpectators == 1, "start keeps the slot and the rule");

        // The end of a watched game. Leave the seat (the Pong behind the LeaveGame ends
        // what the old room may still send), then watch.
        const auto gameOver = [](uint32_t ticks, uint8_t team) { net::Writer w; w.u32(ticks); w.u8(team); return w; };
        check(!client.gameOver(), "no game is over before the server says so");
        client.leaveGame();
        check(server.receive(client).kind == net::Msg::LeaveGame, "leave request");
        server.answerPing(client);
        server.send(net::Msg::GameOver, gameOver(9, 0), client);
        check(!client.gameOver(), "a GameOver outside any room is not this client's");
        client.spectate(12, "");
        check(server.receive(client).kind == net::Msg::Spectate, "watch request");
        server.send(net::Msg::GameOver, gameOver(900, 1), client);   // before its GameStarting: a watcher still loading
        check(client.gameOver() && client.gameOverTicks() == 900 && client.gameOverTeam() == 1,
              "GameOver names the game's length and the winning team");
        server.send(net::Msg::GameStarting, gameStarting(12, 0xFF, 1), client);
        check(client.isSpectator() && client.gameOver() && client.gameOverTicks() == 900,
              "a GameOver that came before the watcher's GameStarting is kept");
        server.send(net::Msg::GameOver, gameOver(901, 0xFF), client);
        check(client.gameOverTeam() == -1 && client.gameOverTicks() == 901, "0xFF is nobody");
        client.leaveGame();
        check(!client.gameOver(), "leaving forgets the game that was over");
    } catch (const std::exception& e) {
        std::printf("  FAIL %s\n", e.what()); ++failures;
    }
    std::printf("spectator_client: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
