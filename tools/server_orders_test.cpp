// server_orders_test -- the server's selection-order transactions (server/orders.h,
// protocol 267), frame by frame: a whole transaction commits the exact order; an
// abort or a dropped (over-limit) transaction never commits; every malformed shape
// (a second Begin, a foreign serial, a duplicate or decreasing member, more members
// than declared, a bad digest or count, an old serial, a serial under the slot's
// floor, a forbidden template, truncated or trailing bytes) is refused; the
// per-client order window holds; a cleared connection keeps the floor; a frame
// refused for the window spends none of the byte budget; a dropped order names its
// serial, and the client's window (net/orderpack.h OrderWindow) recovers from drops
// it is told of -- without the notice it wedges for good (review R1).
#include "net/orderpack.h"
#include "server/orders.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace tak::net;
using tak::srv::OrderInbox;
using R = OrderInbox::Result;

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", what.c_str()); ++failures; }
}

Command moveTo(float x, float z) {
    Command c;
    c.kind = Cmd::Move;
    c.x = x; c.z = z;
    return c;
}
std::vector<uint8_t> begin(uint32_t serial, const Command& t, OrderShape shape, uint32_t n) {
    Writer w; w.u32(serial); w.cmd(t); w.u8(uint8_t(shape)); w.u32(n);
    return w.b;
}
std::vector<uint8_t> members(uint32_t serial, const std::vector<int32_t>& ids, int32_t& prev) {
    Writer w; w.u32(serial);
    putMembers(w, ids.data(), ids.size(), prev);
    return w.b;
}
std::vector<uint8_t> commit(uint32_t serial, const std::vector<int32_t>& all) {
    Writer w; w.u32(serial); w.u32(uint32_t(all.size())); w.u64(orderDigest(all));
    return w.b;
}
std::vector<uint8_t> abortFrame(uint32_t serial) { Writer w; w.u32(serial); return w.b; }
std::vector<int32_t> range(int32_t from, int32_t n, int32_t step = 1) {
    std::vector<int32_t> v;
    for (int32_t i = 0; i < n; ++i) v.push_back(from + i * step);
    return v;
}

struct Client {
    OrderInbox in;
    uint32_t floor = 0;
    bool admitAll = true;
    size_t charged = 0;   // bytes the admitted frames charged to the budget
    std::shared_ptr<const SelectionOrder> last;
    R send(Msg kind, const std::vector<uint8_t>& p) {
        std::shared_ptr<const SelectionOrder> out;
        const R r = in.frame(kind, p, floor, [&](size_t b) { if (admitAll) charged += b; return admitAll; }, out);
        if (r == R::Committed) { last = out; in.queued(orderRecordBytes(*out)); }
        return r;
    }
    // A whole transaction in chunks of `chunk`.
    R whole(uint32_t serial, const std::vector<int32_t>& ids, const Command& t = moveTo(100, 100),
            OrderShape shape = OrderShape::Shared, size_t chunk = kOrderChunkMembers) {
        R r = send(Msg::OrderBegin, begin(serial, t, shape, uint32_t(ids.size())));
        if (r != R::Ok) return r;
        int32_t prev = -1;
        for (size_t at = 0; at < ids.size(); at += chunk) {
            const std::vector<int32_t> part(ids.begin() + ptrdiff_t(at),
                                            ids.begin() + ptrdiff_t(std::min(ids.size(), at + chunk)));
            if ((r = send(Msg::OrderMembers, members(serial, part, prev))) != R::Ok) return r;
        }
        return send(Msg::OrderCommit, commit(serial, ids));
    }
};

void wholeTransactions() {
    Client c;
    const auto ids = range(5, 5000, 3);
    check(c.whole(1, ids, moveTo(1234.5f, 777.25f), OrderShape::Offsets) == R::Committed, "a whole transaction commits");
    check(c.last && c.last->serial == 1 && c.last->members == ids && c.last->shape == OrderShape::Offsets &&
              c.last->tmpl.kind == Cmd::Move && c.last->tmpl.x == 1234.5f && c.last->tmpl.z == 777.25f,
          "the committed order is exactly what was sent");
    check(validOrder(*c.last), "the committed order is valid");
    // The biggest order.
    const auto big = range(0, int32_t(kOrderMaxMembers));
    check(c.whole(2, big) == R::Committed && c.last->members.size() == kOrderMaxMembers, "kOrderMaxMembers commits");
    check(orderRecordBytes(*c.last) <= kOrderMaxBytes, "the biggest dense order fits kOrderMaxBytes");
    // Abort discards; the next serial still has to be higher.
    check(c.send(Msg::OrderBegin, begin(3, moveTo(1, 1), OrderShape::Shared, 10)) == R::Ok &&
              c.send(Msg::OrderAbort, abortFrame(3)) == R::Ok && !c.in.open(),
          "an abort discards the open transaction");
    check(c.whole(3, range(1, 10)) == R::Malformed, "an aborted serial is not reused");
    check(c.whole(4, range(1, 10)) == R::Committed, "the next serial commits after an abort");
    // Every order verb is a valid template; others are not.
    uint32_t serial = 10;
    for (unsigned k = 0; k <= unsigned(Cmd::Order); ++k) {
        Command t; t.kind = Cmd(k);
        if (t.kind == Cmd::Order) continue;
        Client v;
        const R r = v.whole(serial++, range(1, 3), t);
        check((r == R::Committed) == orderVerb(t.kind), "order verb " + std::to_string(k) + " accepted iff an order verb");
    }
}

void malformed() {
    auto fresh = [] { return Client{}; };
    {
        Client c = fresh();
        c.send(Msg::OrderBegin, begin(1, moveTo(1, 1), OrderShape::Shared, 4));
        check(c.send(Msg::OrderBegin, begin(2, moveTo(1, 1), OrderShape::Shared, 4)) == R::Malformed,
              "a second Begin while one is open");
    }
    {
        Client c = fresh();
        c.send(Msg::OrderBegin, begin(1, moveTo(1, 1), OrderShape::Shared, 4));
        int32_t prev = -1;
        check(c.send(Msg::OrderMembers, members(2, {1, 2}, prev)) == R::Malformed, "members for another serial");
    }
    {
        // A duplicate member cannot be encoded: a zero delta is refused.
        Client c = fresh();
        c.send(Msg::OrderBegin, begin(1, moveTo(1, 1), OrderShape::Shared, 3));
        Writer w; w.u32(1); w.u32(3); w.varint(5); w.varint(0); w.varint(1);
        check(c.send(Msg::OrderMembers, w.b) == R::Malformed, "a duplicate member (zero delta)");
    }
    {
        // Members continue across chunks: the second chunk cannot restart below the first.
        Client c = fresh();
        c.send(Msg::OrderBegin, begin(1, moveTo(1, 1), OrderShape::Shared, 4));
        int32_t prev = -1;
        c.send(Msg::OrderMembers, members(1, {10, 20}, prev));
        Writer w; w.u32(1); w.u32(2); w.varint(0); w.varint(1);   // 20 again, then 21
        check(c.send(Msg::OrderMembers, w.b) == R::Malformed, "a chunk repeating the previous chunk's last member");
    }
    {
        Client c = fresh();
        c.send(Msg::OrderBegin, begin(1, moveTo(1, 1), OrderShape::Shared, 2));
        int32_t prev = -1;
        check(c.send(Msg::OrderMembers, members(1, {1, 2, 3}, prev)) == R::Malformed, "more members than declared");
    }
    {
        Client c = fresh();
        c.send(Msg::OrderBegin, begin(1, moveTo(1, 1), OrderShape::Shared, kOrderChunkMembers + 1));
        int32_t prev = -1;
        check(c.send(Msg::OrderMembers, members(1, range(0, int32_t(kOrderChunkMembers) + 1), prev)) == R::Malformed,
              "a chunk over kOrderChunkMembers");
    }
    {
        Client c = fresh();
        c.send(Msg::OrderBegin, begin(1, moveTo(1, 1), OrderShape::Shared, 2));
        int32_t prev = -1;
        c.send(Msg::OrderMembers, members(1, {1, 2}, prev));
        Writer w; w.u32(1); w.u32(2); w.u64(orderDigest({1, 3}));
        check(c.send(Msg::OrderCommit, w.b) == R::Malformed, "a commit whose digest differs");
    }
    {
        Client c = fresh();
        c.send(Msg::OrderBegin, begin(1, moveTo(1, 1), OrderShape::Shared, 3));
        int32_t prev = -1;
        c.send(Msg::OrderMembers, members(1, {1, 2}, prev));
        check(c.send(Msg::OrderCommit, commit(1, {1, 2})) == R::Malformed, "a commit before every declared member");
    }
    {
        Client c = fresh();
        check(c.send(Msg::OrderCommit, commit(1, {1})) == R::Malformed, "a commit with no transaction");
        int32_t prev = -1;
        check(c.send(Msg::OrderMembers, members(1, {1}, prev)) == R::Malformed, "members with no transaction");
        check(c.send(Msg::OrderAbort, abortFrame(1)) == R::Malformed, "an abort with no transaction");
    }
    {
        Client c = fresh();
        check(c.whole(5, {1, 2}) == R::Committed && c.whole(5, {3}) == R::Malformed && c.whole(4, {3}) == R::Malformed,
              "serials strictly increase per connection");
    }
    {
        Client c = fresh();
        c.floor = 40;   // the slot's highest activated serial (a reconnect)
        check(c.whole(40, {1}) == R::Malformed && c.whole(41, {1}) == R::Committed, "a serial must be above the slot's floor");
    }
    {
        Client c = fresh();
        check(c.send(Msg::OrderBegin, begin(0, moveTo(1, 1), OrderShape::Shared, 1)) == R::Malformed, "serial 0");
        check(c.send(Msg::OrderBegin, begin(kOrderMaxSerial + 1, moveTo(1, 1), OrderShape::Shared, 1)) == R::Malformed,
              "a serial past kOrderMaxSerial");
        check(c.send(Msg::OrderBegin, begin(1, moveTo(1, 1), OrderShape::Shared, 0)) == R::Malformed, "zero members");
        check(c.send(Msg::OrderBegin, begin(1, moveTo(1, 1), OrderShape::Shared, kOrderMaxMembers + 1)) == R::Malformed,
              "more than kOrderMaxMembers");
        Command attack; attack.kind = Cmd::Attack; attack.targetId = 9;
        check(c.send(Msg::OrderBegin, begin(1, attack, OrderShape::Offsets, 1)) == R::Malformed, "Offsets on a non-Move");
        Command unit = moveTo(1, 1); unit.unitId = 7;
        check(c.send(Msg::OrderBegin, begin(1, unit, OrderShape::Shared, 1)) == R::Malformed, "a template naming a unit");
        Command far = moveTo(40000.f, 1);
        check(c.send(Msg::OrderBegin, begin(1, far, OrderShape::Shared, 1)) == R::Malformed, "a template off the map range");
        auto b = begin(1, moveTo(1, 1), OrderShape::Shared, 1);
        b.pop_back();
        check(c.send(Msg::OrderBegin, b) == R::Malformed, "a truncated Begin");
        b = begin(1, moveTo(1, 1), OrderShape::Shared, 1);
        b.push_back(0);
        check(c.send(Msg::OrderBegin, b) == R::Malformed, "a Begin with a trailing byte");
        // A nested order template: kind byte Cmd::Order where the template starts.
        Writer w; w.u32(1); w.u8(uint8_t(Cmd::Order)); w.u8(0); w.u32(2);
        check(c.send(Msg::OrderBegin, w.b) == R::Malformed, "a nested order template");
        check(c.send(Msg::OrderBegin, begin(1, moveTo(1, 1), OrderShape(7), 1)) == R::Malformed, "an unknown shape");
    }
    {
        // A member id past INT32_MAX.
        Client c = fresh();
        c.send(Msg::OrderBegin, begin(1, moveTo(1, 1), OrderShape::Shared, 2));
        Writer w; w.u32(1); w.u32(2); w.varint(0x7fffffffu); w.varint(1);   // -1 + 2^31-1 = INT32_MAX-1, then INT32_MAX
        check(c.send(Msg::OrderMembers, w.b) == R::Ok, "members up to INT32_MAX");
        Client d = fresh();
        d.send(Msg::OrderBegin, begin(1, moveTo(1, 1), OrderShape::Shared, 2));
        Writer v; v.u32(1); v.u32(2); v.varint(0x7fffffffu); v.varint(2);
        check(d.send(Msg::OrderMembers, v.b) == R::Malformed, "a member past INT32_MAX");
    }
}

void limits() {
    {
        // Over the rate budget: followed to its Commit and dropped, never committed.
        Client c;
        c.admitAll = false;
        check(c.whole(1, range(1, 100)) == R::Dropped && !c.last && c.in.dropped() == 1, "an over-budget order is dropped");
        check(c.in.droppedSerial() == 1, "a dropped order names its serial (Msg::InputDropped)");
        c.admitAll = true;
        check(c.whole(2, range(1, 100)) == R::Committed, "the next order commits once the budget allows");
    }
    {
        // The window: kOrderWindow queued orders, then a drop until one drains.
        Client c;
        for (int i = 0; i < kOrderWindow; ++i) check(c.whole(uint32_t(i + 1), {1}) == R::Committed, "within the window");
        const size_t before = c.charged;
        check(c.whole(uint32_t(kOrderWindow + 1), {1}) == R::Dropped, "the order past the window is dropped");
        check(c.charged == before, "a transaction refused for the window charges none of the byte budget");
        check(c.in.droppedSerial() == uint32_t(kOrderWindow + 1), "the window drop names its serial");
        c.in.drained(orderRecordBytes(*c.last));
        check(c.whole(uint32_t(kOrderWindow + 2), {1}) == R::Committed, "a drained order frees the window");
    }
    {
        // The byte window: big orders up to kOrderWindowBytes.
        Client c;
        const auto big = range(0, int32_t(kOrderMaxMembers), 131000);   // 3-byte deltas: the largest records ids allow
        int committed = 0;
        for (uint32_t s = 1; s <= uint32_t(kOrderWindow); ++s) committed += c.whole(s, big) == R::Committed;
        check(committed * orderRecordBytes(*c.last) <= kOrderWindowBytes && committed >= 4 && committed < kOrderWindow,
              "the record-byte window bounds big orders (" + std::to_string(committed) + " committed)");
        // Past the record bytes the order is dropped at the chunk that crosses them, and from
        // there on its chunks are neither charged nor kept.
        // Fill the record bytes with one-chunk orders until one no longer fits: less than a
        // chunk's record is left, so the next order's first chunk crosses the window and only
        // its Begin is charged.
        Client d;
        const auto chunk = range(0, int32_t(kOrderChunkMembers), 131000);
        uint32_t s = 1;
        while (d.whole(s, chunk) == R::Committed) ++s;
        check(d.in.queuedOrders() < kOrderWindow, "the record bytes, not the count, stopped the one-chunk orders");
        const size_t before = d.charged;
        check(d.whole(++s, big) == R::Dropped, "the order past the record-byte window is dropped");
        check(d.charged - before == begin(s, moveTo(100, 100), OrderShape::Shared, uint32_t(big.size())).size(),
              "only the Begin of the order past the record-byte window is charged (" +
                  std::to_string(d.charged - before) + " bytes)");
    }
    {
        // A cleared connection (disconnect, new room) forgets its serials, not the floor.
        Client c;
        c.whole(9, {1});
        c.in.clear();
        c.floor = 9;
        check(c.in.queuedOrders() == 0 && c.whole(9, {1}) == R::Malformed && c.whole(10, {1}) == R::Committed,
              "after a clear the slot's floor still holds");
    }
}

// Review R1: the client's order window against a server that drops what the window
// let through (the byte budgets are shared with the account's other connections and
// the whole server, so the window alone cannot prevent it). A client that hears of
// each drop (Msg::InputDropped -> OrderWindow::retire) keeps sending; one that does
// not -- the client before the notice -- has its window full of orders that will
// never come back after kOrderWindow drops, and every later order and every command
// queued behind it in the outbox waits for good.
void dropsReleaseTheWindow() {
    Client server;
    OrderWindow told, deaf;
    uint32_t serial = 0;
    int sentTold = 0, sentDeaf = 0;
    server.admitAll = false;   // a budget another connection of this account spent
    for (int click = 0; click < 3 * kOrderWindow; ++click) {
        const auto ids = range(click * 10, 10);
        const size_t bytes = orderRecordBytes(SelectionOrder{0, moveTo(1, 1), OrderShape::Shared, ids, 0});
        if (deaf.fits(bytes)) { deaf.sent(uint32_t(click + 1), bytes); ++sentDeaf; }
        if (!told.fits(bytes)) continue;
        ++serial;
        told.sent(serial, bytes);
        ++sentTold;
        const R r = server.whole(serial, ids);
        check(r == R::Dropped, "the server drops while the budget is spent");
        if (r == R::Dropped) check(told.retire(server.in.droppedSerial()), "the notice names an outstanding order");
    }
    check(sentTold == 3 * kOrderWindow && told.count() == 0 && told.bytes() == 0,
          "a told client's window recovers from every drop (" + std::to_string(sentTold) + " sent)");
    check(sentDeaf == kOrderWindow && !deaf.fits(kOrderRecordHeader + 1),
          "without the notice the window wedges after kOrderWindow drops (the R1 failure)");
    // The budget turns: the told client's next order commits and comes back.
    server.admitAll = true;
    check(server.whole(++serial, {1, 2, 3}) == R::Committed, "after the drops the next order commits");
    told.sent(serial, orderRecordBytes(*server.last));
    check(told.retire(serial) && told.count() == 0, "a bundle's order retires it");
    check(!told.retire(serial), "an order retires once");
}
}  // namespace

int main() {
    wholeTransactions();
    malformed();
    limits();
    dropsReleaseTheWindow();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("server_orders_test: all passed\n");
    return 0;
}
