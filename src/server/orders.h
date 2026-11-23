#pragma once
// One client's selection-order transactions at the server (the
// formats and limits are in net/protocol.h beside kOrderMaxMembers). Header-only
// and free of the Server so server_orders_test can drive it frame by frame.
//
// The server is a relay with a referee: it checks what it can without the
// world -- framing, the template, serials, counts, the digest and the
// per-client limits -- and leaves ownership and existence to the sim, which
// checks them per member at the activation tick (sim/matchsetup.cpp).
//
//  * One open transaction per client. A Begin while one is open, a frame for
//    another serial, a truncated or trailing payload, a member stream that is
//    not strictly increasing (a duplicate cannot be encoded), more members than
//    declared, a Commit whose count or digest differs, a serial not above both
//    the connection's last and the slot's floor: MALFORMED -- the connection is
//    failed, as for invalid PlayerCommands.
//  * Over a limit (the byte budget the server passes in, the orders or record
//    bytes the client may have queued): the transaction is DROPPED -- followed
//    to its Commit or Abort and discarded, and counted; at its Commit the
//    server tells the client (Msg::InputDropped), which releases it from the
//    client's window. orderSendWindow keeps an honest client inside the window
//    and the per-second member budget delays an order at the drain instead of
//    dropping it (server.cpp closeTick), but the byte budgets are shared with
//    the account's other connections and the whole server, so a drop can still
//    happen to an honest client. The limits are tested before the budget is
//    charged: a frame refused for the window spends none of it.
//  * Abort discards the open transaction; a disconnect discards it with the
//    connection (Client::clearCommands). Nothing of either reaches a bundle.
#include "net/protocol.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace tak::srv {

class OrderInbox {
public:
    enum class Result { Ok, Committed, Dropped, Malformed };
    // admit(bytes): may this frame proceed under the server's byte budgets?
    // Called for every frame with its payload size. (An order's members are
    // charged to the command budget when the server drains it, server.cpp
    // closeTick: over that budget it waits, it is never dropped.)
    using Admit = std::function<bool(size_t bytes)>;

    // One frame. On Committed `out` holds the order (serial, template, shape,
    // members); the caller queues it as one Cmd::Order item and calls queued().
    Result frame(net::Msg kind, const std::vector<uint8_t>& payload, uint32_t floor, const Admit& admit,
                 std::shared_ptr<const net::SelectionOrder>& out) {
        using net::Msg;
        net::Reader rd(payload.data(), payload.size());
        const uint32_t serial = rd.u32();
        if (!rd.ok) return Result::Malformed;
        switch (kind) {
        case Msg::OrderBegin: {
            if (stage_) return Result::Malformed;
            if (!rd.avail(1) || net::Cmd(*rd.p) == net::Cmd::Order) return Result::Malformed;
            Stage st;
            st.serial = serial;
            st.tmpl = rd.cmd();
            st.shape = net::OrderShape(rd.u8());
            st.expect = rd.u32();
            if (!rd.ok || rd.p != rd.end || serial == 0 || serial > net::kOrderMaxSerial ||
                serial <= last_ || serial <= floor || st.expect == 0 || st.expect > net::kOrderMaxMembers ||
                !net::validOrderTemplate(st.tmpl, st.shape)) return Result::Malformed;
            last_ = serial;
            st.dropped = queued_ >= net::kOrderWindow || !admit(payload.size());
            st.members.reserve(st.dropped ? 0 : st.expect);
            stage_ = std::move(st);
            return Result::Ok;
        }
        case Msg::OrderMembers: {
            if (!stage_ || stage_->serial != serial) return Result::Malformed;
            const uint32_t n = rd.u32();
            if (!rd.ok || n == 0 || n > net::kOrderChunkMembers || !rd.avail(n)) return Result::Malformed;
            std::vector<int32_t> chunk;
            chunk.reserve(n);
            int32_t prev = stage_->prev;
            if (!net::getMembers(rd, n, prev, chunk) || rd.p != rd.end ||
                uint64_t(stage_->received) + n > stage_->expect) return Result::Malformed;
            stage_->prev = prev;
            stage_->received += n;
            // The record grows by this chunk's varints (the payload past serial and count):
            // past the client's record-byte window the order cannot be queued, so it is
            // dropped here, before the chunk is charged or kept.
            stage_->recordBytes += payload.size() - 8;
            if (!stage_->dropped && queuedBytes_ + stage_->recordBytes > net::kOrderWindowBytes) drop();
            if (!stage_->dropped && !admit(payload.size())) drop();
            if (!stage_->dropped) stage_->members.insert(stage_->members.end(), chunk.begin(), chunk.end());
            return Result::Ok;
        }
        case Msg::OrderCommit: {
            const uint32_t n = rd.u32();
            const uint64_t digest = rd.u64();
            if (!rd.ok || rd.p != rd.end || !stage_ || stage_->serial != serial || n != stage_->expect ||
                stage_->received != n) return Result::Malformed;
            Stage st = std::move(*stage_);
            stage_.reset();
            if (st.dropped) { ++dropped_; droppedSerial_ = st.serial; return Result::Dropped; }
            if (net::orderDigest(st.members) != digest) return Result::Malformed;
            auto o = std::make_shared<net::SelectionOrder>();
            o->serial = st.serial;
            o->tmpl = st.tmpl;
            o->shape = st.shape;
            o->members = std::move(st.members);
            if (!net::validOrder(*o)) return Result::Malformed;
            o->bytes = uint32_t(net::orderRecordBytes(*o));
            const size_t bytes = o->bytes;
            if (queued_ >= net::kOrderWindow || queuedBytes_ + bytes > net::kOrderWindowBytes) {
                ++dropped_;
                droppedSerial_ = st.serial;
                return Result::Dropped;
            }
            out = std::move(o);
            return Result::Committed;
        }
        case Msg::OrderAbort:
            if (!rd.ok || rd.p != rd.end || !stage_ || stage_->serial != serial) return Result::Malformed;
            stage_.reset();
            return Result::Ok;
        default:
            return Result::Malformed;
        }
    }

    // A committed order of `bytes` record bytes joined the client's queue / was
    // drained into a tick.
    void queued(size_t bytes) { ++queued_; queuedBytes_ += bytes; }
    void drained(size_t bytes) {
        queued_ = queued_ > 0 ? queued_ - 1 : 0;
        queuedBytes_ -= std::min(queuedBytes_, bytes);
    }
    // The connection's commands are discarded (disconnect, a new room).
    void clear() { stage_.reset(); last_ = 0; queued_ = 0; queuedBytes_ = 0; }
    bool open() const { return stage_.has_value(); }
    int queuedOrders() const { return queued_; }
    size_t queuedBytes() const { return queuedBytes_; }
    uint64_t dropped() const { return dropped_; }
    // The serial of the transaction the last Dropped result discarded.
    uint32_t droppedSerial() const { return droppedSerial_; }

private:
    struct Stage {
        uint32_t serial = 0, expect = 0, received = 0;
        size_t recordBytes = net::kOrderRecordHeader;   // the bundle record so far (orderRecordBytes)
        net::Command tmpl;
        net::OrderShape shape = net::OrderShape::Shared;
        std::vector<int32_t> members;
        int32_t prev = -1;
        bool dropped = false;
    };
    void drop() { stage_->dropped = true; stage_->members.clear(); stage_->members.shrink_to_fit(); }
    std::optional<Stage> stage_;
    uint32_t last_ = 0;
    int queued_ = 0;
    size_t queuedBytes_ = 0;
    uint64_t dropped_ = 0;
    uint32_t droppedSerial_ = 0;
};

}  // namespace tak::srv
