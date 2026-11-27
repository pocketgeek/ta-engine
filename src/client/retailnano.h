#pragma once
// TotalA.exe's nanolathe spray, the arithmetic the client reproduces
// (cosmetic: retail draws it with the CRT rand and nothing reads it back).
// Checked by emulation: tools/re/nano_emu.py runs the spray object's init
// 0x473b50 and its tick 0x472eb0 on synthetic boxes.
//
// A worker's spray is one call of 0x4720d0(point, box, 6) (or 0x472200(box,
// point, 6), the reverse) per work wake. The call makes an object (vtable
// 0x4fd5b8, 0x4c bytes) on effect list 6, at most 401 objects (0x47219f: when
// more than 0x190 are live the oldest is deleted, particles and all, before
// the new one goes in). Its init 0x473b50 keeps the middle of each box (min +
// 4/11 of the size, size 3/11, per axis, integer /11 in 16.16) and sets its
// emit window to [now, now + 1] (0x471d70(1)). The emit 0x473d50 runs at once
// and again on the next tick (0x472eb0 -> 0x472f60 while last emit <= end):
// two batches of five particles, both from the boxes as they were at the
// call. A particle runs from a random point of the source box to a random
// point of the target box (three rand() * size >> 15 draws each) in
// floor(length / 4) ticks (0x473ebd..0x473ed5, the high word of
// (length << 16) / 0x40000 in 16.16), velocity (target - source) / ticks,
// and dies on the tick after it arrives (0x473b30: now > birth + ticks).
// It is a single pixel (0x473b1d: a 1x1 fill, 0x4bf6f0) in palette colour
// 0xa1 + i % 7 for the i-th of the batch, and every tick's move (0x4739b0)
// steps the low nibble 1..7 round, so the dots shimmer through the seven
// nano greens 0xa1..0xa7. Each dot is hidden by the line of sight of its own
// projected cell ((x, z - y/2) >> 5, 0x473a7c..0x473b05). List 6 is drawn
// after the whole unit and feature pass (0x469b18 after the row loop
// 0x46999e..0x469af0) and before the air units (0x469ba3): over every ground
// unit, under the flyers.
#include <cmath>
#include <cstdint>
#include <vector>

namespace ta::client::nano {

inline constexpr int kPerBatch = 5;          // 0x473da5: five particles per emit
inline constexpr int kBatches = 2;           // 0x471d70(1): emits at the call and the tick after
inline constexpr int kMaxObjects = 400;      // 0x47219f: more than this live, the oldest goes
inline constexpr int kColourBase = 0xa1;     // 0x473f3b
inline constexpr int kColourCycle = 7;       // 0x473f19 (i % 7), 0x4739d4 (nibble 1..7)
inline constexpr float kPixelsPerTick = 4.0f;// 0x473ec9: / 0x40000

// One axis of a box after 0x473b50: its start and size, from min and max.
struct Span { float lo = 0, size = 0; };
inline Span middle(float lo, float hi) {
    const float c = hi - lo;
    return {lo + c * 4.0f / 11.0f, c * 3.0f / 11.0f};
}

// Ticks a particle travels for a source-to-target length (world pixels); 0
// makes no particle (0x473ede).
inline int travelTicks(float length) {
    return length > 0 ? int(length / kPixelsPerTick) : 0;
}

// Palette index of the i-th particle of a batch, `age` ticks after it was
// made (0x473f41 at birth, 0x4739b0 once per tick after).
inline int colour(int i, uint32_t age) {
    return kColourBase + int((uint32_t(i % kColourCycle) + age) % uint32_t(kColourCycle));
}

// What a work wake sprays. Every row: TotalA.exe caller, direction, calls per
// wake, ticks between wakes (the sleep after it, 0x439e80).
enum class Work : uint8_t {
    Build,          // MobileBuild 0x403ecc / 0x4041c2, factory BUILD 0x402abd: forward, 1, 1
    Repair,         // 0x4056d1 / 0x4058f6, pad SelfRepair 0x4025ae: forward, 1, 1
    Resurrect,      // RESURRECT 0x40509d (to the feature's box): forward, 1, 1
    ReclaimUnit,    // ReclaimUnit 0x404a4e: reverse (box -> nano piece), 1, 2
    ReclaimFeature, // ReclaimUnit's feature branch 0x404d3b + 0x404d4c: reverse, 2, 2
    Capture,        // Capture 0x404676: reverse, 1, 2
};
inline constexpr bool reverse(Work w) {
    return w == Work::ReclaimUnit || w == Work::ReclaimFeature || w == Work::Capture;
}
inline constexpr int callsPerWake(Work w) { return w == Work::ReclaimFeature ? 2 : 1; }
inline constexpr int wakeTicks(Work w) { return reverse(w) ? 2 : 1; }

// A box in retail world pixels: x, y (height, absolute), z; min and max. A
// point is a box with min == max (0x4720d0 copies the point into both).
struct Box { float x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0; };
inline Box point(float x, float y, float z) { return {x, y, z, x, y, z}; }

// One particle: where it starts, its step per tick, the tick it was made and
// how many it travels. Drawn at origin + step * min(age, ticks); gone once
// age > ticks.
struct Dot {
    float x = 0, y = 0, z = 0, vx = 0, vy = 0, vz = 0;
    uint32_t birth = 0, object = 0;
    int ticks = 0;
    uint8_t index = 0;   // i in the batch: the colour phase
    bool dead(uint32_t now) const { return now > birth + uint32_t(ticks); }
    uint32_t age(uint32_t now) const {
        const uint32_t a = now > birth ? now - birth : 0;
        return a < uint32_t(ticks) ? a : uint32_t(ticks);
    }
    void at(uint32_t now, float& ox, float& oy, float& oz) const {
        const float k = float(age(now));
        ox = x + vx * k; oy = y + vy * k; oz = z + vz * k;
    }
    int paletteIndex(uint32_t now) const {   // the move steps it even on the last tick
        return colour(index, now > birth ? now - birth : 0);
    }
};

// List 6: the live spray objects, oldest first. `rand` returns 0..32767
// (retail's CRT rand; any cosmetic stream will do), `sink(const Dot&)`
// takes each new particle, `drop(object)` is told of an object the cap
// deletes so its particles go too.
class Sprayer {
public:
    template<class Rand, class Sink, class Drop>
    void call(uint32_t now, const Box& src, const Box& dst, Rand&& rand, Sink&& sink, Drop&& drop) {
        // Finished objects (no emit left, every particle gone) are deleted
        // by the list's own pass; only live ones count against the cap.
        for (size_t i = 0; i < objs_.size();)
            if (objs_[i].next > objs_[i].end && now > objs_[i].lastDeath) objs_.erase(objs_.begin() + long(i));
            else ++i;
        if (objs_.size() > size_t(kMaxObjects)) { drop(objs_.front().id); objs_.erase(objs_.begin()); }
        Obj o;
        o.id = ++serial_;
        const float s0[2] = {src.x0, src.x1}, s1[2] = {src.y0, src.y1}, s2[2] = {src.z0, src.z1};
        const float d0[2] = {dst.x0, dst.x1}, d1[2] = {dst.y0, dst.y1}, d2[2] = {dst.z0, dst.z1};
        o.s[0] = middle(s0[0], s0[1]); o.s[1] = middle(s1[0], s1[1]); o.s[2] = middle(s2[0], s2[1]);
        o.d[0] = middle(d0[0], d0[1]); o.d[1] = middle(d1[0], d1[1]); o.d[2] = middle(d2[0], d2[1]);
        o.end = now + uint32_t(kBatches - 1);
        o.next = now;
        o.lastDeath = now;
        objs_.push_back(o);
        emit(objs_.back(), now, rand, sink);   // 0x473d39: init emits at once
    }
    // The per-tick pass (0x472eb0): the second batch of every object made
    // the tick before. Run it before the tick's calls.
    template<class Rand, class Sink>
    void tick(uint32_t now, Rand&& rand, Sink&& sink) {
        for (Obj& o : objs_)
            if (o.next <= o.end && o.next <= now) emit(o, now, rand, sink);
    }
    size_t objects() const { return objs_.size(); }
    void clear() { objs_.clear(); }

private:
    struct Obj {
        uint32_t id = 0, end = 0, next = 0, lastDeath = 0;
        Span s[3], d[3];
    };
    template<class Rand, class Sink>
    void emit(Obj& o, uint32_t now, Rand& rand, Sink& sink) {
        for (int i = 0; i < kPerBatch; ++i) {
            // 0x473dad..0x473e54: x, y, z of the source, then of the target.
            float p[3], q[3];
            for (int a = 0; a < 3; ++a) p[a] = o.s[a].lo + float(rand()) * o.s[a].size / 32768.0f;
            for (int a = 0; a < 3; ++a) q[a] = o.d[a].lo + float(rand()) * o.d[a].size / 32768.0f;
            const float dx = q[0] - p[0], dy = q[1] - p[1], dz = q[2] - p[2];
            const int ticks = travelTicks(std::sqrt(dx * dx + dy * dy + dz * dz));
            if (ticks <= 0) continue;
            Dot d;
            d.x = p[0]; d.y = p[1]; d.z = p[2];
            d.vx = dx / float(ticks); d.vy = dy / float(ticks); d.vz = dz / float(ticks);
            d.birth = now; d.ticks = ticks; d.index = uint8_t(i); d.object = o.id;
            if (now + uint32_t(ticks) > o.lastDeath) o.lastDeath = now + uint32_t(ticks);
            sink(d);
        }
        o.next = now + 1;   // 0x47411e
    }
    std::vector<Obj> objs_;
    uint32_t serial_ = 0;
};

} // namespace ta::client::nano
