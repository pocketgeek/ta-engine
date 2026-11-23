#pragma once
// issueSelection: one player order over a selection, turned into the Cmd stream
// the real client would land in the sim, tick by tick. Header-only, tools only.
//
// Two halves, both mirrors of client/server code that cannot be linked from
// tools/ (GameView is the SDL app):
//
//  1. hudCommands -- the commands the HUD builds for one click, in selection
//     order. Each verb copies one client path:
//       Move    right-click on open ground   gameview_hud.cpp:258-303
//               (flyers take retail's spread, sim/retailflyerspread.h: their
//               unclamped offset from the selection's centre within
//               sqrt(3000 n) px, else the point; Legion shares the point for
//               surface movers with footprints 1..8; oversize bodies and every
//               ground unit outside Legion keep their offset from the
//               selection centroid, clamped to +-60 px per axis)
//       Attack  right-click on an enemy      gameview_hud.cpp:230-237
//       Fight / Patrol  armed order (F / P)  gameview_hud.cpp:1062-1090
//               (flyers take retail's spread, everyone else the point)
//       Guard   armed order (G)              gameview_hud.cpp:994-1013
//       Stop    Act::Stop                    gameview_impl.cpp:4707-4714
//       Squad   Ctrl/Alt+N (assignSquad)     gameview_impl.cpp:4748-4767
//     The client reads positions from its render frame (UnitR::x, the
//     current-tick pose, renderframe.h:65); here they come from the World at
//     the issue tick, which is the same value.
//
//     In a Legion game the client then packs the click (GameView::flushClick,
//     net/orderpack.h's packClick, called here too): one explicit selection
//     order (Cmd::Order, protocol 267) per verb run, the right-click Move as
//     OrderShape::Offsets at the click. Retail keeps the per-unit commands.
//
//  2. Uplink -- the path from the client's outbox to the tick a command is
//     applied in: the client's rate credit and outstanding window
//     (gameview_net.cpp mpStep, retired by its own commands coming back in a
//     bundle; cmdSendCredit/cmdSendWindow are protocol.h's own) and the server's
//     FIFO, drained kCmdCapPerTick per client per tick (server.cpp closeTick). A
//     selection over 64 per-unit commands therefore lands over consecutive ticks.
//     With a round trip R set, at most kCmdQueueCap (512) commands are
//     outstanding, so a selection over 512 arrives with the gap a real uplink
//     makes (R - 8 ticks per 512 once the window fills). An explicit order is one
//     FIFO item under its own window (orderSendWindow) and the server's per-tick
//     order budget (kTickOrderBytes): it lands whole, in one tick.
//
// Nothing here is hashed or read by the sim; a fixture applies the drained
// commands with tak::sim::applyCommand exactly as the client does.
#include "net/lockstep.h"
#include "net/protocol.h"
#include "net/orderpack.h"
#include "sim/pathmode.h"
#include "sim/retailflyerspread.h"
#include "sim/sim.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <map>
#include <unordered_set>
#include <vector>

namespace tak::scn {

enum class Verb { Move, Fight, Patrol, Attack, Guard, Stop, Squad };

struct SelectionOrder {
    Verb verb = Verb::Move;
    float x = 0, z = 0;    // world px (Move/Fight/Patrol)
    int targetId = 0;      // Attack: the enemy clicked; Guard: the friend clicked
    bool queue = false;    // Shift
    int squad = 0;         // Squad: +N group N (Ctrl+N), -N formation N (Alt+N), 0 none
    bool append = false;   // Squad: Shift held (append instead of replace)
};

// gameview_hud.cpp flyerSpreadGoal: retail's flyer spread, from the frame's floats.
inline void flyerSpreadGoal(const tak::sim::FlyerGroupCentre& air, const tak::sim::Unit& u, float wx, float wz,
                            float& gx, float& gz) {
    using tak::sim::Fixed;
    int32_t x = 0, z = 0;
    air.goal(Fixed::fromFloat(u.x.toFloat()).v, Fixed::fromFloat(u.z.toFloat()).v, Fixed::fromFloat(wx).v,
             Fixed::fromFloat(wz).v, x, z);
    gx = Fixed::raw(x).toFloat();
    gz = Fixed::raw(z).toFloat();
}

// The commands one click builds, in the order the client issues them (before
// GameView::flushClick packs them; hudItems below is what reaches the outbox).
inline std::vector<tak::net::Command> hudCommands(const tak::sim::World& world,
                                                  const std::vector<int>& selection,
                                                  const SelectionOrder& order, int player = 0) {
    using tak::net::Cmd;
    std::vector<tak::net::Command> out;
    if (selection.empty()) return out;
    auto make = [&](Cmd kind, int id) {
        tak::net::Command c;
        c.kind = kind;
        c.player = uint8_t(player);   // GameView::issue stamps the local player
        c.unitId = id;
        return c;
    };
    auto live = [&](int id) -> const tak::sim::Unit* {
        const auto* u = world.unit(id);
        return u && u->alive() ? u : nullptr;
    };
    switch (order.verb) {
    case Verb::Move: {
        const bool legion = tak::sim::isLegionPathfinding(world.pathfindingMode());
        float cx = 0, cz = 0;
        int n = 0;
        tak::sim::FlyerGroupCentre air;
        for (int id : selection)
            if (const auto* u = live(id)) {
                cx += u->x.toFloat(); cz += u->z.toFloat(); ++n;
                air.add(tak::sim::Fixed::fromFloat(u->x.toFloat()).v, tak::sim::Fixed::fromFloat(u->z.toFloat()).v);
            }
        if (n) { cx /= float(n); cz /= float(n); }
        for (int id : selection) {
            const auto* u = live(id);
            if (!u) continue;
            auto c = make(Cmd::Move, id);
            const auto* t = u->type;
            const bool shared = t && legion && tak::sim::legionSharedClick(t->canFly, t->footX, t->footZ);
            if (t && t->canFly) flyerSpreadGoal(air, *u, order.x, order.z, c.x, c.z);
            else {
                c.x = shared ? order.x : order.x + std::clamp(u->x.toFloat() - cx, -60.0f, 60.0f);
                c.z = shared ? order.z : order.z + std::clamp(u->z.toFloat() - cz, -60.0f, 60.0f);
            }
            c.queue = order.queue;
            out.push_back(c);
        }
        break;
    }
    case Verb::Attack:
        for (int id : selection) {
            auto c = make(Cmd::Attack, id);
            c.targetId = order.targetId;
            c.queue = order.queue;
            out.push_back(c);
        }
        break;
    case Verb::Fight:
    case Verb::Patrol: {
        tak::sim::FlyerGroupCentre air;
        for (int id : selection)
            if (const auto* u = live(id))
                air.add(tak::sim::Fixed::fromFloat(u->x.toFloat()).v, tak::sim::Fixed::fromFloat(u->z.toFloat()).v);
        for (int id : selection) {
            auto c = make(order.verb == Verb::Fight ? Cmd::AttackMove : Cmd::Patrol, id);
            c.x = order.x;
            c.z = order.z;
            const auto* u = live(id);
            if (u && u->type && u->type->canFly) flyerSpreadGoal(air, *u, order.x, order.z, c.x, c.z);
            c.queue = order.queue ? 1 : 0;
            out.push_back(c);
        }
        break;
    }
    case Verb::Guard:
        for (int id : selection) {
            if (id == order.targetId) continue;
            auto c = make(Cmd::Guard, id);
            c.targetId = order.targetId;
            c.queue = order.queue ? 1 : 0;
            out.push_back(c);
        }
        break;
    case Verb::Stop:
        for (int id : selection) out.push_back(make(Cmd::Stop, id));
        break;
    case Verb::Squad: {
        const int num = std::abs(order.squad);
        if (num < 1 || num > 10) break;
        // Members of squad N outside the selection are evicted (or, with Shift,
        // retyped), walking the live list first; then every selected own unit.
        const std::unordered_set<int> sel(selection.begin(), selection.end());
        for (const auto& u : world.units())
            if (u.alive() && u.player == player && std::abs(int(u.squad)) == num && !sel.count(u.id)) {
                auto c = make(Cmd::SetSquad, u.id);
                c.targetId = order.append ? order.squad : 0;
                out.push_back(c);
            }
        for (int id : selection)
            if (const auto* u = live(id); u && u->player == player) {
                auto c = make(Cmd::SetSquad, id);
                c.targetId = order.squad;
                out.push_back(c);
            }
        break;
    }
    }
    return out;
}

// What one click puts in the client's outbox: in a Legion game the packed
// explicit orders (serials from `serial`, the player's next), else hudCommands.
inline std::vector<tak::net::Command> hudItems(const tak::sim::World& world, const std::vector<int>& selection,
                                               const SelectionOrder& order, int player, uint32_t& serial) {
    auto cmds = hudCommands(world, selection, order, player);
    if (!tak::sim::isLegionPathfinding(world.pathfindingMode())) return cmds;
    tak::net::ClickShape shape;
    if (order.verb == Verb::Move) { shape.moveOffsets = true; shape.x = order.x; shape.z = order.z; }
    if (order.verb == Verb::Patrol || order.verb == Verb::Fight) { shape.flyerOffsets = true; shape.x = order.x; shape.z = order.z; }
    return tak::net::packClick(cmds, shape, serial);
}

// Client outbox -> server FIFO -> sim tick. Call issue() for every click made at
// tick t, then land(t) once per tick in increasing order; land(t) returns the
// commands (and explicit orders) applied at the start of tick t.
class Uplink {
public:
    // roundTrip < 0: no outstanding window (an ideal uplink: the stream is the
    // server's 64-per-tick drain alone). roundTrip = R >= 1: a command drained
    // in tick s is seen back by the client at s + R, which is the first tick a
    // command it frees can land in.
    explicit Uplink(int roundTrip = -1) : roundTrip_(roundTrip) {}

    void issue(const std::vector<tak::net::Command>& cmds) {
        outbox_.insert(outbox_.end(), cmds.begin(), cmds.end());
    }
    bool idle() const { return outbox_.empty() && server_.empty(); }
    size_t pending() const { return outbox_.size() + server_.size(); }

    std::vector<tak::net::Command> land(uint32_t tick) {
        using tak::net::Cmd;
        auto bytesOf = [](const tak::net::Command& c) { return c.order ? tak::net::orderRecordBytes(*c.order) : 0; };
        if (roundTrip_ < 0) {
            server_.insert(server_.end(), outbox_.begin(), outbox_.end());
            outbox_.clear();
        } else {
            for (auto it = acks_.begin(); it != acks_.end() && it->first <= tick; it = acks_.erase(it)) {
                inFlight_ -= std::min(inFlight_, it->second.cmds);
                orderInFlight_ -= std::min(orderInFlight_, it->second.orders);
                orderBytes_ -= std::min(orderBytes_, it->second.bytes);
            }
            if (tick != lastSendTick_) {
                credit_ = tak::net::cmdSendCredit(credit_, tick - lastSendTick_);
                lastSendTick_ = tick;
            }
            // The client's send loop (gameview_net.cpp mpStep): FIFO; commands under the
            // credit and window, an order whole under the order window.
            int sendable = tak::net::cmdSendWindow(credit_, inFlight_);
            while (!outbox_.empty()) {
                const auto& c = outbox_.front();
                if (c.kind == Cmd::Order) {
                    const size_t b = bytesOf(c);
                    if (!tak::net::orderSendWindow(orderInFlight_, orderBytes_, b)) break;
                    ++orderInFlight_;
                    orderBytes_ += b;
                } else {
                    if (sendable <= 0) break;
                    --sendable;
                    --credit_;
                    ++inFlight_;
                }
                server_.push_back(c);
                outbox_.pop_front();
            }
        }
        // The server's drain (server.cpp closeTick): kCmdCapPerTick commands, orders
        // within the tick's order budget, stopping at the first item that does not fit.
        std::vector<tak::net::Command> out;
        Ack ack;
        size_t orderBudget = 0;
        while (!server_.empty()) {
            const auto& c = server_.front();
            if (c.kind == Cmd::Order) {
                const size_t b = bytesOf(c);
                if (orderBudget + b > tak::net::kTickOrderBytes) break;
                orderBudget += b;
                ++ack.orders;
                ack.bytes += b;
            } else {
                if (ack.cmds >= tak::net::kCmdCapPerTick) break;
                ++ack.cmds;
            }
            out.push_back(c);
            server_.pop_front();
        }
        if (roundTrip_ >= 0 && !out.empty()) {
            Ack& a = acks_[tick + uint32_t(std::max(roundTrip_, 1))];
            a.cmds += ack.cmds;
            a.orders += ack.orders;
            a.bytes += ack.bytes;
        }
        return out;
    }

private:
    struct Ack {
        int cmds = 0, orders = 0;
        size_t bytes = 0;
    };
    int roundTrip_;
    std::deque<tak::net::Command> outbox_, server_;
    // The client's credit starts at one tick's worth (gameview.h cmdCredit_).
    int credit_ = tak::net::kCmdCapPerTick, inFlight_ = 0;
    int orderInFlight_ = 0;
    size_t orderBytes_ = 0;
    uint32_t lastSendTick_ = 0;
    std::map<uint32_t, Ack> acks_;
};

struct CmdBatch {
    uint32_t tick = 0;
    std::vector<tak::net::Command> cmds;
};

// One click at `tick` on an otherwise idle uplink: the batches it lands as,
// one per tick from `tick` until the last command (empty ticks inside a
// window gap are kept, so batch i is tick + i). `serial` is the player's next
// explicit-order serial (Legion; see hudItems).
inline std::vector<CmdBatch> issueSelection(const tak::sim::World& world, const std::vector<int>& selection,
                                            const SelectionOrder& order, uint32_t tick, int player = 0,
                                            int roundTrip = -1, uint32_t serial = 0) {
    if (!serial) serial = tick + 1;   // the default numbers a click by its tick: unique for one click a tick
    Uplink link(roundTrip);   // idle since tick 0: its credit accrues to `tick`
    link.issue(hudItems(world, selection, order, player, serial));
    std::vector<CmdBatch> out;
    for (uint32_t t = tick; !link.idle(); ++t) out.push_back({t, link.land(t)});
    return out;
}

}  // namespace tak::scn
