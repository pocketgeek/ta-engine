#pragma once

// Stable generational ownership of per-unit render cache entries (rendering
// optimization workstream C).
//
// Before this, GameView kept its per-unit geometry in a vector indexed by the
// unit's position in the depth-sorted visible list: entry i belonged to
// whichever unit sorted i-th this frame. A unit that moved one place in depth
// order, or any unit entering/leaving view ahead of it, landed in an entry
// built for another unit and rebuilt from scratch.
//
// Here every drawn unit owns ONE entry for as long as it stays alive and
// recently visible. The painter list holds ordered pointers to those entries;
// sorting changes only the order of the pointers, never which entry a unit has.
//
// Identity. A unit id alone is not an identity: ids restart at 1 when the world
// is reset for a replay/rejoin, and a unit keeps its id when it switches to a
// replacement model. UnitOwner therefore combines the id with the world epoch,
// the unit type and the replacement model. A claim whose identity differs from
// the entry's owner releases that entry and acquires a fresh one (cold: no key,
// so nothing is inherited). Every acquisition takes a new generation from
// HandleSlots, so a stale handle can never alias the new occupant.
//
// Lifetime and memory.
//   * Visible entries are always kept (a visible unit must be drawable).
//   * An entry not claimed this frame is RETAINED while its unit is alive, for
//     at most retainMs (and retainMinFrames), within retainBytes/retainEntries
//     (oldest first beyond that). A destroyed unit's entry is released at once.
//   * Released entries are cold at once and keep their vector capacity for
//     reuse ("warm") within freeBytes, for at most freeTrimMs; then their memory
//     is returned (releaseMemory).
//   * Free indices are reused lowest first; whole trailing chunks that hold
//     only trimmed free entries are deallocated (index space shrinks too).
//   Nothing is allocated per unit EVER spawned: storage is bounded by the peak
//   of (visible + retained) entries, and shrinks again after demand falls.
//
// Threads. Every method runs on the render thread. Workers only touch the
// payloads handed out by claim(), each by exactly one worker, during a parallel
// pass that runs after all claims and the frame's reclaim(). Storage grows in
// fixed chunks that never move, so a payload pointer stays valid across growth
// (until that entry is released and its chunk trimmed, which only reclaim()
// does, outside the parallel pass).
//
// Payload requirements:
//   void   makeCold();              drop cached content (key) but keep capacity
//   size_t retainedBytes() const;   bytes held (capacity-based, incl. sizeof)
//   void   releaseMemory();         free all owned heap memory
//
// Presentation state only: nothing here may feed the simulation.

#include "client/rendertelemetry.h"
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace tak::render {

struct UnitOwner {
    int id = -1;
    uint32_t epoch = 0;            // world epoch: ids restart at 1 after a world reset
    const void* type = nullptr;    // unit type
    const void* model = nullptr;   // replacement model, null = the type's own model
    friend bool operator==(const UnitOwner&, const UnitOwner&) = default;
};

// Unit id -> entry index, open addressing with linear probing and backward-shift
// deletion. Sized from the number of live entries, never from the largest id.
class OwnerIndex {
public:
    static constexpr uint32_t kNone = UINT32_MAX;
    uint32_t find(int id) const {
        if (keys_.empty()) return kNone;
        for (size_t i = bucket(id);; i = (i + 1) & mask_) {
            if (values_[i] == kNone) return kNone;
            if (keys_[i] == id) return values_[i];
        }
    }
    // Inserts or overwrites.
    void insert(int id, uint32_t value) {
        if ((size_ + 1) * 2 > keys_.size()) rehash(std::max<size_t>(64, keys_.size() * 2));
        for (size_t i = bucket(id);; i = (i + 1) & mask_) {
            if (values_[i] == kNone) { keys_[i] = id; values_[i] = value; ++size_; return; }
            if (keys_[i] == id) { values_[i] = value; return; }
        }
    }
    bool erase(int id) {
        if (keys_.empty()) return false;
        size_t i = bucket(id);
        for (;; i = (i + 1) & mask_) {
            if (values_[i] == kNone) return false;
            if (keys_[i] == id) break;
        }
        // Backward-shift: move later members of the probe run into the hole.
        for (size_t j = (i + 1) & mask_;; j = (j + 1) & mask_) {
            if (values_[j] == kNone) break;
            const size_t home = bucket(keys_[j]);
            // Is home cyclically outside (i, j]? Then j may fill the hole at i.
            if (i <= j ? (home <= i || home > j) : (home <= i && home > j)) {
                keys_[i] = keys_[j]; values_[i] = values_[j]; i = j;
            }
        }
        values_[i] = kNone;
        --size_;
        return true;
    }
    // Shrink when mostly empty (memory recovery); call outside hot loops.
    void compact() {
        if (keys_.size() > 64 && size_ * 8 < keys_.size())
            rehash(std::max<size_t>(64, std::bit_ceil(size_ * 4)));
    }
    void clear() { keys_.clear(); values_.clear(); keys_.shrink_to_fit(); values_.shrink_to_fit(); size_ = 0; mask_ = 0; shift_ = 63; }
    size_t size() const { return size_; }
    size_t bytes() const { return keys_.capacity() * sizeof(int) + values_.capacity() * sizeof(uint32_t); }
private:
    // Fibonacci hashing: the HIGH bits of the product, so ids that differ only
    // above the table's bit width (strides) still spread over the buckets.
    size_t bucket(int id) const { return size_t((uint64_t(uint32_t(id)) * 0x9E3779B97F4A7C15ull) >> shift_); }
    void rehash(size_t buckets) {
        std::vector<int> keys(buckets, 0);
        std::vector<uint32_t> values(buckets, kNone);
        keys.swap(keys_); values.swap(values_);
        mask_ = buckets - 1; size_ = 0;
        shift_ = 64 - unsigned(std::countr_zero(buckets));
        for (size_t i = 0; i < keys.size(); ++i)
            if (values[i] != kNone) insert(keys[i], values[i]);
    }
    std::vector<int> keys_;
    std::vector<uint32_t> values_;
    size_t size_ = 0, mask_ = 0;
    unsigned shift_ = 63;
};

struct EntryPoolConfig {
    uint64_t retainMs = 2000;          // invisible-but-alive entries kept at most this long ...
    uint64_t retainMinFrames = 30;     // ... and at least this many frames (a long hitch must not purge)
    size_t retainBytes = size_t(32) << 20;   // invisible entries' bytes; oldest released beyond
    uint32_t retainEntries = 8192;
    uint64_t freeTrimMs = 4000;        // released entries keep capacity at most this long ...
    size_t freeBytes = size_t(8) << 20;      // ... and within this many bytes (oldest trimmed first)
    uint32_t maxEntries = 1u << 22;    // hard index bound (far above any drawable unit count)
};

// Per-frame events (reset by beginFrame) and gauges (updated by reclaim).
struct EntryPoolStats {
    // events this frame
    uint32_t acquired = 0;        // fresh entries handed to a unit (cold)
    uint32_t replaced = 0;        // ... of which the unit's id had an entry with another identity
    uint32_t duplicates = 0;      // second claim of the same owner in one frame (refused)
    uint32_t releasedDestroyed = 0, releasedExpired = 0, releasedBudget = 0;
    uint32_t trimmed = 0;         // released entries whose memory was returned
    uint32_t chunksFreed = 0;
    uint32_t refused = 0;         // claim failed at maxEntries with nothing evictable
    // gauges after reclaim
    uint32_t visible = 0, retained = 0, warmFree = 0, trimmedFree = 0, allocated = 0;
    size_t retainedBytes = 0, warmFreeBytes = 0, indexBytes = 0, metaBytes = 0;
};

template <class Payload, uint32_t ChunkSize = 256>
class StableEntryPool {
public:
    explicit StableEntryPool(EntryPoolConfig config = {}) : config_(config), slots_(config.maxEntries) {}
    StableEntryPool(const StableEntryPool&) = delete;
    StableEntryPool& operator=(const StableEntryPool&) = delete;

    const EntryPoolConfig& config() const { return config_; }
    void setConfig(const EntryPoolConfig& config) { config_ = config; slots_.setCapacity(config.maxEntries); }

    // Start a frame: `nowMs` is the presentation clock (fixed-step in the render
    // study, wall time otherwise).
    void beginFrame(uint64_t nowMs) {
        ++frame_;
        nowMs_ = nowMs;
        const EntryPoolStats gauges = stats_;
        stats_ = {};
        stats_.visible = gauges.visible; stats_.retained = gauges.retained;
        stats_.warmFree = gauges.warmFree; stats_.trimmedFree = gauges.trimmedFree;
        stats_.allocated = gauges.allocated; stats_.retainedBytes = gauges.retainedBytes;
        stats_.warmFreeBytes = gauges.warmFreeBytes; stats_.indexBytes = gauges.indexBytes;
        stats_.metaBytes = gauges.metaBytes;
    }

    // The entry this unit owns, acquiring a cold one on first sight or when the
    // identity changed. Stable address while the unit keeps it. nullptr for a
    // second claim of the same owner in one frame (two workers must never share
    // an entry) or when maxEntries is reached with nothing evictable.
    Payload* claim(const UnitOwner& owner, RenderHandle* handle = nullptr) {
        uint32_t index = index_.find(owner.id);
        if (index != OwnerIndex::kNone) {
            Meta& m = meta_[index];
            if (m.owner == owner) {
                if (m.seenFrame == frame_) { ++stats_.duplicates; return nullptr; }
                m.seenFrame = frame_; m.seenMs = nowMs_;
                if (handle) *handle = m.handle;
                return &payload(index);
            }
            // Already handed out this frame under the other identity: releasing it
            // would clear a payload a worker is about to use. Refuse instead.
            if (m.seenFrame == frame_) { ++stats_.duplicates; return nullptr; }
            release(index, nullptr);
            ++stats_.replaced;
        }
        RenderHandle h = slots_.acquire();
        if (!h.valid() && evictOldestInvisible()) h = slots_.acquire();
        if (!h.valid()) { ++stats_.refused; return nullptr; }
        ensureStorage(h.index);
        Meta& m = meta_[h.index];
        m.owner = owner; m.handle = h; m.live = true; m.trimmed = false;
        m.seenFrame = frame_; m.seenMs = nowMs_;
        Payload& p = payload(h.index);
        p.makeCold();
        index_.insert(owner.id, h.index);
        ++stats_.acquired;
        if (handle) *handle = h;
        return &p;
    }

    // After this frame's claims: release destroyed units' entries, expire and
    // budget the retained ones, trim released capacity, free empty tail chunks.
    // alive(const UnitOwner&) -> bool: does that unit still exist (this epoch,
    // same type and model)?
    template <class Alive>
    void reclaim(Alive&& alive) {
        retainedScratch_.clear(); freeScratch_.clear();
        uint32_t visible = 0;
        const uint32_t count = slots_.allocated();
        for (uint32_t i = 0; i < count; ++i) {
            Meta& m = meta_[i];
            if (!m.live) {
                if (m.trimmed) continue;
                if (elapsed(m.releasedMs) >= config_.freeTrimMs) { trim(i); continue; }
                freeScratch_.push_back({m.releasedMs, i, payload(i).retainedBytes()});
                continue;
            }
            if (m.seenFrame == frame_) { ++visible; continue; }
            // Released here, its capacity joins the warm free list at once.
            if (!alive(m.owner)) {
                release(i, &stats_.releasedDestroyed);
                freeScratch_.push_back({nowMs_, i, payload(i).retainedBytes()});
                continue;
            }
            if (elapsed(m.seenMs) > config_.retainMs && frame_ - m.seenFrame > config_.retainMinFrames) {
                release(i, &stats_.releasedExpired);
                freeScratch_.push_back({nowMs_, i, payload(i).retainedBytes()});
                continue;
            }
            retainedScratch_.push_back({m.seenMs, i, payload(i).retainedBytes()});
        }
        size_t retainedBytes = 0;
        for (const auto& c : retainedScratch_) retainedBytes += c.bytes;
        if (retainedBytes > config_.retainBytes || retainedScratch_.size() > config_.retainEntries) {
            // Oldest first; index breaks ties so the order never depends on sort stability.
            std::sort(retainedScratch_.begin(), retainedScratch_.end(), [](const auto& a, const auto& b) {
                return a.ms != b.ms ? a.ms < b.ms : a.index < b.index;
            });
            size_t drop = 0;
            while (drop < retainedScratch_.size() &&
                   (retainedBytes > config_.retainBytes ||
                    retainedScratch_.size() - drop > config_.retainEntries)) {
                const auto& c = retainedScratch_[drop++];
                retainedBytes -= c.bytes;
                // Its capacity moves to the warm free list (counted below).
                freeScratch_.push_back({nowMs_, c.index, c.bytes});
                release(c.index, &stats_.releasedBudget);
            }
            retainedScratch_.erase(retainedScratch_.begin(), retainedScratch_.begin() + ptrdiff_t(drop));
        }
        size_t freeBytes = 0;
        for (const auto& c : freeScratch_) freeBytes += c.bytes;
        if (freeBytes > config_.freeBytes) {
            std::sort(freeScratch_.begin(), freeScratch_.end(), [](const auto& a, const auto& b) {
                return a.ms != b.ms ? a.ms < b.ms : a.index < b.index;
            });
            size_t drop = 0;
            while (drop < freeScratch_.size() && freeBytes > config_.freeBytes) {
                const auto& c = freeScratch_[drop++];
                freeBytes -= c.bytes;
                trim(c.index);
            }
            freeScratch_.erase(freeScratch_.begin(), freeScratch_.begin() + ptrdiff_t(drop));
        }
        trimTail();
        index_.compact();
        stats_.visible = visible;
        stats_.retained = uint32_t(retainedScratch_.size());
        stats_.retainedBytes = retainedBytes;
        stats_.warmFree = uint32_t(freeScratch_.size());
        stats_.warmFreeBytes = freeBytes;
        stats_.allocated = slots_.allocated();
        stats_.trimmedFree = stats_.allocated - slots_.live() - stats_.warmFree;
        stats_.indexBytes = index_.bytes();
        stats_.metaBytes = meta_.capacity() * sizeof(Meta) + chunks_.capacity() * sizeof(chunks_[0]) +
            (retainedScratch_.capacity() + freeScratch_.capacity()) * sizeof(ScratchItem);
    }

    // Renderer reset / asset reload: every entry keeps its owner but loses its
    // content (the next claim rebuilds). Released entries are already cold.
    void makeAllCold() {
        for (uint32_t i = 0; i < slots_.allocated(); ++i)
            if (meta_[i].live) payload(i).makeCold();
    }

    // Release everything and return all memory (teardown).
    void clear() {
        chunks_.clear(); chunks_.shrink_to_fit();
        meta_.clear(); meta_.shrink_to_fit();
        index_.clear();
        slots_ = HandleSlots(config_.maxEntries);
        retainedScratch_.clear(); retainedScratch_.shrink_to_fit();
        freeScratch_.clear(); freeScratch_.shrink_to_fit();
        const uint64_t frame = frame_, now = nowMs_;
        stats_ = {};
        frame_ = frame; nowMs_ = now;
    }

    // Every allocated payload (live, retained, free), for memory accounting.
    template <class F> void forEachPayload(F&& f) const {
        for (uint32_t i = 0; i < slots_.allocated(); ++i) f(payload(i));
    }
    // Owner of the entry a payload pointer belongs to (verification; linear in
    // chunks). Returns nullptr if the pointer is not a live entry of this pool.
    const UnitOwner* ownerOf(const Payload* p) const {
        for (size_t c = 0; c < chunks_.size(); ++c) {
            const Payload* base = chunks_[c].get();
            if (p >= base && p < base + ChunkSize) {
                const size_t i = c * ChunkSize + size_t(p - base);
                return i < slots_.allocated() && meta_[i].live ? &meta_[i].owner : nullptr;
            }
        }
        return nullptr;
    }
    RenderHandle handleOf(int id) const {
        const uint32_t i = index_.find(id);
        return i == OwnerIndex::kNone ? RenderHandle{} : meta_[i].handle;
    }
    bool valid(RenderHandle h) const { return slots_.valid(h); }
    uint32_t live() const { return slots_.live(); }
    uint32_t allocated() const { return slots_.allocated(); }
    size_t chunks() const { return chunks_.size(); }
    const EntryPoolStats& stats() const { return stats_; }
    uint64_t frame() const { return frame_; }

private:
    struct Meta {
        UnitOwner owner;
        RenderHandle handle;
        uint64_t seenFrame = 0, seenMs = 0, releasedMs = 0;
        bool live = false, trimmed = true;
    };
    // The presentation clock may restart (a new study run); never underflow.
    uint64_t elapsed(uint64_t since) const { return nowMs_ >= since ? nowMs_ - since : 0; }
    Payload& payload(uint32_t i) { return chunks_[i / ChunkSize][i % ChunkSize]; }
    const Payload& payload(uint32_t i) const { return chunks_[i / ChunkSize][i % ChunkSize]; }
    void ensureStorage(uint32_t index) {
        while (chunks_.size() * ChunkSize <= index) chunks_.push_back(std::make_unique<Payload[]>(ChunkSize));
        if (meta_.size() <= index) meta_.resize(chunks_.size() * ChunkSize);
    }
    void release(uint32_t i, uint32_t* counter) {
        Meta& m = meta_[i];
        if (index_.find(m.owner.id) == i) index_.erase(m.owner.id);
        slots_.release(m.handle);
        payload(i).makeCold();   // nothing survives into the next occupant
        m.live = false; m.trimmed = false; m.releasedMs = nowMs_;
        m.owner = {}; m.handle = {};
        if (counter) ++*counter;
    }
    void trim(uint32_t i) {
        payload(i).releaseMemory();
        meta_[i].trimmed = true;
        ++stats_.trimmed;
    }
    // Claim-time eviction at maxEntries: the oldest entry not claimed this frame.
    bool evictOldestInvisible() {
        uint32_t best = OwnerIndex::kNone;
        for (uint32_t i = 0; i < slots_.allocated(); ++i) {
            const Meta& m = meta_[i];
            if (!m.live || m.seenFrame == frame_) continue;
            if (best == OwnerIndex::kNone || m.seenMs < meta_[best].seenMs) best = i;
        }
        if (best == OwnerIndex::kNone) return false;
        release(best, &stats_.releasedBudget);
        return true;
    }
    // Deallocate trailing chunks whose entries are all free and trimmed.
    void trimTail() {
        uint32_t keep = slots_.allocated();
        while (keep > 0 && !meta_[keep - 1].live && meta_[keep - 1].trimmed) --keep;
        const size_t chunksKept = (size_t(keep) + ChunkSize - 1) / ChunkSize;
        if (chunksKept >= chunks_.size()) return;
        slots_.trimTrailing(uint32_t(chunksKept * ChunkSize));
        stats_.chunksFreed += uint32_t(chunks_.size() - chunksKept);
        chunks_.resize(chunksKept);
        meta_.resize(std::min(meta_.size(), chunksKept * ChunkSize));
        if (chunks_.size() * 2 < chunks_.capacity()) { chunks_.shrink_to_fit(); meta_.shrink_to_fit(); }
    }

    EntryPoolConfig config_;
    HandleSlots slots_;
    OwnerIndex index_;
    std::vector<std::unique_ptr<Payload[]>> chunks_;
    std::vector<Meta> meta_;
    struct ScratchItem { uint64_t ms; uint32_t index; size_t bytes; };
    std::vector<ScratchItem> retainedScratch_, freeScratch_;
    EntryPoolStats stats_;
    uint64_t frame_ = 0, nowMs_ = 0;
};

}  // namespace tak::render
