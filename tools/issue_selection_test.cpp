// issueSelection client equivalence and the .scn format.
//
//   issue_selection_test [DATA]
//
// GameView (the SDL client) cannot be linked from tools/, so the client's
// path is transcribed below as ClientRef, statement for statement, each part
// citing the lines it copies. ClientRef keeps the client's own structure --
// an outbox drained by mpStep once per frame, a server queue closed once per
// tick, bundles that come back after a real uplink and downlink delay -- and
// issueSelection (tools/legion_issue_selection.h) folds all of that into one
// round trip R. Each case checks the two Cmd streams are equal, field for
// field, tick for tick (shifted by the uplink delay), plus the expected shape
// by hand: in Retail 64 per tick, consecutive ticks, the 512 window's gap; in
// Legion (protocol 267) one explicit order per click, landing whole in one tick,
// the right-click move as OrderShape::Offsets -- and applied in the sim, every
// member gets the goal the per-unit loop gives it.
//
// The second half parses a sample .scn of each map kind (ascii, flat, gen1,
// snapshot), round-trips it through the canonical writer, and builds its
// World twice: the state hashes must agree (and differ for another start
// offset). DATA (a retail install) also builds the data-gated sample.
#include "legion_issue_selection.h"
#include "legion_scn.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <set>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace tak::sim;
using tak::net::Cmd;
using tak::net::Command;
using tak::scn::CmdBatch;
using tak::scn::SelectionOrder;
using tak::scn::Verb;

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", what.c_str()); ++failures; }
}

// ---- the client, transcribed ----------------------------------------------
struct ClientRef {
    const World& world_;
    int localPlayer_ = 0;
    std::vector<int> selection_;
    // gameview.h (cmdCredit_, cmdInFlight_, the order window) and the send state mpStep keeps.
    std::vector<Command> outbox_;
    size_t outboxHead_ = 0;
    int cmdCredit_ = tak::net::kCmdCapPerTick;
    int cmdInFlight_ = 0;
    uint32_t lastSendTick_ = 0;
    uint32_t nextOrderSerial_ = 1;
    tak::net::OrderWindow orderWindow_;
    // The open click (GameView::ClickScope / flushClick).
    std::vector<Command> click_;
    tak::net::ClickShape shape_;

    explicit ClientRef(const World& w) : world_(w) {}
    const Unit* frameUnitP(int id) const {
        const Unit* u = world_.unit(id);
        return u && u->alive() ? u : nullptr;
    }
    // gameview_impl.cpp GameView::issue (mp_ set, a click open: held for the click).
    void issue(Command c) {
        c.player = uint8_t(localPlayer_);
        click_.push_back(c);
    }
    // gameview_impl.cpp GameView::flushClick: a Legion game packs the click.
    void flushClick() {
        uint32_t provisional = nextOrderSerial_;   // the serial is given at send (mpStep)
        if (!isLegionPathfinding(world_.pathfindingMode())) outbox_.insert(outbox_.end(), click_.begin(), click_.end());
        else for (auto& c : tak::net::packClick(click_, shape_, provisional)) outbox_.push_back(std::move(c));
        click_.clear();
        shape_ = {};
    }
    // gameview_hud.cpp rightClickOrder: right-click on open ground (setClickMove first).
    void rightClickMove(float wx, float wz, bool queue) {
        shape_.moveOffsets = true; shape_.x = wx; shape_.z = wz;
        const bool legion = isLegionPathfinding(world_.pathfindingMode());
        float cx = 0, cz = 0;
        int n = 0;
        FlyerGroupCentre air;
        for (int id : selection_)
            if (const auto* u = frameUnitP(id)) {
                cx += u->x.toFloat(); cz += u->z.toFloat(); ++n;
                air.add(Fixed::fromFloat(u->x.toFloat()).v, Fixed::fromFloat(u->z.toFloat()).v);
            }
        if (n) { cx /= float(n); cz /= float(n); }
        for (int id : selection_) {
            const auto* u = frameUnitP(id);
            if (!u) continue;
            Command c;
            c.kind = Cmd::Move;
            c.unitId = id;
            const bool shared = u->type && legion &&
                legionSharedClick(u->type->canFly, u->type->footX, u->type->footZ);
            if (u->type && u->type->canFly) tak::scn::flyerSpreadGoal(air, *u, wx, wz, c.x, c.z);
            else {
                c.x = shared ? wx : wx + std::clamp(u->x.toFloat() - cx, -60.0f, 60.0f);
                c.z = shared ? wz : wz + std::clamp(u->z.toFloat() - cz, -60.0f, 60.0f);
            }
            c.queue = queue;
            issue(c);
        }
    }
    // gameview_hud.cpp:230-237: right-click on an enemy.
    void rightClickAttack(int enemy, bool queue) {
        for (int id : selection_) {
            Command c;
            c.kind = Cmd::Attack;
            c.unitId = id;
            c.targetId = enemy;
            c.queue = queue;
            issue(c);
        }
    }
    // gameview_hud.cpp:994-1013 (armed G) and 1062-1090 (armed F / P / M, no enemy;
    // M and P give flyers retail's spread and declare it, setFlyerSpread).
    void armedOrder(char cmd, float wx, float wz, int buddy, bool queue) {
        if (cmd == 'g') {
            for (int id : selection_) {
                if (id == buddy) continue;
                Command c;
                c.kind = Cmd::Guard;
                c.unitId = id;
                c.targetId = buddy;
                c.queue = queue ? 1 : 0;
                issue(c);
            }
            return;
        }
        const bool spread = cmd == 'm' || cmd == 'p' || cmd == 'f' || cmd == 'a';
        FlyerGroupCentre air;
        if (spread) {
            for (int id : selection_)
                if (const auto* u = frameUnitP(id))
                    air.add(Fixed::fromFloat(u->x.toFloat()).v, Fixed::fromFloat(u->z.toFloat()).v);
            shape_.flyerOffsets = true; shape_.x = wx; shape_.z = wz;   // setFlyerSpread
        }
        for (int id : selection_) {
            Command c;
            c.kind = (cmd == 'f' || cmd == 'a') ? Cmd::AttackMove : cmd == 'p' ? Cmd::Patrol : Cmd::Move;
            c.x = wx;
            c.z = wz;
            const auto* u = spread ? frameUnitP(id) : nullptr;
            if (u && u->type && u->type->canFly) tak::scn::flyerSpreadGoal(air, *u, wx, wz, c.x, c.z);
            c.unitId = id;
            c.queue = queue ? 1 : 0;
            issue(c);
        }
    }
    // gameview_impl.cpp:4707-4714 (Act::Stop).
    void stop() {
        for (int id : selection_) {
            Command c;
            c.kind = Cmd::Stop;
            c.unitId = id;
            issue(c);
        }
    }
    // gameview_impl.cpp:4748-4767 (issueSquad, assignSquad).
    void issueSquad(int unitId, int val) {
        Command c;
        c.kind = Cmd::SetSquad;
        c.unitId = unitId;
        c.targetId = val;
        issue(c);
    }
    void assignSquad(int num, int sign, bool append) {
        if (num < 1 || num > 10 || selection_.empty()) return;
        int want = sign * num;
        std::set<int> sel(selection_.begin(), selection_.end());
        for (const auto& u : world_.units())   // front().live
            if (u.alive() && u.player == localPlayer_ && std::abs(int(u.squad)) == num && sel.find(u.id) == sel.end())
                issueSquad(u.id, append ? want : 0);
        for (int id : selection_)
            if (const auto* u = frameUnitP(id); u && u->alive() && u->player == localPlayer_) issueSquad(id, want);
    }
    // gameview_net.cpp mpStep, the send half: one FIFO; command runs under the
    // credit and window, an order whole (its transaction) under the order window.
    // netTick_ is the last bundle this client has simulated.
    std::vector<Command> mpStepSend(uint32_t netTick_) {
        if (netTick_ != lastSendTick_) {
            cmdCredit_ = tak::net::cmdSendCredit(cmdCredit_, netTick_ - lastSendTick_);
            lastSendTick_ = netTick_;
        }
        std::vector<Command> sent;
        if (outboxHead_ < outbox_.size()) {
            int sendable = tak::net::cmdSendWindow(cmdCredit_, cmdInFlight_);
            size_t at = outboxHead_;
            while (at < outbox_.size()) {
                size_t end = at;
                while (end < outbox_.size() && outbox_[end].kind != Cmd::Order && int(end - at) < sendable) ++end;
                if (end > at) {
                    const int n = int(end - at);
                    cmdCredit_ -= n; cmdInFlight_ += n; sendable -= n;
                    sent.insert(sent.end(), outbox_.begin() + ptrdiff_t(at), outbox_.begin() + ptrdiff_t(end));
                    at = end;
                    continue;
                }
                auto& item = outbox_[at];
                if (item.kind != Cmd::Order || !item.order) break;
                const size_t bytes = tak::net::orderRecordBytes(*item.order);
                if (!orderWindow_.fits(bytes)) break;
                std::const_pointer_cast<tak::net::SelectionOrder>(item.order)->serial = nextOrderSerial_++;
                sent.push_back(item);
                orderWindow_.sent(item.order->serial, bytes);
                ++at;
            }
            outboxHead_ = at;
            if (outboxHead_ == outbox_.size()) { outbox_.clear(); outboxHead_ = 0; }
        }
        return sent;
    }
    // gameview_net.cpp simTick: our own commands and orders in a bundle retire their windows.
    void bundleArrived(const std::vector<Command>& cmds) {
        for (const auto& c : cmds) {
            if (int(c.player) != localPlayer_) continue;
            if (c.kind == Cmd::Order) {
                if (c.order) orderWindow_.retire(c.order->serial);
            } else if (cmdInFlight_ > 0) --cmdInFlight_;
        }
    }
};

// The wire around it: a client frame per tick, `up` ticks to the server, the
// server's closeTick drain (server.cpp:2819-2823), `down` ticks back. Returns
// the bundle of every server tick from `issueTick` until all is delivered.
struct Wire {
    ClientRef& client;
    int up = 0, down = 0;
    Wire(ClientRef& c, int u, int d) : client(c), up(u), down(d) {}
    std::map<uint32_t, std::vector<Command>> toServer;   // arrival tick -> commands
    std::deque<Command> cmdQueue;                        // Server::Client::cmdQueue
    std::map<uint32_t, std::vector<Command>> bundles;    // server tick -> bundle
    uint32_t netTick = 0;                                // last bundle the client simulated
    bool any = false;

    void frame(uint32_t t) {
        // Client frame at time t: send (mpStep's first half) ...
        if (auto sent = client.mpStepSend(netTick); !sent.empty()) {
            auto& q = toServer[t + uint32_t(up)];
            q.insert(q.end(), sent.begin(), sent.end());
        }
        // ... the server closes tick t ...
        if (auto it = toServer.find(t); it != toServer.end()) {
            cmdQueue.insert(cmdQueue.end(), it->second.begin(), it->second.end());
            toServer.erase(it);
        }
        // server.cpp closeTick: kCmdCapPerTick commands, orders within kTickOrderBytes, FIFO.
        std::vector<Command> pending;
        int taken = 0;
        size_t orderBytes = 0;
        while (!cmdQueue.empty()) {
            const Command& head = cmdQueue.front();
            if (head.kind == Cmd::Order) {
                const size_t b = head.order ? tak::net::orderRecordBytes(*head.order) : 0;
                if (orderBytes + b > tak::net::kTickOrderBytes) break;
                orderBytes += b;
            } else if (taken < tak::net::kCmdCapPerTick) ++taken;
            else break;
            pending.push_back(head);
            cmdQueue.pop_front();
        }
        bundles[t] = pending;
        // ... and the client drains every bundle that has reached it (mpStep's second half).
        if (t >= uint32_t(down)) {
            const uint32_t s = t - uint32_t(down);
            client.bundleArrived(bundles[s]);
            netTick = s;
        }
    }
    bool drained() const {
        return client.outbox_.size() == client.outboxHead_ && toServer.empty() && cmdQueue.empty();
    }
};

// Bundles [from, ...) until the wire drains, trimmed of trailing empties.
std::vector<CmdBatch> runWire(Wire& wire, uint32_t from, uint32_t issueTick, const std::function<void()>& click) {
    for (uint32_t t = 0; t < issueTick; ++t) wire.frame(t);   // an idle client before the click
    click();
    wire.client.flushClick();
    std::vector<CmdBatch> out;
    uint32_t t = issueTick;
    for (; !wire.drained() || t <= issueTick; ++t) wire.frame(t);
    for (uint32_t s = from; s < t; ++s) out.push_back({s, wire.bundles[s]});
    while (!out.empty() && out.back().cmds.empty()) out.pop_back();
    return out;
}

bool sameCmd(const Command& a, const Command& b) {
    const bool fields = a.kind == b.kind && a.player == b.player && a.unitId == b.unitId && a.targetId == b.targetId &&
           std::memcmp(&a.x, &b.x, sizeof a.x) == 0 && std::memcmp(&a.z, &b.z, sizeof a.z) == 0 &&
           a.queue == b.queue && std::memcmp(a.type, b.type, sizeof a.type) == 0 && a.x2 == b.x2 && a.z2 == b.z2;
    if (!fields || !a.order != !b.order) return false;
    if (!a.order) return true;
    const auto &p = *a.order, &q = *b.order;
    Command pt = p.tmpl, qt = q.tmpl;
    pt.order = qt.order = nullptr;
    return p.serial == q.serial && p.shape == q.shape && p.members == q.members && sameCmd(pt, qt);
}
// The client's stream (shifted back by `shift` ticks of uplink delay) equals issueSelection's.
bool sameStream(const std::vector<CmdBatch>& mine, const std::vector<CmdBatch>& client, int shift,
                const std::string& what) {
    auto trimmed = mine;
    while (!trimmed.empty() && trimmed.back().cmds.empty()) trimmed.pop_back();
    if (trimmed.size() != client.size()) {
        std::fprintf(stderr, "  %s: %zu ticks vs client %zu\n", what.c_str(), trimmed.size(), client.size());
        return false;
    }
    for (size_t i = 0; i < trimmed.size(); ++i) {
        if (trimmed[i].tick + uint32_t(shift) != client[i].tick || trimmed[i].cmds.size() != client[i].cmds.size()) {
            std::fprintf(stderr, "  %s: batch %zu tick %u size %zu vs client tick %u size %zu\n", what.c_str(), i,
                         trimmed[i].tick, trimmed[i].cmds.size(), client[i].tick, client[i].cmds.size());
            return false;
        }
        for (size_t j = 0; j < trimmed[i].cmds.size(); ++j)
            if (!sameCmd(trimmed[i].cmds[j], client[i].cmds[j])) {
                std::fprintf(stderr, "  %s: batch %zu cmd %zu differs\n", what.c_str(), i, j);
                return false;
            }
    }
    return true;
}

// ---- fixtures --------------------------------------------------------------
UnitType ground(int foot) {
    UnitType t{};
    t.id = t.name = "sel-foot-" + std::to_string(foot);
    t.canMove = true; t.maxHp = 100; t.footX = t.footZ = foot; t.sight = 4096;
    t.maxVel = Fixed::raw(117964); t.accel = t.brake = Fixed::fromInt(10);
    t.turnRate = t.turnInPlaceRate = 2500; t.halfCellTicks = 3; t.buildTime = 1;
    return t;
}
UnitType flyer() {
    UnitType t{};
    t.id = t.name = "sel-flyer";
    t.canFly = t.canMove = true; t.maxHp = 100; t.footX = t.footZ = 2; t.sight = 4096;
    t.maxVel = Fixed::fromInt(4); t.accel = t.brake = Fixed::fromInt(1); t.turnRate = 1200; t.cruiseAlt = 80;
    t.buildTime = 1; t.vtolStandby = true;
    return t;
}
struct Field {
    World world;
    Field(PathfindingMode mode) {
        world.setGameSeed(7); world.setSerialThreads(true); world.setPathService(true);
        world.setPathfindingMode(mode);
        world.setPlayerCount(2); world.setTeam(0, 0); world.setTeam(1, 1);
        world.setTerrain(std::vector<uint8_t>(size_t(512) * 512, 100), 512, 512, 64);
    }
    // Bodies in rows of `perRow` from cell (x, z), pitch `pitch`.
    std::vector<int> block(const UnitType& t, int n, int x, int z, int perRow, int pitch, int player = 0) {
        std::vector<int> ids;
        for (int i = 0; i < n; ++i) {
            const int id = world.spawn(&t, float((x + (i % perRow) * pitch) * 16), float((z + (i / perRow) * pitch) * 16),
                                       std::nullopt, player);
            if (id <= 0) throw std::runtime_error("spawn failed");
            ids.push_back(id);
        }
        return ids;
    }
};

constexpr uint32_t kIssueTick = 100;   // an idle client: its credit has saturated

// issueSelection vs the client for one click; every (up, down) split of R.
void compare(const World& w, const std::vector<int>& sel, const SelectionOrder& o, int roundTrip,
             const std::function<void(ClientRef&)>& click, const std::string& what,
             std::vector<CmdBatch>* keep = nullptr) {
    const auto mine = tak::scn::issueSelection(w, sel, o, kIssueTick, 0, roundTrip, 1);   // serials from 1, as ClientRef
    if (keep) *keep = mine;
    // No window (roundTrip < 0) is an uplink whose acks come back before the
    // window can fill: R = 1, the client one frame behind the server.
    const int R = roundTrip < 0 ? 1 : roundTrip;
    for (int up = 0; up < R; ++up) {
        ClientRef client(w);
        client.selection_ = sel;
        Wire wire{client, up, R - 1 - up};
        const auto theirs = runWire(wire, kIssueTick + uint32_t(up), kIssueTick, [&] { click(client); });
        check(sameStream(mine, theirs, up, what), what + ": issueSelection == client HUD path (up " +
                                                       std::to_string(up) + ", down " + std::to_string(R - 1 - up) + ")");
    }
}

// The expected shape: per-tick sizes from `from`, every unit once in selection order.
void shape(const std::vector<CmdBatch>& b, const std::vector<int>& sizes, const std::string& what) {
    bool ok = b.size() >= sizes.size();
    for (size_t i = 0; ok && i < sizes.size(); ++i) ok = b[i].tick == kIssueTick + i && int(b[i].cmds.size()) == sizes[i];
    for (size_t i = sizes.size(); ok && i < b.size(); ++i) ok = b[i].cmds.empty();
    if (!ok) {
        std::fprintf(stderr, "  %s sizes:", what.c_str());
        for (const auto& x : b) std::fprintf(stderr, " %u:%zu", x.tick, x.cmds.size());
        std::fprintf(stderr, "\n");
    }
    check(ok, what + ": lands 64 per tick over consecutive ticks");
}
std::vector<int> split(int n) {
    std::vector<int> s;
    for (; n > 0; n -= 64) s.push_back(std::min(n, 64));
    return s;
}
std::vector<int> unitsOf(const std::vector<CmdBatch>& b) {
    std::vector<int> ids;
    for (const auto& x : b) for (const auto& c : x.cmds) ids.push_back(c.unitId);
    return ids;
}
// Legion: the click landed as ONE explicit order in the click's tick, over exactly `ids`.
const tak::net::SelectionOrder* oneOrder(const std::vector<CmdBatch>& b, std::vector<int> ids, Cmd verb,
                                         const std::string& what) {
    std::sort(ids.begin(), ids.end());
    const bool ok = !b.empty() && b[0].tick == kIssueTick && b[0].cmds.size() == 1 &&
                    b[0].cmds[0].kind == Cmd::Order && b[0].cmds[0].order &&
                    b[0].cmds[0].order->tmpl.kind == verb &&
                    std::vector<int>(b[0].cmds[0].order->members.begin(), b[0].cmds[0].order->members.end()) == ids &&
                    std::all_of(b.begin() + 1, b.end(), [](const CmdBatch& x) { return x.cmds.empty(); });
    check(ok, what + ": one explicit order over the selection, whole in the click's tick");
    return ok ? b[0].cmds[0].order.get() : nullptr;
}
// Apply batches in a world from its current tick.
void landAll(World& w, const std::vector<CmdBatch>& b) {
    tak::sim::TypeRegistry reg;
    for (const auto& x : b) {
        while (w.tickCount() < x.tick) w.tick(1.f / 30);
        for (const auto& c : x.cmds) applyCommand(w, reg, c);
    }
}

// Retail's flyer spread (0x5214b0), written out by hand: the centre is the
// truncated mean of the selection's whole-pixel positions; a flyer keeps its
// (sub-pixel) offset from it when floor(dx^2) + floor(dz^2) <= 3000 n, else it
// takes the click. Ground units are not given it.
std::pair<double, double> retailFlyerGoal(const World& w, const std::vector<int>& sel, int id, double wx, double wz) {
    long long sx = 0, sz = 0, n = 0;
    for (int s : sel) {
        const auto* u = w.unit(s);
        sx += (long long)std::floor((u->x.v / 65536.0)); sz += (long long)std::floor((u->z.v / 65536.0)); ++n;
    }
    const auto* u = w.unit(id);
    const double dx = (u->x.v / 65536.0) - double(sx / n), dz = (u->z.v / 65536.0) - double(sz / n);
    if (std::floor(dx * dx) + std::floor(dz * dz) > 3000.0 * double(n)) return {wx, wz};
    return {wx + dx, wz + dz};
}

// The goal each unit took: from the commands (Retail), or as the sim gave them
// (Legion's explicit order). A Move's goal is its last leg, a Patrol's its first.
std::map<int, std::pair<double, double>> goalsOf(World& w, const std::vector<CmdBatch>& b, bool legion, bool patrol) {
    std::map<int, std::pair<double, double>> out;
    if (!legion) {
        for (const auto& x : b)
            for (const auto& c : x.cmds) out[c.unitId] = {c.x, c.z};
        return out;
    }
    landAll(w, b);
    for (const auto& x : b)
        for (const auto& c : x.cmds)
            if (c.order)
                for (const int id : c.order->members)
                    if (const auto* u = w.unit(id); u && !u->orders.empty()) {
                        const auto& o = patrol ? u->orders.front() : u->orders.back();
                        out[id] = {o.x.v / 65536.0, o.z.v / 65536.0};
                    }
    return out;
}

void flyerSpread() {
    static const auto a = flyer(), g = ground(2);   // the World keeps a pointer to the type
    auto spawnAt = [](Field& f, const UnitType& t, const std::vector<std::pair<float, float>>& at) {
        std::vector<int> ids;
        for (const auto& [x, z] : at) {
            const int id = f.world.spawn(&t, x, z, std::nullopt, 0);
            if (id <= 0) throw std::runtime_error("spawn failed");
            ids.push_back(id);
        }
        return ids;
    };
    auto near = [](double a, double b) { return std::abs(a - b) < 1e-3; };
    for (PathfindingMode mode : {PathfindingMode::Legion, PathfindingMode::Retail}) {
        const bool legion = mode == PathfindingMode::Legion;
        const std::string m = legion ? "legion" : "retail";
        // Retail's probe groups (emulated 0x5214b0): five flyers in a line 40 px
        // apart keep -80..+80 (past our old +-60 clamp); 80 px apart, the two
        // ends (160 px out, reach sqrt(15000) = 122) take the click; three at
        // sub-pixel x keep their offsets from the WHOLE-pixel centre (150).
        struct Case { const char* name; std::vector<std::pair<float, float>> at; std::vector<std::pair<double, double>> want; };
        const std::vector<Case> cases = {
            {"line 40", {{200, 300}, {240, 300}, {280, 300}, {320, 300}, {360, 300}},
             {{920, 1000}, {960, 1000}, {1000, 1000}, {1040, 1000}, {1080, 1000}}},
            {"line 80", {{200, 300}, {280, 300}, {360, 300}, {440, 300}, {520, 300}},
             {{1000, 1000}, {920, 1000}, {1000, 1000}, {1080, 1000}, {1000, 1000}}},
            {"sub-pixel", {{100.75f, 100}, {150.5f, 100}, {201.25f, 100}},
             {{950.75, 1000}, {1000.5, 1000}, {1051.25, 1000}}},
        };
        for (const auto& k : cases) {
            Field f(mode);
            const auto ids = spawnAt(f, a, k.at);
            SelectionOrder o;
            o.verb = Verb::Move; o.x = 1000; o.z = 1000;
            const std::string what = m + " flyer spread " + k.name;
            std::vector<CmdBatch> b;
            compare(f.world, ids, o, -1, [&](ClientRef& c) { c.rightClickMove(o.x, o.z, false); }, what, &b);
            if (legion) oneOrder(b, ids, Cmd::Move, what);
            bool spawned = true;
            for (size_t i = 0; i < ids.size(); ++i)
                spawned &= f.world.unit(ids[i])->x.toFloat() == k.at[i].first && f.world.unit(ids[i])->z.toFloat() == k.at[i].second;
            check(spawned, what + ": the flyers stand where they were spawned");
            const auto got = goalsOf(f.world, b, legion, false);
            bool ok = got.size() == ids.size();
            for (size_t i = 0; ok && i < ids.size(); ++i) {
                const auto& [gx, gz] = got.at(ids[i]);
                ok = near(gx, k.want[i].first) && near(gz, k.want[i].second);
                if (!ok) std::fprintf(stderr, "  %s: unit %d goal %.3f,%.3f want %.3f,%.3f\n", what.c_str(), ids[i], gx, gz,
                                      k.want[i].first, k.want[i].second);
            }
            check(ok, what + ": flyers keep retail's unclamped offset within sqrt(3000 n), else the click");
        }

        // A mixed group, right-click: the flyers' centre counts the ground units
        // too; the ground keeps OUR rule (Retail: +-60 px per axis; Legion: the click).
        // Armed PATROL: flyers the spread, ground the point (retail's ground Patrol
        // has no offset flag). Armed FIGHT: a flyer's leg is VTOL_Move, which carries
        // the flag (retail has no fight-move), so the flyers spread as on a move.
        for (const Verb verb : {Verb::Move, Verb::Patrol, Verb::Fight}) {
            Field f(mode);
            auto sel = spawnAt(f, a, {{1600, 1600}, {1690, 1600}, {1600, 1690}, {1440, 1440}, {2400, 2400}});
            const auto foot = f.block(g, 12, 100, 100, 4, 3);   // px 1600..1744 x 1600..1696
            sel.insert(sel.end(), foot.begin(), foot.end());
            SelectionOrder o;
            o.verb = verb; o.x = 4000; o.z = 3000;
            const char* v = verb == Verb::Move ? "move" : verb == Verb::Patrol ? "patrol" : "fight";
            const std::string what = m + " mixed flyer spread " + v;
            std::vector<CmdBatch> b;
            if (verb == Verb::Move) compare(f.world, sel, o, -1, [&](ClientRef& c) { c.rightClickMove(o.x, o.z, false); }, what, &b);
            else compare(f.world, sel, o, -1, [&](ClientRef& c) { c.armedOrder(verb == Verb::Patrol ? 'p' : 'f', o.x, o.z, -1, false); }, what, &b);
            if (legion) {
                if (const auto* order = oneOrder(b, sel, verb == Verb::Move ? Cmd::Move : verb == Verb::Patrol ? Cmd::Patrol : Cmd::AttackMove, what))
                    check(order->shape == (verb == Verb::Move ? tak::net::OrderShape::Offsets
                                                              : tak::net::OrderShape::FlyerOffsets),
                          what + ": the order's shape");
            }
            // The expected goals, from the positions the click saw (goalsOf lands a Legion order later).
            std::map<int, std::pair<double, double>> want;
            float cx = 0, cz = 0;
            for (int id : sel) { cx += f.world.unit(id)->x.toFloat(); cz += f.world.unit(id)->z.toFloat(); }
            cx /= float(sel.size()); cz /= float(sel.size());
            for (int id : sel) {
                const auto* u = f.world.unit(id);
                if (u->type->canFly) want[id] = retailFlyerGoal(f.world, sel, id, o.x, o.z);
                else if (legion || verb != Verb::Move) want[id] = {o.x, o.z};
                else want[id] = {o.x + std::clamp(u->x.toFloat() - cx, -60.0f, 60.0f), o.z + std::clamp(u->z.toFloat() - cz, -60.0f, 60.0f)};
            }
            const auto got = goalsOf(f.world, b, legion, verb != Verb::Move);
            bool ok = got.size() == sel.size(), kept = false, far = false;
            for (int id : sel) {
                if (!got.count(id)) { ok = false; continue; }
                const auto& [gx, gz] = got.at(id);
                const auto& [wx, wz] = want.at(id);
                // Legion packs its ground's shared click into arrival slots: not this rule's to check.
                if (legion && !f.world.unit(id)->type->canFly) continue;
                if (!near(gx, wx) || !near(gz, wz)) {
                    ok = false;
                    std::fprintf(stderr, "  %s: unit %d goal %.3f,%.3f want %.3f,%.3f\n", what.c_str(), id, gx, gz, wx, wz);
                }
                if (f.world.unit(id)->type->canFly) {
                    kept |= std::abs(gx - o.x) > 60.5 || std::abs(gz - o.z) > 60.5;
                    far |= gx == o.x && gz == o.z;
                }
            }
            check(ok, what + ": flyers take retail's spread, ground our rule");
            check(kept && far, what + ": a flyer keeps an offset past 60 px and the straggler takes the click");
        }
    }
}

void selections() {
    // 1, 64, 65, 200, 450 ground bodies, right-click move, both modes.
    for (PathfindingMode mode : {PathfindingMode::Legion, PathfindingMode::Retail}) {
        const char* m = mode == PathfindingMode::Legion ? "legion" : "retail";
        for (int n : {1, 64, 65, 200, 450}) {
            Field f(mode);
            const auto t = ground(2);
            const auto ids = f.block(t, n, 20, 20, 25, 3);
            SelectionOrder o;
            o.verb = Verb::Move; o.x = 400 * 16; o.z = 300 * 16;
            const std::string what = std::string(m) + " move " + std::to_string(n);
            std::vector<CmdBatch> b;
            compare(f.world, ids, o, -1, [&](ClientRef& c) { c.rightClickMove(o.x, o.z, false); }, what, &b);
            if (mode == PathfindingMode::Legion) {
                if (const auto* order = oneOrder(b, ids, Cmd::Move, what))
                    check(order->shape == tak::net::OrderShape::Offsets && order->tmpl.x == o.x && order->tmpl.z == o.z,
                          what + ": the right-click move carries the click, shape Offsets");
            } else {
                shape(b, split(n), what);
                check(unitsOf(b) == ids, what + ": every unit once, in selection order");
                bool shared = true, offset = true;
                for (const auto& x : b)
                    for (const auto& c : x.cmds) {
                        shared &= c.x == o.x && c.z == o.z;
                        offset &= std::abs(c.x - o.x) <= 60 && std::abs(c.z - o.z) <= 60;
                    }
                check(offset && (n == 1 || !shared), what + ": Retail keeps per-axis offsets within 60 px");
            }
            // Applied in the sim, every body takes the order (Legion: its goal is the click).
            if (n == 200) {
                landAll(f.world, b);
                int ordered = 0, atClick = 0;
                for (int id : ids) {
                    const auto& orders = f.world.unit(id)->orders;
                    ordered += !orders.empty();
                    atClick += !orders.empty() && orders.back().x == Fixed::fromFloat(o.x) &&
                               orders.back().z == Fixed::fromFloat(o.z);
                }
                check(ordered == n, what + ": the sim accepts every command");
                if (mode == PathfindingMode::Legion) check(atClick == n, what + ": Legion ground shares the click");
            }
        }
    }

    // Mixed: flyers first in the selection, at opposite corners more than
    // 60 px from the centroid on both axes; ground in the middle.
    for (PathfindingMode mode : {PathfindingMode::Legion, PathfindingMode::Retail}) {
        Field f(mode);
        const auto g = ground(2), a = flyer();
        auto air = f.block(a, 6, 10, 10, 3, 3);
        const auto air2 = f.block(a, 6, 90, 90, 3, 3);
        air.insert(air.end(), air2.begin(), air2.end());
        const auto foot = f.block(g, 120, 40, 40, 12, 3);
        std::vector<int> sel = air;
        sel.insert(sel.end(), foot.begin(), foot.end());
        SelectionOrder o;
        o.verb = Verb::Move; o.x = 300 * 16; o.z = 200 * 16;
        const std::string what = std::string(mode == PathfindingMode::Legion ? "legion" : "retail") + " mixed 132";
        std::vector<CmdBatch> b;
        compare(f.world, sel, o, -1, [&](ClientRef& c) { c.rightClickMove(o.x, o.z, false); }, what, &b);
        // The goals, from the commands (Retail) or as the sim gave them (Legion: OrderShape::Offsets).
        // The flyers are farther than sqrt(3000 * 132) = 629 px from the centre: retail's
        // spread sends them to the click itself (they used to keep a +-60 px offset).
        bool atClick = true, sharedGround = true;
        if (mode == PathfindingMode::Legion) {
            oneOrder(b, sel, Cmd::Move, what);
            landAll(f.world, b);
            for (int id : sel) {
                const auto* u = f.world.unit(id);
                if (u->orders.empty()) { atClick = sharedGround = false; continue; }
                const float gx = u->orders.back().x.toFloat(), gz = u->orders.back().z.toFloat();
                if (u->type->canFly) atClick &= gx == o.x && gz == o.z;
                else sharedGround &= gx == o.x && gz == o.z;
            }
        } else {
            shape(b, split(int(sel.size())), what);
            for (const auto& x : b)
                for (const auto& c : x.cmds) {
                    const bool isAir = f.world.unit(c.unitId)->type->canFly;
                    if (isAir) atClick &= c.x == o.x && c.z == o.z;
                    else sharedGround &= c.x == o.x && c.z == o.z;
                }
        }
        check(atClick, what + ": flyers out of retail's reach take the click itself");
        if (mode == PathfindingMode::Legion) check(sharedGround, what + ": Legion ground shares the click");
    }

    // All air, over 64.
    {
        Field f(PathfindingMode::Legion);
        static const auto idsType = flyer();   // the World keeps a pointer to the type
        const auto ids = f.block(idsType, 100, 30, 30, 10, 4);
        SelectionOrder o;
        o.verb = Verb::Move; o.x = 200 * 16; o.z = 220 * 16;
        std::vector<CmdBatch> b;
        compare(f.world, ids, o, -1, [&](ClientRef& c) { c.rightClickMove(o.x, o.z, false); }, "air 100", &b);
        oneOrder(b, ids, Cmd::Move, "air 100");
    }

    // The other verbs on 65 bodies, queued and not.
    {
        Field f(PathfindingMode::Legion);
        static const auto idsType = ground(2);   // the World keeps a pointer to the type
        const auto ids = f.block(idsType, 65, 20, 20, 13, 3);
        static const auto enemyType = ground(2);   // the World keeps a pointer to the type
        const auto enemy = f.block(enemyType, 1, 200, 200, 1, 3, 1);
        static const auto buddyType = ground(2);   // the World keeps a pointer to the type
        const auto buddy = f.block(buddyType, 1, 10, 10, 1, 3);
        for (bool queue : {false, true}) {
            const std::string q = queue ? " queued" : "";
            SelectionOrder o;
            o.queue = queue;
            o.verb = Verb::Attack; o.targetId = enemy[0];
            compare(f.world, ids, o, -1, [&](ClientRef& c) { c.rightClickAttack(enemy[0], queue); }, "attack 65" + q);
            o.verb = Verb::Fight; o.x = 150 * 16; o.z = 160 * 16;
            compare(f.world, ids, o, -1, [&](ClientRef& c) { c.armedOrder('f', o.x, o.z, -1, queue); }, "fight 65" + q);
            o.verb = Verb::Patrol;
            compare(f.world, ids, o, -1, [&](ClientRef& c) { c.armedOrder('p', o.x, o.z, -1, queue); }, "patrol 65" + q);
            o.verb = Verb::Guard; o.targetId = buddy[0];
            std::vector<int> withBuddy;   // a buddy in the selection is skipped
            for (size_t i = 0; i < ids.size(); ++i) {
                if (i == 30) withBuddy.push_back(buddy[0]);
                withBuddy.push_back(ids[i]);
            }
            compare(f.world, withBuddy, o, -1, [&](ClientRef& c) { c.armedOrder('g', 0, 0, buddy[0], queue); },
                    "guard 65" + q);
        }
        SelectionOrder o;
        o.verb = Verb::Stop;
        compare(f.world, ids, o, -1, [&](ClientRef& c) { c.stop(); }, "stop 65");
    }

    // Alt+N then a move: SetSquad shares the 64-per-tick budget ahead of the
    // move; members of squad N outside the selection are evicted first.
    {
        Field f(PathfindingMode::Legion);
        static const auto idsType = ground(2);   // the World keeps a pointer to the type
        const auto ids = f.block(idsType, 200, 20, 20, 20, 3);
        static const auto oldType = ground(2);   // the World keeps a pointer to the type
        const auto old = f.block(oldType, 10, 100, 100, 5, 3);
        tak::sim::TypeRegistry reg;
        for (int id : old) {
            Command c;
            c.kind = Cmd::SetSquad; c.unitId = id; c.targetId = -3;
            applyCommand(f.world, reg, c);
        }
        for (bool append : {false, true}) {
            SelectionOrder sq;
            sq.verb = Verb::Squad; sq.squad = -3; sq.append = append;
            const std::string what = std::string("alt+3 200") + (append ? " shift" : "");
            std::vector<CmdBatch> b;
            compare(f.world, ids, sq, -1, [&](ClientRef& c) { c.assignSquad(3, -1, append); }, what, &b);
            // Shift retypes the old members with the selection's own command: one run, so one order over all 210.
            if (append) {
                std::vector<int> all = old;
                all.insert(all.end(), ids.begin(), ids.end());
                oneOrder(b, all, Cmd::SetSquad, what);
                continue;
            }
            // Two orders, in the client's order: the old members evicted, then the selection.
            const bool two = !b.empty() && b[0].tick == kIssueTick && b[0].cmds.size() == 2 &&
                b[0].cmds[0].order && b[0].cmds[1].order &&
                std::vector<int>(b[0].cmds[0].order->members.begin(), b[0].cmds[0].order->members.end()) == old &&
                b[0].cmds[0].order->tmpl.targetId == (append ? -3 : 0) &&
                std::vector<int>(b[0].cmds[1].order->members.begin(), b[0].cmds[1].order->members.end()) == ids &&
                b[0].cmds[1].order->tmpl.targetId == -3 && b[0].cmds[0].order->serial < b[0].cmds[1].order->serial;
            check(two, what + ": the old members are evicted first, as an order of their own");
        }
        // The press and the click together, as one outbox: three orders in one tick.
        tak::scn::Uplink link;
        SelectionOrder sq, mv;
        sq.verb = Verb::Squad; sq.squad = -3;
        mv.verb = Verb::Move; mv.x = 300 * 16; mv.z = 300 * 16;
        uint32_t serial = 1;
        link.issue(tak::scn::hudItems(f.world, ids, sq, 0, serial));
        link.issue(tak::scn::hudItems(f.world, ids, mv, 0, serial));
        std::vector<CmdBatch> mine;
        for (uint32_t t = kIssueTick; !link.idle(); ++t) mine.push_back({t, link.land(t)});
        ClientRef client(f.world);
        client.selection_ = ids;
        Wire wire{client, 0, 0};
        const auto theirs = runWire(wire, kIssueTick, kIssueTick, [&] {
            client.assignSquad(3, -1, false);
            client.flushClick();   // two input events: two clicks
            client.rightClickMove(mv.x, mv.z, false);
        });
        check(sameStream(mine, theirs, 0, "alt+3 then move"), "alt+3 then move: one stream");
        check(mine.size() == 1 && mine[0].cmds.size() == 3, "alt+3 then move: three orders, whole in one tick");
    }

    // The command window: 974 bodies with a 12-tick round trip land as 512
    // over 8 ticks, a 4-tick gap, then the rest -- the same for every split
    // of the round trip into uplink and downlink.
    {
        Field f(PathfindingMode::Legion);
        static const auto idsType = ground(1);   // the World keeps a pointer to the type
        const auto ids = f.block(idsType, 974, 10, 10, 60, 2);
        SelectionOrder o;
        o.verb = Verb::Move; o.x = 400 * 16; o.z = 400 * 16;
        std::vector<CmdBatch> b;
        // Legion: the command window no longer applies to a click -- one order, whole, at any round trip.
        compare(f.world, ids, o, 12, [&](ClientRef& c) { c.rightClickMove(o.x, o.z, false); }, "window 974 R12", &b);
        oneOrder(b, ids, Cmd::Move, "window 974 R12");
        compare(f.world, ids, o, 8, [&](ClientRef& c) { c.rightClickMove(o.x, o.z, false); }, "window 974 R8", &b);
        oneOrder(b, ids, Cmd::Move, "window 974 R8");
        b = tak::scn::issueSelection(f.world, ids, o, kIssueTick);
        oneOrder(b, ids, Cmd::Move, "974 no window");
    }
    // Retail keeps the per-unit stream: 974 bodies with a 12-tick round trip land as
    // 512 over 8 ticks, a 4-tick gap, then the rest -- the same for every split of
    // the round trip into uplink and downlink.
    {
        Field f(PathfindingMode::Retail);
        static const auto idsType = ground(1);   // the World keeps a pointer to the type
        const auto ids = f.block(idsType, 974, 10, 10, 60, 2);
        SelectionOrder o;
        o.verb = Verb::Move; o.x = 400 * 16; o.z = 400 * 16;
        std::vector<CmdBatch> b;
        compare(f.world, ids, o, 12, [&](ClientRef& c) { c.rightClickMove(o.x, o.z, false); }, "retail window 974 R12", &b);
        std::vector<int> want(8, 64);
        want.insert(want.end(), 4, 0);
        want.insert(want.end(), 7, 64);
        want.push_back(974 - 512 - 7 * 64);
        shape(b, want, "retail window 974 R12");
        // An 8-tick round trip is exactly the window: no gap.
        compare(f.world, ids, o, 8, [&](ClientRef& c) { c.rightClickMove(o.x, o.z, false); }, "retail window 974 R8", &b);
        shape(b, split(974), "retail window 974 R8");
        // No window: the server's drain alone.
        b = tak::scn::issueSelection(f.world, ids, o, kIssueTick);
        shape(b, split(974), "retail 974 no window");
    }
}

// ---- .scn samples ----------------------------------------------------------
const char* kAscii = R"(scn 1
name sample-ascii
ticks 900
seed 11
players 2
team 1 1
weapons off
map ascii
................................................................
................................................................
....................#...........................................
....................#...........................................
....................#.......................,,,,,~~~~~..........
....................#.......................,,,,,~~~~~..........
....................#.......................,,,,,~~~~~..........
....................#.......................,,,,,~~~~~..........
....................#...........................................
....................#...........................................
....................#...........................................
................................................................
..........................................^^^^..................
....................#.....................^^^^..................
....................#...........................................
....................#...........................................
....................#...........................................
....................#...........................................
....................#...........................................
....................#...........................................
....................#...........................................
....................#...........................................
................................................................
................................................................
end
type inf mover 2 2500 10 1.8
type slow mover 3 400 2 1.2 turninplace=0 sight=320
type jet flyer 2 1200 1 4 cruise=96
type army roster gun=150,30,1.5
group A 0 inf 24 rect 2 1 18 12 squad=f1
group B 0 slow 6 cells 24,2 28,2 32,2 24,6 28,6 32,6
group F 0 jet 4 rect 2 15 18 20 pitch=3
group E 1 army 12 rect 26 14 62 22 pitch=4 weapons=on
gate door 20 11 20 13
line finish 40 0 40 24
region home 0 0 18 24
gate wide 10 0 20 24 lateral=z band=3 mincount=2 edge=3 pairwindow=200 flip=2
lane swap 5 8 5 12 30 8 30 12 bsign=-1 window=100 mincells=3 across=all
pair near A,B E* cells=3
reach shot A,B E* range=96 marks=300,600
creep tail A,B
behind later E* A,B cells=6
hug turn 5 0 5 12 30 8 30 12 tip1=11 tip2=8 cells=1
probe claims
probe aware
probe lanes
probe w9
churn 40 9 2 2 every=5 walk=2,0,8 toggle until=60
at 30 move F,A,B 50 8
at 60 fight A,B @E queue
at 90 attack F @E
at 120 squad B g2 append
at 150 guard E @E
at 200 patrol E 5 5
at 400 stop A,B,F
)";

const char* kFlat = R"(scn 1
name sample-flat
ticks 600
players 1
uplink 12
map flat 160 120
wall 60 0 4 50
wall 60 56 4 64
height 100 80 10 10 160
type inf mover 1 2500 10 1.8
type keel boat 3 900 4 2 depth=10
type fort structure 4 4
group A 0 inf 974 rect 1 1 58 118 pitch=2
group K 0 fort 2 cells 120,20 130,20
at 0 move A 140.5 60.25
)";

std::string gen1Text() {
    return std::string("scn 1\nname sample-gen1\nticks 300\nplayers 2\nmap gen1 ") +
           tak::mapgen::encodeMapId([] {
               tak::mapgen::Params p;
               p.seed = 0x5eed;
               p.widthCells = p.heightCells = 256;
               p.waterDensity = 0;
               p.reliefDensity = 0;
               p.plateauDensity = p.mountainDensity = p.townDensity = 0;
               return p;
           }()) +
           "\ntype inf mover 2 2500 10 1.8\ngroup A 0 inf 48 rect 40 40 70 70\ngroup B 1 inf 48 rect 180 180 210 210\n"
           "at 0 move A 190 190\nat 0 move B 50 50\n";
}

const char* kSnapshot = R"(scn 1
name sample-snapshot
players 2
map snapshot sample.tnt
type inf mover 2 2500 10 1.8
group A 0 inf 16 rect 20 20 40 40
at 5 move A 100 100
)";

const char* kData = R"(scn 1
name sample-data
players 2
map flat 96 96
type hunter fbi zonter
type knight fbi arasword
group H 0 hunter 8 rect 10 10 30 30
group K 1 knight 8 rect 60 60 80 80 weapons=on
at 0 fight H @K
)";

// Build, feed orders for `ticks` ticks, return the hash.
uint64_t runHash(const tak::scn::Scenario& s, const tak::scn::BuildOptions& opt, uint32_t ticks,
                 uint64_t* initial = nullptr) {
    auto b = tak::scn::build(s, opt);
    if (!b->skipped.empty()) throw std::runtime_error("unexpectedly skipped: " + b->skipped);
    if (initial) *initial = b->world->stateHash();
    tak::scn::OrderFeed feed(s, *b);
    for (uint32_t t = 0; t < ticks; ++t) {
        feed.apply(t);
        b->world->tick(1.f / 30);
    }
    return b->world->stateHash();
}

void roundTrip(const std::string& text, const std::string& what, const std::string& dir = {}) {
    const auto a = tak::scn::parse(text, what, dir);
    const auto canon = tak::scn::format(a);
    const auto b = tak::scn::parse(canon, what + " (canonical)", dir);
    check(tak::scn::format(b) == canon, what + ": parse(format(parse(file))) formats identically");
}

void determinism(const tak::scn::Scenario& s, const std::string& what, uint32_t ticks, const char* data = nullptr,
                 const tak::hpi::Vfs* install = nullptr) {
    for (auto mode : {PathfindingMode::Legion, PathfindingMode::Retail}) {
        const std::string m = what + (mode == PathfindingMode::Legion ? " legion" : " retail");
        tak::scn::BuildOptions opt;
        opt.mode = mode;
        opt.data = data;
        opt.install = install;
        uint64_t i0 = 0, i1 = 0, ip = 0, im = 0;
        const uint64_t h0 = runHash(s, opt, ticks, &i0), h1 = runHash(s, opt, ticks, &i1);
        check(i0 == i1, m + ": two builds of the file hash the same");
        check(h0 == h1, m + ": two runs of the file hash the same");
        opt.offset = 1;
        const uint64_t hp = runHash(s, opt, 0, &ip);
        opt.offset = -1;
        const uint64_t hm = runHash(s, opt, 0, &im);
        (void)hp; (void)hm;
        check(ip != i0 && im != i0 && ip != im, m + ": start offsets 0/+1/-1 build different worlds");
        opt.offset = 0;
        opt.serial = false;
        check(runHash(s, opt, ticks) == h0, m + ": serial == workers");
        std::printf("%s: initial=%016llx after %u ticks=%016llx\n", m.c_str(), (unsigned long long)i0, ticks,
                    (unsigned long long)h0);
    }
}

void scenarios(const char* data, const std::filesystem::path& scratch) {
    // Ascii: every directive in one file.
    roundTrip(kAscii, "ascii");
    const auto ascii = tak::scn::parse(kAscii, "ascii");
    check(ascii.map.width == 64 && ascii.map.height == 24, "ascii: size from the rows");
    check(ascii.groups.size() == 4 && ascii.orders.size() == 7 && ascii.shapes.size() == 10, "ascii: counts");
    {
        const auto& g = *ascii.shape("wide");
        check(g.lateral == 1 && g.band == 3 && g.minCount == 2 && g.edge == 3 && g.pairWindow == 200 && g.flip == 2,
              "ascii: gate options");
        const auto& l = *ascii.shape("swap");
        check(l.kind == "lane" && l.ax0 == 30 && l.az1 == 12 && l.beforeSign == -1 && l.afterSign == 1 &&
                  l.window == 100 && l.minCells == 3 && l.acrossGroups,
              "ascii: lane shape");
        check(ascii.churns.size() == 1 && ascii.churns[0].x == 40 && ascii.churns[0].z == 9 &&
                  ascii.churns[0].every == 5 && ascii.churns[0].dx == 2 && ascii.churns[0].row == 8 &&
                  ascii.churns[0].toggle && ascii.churns[0].until == 60,
              "ascii: churn");
        const auto& p = *ascii.shape("near");
        check(p.kind == "pair" && p.a == "A,B" && p.b == "E*" && p.cells == 3, "ascii: pair shape");
        const auto& r = *ascii.shape("shot");
        check(r.kind == "reach" && r.a == "A,B" && r.b == "E*" && r.range == 96 && r.marks == std::vector<int>({300, 600}),
              "ascii: reach shape");
        const auto& c = *ascii.shape("tail");
        check(c.kind == "creep" && c.a == "A,B" && ascii.probeClaims && !ascii.probeApproach, "ascii: creep shape, probe claims");
        const auto& bh = *ascii.shape("later");
        check(bh.kind == "behind" && bh.a == "E*" && bh.b == "A,B" && bh.cells == 6 && ascii.probeAware, "ascii: behind shape, probe aware");
        const auto& hg = *ascii.shape("turn");
        check(hg.kind == "hug" && hg.ax0 == 30 && hg.az1 == 12 && hg.tip1 == 11 && hg.tip2 == 8 && hg.cells == 1 && ascii.probeLanes && ascii.probeW9,
              "ascii: hug shape, probe lanes, probe w9");
    }
    check(ascii.group("A")->squad == -1 && ascii.group("E")->weapons == 1, "ascii: group options");
    check(ascii.orders[0].selection == std::vector<std::string>{"F", "A", "B"}, "ascii: selection order kept");
    check(ascii.orders[1].queue && ascii.orders[1].target == "E", "ascii: fight @E queue");
    check(ascii.needsData().empty(), "ascii: asset-free");
    {
        auto b = tak::scn::build(ascii, {});
        check(b->groups["A"].size() == 24 && b->groups["E"].size() == 12, "ascii: groups spawned");
        check(b->world->unit(b->groups["E"][0])->type->weapons.size() == 1 &&
                  b->world->unit(b->groups["A"][0])->type->weapons.empty(),
              "ascii: weapons on/off per group");
        // The F,A,B click at tick 30 lands as ONE explicit order (Legion) over its 34 bodies, flyers included.
        tak::scn::OrderFeed feed(ascii, *b);
        size_t at0 = 0, at30 = 0;
        auto units = [](const std::vector<Command>& cmds) {
            size_t n = 0;
            for (const auto& c : cmds) n += c.kind == Cmd::Order && c.order ? c.order->members.size() : 1;
            return n;
        };
        for (uint32_t t = 0; t <= 30; ++t) {
            const auto cmds = feed.commandsFor(t);
            if (t == 0) at0 = units(cmds);
            if (t == 30) {
                at30 = units(cmds);
                const bool one = cmds.size() == 1 && cmds[0].kind == Cmd::Order && cmds[0].order;
                check(one && std::any_of(cmds[0].order->members.begin(), cmds[0].order->members.end(),
                                         [&](int id) { return b->world->unit(id)->type->canFly; }),
                      "ascii: the click is one order, flyers in it");
            }
        }
        check(at0 == 24 && at30 == 34, "ascii: tick-0 Alt+1 (24) and the tick-30 click (34)");
        // The churn: an event every 5 ticks from 0 to 55 (12 events: six cells, placed then lifted).
        tak::scn::OrderFeed churn(ascii, *b);
        const size_t before = b->world->features().size();
        for (uint32_t t = 0; t < 120; ++t) churn.applyChurn(t);
        const auto& fs = b->world->features();
        check(fs.size() - before == 12, "ascii: churn lands 12 events");
        check(fs[before].blocks && !fs[before + 1].blocks && fs[before + 2].x.toFloat() == 42 * 16 + 16 &&
                  fs[before + 2].blocks,
              "ascii: churn places on even events, lifts on odd, and walks a cell per pair");
    }
    determinism(ascii, "ascii", 300);

    // Flat with overlays and the command window.
    roundTrip(kFlat, "flat");
    const auto flat = tak::scn::parse(kFlat, "flat");
    check(flat.roundTrip == 12 && flat.map.overlays.size() == 3, "flat: uplink and overlays");
    {
        auto b = tak::scn::build(flat, {});
        tak::scn::OrderFeed feed(flat, *b);
        std::vector<size_t> sizes;
        for (uint32_t t = 0; t < 25; ++t) {
            const auto cmds = feed.commandsFor(t);
            sizes.push_back(cmds.size() == 1 && cmds[0].order ? cmds[0].order->members.size() : cmds.size());
        }
        std::vector<size_t> want(25, 0);
        want[0] = 974;
        check(sizes == want, "flat: a 974-body click at tick 0 lands as one order, whole, despite R=12 (Legion)");
        check(feed.done(), "flat: the feed drains");
    }
    determinism(flat, "flat", 60);

    // gen1 and snapshot maps take their features from an install: without
    // one they report a skip; with mapgen_test's synthetic Aramon features
    // they build asset-free (no coast or relief prefabs).
    std::filesystem::create_directories(scratch / "features" / "aramon");
    {
        std::ofstream out(scratch / "features" / "aramon" / "fixture.tdf");
        for (const auto& [kind, count] : std::array<std::pair<const char*, int>, 4>{
                 {{"Tree", 10}, {"Rock", 7}, {"Henge", 9}, {"Mana", 3}}})
            for (int i = 1; i <= count; ++i) {
                char name[32];
                std::snprintf(name, sizeof name, "Ara%s%02d", kind, i);
                const bool mana = std::string(kind) == "Mana";
                out << "[" << name << "] {\nfootprintx=" << (mana ? 2 : 3) << ";\nfootprintz=" << (mana ? 2 : 4)
                    << ";\nblocking=" << (!mana) << "; }\n";
            }
    }
    tak::hpi::Vfs synthetic;
    synthetic.addLayer(tak::hpi::MountSet(scratch));
    const auto genText = gen1Text();
    roundTrip(genText, "gen1");
    const auto gen = tak::scn::parse(genText, "gen1");
    check(tak::scn::build(gen, {})->skipped == "needs --data: gen1 map", "gen1: skipped without an install");
    determinism(gen, "gen1", 200, nullptr, &synthetic);

    // Snapshot: a .tnt beside the file.
    {
        const auto g = tak::mapgen::generate(tak::mapgen::decodeMapId(gen.map.recipe), synthetic);
        const auto bytes = g.map.save();
        std::ofstream(scratch / "sample.tnt", std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                                     std::streamsize(bytes.size()));
        roundTrip(kSnapshot, "snapshot", scratch.string());
        const auto snap = tak::scn::parse(kSnapshot, "snapshot", scratch.string());
        check(tak::scn::build(snap, {})->skipped == "needs --data: snapshot sample.tnt",
              "snapshot: skipped without an install");
        determinism(snap, "snapshot", 100, nullptr, &synthetic);
    }

    // Data-gated: skipped with a status without an install, built with one.
    roundTrip(kData, "data");
    const auto gated = tak::scn::parse(kData, "data");
    {
        auto b = tak::scn::build(gated, {});
        check(b->skipped == "needs --data: fbi type 'zonter'", "data: skipped without --data ('" + b->skipped + "')");
        std::printf("data sample without --data: %s\n", b->skipped.c_str());
    }
    if (data) {
        determinism(gated, "data", 100, data);
        // The user's map (movement_orders_test's recipe family) with the real install.
        const auto user = tak::scn::parse(
            "scn 1\nname user-gen1\nplayers 2\nmap gen1 ~gen1~0a0001000000000000000100030003020000ff60800300000000\n"
            "type inf mover 2 2500 10 1.8\ngroup A 0 inf 32 rect 100 100 124 124\nat 0 move A 160 160\n",
            "user-gen1");
        roundTrip(tak::scn::format(user), "user-gen1");
        determinism(user, "user-gen1", 100, data);
    }

    // A harvested situation (`spots`, `%N` selection, `clock`, `truth`): the grammar round-trips, bodies spawn exactly
    // (position, heading, hit points, individual speed, recorded id), and a click keeps the order of its `%N` list.
    {
        const std::string text =
            "scn 1\nname situation\nticks 200\nseed 4000000000\nplayers 2\nweapons on\n"
            "map flat 96 96\ntype m mover 2 2500 10 1.8\n"
            "group a 0 m 3 spots 1638400,1638400,16384,3276800,70000,0,17,1000 2686976,1638400,0,0 1638400,2686976,65535,6553600,0,0,40,0\n"
            "group b 1 m 1 spots 4194304,4194304,0,0\n"
            "clock 1234 987654321\nat 3 move %2,%0,%1 50 50\ntruth 50 1,2 3,4 5,6 7,8\ntruth 100 1,2 -2147483648,-2147483648 3,4 5,6\n";
        roundTrip(text, "situation");
        const auto sc = tak::scn::parse(text, "situation");
        check(sc.seed == 4000000000u && sc.hasClock && sc.clockTick == 1234 && sc.clockRng == 987654321u,
              "situation: seed above 2^31 and the clock survive");
        check(sc.groups[0].spots.size() == 3 && sc.groups[0].spots[0].id == 17 && sc.groups[0].spots[0].vel == 1000 &&
              sc.groups[0].spots[2].id == 40 && sc.truths.size() == 2 && sc.truths[1].pos.size() == 4 && sc.truths[1].tick == 100,
              "situation: spots and truth parsed");
        check(sc.groupOfBody(3) == 1 && sc.groupOfBody(4) == -1, "situation: body numbers span the groups in file order");
        {   // the body locator is a cumulative index: it agrees with a linear walk, and a named map round-trips
            const auto at = sc.locateBody(2);
            check(at.first == 0 && at.second == 2 && sc.locateBody(3).first == 1 && sc.locateBody(3).second == 0,
                  "situation: a body number locates its group and its index inside it");
            const std::string named = "scn 1\nname n\nplayers 2\nmap named Ulasem Arena\ntype m fbi aracommander\ngroup a 0 m 1 cells 5,5\n";
            roundTrip(named, "named map");
            const auto nm = tak::scn::parse(named, "named map");
            check(nm.map.kind == tak::scn::MapSpec::Named && nm.map.recipe == "Ulasem Arena" && !nm.needsData().empty(),
                  "situation: `map named` keeps a multi-word name and needs the install");
        }
        tak::scn::BuildOptions opt;
        auto b = tak::scn::build(sc, opt);
        const auto& ids = b->groups.at("a");
        const auto* u0 = b->world->unit(ids[0]);
        const auto* u2 = b->world->unit(ids[2]);
        check(ids[0] == 17 && ids[2] == 40 && u0 && u2, "situation: bodies take their recorded ids");
        check(u0 && u0->x.v == 1638400 && u0->z.v == 1638400 && u0->heading.v == 16384 && u0->hp.v == 3276800 &&
              u0->baseSpeed.v == 70000 && u0->speed.v == 1000, "situation: position, heading, hp, speed and velocity are exact");
        check(u2 && u2->heading.v == 65535 && u2->hp.v == 6553600, "situation: the last body is exact too");
        check(b->world->tickCount() == 1234 && b->world->gameRngState() == 987654321u, "situation: the world resumes the recorded clocks");
        // The click lists bodies 2, 0, 1: the selection (and hudCommands, which walks it) keeps that order;
        // in Legion the click is then one explicit order over the three (members in id order).
        tak::scn::OrderFeed feed(sc, *b);
        const auto sel = feed.selection(sc.orders[0]);
        check(sel.size() == 3 && sel[0] == ids[2] && sel[1] == ids[0] && sel[2] == ids[1],
              "situation: a %N selection keeps the recording's command order");
        std::vector<int> members;
        size_t items = 0;
        for (uint32_t t = 0; t < 8; ++t)
            for (const auto& c : feed.commandsFor(t)) {
                ++items;
                if (c.order) members.assign(c.order->members.begin(), c.order->members.end());
            }
        std::vector<int> sorted(ids.begin(), ids.begin() + 3);
        std::sort(sorted.begin(), sorted.end());
        check(items == 1 && members == sorted, "situation: the %N click is one explicit order over its three bodies");
        // The shifted start offset moves the truth the same way (whole cells on both axes).
        bool threw2 = false;
        try { tak::scn::parse("scn 1\nmap flat 64 64\ntype m mover 2 2500 10 1.8\ngroup a 0 m 1 rect 0 0 4 4\nat 0 move %5 1 1\n", "bad%.scn"); }
        catch (const std::runtime_error&) { threw2 = true; }
        check(threw2, "situation: a body number past the last body is an error");
    }

    // Errors carry the file and line.
    bool threw = false;
    try { tak::scn::parse("scn 1\nmap flat 64 64\ngroup A 0 nope 3 rect 0 0 9 9\n", "bad.scn"); }
    catch (const std::runtime_error& e) { threw = std::string(e.what()).rfind("bad.scn:3:", 0) == 0; }
    check(threw, "a bad line is reported as file:line");
}
}  // namespace

int main(int argc, char** argv) {
    try {
        selections();
        flyerSpread();
        scenarios(argc > 1 ? argv[1] : nullptr,
                  std::filesystem::temp_directory_path() / "issue_selection_test-scn");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("issue_selection_test: all passed\n");
    return 0;
}
