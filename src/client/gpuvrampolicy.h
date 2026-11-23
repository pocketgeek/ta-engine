#pragma once
// The arithmetic behind gpuvram's cap, kept free of SDL so tools/gpuvram_test.cpp can
// drive it with a fake clock. gpuvram.h owns the one live instance; see its header for
// what the cap is for and who obeys it.
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace gpuvram {

inline constexpr size_t   kMiB        = size_t(1) << 20;
inline constexpr size_t   kDefaultLimit = 1280 * kMiB; // Options GPU MEMORY LIMIT default (1.25 GiB)
inline constexpr size_t   kFloor      = 384 * kMiB;    // no limit or tightening goes below this
inline constexpr size_t   kUnlimited  = SIZE_MAX;      // Options UNLIMITED: only the per-subsystem ceilings remain
inline constexpr uint64_t kBackoffMs  = 3000;          // bakes pause this long after ANY failed create
inline constexpr uint64_t kRelaxMs    = 60000;         // a tightening lifts after this long without a failure...
inline constexpr uint64_t kRelaxMaxMs = 8 * 60000;     // ...doubling per re-tightening, up to 8 minutes

struct Policy {
    size_t   limit = kDefaultLimit;  // the ceiling the player chose
    size_t   cap   = kDefaultLimit;  // in force now: the limit, or a temporary tightening below it
    uint64_t backoffUntil = 0;       // ms: bakes skip until then
    uint64_t relaxAt = 0;            // ms: when a tightened cap returns to the limit; 0 = not tightened
    uint64_t relaxWait = kRelaxMs;   // the next tightening's wait
    int      tightenings = 0;

    // A new limit takes effect at once in both directions and replaces any tightening.
    // Re-applying the same limit (Options re-applies every setting on each tweak) keeps
    // a tightening in place. Returns whether the limit changed.
    bool setLimit(size_t b) {
        b = std::max(b, kFloor);
        if (b == limit) return false;
        limit = cap = b; relaxAt = 0;
        return true;
    }
    bool tightened() const { return relaxAt != 0; }
    bool blocked(uint64_t now) const { return now < backoffUntil; }
    // Overflow-safe g_bytes + add <= cap (cap may be kUnlimited).
    bool fits(size_t held, size_t add) const { return add <= cap && held <= cap - add; }

    // A create failed while `held` bytes were live. Pause bakes; clamp the cap to 87.5%
    // of what is held so the subsystems that can settle below the failing point do, and
    // schedule the return to the limit. A failure while already tightened postpones that
    // return instead of tightening further than the evidence warrants. Returns whether
    // the cap dropped.
    bool fail(size_t held, uint64_t now) {
        backoffUntil = now + kBackoffMs;
        const size_t adapted = std::max(held - (held >> 3), kFloor);
        if (held <= kFloor || adapted >= cap) {
            if (relaxAt) relaxAt = now + relaxWait;
            return false;
        }
        if (tightenings++) relaxWait = std::min(relaxWait * 2, kRelaxMaxMs);
        cap = adapted; relaxAt = now + relaxWait;
        return true;
    }
    // Lift an expired tightening. Returns whether the cap went back up.
    bool relax(uint64_t now) {
        if (!relaxAt || now < relaxAt) return false;
        cap = limit; relaxAt = 0;
        return true;
    }
};

}  // namespace gpuvram
