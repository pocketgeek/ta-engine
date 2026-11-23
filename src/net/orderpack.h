#pragma once
// packClick: the commands one player click built (in the order the client
// issued them) turned into what the uplink carries -- explicit selection orders
// (Cmd::Order) for the per-unit verbs, ordinary commands for the
// rest. Header-only; the client (GameView's click scope) and the tools' client
// mirror (tools/legion_issue_selection.h) call the same function.
//
// A run of consecutive commands that differ only in their unit (same verb,
// target, queue flag, type and area endpoint; the same point, or any point for
// the right-click Move when the click declared it -- OrderShape::Offsets -- and
// for an armed Move, Patrol or fight-move that declared its flyers' spread --
// OrderShape::FlyerOffsets; the sim recomputes the offsets) becomes ONE order
// over those units. A unit appearing
// twice starts a new run (an order holds a unit once), as does a run reaching
// kOrderMaxMembers. The Squad verb's two loops (evict, then assign) are two runs,
// so two orders, in that order. Members are sent in increasing id order, which is
// the order the sim applies them in: the member order is not carried (it would
// cost a second list as large as the members), so only verbs whose effect on a
// member does not depend on the others are order verbs (orderVerb: Load and
// GiveUnit stay per-unit, in issue order). The movement verbs register their
// members with the navigator in id order instead of selection order; that is
// part of stage 3's licensed behaviour change.
#include "net/protocol.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tak::net {

struct ClickShape {
    bool moveOffsets = false;   // the right-click move: Move runs carry the click, shape Offsets
    float x = 0, z = 0;         // the click (world px)
    bool flyerOffsets = false;  // an armed move / patrol / fight-move: Move, Patrol and AttackMove runs carry the click, shape FlyerOffsets
};

inline std::vector<Command> packClick(const std::vector<Command>& cmds, const ClickShape& click, uint32_t& nextSerial) {
    std::vector<Command> out;
    auto same = [](const Command& a, const Command& b, bool anyPoint) {
        return a.kind == b.kind && a.player == b.player && a.targetId == b.targetId && a.queue == b.queue &&
               std::memcmp(a.type, b.type, sizeof a.type) == 0 && a.x2 == b.x2 && a.z2 == b.z2 &&
               (anyPoint || (a.x == b.x && a.z == b.z));
    };
    for (size_t i = 0; i < cmds.size();) {
        const Command& c = cmds[i];
        if (!orderVerb(c.kind) || c.unitId < 0 || c.order || nextSerial == 0 || nextSerial > kOrderMaxSerial) {
            out.push_back(c);
            ++i;
            continue;
        }
        const bool offsets = click.moveOffsets && c.kind == Cmd::Move;
        const bool flyers = !offsets && click.flyerOffsets &&
                            (c.kind == Cmd::Move || c.kind == Cmd::Patrol || c.kind == Cmd::AttackMove);
        auto o = std::make_shared<SelectionOrder>();
        std::unordered_set<int32_t> seen;
        size_t j = i;
        for (; j < cmds.size() && o->members.size() < kOrderMaxMembers; ++j) {
            const Command& d = cmds[j];
            if (d.order || d.unitId < 0 || !same(d, c, offsets || flyers) || !seen.insert(d.unitId).second) break;
            o->members.push_back(d.unitId);
        }
        std::sort(o->members.begin(), o->members.end());
        o->serial = nextSerial++;
        o->tmpl = c;
        o->tmpl.unitId = 0;
        o->tmpl.player = 0;
        o->shape = offsets ? OrderShape::Offsets : flyers ? OrderShape::FlyerOffsets : OrderShape::Shared;
        if (offsets || flyers) { o->tmpl.x = click.x; o->tmpl.z = click.z; }
        o->bytes = uint32_t(orderRecordBytes(*o));
        Command item;
        item.kind = Cmd::Order;
        item.player = c.player;
        item.order = std::move(o);
        out.push_back(std::move(item));
        i = j;
    }
    return out;
}

// The client's order send window (orderSendWindow): the orders sent and not yet
// retired, by serial, with their record bytes. An order has exactly two ends: it
// comes back in a tick bundle (the server activated it), or the server drops it and
// says so (Msg::InputDropped). Either retires it, so the window always recovers --
// a dropped order once held its place for good, and enough of them (kOrderWindow,
// or kOrderWindowBytes of records) stopped the outbox, and every command behind
// it, for the rest of the game. At most kOrderWindow entries: a linear scan.
class OrderWindow {
public:
    bool fits(size_t bytes) const { return orderSendWindow(int(sent_.size()), bytes_, bytes); }
    void sent(uint32_t serial, size_t bytes) { sent_.push_back({serial, bytes}); bytes_ += bytes; }
    // `serial` came back or was dropped; false when it was not outstanding (an
    // order of a previous connection, or one already retired).
    bool retire(uint32_t serial) {
        for (auto it = sent_.begin(); it != sent_.end(); ++it)
            if (it->first == serial) { bytes_ -= it->second; sent_.erase(it); return true; }
        return false;
    }
    void clear() { sent_.clear(); bytes_ = 0; }
    int count() const { return int(sent_.size()); }
    size_t bytes() const { return bytes_; }

private:
    std::vector<std::pair<uint32_t, size_t>> sent_;
    size_t bytes_ = 0;
};

}  // namespace tak::net
