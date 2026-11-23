#include "net/protocol.h"

#include <climits>
#include <cmath>

namespace tak::net {

// Base command layout is unchanged; area commands append their second endpoint.
// An order (Cmd::Order, 267) is its own record: u8 kind, u8 player, u32 serial,
// the template command, u8 shape, u32 count and the member deltas (putMembers).
void Writer::cmd(const Command& c) {
    if (c.kind == Cmd::Order) {
        u8(uint8_t(c.kind));
        u8(c.player);
        static const SelectionOrder kEmpty{};
        const SelectionOrder& o = c.order ? *c.order : kEmpty;
        u32(o.serial);
        cmd(o.tmpl);
        u8(uint8_t(o.shape));
        int32_t prev = -1;
        putMembers(*this, o.members.data(), o.members.size(), prev);
        return;
    }
    u8(uint8_t(c.kind));
    u8(c.player);
    u32(uint32_t(c.unitId));
    u32(uint32_t(c.targetId));
    f32(c.x);
    f32(c.z);
    u8(c.queue);
    b.insert(b.end(), c.type, c.type + 16);
    if (c.kind == Cmd::ReclaimArea || c.kind == Cmd::BuildManaArea) { f32(c.x2); f32(c.z2); }
}

Command Reader::cmd() {
    const uint8_t* const at = p;
    Command c;
    c.kind = Cmd(u8());
    c.player = u8();
    if (c.kind == Cmd::Order) {
        auto o = std::make_shared<SelectionOrder>();
        o->serial = u32();
        if (!ok || !avail(1) || Cmd(*p) == Cmd::Order) { ok = false; return c; }   // no nested order
        o->tmpl = cmd();
        o->shape = OrderShape(u8());
        const uint32_t n = u32();
        // Every delta is at least one byte: a corrupt count cannot reserve gigabytes.
        if (!ok || n > kOrderMaxMembers || !avail(n)) { ok = false; return c; }
        int32_t prev = -1;
        o->members.reserve(n);
        if (!getMembers(*this, n, prev, o->members)) { ok = false; return c; }
        o->bytes = uint32_t(p - at);   // the record as read
        c.order = std::move(o);
        return c;
    }
    c.unitId = int32_t(u32());
    c.targetId = int32_t(u32());
    c.x = f32();
    c.z = f32();
    c.queue = u8();
    if (avail(16)) { std::memcpy(c.type, p, 16); p += 16; }
    else ok = false;
    c.type[15] = 0;
    if (c.kind == Cmd::ReclaimArea || c.kind == Cmd::BuildManaArea) { c.x2 = f32(); c.z2 = f32(); }
    return c;
}

bool orderVerb(Cmd c) {
    // Not Load or GiveUnit: their outcome depends on the order the members are applied
    // in (who boards a transport first -- its pickup list is built in that order; which
    // gifts a recipient near its unit cap still takes), and an order applies its members
    // in increasing id order, not the player's selection order (review R6). They keep
    // the per-unit path, which applies them as issued. Every verb here sets only that
    // member's own state, so the member order cannot change what the order does
    // (selection_order_test issueOrder); it decides only the order in which the
    // members' path requests are made (Legion: registered), which is id order.
    switch (c) {
    case Cmd::Move: case Cmd::Attack: case Cmd::AttackMove: case Cmd::Patrol: case Cmd::Stop:
    case Cmd::Unload: case Cmd::Guard: case Cmd::SetWeapon: case Cmd::Destroy:
    case Cmd::Assist: case Cmd::Repair: case Cmd::Stance: case Cmd::Cloak: case Cmd::SetActive:
    case Cmd::SetSquad: case Cmd::AttackGround:
        return true;
    default:
        return false;
    }
}

void putMembers(Writer& w, const int32_t* ids, size_t n, int32_t& prev) {
    w.u32(uint32_t(n));
    for (size_t i = 0; i < n; ++i) {
        w.varint(uint32_t(int64_t(ids[i]) - prev));
        prev = ids[i];
    }
}

bool getMembers(Reader& r, uint32_t count, int32_t& prev, std::vector<int32_t>& out) {
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t d = r.varint();
        const int64_t id = int64_t(prev) + d;
        if (!r.ok || d == 0 || id > INT32_MAX) { r.ok = false; return false; }
        prev = int32_t(id);
        out.push_back(prev);
    }
    return true;
}

uint64_t orderDigest(const std::vector<int32_t>& members) {
    uint64_t h = 1469598103934665603ULL;
    for (const int32_t id : members)
        for (int i = 0; i < 4; ++i) { h ^= (uint32_t(id) >> (8 * i)) & 0xff; h *= 1099511628211ULL; }
    return h;
}

bool validOrderTemplate(const Command& t, OrderShape shape) {
    return orderVerb(t.kind) && t.unitId == 0 && !t.order &&
           (shape == OrderShape::Shared || (shape == OrderShape::Offsets && t.kind == Cmd::Move) ||
            (shape == OrderShape::FlyerOffsets &&
             (t.kind == Cmd::Move || t.kind == Cmd::Patrol || t.kind == Cmd::AttackMove))) &&
           commandCoordinate(t.x) && commandCoordinate(t.z) && commandCoordinate(t.x2) && commandCoordinate(t.z2);
}

size_t orderRecordBytes(const SelectionOrder& o) {
    if (o.bytes) return o.bytes;
    size_t n = kOrderRecordHeader;
    int64_t prev = -1;
    for (const int32_t id : o.members) {
        uint32_t d = uint32_t(int64_t(id) - prev);
        prev = id;
        do { ++n; d >>= 7; } while (d);
    }
    return n;
}

bool validOrder(const SelectionOrder& o) {
    if (o.serial == 0 || o.serial > kOrderMaxSerial || !validOrderTemplate(o.tmpl, o.shape) ||
        o.members.empty() || o.members.size() > kOrderMaxMembers || o.members.front() < 0) return false;
    for (size_t i = 1; i < o.members.size(); ++i)
        if (o.members[i] <= o.members[i - 1]) return false;
    return orderRecordBytes(o) <= kOrderMaxBytes;
}

// The byte order is the wire format and the replay header's (match_codec_test pins
// it): append new fields at the end, matching get().
void put(Writer& w, const GameOptions& o) {
    w.u8(o.crusades); w.u8(o.forfeitSelfDestruct); w.u8(o.gameMods);
    w.u8(o.speed); w.u8(o.speedUnlock); w.u32(o.unitCap); w.u8(o.monarchExpendable);
    w.u8(o.stressTest); w.u8(o.fogExplored); w.u8(o.benchmark); w.u8(o.randomStarts);
    w.u8(o.doubleSight); w.u8(uint8_t(o.pathfindingMode));
    w.u8(o.allowSpectators);   // room admission
    w.u8(o.mapScenario);       // map scenario
    w.u8(o.powerCodes);        // power codes
}

void get(Reader& r, GameOptions& o) {
    o.crusades = r.u8(); o.forfeitSelfDestruct = r.u8(); o.gameMods = r.u8();
    o.speed = r.u8(); o.speedUnlock = r.u8(); o.unitCap = uint16_t(r.u32()); o.monarchExpendable = r.u8();
    o.stressTest = r.u8(); o.fogExplored = r.u8(); o.benchmark = r.u8(); o.randomStarts = r.u8();
    o.doubleSight = r.u8();
    const auto pathMode = r.u8();
    if (!sim::validPathfindingMode(pathMode)) r.ok = false;
    o.pathfindingMode = sim::PathfindingMode(pathMode);
    o.allowSpectators = r.u8();
    o.mapScenario = r.u8();
    o.powerCodes = r.u8();
}

void put(Writer& w, const SlotInfo& s) {
    w.u8(s.type); w.u8(s.faction); w.u8(s.color); w.u8(s.team); w.u8(s.ready); w.u8(s.aiLevel);
    w.str(s.name);
}

void get(Reader& r, SlotInfo& s) {
    s.type = r.u8(); s.faction = r.u8(); s.color = r.u8(); s.team = r.u8();
    s.ready = r.u8(); s.aiLevel = r.u8(); s.name = r.str();
}

void put(Writer& w, const RoomState& s) {
    w.u32(s.id); w.str(s.name); w.str(s.mapId); w.str(s.mission);
    put(w, s.opts);
    w.u32(s.hostId); w.u8(s.mapsReady ? 1 : 0);
    w.u8(s.capacity); w.u8(s.passworded ? 1 : 0); w.u32(s.roomRev); w.u8(s.changeBits); w.u32(s.mapSerial);
    for (const auto& slot : s.slots) put(w, slot);
}

void get(Reader& r, RoomState& s) {
    s.id = r.u32(); s.name = r.str(); s.mapId = r.str(); s.mission = r.str();
    get(r, s.opts);
    s.hostId = r.u32(); s.mapsReady = r.u8() != 0;
    s.capacity = r.u8(); s.passworded = r.u8() != 0; s.roomRev = r.u32(); s.changeBits = r.u8(); s.mapSerial = r.u32();
    for (auto& slot : s.slots) get(r, slot);
}

void putBundle(Writer& w, uint32_t tick, const std::vector<Command>& cmds, const std::vector<Event>& events) {
    w.u32(tick);
    w.u32(uint32_t(cmds.size()));
    for (const auto& c : cmds) w.cmd(c);
    w.u32(uint32_t(events.size()));
    for (const auto& e : events) { w.u8(uint8_t(e.kind)); w.u8(e.player); }
}

bool getBundle(Reader& r, uint32_t& tick, Bundle& out, bool strict) {
    tick = r.u32();
    if (!r.ok) return false;
    // Counts are bounded by what is left before anything is reserved: a command is
    // at least 35 bytes and an event 2, so a corrupt count cannot ask for gigabytes.
    const uint32_t nc = r.u32();
    if (uint64_t(nc) * 35 > uint64_t(r.end - r.p)) return false;
    constexpr auto maxKind = Cmd::BenchClock;   // the last command kind
    for (uint32_t i = 0; i < nc && r.ok; ++i) {
        auto c = r.cmd();
        if (strict && (uint8_t(c.kind) > uint8_t(maxKind) || c.player >= kMaxSlots ||
            !std::isfinite(c.x) || !std::isfinite(c.z) || !std::isfinite(c.x2) || !std::isfinite(c.z2) ||
            (c.kind == Cmd::Order && (!c.order || !validOrder(*c.order))))) return false;
        out.cmds.push_back(std::move(c));
    }
    const uint32_t ne = r.u32();
    if (!r.ok || uint64_t(ne) * 2 > uint64_t(r.end - r.p)) return false;
    // Kinds 1 (Forfeit), 2 (Leave) and 4 (AiTakeover). Kind 3 is reserved and refused.
    const auto validEvent = [](uint8_t kind) {
        return kind == uint8_t(Event::Kind::Forfeit) || kind == uint8_t(Event::Kind::Leave) ||
               kind == uint8_t(Event::Kind::AiTakeover);
    };
    for (uint32_t i = 0; i < ne && r.ok; ++i) {
        Event e; e.kind = Event::Kind(r.u8()); e.player = r.u8();
        if (strict && (e.player >= kMaxSlots || !validEvent(uint8_t(e.kind)))) return false;
        out.events.push_back(e);
    }
    return r.ok && (!strict || r.p == r.end);   // truncated or trailing inner data
}

}  // namespace tak::net
