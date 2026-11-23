// room_scope_test -- a room's messages count only while the server has the client in that
// room: a real MpClient against a scripted loopback server (no game data, no takserver).
//
// The server answers a connection's messages in order, and what it sent a client as a
// member of a room is still on its way when the client has left. The script plays the
// server's side of that: it reads the client's LeaveGame (and the Ping MpClient sends
// behind it), sends what the real server would have sent BEFORE it took the leave, and
// only then answers the Ping.
//
//  - A LobbyState after leaveGame is dropped: it used to put the client back in the room
//    it had left (create, SHOW, LEAVE in quick succession reopened a dissolved room).
//  - Every other room message that arrives after leaving changes nothing and is answered
//    with nothing.
//  - Leave A, ask for B: a late table (and a late seat move) of A never shows A; B's
//    JoinResult and LobbyState open B. A refused B leaves the client in the game list
//    with the reason.
//  - A request taken back (leaveGame while it is on its way): its late yes opens no room
//    (LeaveGame follows the request, so the server leaves the room it opened), its late
//    no is not news, and the next request's answer is not taken for it. A taken-back
//    Spectate starts no game and does not make the next seated start a spectator's.
//  - A Spectate taken back while its GameStarting waits for a package (watchLoading): the
//    bundles, pause, wait and chat taken behind it are gone, and the next game on the
//    connection plays only its own ticks.
//  - The first LobbyState after each real way in is still taken: CreateGame, JoinGame, a
//    seat move, a host succession, Spectate (GameStarting) and Rejoin (Reseated).
#include "net/client.h"
#include "net/netcompat.h"

#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace tak;
using S = net::MpClient::State;

namespace {
int checks = 0, failures = 0;
void check(bool value, const char* what) {
    ++checks;
    if (!value) { ++failures; std::printf("  FAIL %s\n", what); }
}

using Frames = std::vector<std::pair<net::Msg, net::Writer>>;

struct Server {
    int listener = -1; uint16_t port = 0; net::Conn conn;
    std::deque<net::Frame> inbox;   // what the client sent, in order (its Pings too)
    int pongs = 0;                  // the client's answers to this server's own Pings
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
        inbox.clear(); pongs = 0;
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
    // Send the frames, then a Ping: the client's Pong proves it has processed them all, and
    // everything it sent before that Pong is in the inbox.
    void send(net::MpClient& client, const Frames& frames = {}) {
        for (const auto& [kind, w] : frames) conn.send(kind, w);
        conn.send(net::Msg::Ping);
        const int want = pongs + 1;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (pongs < want) {
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error("timed out waiting for the client to process a message");
            conn.flushWrite(); client.poll();
            if (!conn.recv()) throw std::runtime_error("server receive");
            net::Frame f;
            while (conn.poll(f)) { if (f.kind == net::Msg::Pong) ++pongs; else inbox.push_back(f); }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    // The kinds of everything the client has sent since the last call, in order.
    std::vector<net::Msg> sent(net::MpClient& client) {
        send(client);
        std::vector<net::Msg> kinds;
        for (const auto& f : inbox) kinds.push_back(f.kind);
        inbox.clear();
        return kinds;
    }
    // The server takes the client's leave: the Pong to the Ping that came behind LeaveGame.
    void takeLeave() { conn.send(net::Msg::Pong); }
};

using Kinds = std::vector<net::Msg>;

// A mission room: the client waits for no map package and offers none.
net::RoomState room(uint32_t id, const std::string& name, uint32_t host, uint32_t rev, int mySlot) {
    net::RoomState r; r.id = id; r.name = name; r.mapId = "Inner Circle"; r.mission = "mission";
    r.hostId = host; r.mapsReady = true; r.capacity = 2; r.roomRev = rev;
    for (int i = 0; i < net::kMaxSlots; ++i) { r.slots[i].type = i < 2 ? 0 : 3; r.slots[i].color = uint8_t(i); }
    if (mySlot >= 0) { r.slots[mySlot].type = 1; r.slots[mySlot].name = "Player"; }
    return r;
}
net::Writer lobbyState(const net::RoomState& r) { net::Writer w; net::put(w, r); return w; }
net::Writer joinResult(bool ok, uint8_t slot, const std::string& why, uint32_t rev) {
    net::Writer w; w.u8(ok ? 1 : 0); w.u8(slot); w.str(why); w.u32(rev); return w;
}
// As Server::writeSlots + sendGameStarting compose it.
net::Writer gameStarting(const net::RoomState& r, uint8_t slot) {
    net::Writer w; net::put(w, r); w.u8(slot); w.u32(7); w.u64(42); w.u32(0); return w;
}
net::Writer tickBundle(uint32_t tick) { net::Writer w; net::putBundle(w, tick, {}, {}); return w; }
net::Writer offer(uint32_t roomId, const std::string& label) {
    net::Writer w; w.u32(roomId); w.str(label); w.str(std::string(64, 'a')); w.u32(4096); return w;
}
net::Writer roomText(uint32_t roomId, const std::string& why) { net::Writer w; w.u32(roomId); w.str(why); return w; }
net::Writer chat(const std::string& who, const std::string& text) { net::Writer w; w.str(who); w.str(text); return w; }
net::Writer roomError() { net::Writer w; w.u8(uint8_t(net::RoomRequest::Map)); w.u32(1); w.str("late refusal"); return w; }
net::Writer waiting() { net::Writer w; w.u8(1); w.u8(0); w.u32(5000); w.u8(0); w.u32(30000); return w; }
net::Writer byte(uint8_t v) { net::Writer w; w.u8(v); return w; }

void welcome(Server& server, net::MpClient& client) {
    client.poll();
    const auto hello = server.sent(client);
    check(hello == Kinds{net::Msg::Hello}, "hello");
    net::Writer w; w.u32(5); w.str("Player"); w.u8(0);
    server.send(client, {{net::Msg::Welcome, w}});
}

// In the game list, in no room, with nothing of one applied.
bool inList(const net::MpClient& c) {
    return c.state() == S::Lobby && c.room().id == 0 && c.mapStatus().empty() && c.modStatus().empty() &&
           !c.starting() && c.bufferedBundles() == 0;
}

void lobbyCases(const std::filesystem::path& scratch) {
    Server server; net::MpClient client;
    client.setMapRoot(scratch / "data"); client.setCacheRoot(scratch / "cache");
    check(client.connect("127.0.0.1", server.port, "Player"), "connect");
    server.accept(client);
    welcome(server, client);
    check(client.state() == S::Lobby, "in the game list");
    net::GameOptions opts; opts.gameMods = 0;
    const auto create = [&](const char* name) { client.createGame(name, "", "", opts, 2, false, false, "mission"); };

    // ---- CreateGame: JoinResult, then the room's first LobbyState --------------------------
    create("First");
    check(server.sent(client) == Kinds{net::Msg::CreateGame}, "create request");
    server.send(client, {{net::Msg::JoinResult, joinResult(true, 0, "", 1)},
                         {net::Msg::LobbyState, lobbyState(room(9, "First", 5, 1, 0))}});
    check(client.state() == S::InRoom && client.room().id == 9 && client.room().name == "First" && client.room().mySlot == 0,
          "create: the first LobbyState after the JoinResult is taken");

    // ---- A LobbyState after leaveGame is dropped ------------------------------------------
    uint64_t seq = client.lobbySeq();
    client.leaveGame();
    check(client.state() == S::Lobby, "left: in the game list");
    check(server.sent(client) == Kinds({net::Msg::LeaveGame, net::Msg::Ping}), "leaveGame sends LeaveGame, then a Ping to mark it");
    // The server sent this table before it took the LeaveGame (a package it had just verified).
    server.send(client, {{net::Msg::LobbyState, lobbyState(room(9, "First", 5, 2, 0))}});
    check(client.state() == S::Lobby, "a LobbyState after leaveGame put the client back in the room it left");
    check(inList(client) && client.lobbySeq() == seq, "a LobbyState after leaveGame changed the room view or its transfer state");
    server.takeLeave();
    // No server sends one now; the table alone still opens nothing.
    server.send(client, {{net::Msg::LobbyState, lobbyState(room(9, "First", 5, 3, 0))}});
    check(inList(client) && client.lobbySeq() == seq, "a LobbyState with no JoinResult before it opened a room");

    // ---- Every other room message after leaving: nothing changes, nothing is answered ------
    server.send(client, {{net::Msg::Chat, chat("Other", "late line")},
                         {net::Msg::RoomError, roomError()},
                         {net::Msg::MapOffer, offer(9, "Late Map")},
                         {net::Msg::ModOffer, offer(9, "mods")},
                         {net::Msg::MapError, roomText(9, "late map error")},
                         {net::Msg::ModError, roomText(9, "late mod error")},
                         {net::Msg::SpeedUpdate, byte(20)},
                         {net::Msg::Pause, byte(1)},
                         {net::Msg::Waiting, waiting()},
                         {net::Msg::TickBundle, tickBundle(0)},
                         {net::Msg::GameStarting, gameStarting(room(9, "First", 5, 3, 0), 0)},
                         {net::Msg::JoinResult, joinResult(true, 1, "", 3)},
                         {net::Msg::JoinResult, joinResult(false, 0, "late refusal", 0)}});
    check(inList(client), "a late room message changed the client's state, room or transfers");
    check(client.takeChat().empty(), "a left room's chat line was kept");
    check(client.roomError().empty() && client.joinError().empty() && client.error().empty(), "a late refusal or error was kept");
    check(!client.paused() && client.waiting().empty() && client.gameSpeed() == 10, "a left game's pause, wait or speed was applied");
    check(server.sent(client).empty(), "the client answered a message of a room it is not in");

    // ---- Leave A, ask for B: A never shows, B opens ---------------------------------------
    create("A");
    server.sent(client);
    server.send(client, {{net::Msg::JoinResult, joinResult(true, 0, "", 1)},
                         {net::Msg::LobbyState, lobbyState(room(9, "A", 5, 1, 0))}});
    check(client.state() == S::InRoom && client.room().name == "A", "in room A");
    client.leaveGame();
    client.joinGame(12, "");
    check(server.sent(client) == Kinds({net::Msg::LeaveGame, net::Msg::Ping, net::Msg::JoinGame}), "leave A, then the request for B");
    // Before the server took the leave: the capacity moved this client's seat in A
    // (JoinResult, then the table), with B's request already waiting here.
    server.send(client, {{net::Msg::JoinResult, joinResult(true, 1, "", 2)},
                         {net::Msg::LobbyState, lobbyState(room(9, "A", 5, 2, 1))}});
    check(client.state() == S::Lobby && client.room().id == 0, "a late seat move and table of A showed A again");
    server.takeLeave();
    server.send(client, {{net::Msg::JoinResult, joinResult(true, 1, "", 1)},
                         {net::Msg::LobbyState, lobbyState(room(12, "B", 7, 1, 1))}});
    check(client.state() == S::InRoom && client.room().id == 12 && client.room().name == "B" && client.room().mySlot == 1,
          "join: B's JoinResult and first LobbyState open B");

    // ---- In the room: a seat move, a host succession, another room's table -----------------
    seq = client.lobbySeq();
    server.send(client, {{net::Msg::JoinResult, joinResult(true, 0, "", 2)},
                         {net::Msg::LobbyState, lobbyState(room(12, "B", 7, 2, 0))}});
    check(client.state() == S::InRoom && client.room().mySlot == 0 && client.room().roomRev == 2 && client.lobbySeq() == seq + 1,
          "a seat move: its JoinResult and LobbyState are taken");
    server.send(client, {{net::Msg::LobbyState, lobbyState(room(12, "B", 5, 2, 0))}});
    check(client.room().hostId == 5 && client.lobbySeq() == seq + 2, "a host succession: the LobbyState alone is taken in the room");
    server.send(client, {{net::Msg::LobbyState, lobbyState(room(9, "A", 5, 9, 0))}});
    check(client.room().id == 12 && client.room().name == "B" && client.lobbySeq() == seq + 2, "another room's table replaced this room's");

    // ---- Leave B, ask for C, refused: in the game list with the reason ----------------------
    client.leaveGame();
    client.joinGame(13, "guess");
    server.sent(client);
    server.send(client, {{net::Msg::LobbyState, lobbyState(room(12, "B", 7, 3, -1))}});
    check(client.state() == S::Lobby && client.room().id == 0, "a late table of B showed B again");
    server.takeLeave();
    server.send(client, {{net::Msg::JoinResult, joinResult(false, 0, "wrong password", 0)}});
    check(inList(client) && client.joinError() == "wrong password", "a refused request leaves the client in the game list, with the reason");

    // ---- A request taken back: its late yes ------------------------------------------------
    client.joinGame(12, "");
    client.leaveGame();
    check(server.sent(client) == Kinds({net::Msg::JoinGame, net::Msg::LeaveGame, net::Msg::Ping}),
          "a request taken back is followed by LeaveGame (the server leaves the room it opens)");
    check(client.joinError().empty(), "a new request clears the last refusal");
    server.send(client, {{net::Msg::JoinResult, joinResult(true, 1, "", 3)},
                         {net::Msg::LobbyState, lobbyState(room(12, "B", 7, 3, 1))},
                         {net::Msg::MapOffer, offer(12, "Late Map")}});
    check(inList(client), "the late yes to a request taken back opened its room");
    check(server.sent(client).empty(), "the client answered the room of a request it took back");
    server.takeLeave();
    // ---- ... its late no -------------------------------------------------------------------
    client.joinGame(13, "guess");
    client.leaveGame();
    server.sent(client);
    server.send(client, {{net::Msg::JoinResult, joinResult(false, 0, "wrong password", 0)}});
    check(inList(client) && client.joinError().empty(), "the late no to a request taken back was kept as a refusal");
    server.takeLeave();
    // ---- ... and the next request's own answer, right behind it ------------------------------
    client.joinGame(12, "");
    client.leaveGame();
    client.joinGame(14, "");
    check(server.sent(client) == Kinds({net::Msg::JoinGame, net::Msg::LeaveGame, net::Msg::Ping, net::Msg::JoinGame}),
          "a request, taken back, and the next one");
    server.send(client, {{net::Msg::JoinResult, joinResult(true, 1, "", 3)},
                         {net::Msg::LobbyState, lobbyState(room(12, "B", 7, 3, 1))}});
    check(inList(client), "the first request's yes was taken for the second's");
    server.takeLeave();
    server.send(client, {{net::Msg::JoinResult, joinResult(false, 0, "game is full", 0)}});
    check(inList(client) && client.joinError() == "game is full", "the second request's refusal: in the game list, with the reason");

    // ---- A Spectate taken back starts no game ---------------------------------------------
    client.spectate(20, "");
    client.leaveGame();
    check(server.sent(client) == Kinds({net::Msg::Spectate, net::Msg::LeaveGame, net::Msg::Ping}), "a watch taken back is followed by LeaveGame");
    server.send(client, {{net::Msg::GameStarting, gameStarting(room(20, "Live", 7, 1, -1), 0xFF)},
                         {net::Msg::TickBundle, tickBundle(0)},
                         {net::Msg::Waiting, waiting()}});
    check(inList(client) && !client.isCatchUp() && client.waiting().empty(), "a watch taken back still opened the game");
    server.takeLeave();
    // The next join is seated and starts as a player.
    client.joinGame(12, "");
    server.sent(client);
    server.send(client, {{net::Msg::JoinResult, joinResult(true, 1, "", 3)},
                         {net::Msg::LobbyState, lobbyState(room(12, "B", 7, 3, 1))}});
    check(client.state() == S::InRoom && client.room().id == 12 && client.room().mySlot == 1, "joined after a watch taken back");
    server.send(client, {{net::Msg::GameStarting, gameStarting(room(12, "B", 7, 3, 1), 1)}});
    check(client.starting() && !client.isSpectator() && !client.isCatchUp() && client.room().mySlot == 1,
          "the start after a watch taken back is a seated player's");

    // ---- In the game: the room's table (the speed, the host) is still taken -----------------
    server.send(client, {{net::Msg::TickBundle, tickBundle(0)}});
    check(client.state() == S::InGame && client.bufferedBundles() == 1, "in the game");
    seq = client.lobbySeq();
    {
        auto r = room(12, "B", 5, 3, 1); r.opts.speed = 15;
        server.send(client, {{net::Msg::LobbyState, lobbyState(r)}});
    }
    check(client.room().hostId == 5 && client.gameSpeed() == 15 && client.lobbySeq() == seq + 1 && client.state() == S::InGame,
          "a running game's LobbyState (the host, the speed) is taken");

    // ---- Rejoin: Reseated, then the room as it is now ---------------------------------------
    server.conn.closeNow();
    for (int i = 0; i < 2000 && client.poll(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(client.state() == S::Done && client.canReconnect(), "the link dropped in the game: a reconnect is possible");
    check(client.reconnect(), "reconnect");
    server.accept(client);
    client.poll();
    check(server.sent(client) == Kinds{net::Msg::Hello}, "reconnect: hello");
    { net::Writer w; w.u32(6); w.str("Player"); w.u8(0); server.send(client, {{net::Msg::Welcome, w}}); }
    check(server.sent(client) == Kinds{net::Msg::Rejoin}, "reconnect: Rejoin");
    seq = client.lobbySeq();
    {
        net::Writer reseated; reseated.u8(1); reseated.u64(43); reseated.u32(1); reseated.u32(1);
        auto r = room(12, "B", 6, 3, 1); r.opts.speed = 20;
        server.send(client, {{net::Msg::Reseated, reseated}, {net::Msg::LobbyState, lobbyState(r)}});
    }
    check(client.state() == S::InGame && client.room().hostId == 6 && client.gameSpeed() == 20 && client.lobbySeq() == seq + 1,
          "rejoin: the LobbyState after Reseated is taken");
}

// Spectate: (the packages,) GameStarting, then the room's table in its game.
void watchCase(const std::filesystem::path& scratch) {
    Server server; net::MpClient client;
    client.setMapRoot(scratch / "data"); client.setCacheRoot(scratch / "cache");
    check(client.connect("127.0.0.1", server.port, "Player"), "watch: connect");
    server.accept(client);
    welcome(server, client);
    client.spectate(20, "");
    check(server.sent(client) == Kinds{net::Msg::Spectate}, "watch: spectate request");
    server.send(client, {{net::Msg::GameStarting, gameStarting(room(20, "Live", 7, 1, -1), 0xFF)}});
    check(client.starting() && client.isSpectator() && client.isCatchUp() && client.room().id == 20, "watch: GameStarting opens the game");
    server.send(client, {{net::Msg::LobbyState, lobbyState(room(20, "Live", 8, 1, -1))}});
    check(client.room().hostId == 8 && client.room().mySlot == -1 && client.starting(), "watch: the room's table after GameStarting is taken");
    server.send(client, {{net::Msg::TickBundle, tickBundle(0)}});
    check(client.state() == S::InGame && client.bufferedBundles() == 1, "watch: the bundle log follows");
}

// A WATCH whose GameStarting waits for a package (the game's mods, which this client does
// not hold): the stream behind it is taken while it waits, and the watch is then taken
// back. Nothing of the watched game may be left for the next game on this connection.
void heldWatchCase(const std::filesystem::path& scratch) {
    Server server; net::MpClient client;
    client.setMapRoot(scratch / "data"); client.setCacheRoot(scratch / "cache");
    check(client.connect("127.0.0.1", server.port, "Player"), "held watch: connect");
    server.accept(client);
    welcome(server, client);
    client.spectate(20, "");
    check(server.sent(client) == Kinds{net::Msg::Spectate}, "held watch: spectate request");
    check(!client.watchLoading(), "held watch: nothing is loading before the server's answer");
    // As Server::sendGameStarting(catchUp): ModOffer, GameStarting, then the bundle log (a
    // generated map: nothing holds the stream at the server) and what the room is told.
    {
        auto live = room(20, "Live", 7, 1, -1); live.mission.clear(); live.mapId = "~gen1~x"; live.opts.gameMods = 1;
        net::Writer start; net::put(start, live); start.u8(0xFF); start.u32(7); start.u64(0); start.u32(3);
        server.send(client, {{net::Msg::ModOffer, offer(20, "mods")},
                             {net::Msg::GameStarting, start},
                             {net::Msg::TickBundle, tickBundle(0)},
                             {net::Msg::TickBundle, tickBundle(1)},
                             {net::Msg::TickBundle, tickBundle(2)},
                             {net::Msg::Pause, byte(1)},
                             {net::Msg::Waiting, waiting()},
                             {net::Msg::Chat, chat("Other", "a line of the watched game")}});
    }
    check(server.sent(client) == Kinds{net::Msg::ModRequest}, "held watch: the game's mods are asked for");
    check(client.state() == S::Lobby && !client.starting(), "held watch: GameStarting waits for the game's mods");
    check(client.watchLoading() && client.modStatus() == "DOWNLOADING GAME MODS", "held watch: it says it is loading, and what");
    check(client.bufferedBundles() == 3 && client.paused(), "held watch: the stream behind GameStarting is taken while it waits");
    // CANCEL in the WATCH window: the request is taken back.
    client.leaveGame();
    check(server.sent(client) == Kinds({net::Msg::LeaveGame, net::Msg::Ping}), "held watch: taken back with LeaveGame, then a Ping");
    server.send(client, {{net::Msg::TickBundle, tickBundle(3)}});   // still on its way
    server.takeLeave();
    server.send(client);
    check(inList(client) && !client.watchLoading(), "held watch taken back: in the game list, nothing loading");
    check(client.bufferedBundles() == 0, "held watch taken back: its bundles are still buffered");
    check(!client.paused() && client.waiting().empty(), "held watch taken back: the watched game's pause or wait is still set");
    check(client.takeChat().empty(), "held watch taken back: the watched game's chat line was kept");
    // The player then joins a room and its game starts: its ticks are its own.
    client.joinGame(12, "");
    server.sent(client);
    server.send(client, {{net::Msg::JoinResult, joinResult(true, 1, "", 3)},
                         {net::Msg::LobbyState, lobbyState(room(12, "B", 7, 3, 1))}});
    check(client.state() == S::InRoom && client.room().id == 12, "joined after a held watch taken back");
    server.send(client, {{net::Msg::GameStarting, gameStarting(room(12, "B", 7, 3, 1), 1)}});
    check(client.starting() && !client.isSpectator() && client.bufferedBundles() == 0 && !client.paused(),
          "the start after a held watch taken back holds none of the watched game's ticks");
    server.send(client, {{net::Msg::TickBundle, tickBundle(0)}});   // the game's first tick, and only that
    net::Bundle bundle;
    const bool first = client.takeBundle(0, bundle), second = client.takeBundle(1, bundle), third = client.takeBundle(2, bundle);
    check(first && !second && !third, "the game after a held watch taken back played the watched game's bundles as its ticks");
}
}  // namespace

int main() {
    // The client's data and cache folders: empty scratch ones (nothing here reads or writes a profile).
    const auto scratch = std::filesystem::temp_directory_path() /
        ("tak-room-scope-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(scratch / "data");
    std::filesystem::create_directories(scratch / "cache");
    for (const auto& run : {lobbyCases, watchCase, heldWatchCase}) {
        try { run(scratch); }
        catch (const std::exception& e) { std::printf("  FAIL %s\n", e.what()); ++failures; }
    }
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    std::printf("room_scope: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
