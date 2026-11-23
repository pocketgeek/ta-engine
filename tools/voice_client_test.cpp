// voice_client_test -- what a real MpClient puts on the wire for voice chat and makes of
// what comes back (protocol 276), against a scripted loopback server (no game data, no
// takserver; the server's routing is tools/server_voice_test.py).
//
//  - VoiceListen goes out only once the host switches voice on (the server's default is
//    off), and again after a reconnect.
//  - sendVoice is ignored outside a room; in one it sends exactly one VoiceUp with the
//    talker's VOICE mask, and refuses an oversized or empty (non-END) frame.
//  - VoiceDown parses with and without the talker's name, a malformed one is dropped,
//    the inbox is bounded, and a client that is not listening keeps nothing.
//  - Voice never reaches the replay log: only tick bundles do.
#include "net/client.h"
#include "net/netcompat.h"

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

using namespace tak;

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
    // Everything the client sends within `ms` (keepalives answered and skipped).
    std::vector<net::Frame> drain(net::MpClient& client, int ms = 60) {
        std::vector<net::Frame> out;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < deadline) {
            client.poll();
            if (!conn.recv()) throw std::runtime_error("server receive");
            net::Frame f;
            while (conn.poll(f)) {
                if (f.kind == net::Msg::Ping) { conn.send(net::Msg::Pong); conn.flushWrite(); continue; }
                if (f.kind == net::Msg::Pong) continue;
                out.push_back(f);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return out;
    }
    net::Frame receive(net::MpClient& client) {
        for (int i = 0; i < 40; ++i) { auto f = drain(client, 50); if (!f.empty()) return f.front(); }
        throw std::runtime_error("timed out waiting for a client request");
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

net::Writer voiceDown(uint32_t talker, uint8_t slot, uint16_t seq, uint8_t flags, size_t len, const char* name) {
    net::Writer w; w.u32(talker); w.u8(slot); w.u16(seq);
    w.u8(uint8_t(flags | (name ? net::kVoiceHasName : 0))); w.u16(uint16_t(len));
    for (size_t i = 0; i < len; ++i) w.u8(uint8_t(i * 7));
    if (name) w.str(name);
    return w;
}
}  // namespace

int main() {
    try {
        Server server; net::MpClient client;
        check(client.connect("127.0.0.1", server.port, "Talker"), "connect");
        server.accept(client);
        check(server.receive(client).kind == net::Msg::Hello, "hello");
        { net::Writer w; w.u32(5); w.str("Talker"); server.send(net::Msg::Welcome, w, client); }
        check(client.state() == net::MpClient::State::Lobby, "in the lobby");
        check(server.drain(client).empty(), "voice off: no VoiceListen is sent (the server default is off)");

        client.setVoiceListen(true);
        auto sent = server.drain(client);
        check(sent.size() == 1 && sent[0].kind == net::Msg::VoiceListen && sent[0].payload == std::vector<uint8_t>{1},
              "switching voice on sends VoiceListen 1, once");
        check(server.drain(client).empty(), "and not again while it stays on");

        // Outside a room voice goes nowhere.
        client.sendVoice(0, 0xff, net::kVoiceStart, std::vector<uint8_t>(60, 9));
        check(server.drain(client).empty(), "no VoiceUp outside a room");

        client.joinGame(12, "");
        check(server.receive(client).kind == net::Msg::JoinGame, "join request");
        { net::Writer w; w.u8(1); w.u8(1); w.str(""); w.u32(1); server.send(net::Msg::JoinResult, w, client); }
        check(client.state() == net::MpClient::State::InRoom, "seated");

        const std::vector<uint8_t> opus(60, 0x5a);
        client.sendVoice(300, 0x05, net::kVoiceStart, opus);
        sent = server.drain(client);
        {
            net::Writer want; want.u16(300); want.u8(0x05); want.u8(net::kVoiceStart); want.u16(60);
            want.b.insert(want.b.end(), opus.begin(), opus.end());
            check(sent.size() == 1 && sent[0].kind == net::Msg::VoiceUp && sent[0].payload == want.b,
                  "VoiceUp carries seq, the VOICE mask, flags and the packet exactly");
        }
        client.sendVoice(301, 0xff, 0, std::vector<uint8_t>(net::kVoiceMaxOpus + 1));
        client.sendVoice(302, 0xff, 0, {});
        check(server.drain(client).empty(), "oversized and empty audio frames are not sent");
        client.sendVoice(303, 0xff, net::kVoiceEnd, {});
        sent = server.drain(client);
        check(sent.size() == 1 && sent[0].kind == net::Msg::VoiceUp && sent[0].payload.size() == 6,
              "an END marker goes out with no audio");

        // Incoming.
        server.send(net::Msg::VoiceDown, voiceDown(9, 2, 40, net::kVoiceStart, 50, "Bruce"), client);
        server.send(net::Msg::VoiceDown, voiceDown(9, 2, 41, 0, 52, nullptr), client);
        server.send(net::Msg::VoiceDown, voiceDown(9, 2, 42, 0, net::kVoiceMaxOpus + 1, nullptr), client);
        auto in = client.takeVoice();
        check(in.size() == 2, "two good frames, the oversized one dropped");
        if (in.size() == 2) {
            check(in[0].talker == 9 && in[0].slot == 2 && in[0].seq == 40 && (in[0].flags & net::kVoiceStart) &&
                  in[0].name == "Bruce" && in[0].opus.size() == 50 && in[0].opus[3] == 21, "first frame parsed with its name");
            check(in[1].seq == 41 && in[1].name.empty() && in[1].opus.size() == 52, "second frame parsed without one");
        }
        check(client.takeVoice().empty(), "taking drains");

        // A host that stops draining holds a bounded backlog.
        for (int i = 0; i < int(net::MpClient::kVoiceInbox) + 20; ++i)
            server.conn.send(net::Msg::VoiceDown, voiceDown(9, 2, uint16_t(100 + i), 0, 10, nullptr));
        server.send(net::Msg::Ping, net::Writer{}, client);
        in = client.takeVoice();
        check(in.size() == net::MpClient::kVoiceInbox && client.voiceDropped() == 20 && in.front().seq == 120,
              "the inbox keeps the newest kVoiceInbox frames");

        // Not listening: frames are not kept, and the server is told.
        client.setVoiceListen(false);
        sent = server.drain(client);
        check(sent.size() == 1 && sent[0].kind == net::Msg::VoiceListen && sent[0].payload == std::vector<uint8_t>{0},
              "switching voice off sends VoiceListen 0");
        server.send(net::Msg::VoiceDown, voiceDown(9, 2, 50, 0, 10, nullptr), client);
        check(client.takeVoice().empty(), "a client not listening keeps no voice");

        // Voice never enters the replay log.
        client.startRecording();
        client.setVoiceListen(true);
        server.drain(client);
        server.send(net::Msg::VoiceDown, voiceDown(9, 2, 60, 0, 10, nullptr), client);
        check(client.replayLog().empty() && client.takeVoice().size() == 1, "voice is not recorded");

        // A reconnect forgets what the old connection said, and says it again.
        Server second; net::MpClient& again = client;
        check(again.connect("127.0.0.1", second.port, "Talker"), "reconnect");
        second.accept(again);
        check(second.receive(again).kind == net::Msg::Hello, "hello again");
        { net::Writer w; w.u32(6); w.str("Talker"); second.send(net::Msg::Welcome, w, again); }
        sent = second.drain(again);
        check(sent.size() == 1 && sent[0].kind == net::Msg::VoiceListen && sent[0].payload == std::vector<uint8_t>{1},
              "a new connection is told VoiceListen 1 again");
    } catch (const std::exception& e) {
        std::printf("  FAIL %s\n", e.what()); ++failures;
    }
    std::printf("voice_client: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
