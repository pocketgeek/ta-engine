#pragma once

// Retained body vertices for the native submission path (rendering
// optimization workstream H).
//
// Before H every visible body was copied every frame: each unit's screen-space
// vertices (UnitGeom::verts) into one assembled buffer (GameView::bodyVerts_),
// and that buffer again by the driver, which copies GL client arrays when a
// draw is issued (or by SDL, which converts every vertex of a small batch).
// A stationary army was copied twice per frame although almost none of it
// changed.
//
// Here the body pass is an ordered list of commands -- texture + ranges of
// per-unit sources, in painter order, with features, special units and the
// ground/air shadow boundary as command boundaries -- and on a capable
// renderer the sources live in one GL buffer (the ARENA):
//
//   * A source is one unit's body vertices (identity = geometry entry handle,
//     content = the entry's `revision`, which advances whenever any vertex can
//     change and is never repeated) or a distant-image quad (compared by value:
//     6 vertices).
//   * A source whose content is resident keeps its range: nothing is copied or
//     uploaded for it. Changed and new sources are APPENDED behind everything
//     written since the storage was (re)allocated.
//   * When the appended data would not fit, or the storage must grow or shrink,
//     the frame REBUILDS: the storage is orphaned (glBufferData with no data --
//     the driver keeps the old storage for draws still in flight) and every
//     source of the frame is written contiguously in painter order.
//   * A frame whose content is mostly new (camera motion, a moving army) does
//     not use the arena at all: it STREAMS exactly as before H (assembled
//     buffer, client arrays / SDL), and resident ranges stay valid for later.
//
// No range the GPU may still read is ever written: writes go only to vertices
// not written since the storage was last (re)allocated, and every draw reads
// only ranges written in the current storage. So no fence, persistent mapping
// or unsynchronized overwrite is needed; the only GL used is buffer objects
// (GL 1.5: glBufferData to (re)allocate/orphan, glBufferSubData to write), with
// glMultiDrawArrays (GL 1.4) optional. (A glMapBufferRange write window was
// measured and rejected: mapping freshly orphaned storage cost ~23 ms per
// 14 MB rebuild on the reference driver, see the progress document.)
//
// The planner below is pure bookkeeping (no GL), so it is tested on its own
// (tools/vertex_arena_test.cpp) against a simulated buffer. Everything here is
// presentation state on the render thread; nothing reaches the simulation.

#include <SDL.h>
#include <SDL_opengl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "client/gpuvram.h"

namespace tak::render {

inline constexpr uint32_t kArenaNoSlot = ~0u;     // a source with no retained identity: always written
inline constexpr uint32_t kArenaValueVerts = 6;   // by-value sources: one distant-image quad

struct ArenaSource {
    const SDL_Vertex* data = nullptr;
    uint32_t count = 0;
    uint32_t slot = kArenaNoSlot;   // retained identity: the geometry entry's handle index
    uint32_t generation = 0;        // ... and its handle generation
    // Content revision; 0 = compared by value (count must be kArenaValueVerts).
    // A nonzero revision must change whenever any vertex can change and must
    // never repeat for another content of this slot.
    uint64_t revision = 0;
};

enum class ArenaMode : uint8_t { Stream, Append, Rebuild };
enum class ArenaStreamReason : uint8_t { None, Disallowed, Empty, Retry, Unstable, Budget };

inline constexpr int kArenaSlots = 2;   // physical storages: the current one, and one draining

struct ArenaPolicy {
    size_t minCapacity = size_t(1) << 16;                                // vertices (1.25 MiB)
    size_t maxCapacity = (size_t(64) << 20) / sizeof(SDL_Vertex);        // 64 MiB per storage, as GeometrySubmit's retained buffers
    size_t granule = size_t(1) << 14;                                    // capacity rounding (vertices)
    double headroom = 2.0;          // capacity on allocation = this frame's vertices x headroom
    double minHeadroom = 1.125;     // a capacity below this frame x minHeadroom is not worth it: stream
    // Retain only when at least this fraction of the frame's vertices is resident
    // or unchanged since the previous frame (otherwise the frame streams).
    double stableFraction = 0.5;
    // Migration out of a draining storage: unchanged commands move to the
    // current storage within this many vertices per frame (fraction of the
    // frame, at least migrateMin), so no frame rewrites everything.
    double migrateFraction = 0.125;
    size_t migrateMin = size_t(1) << 15;
    uint64_t idleFrames = 120, idleMs = 2000;      // storage released after no arena frame for both
    uint64_t shrinkFrames = 300, shrinkMs = 5000;  // reallocated smaller after capacity > ratio x demand for both
    double shrinkRatio = 4.0;
    uint64_t retryFrames = 120;                    // after a failed allocation or write
};

struct ArenaRange { uint32_t source, offset, count; };   // a run of one source (vertex offset/count inside it)
struct ArenaUpload { uint32_t source; uint32_t dst; };

struct ArenaFrameStats {
    ArenaMode mode = ArenaMode::Stream;
    ArenaStreamReason reason = ArenaStreamReason::None;
    bool rollover = false;          // the current storage became the draining one, a new one was allocated
    size_t sources = 0, commands = 0, verts = 0, residentVerts = 0, stableVerts = 0, uploadedVerts = 0;
    size_t migratedVerts = 0;       // unchanged sources copied out of the draining storage (budgeted)
    size_t oldCommands = 0;         // commands drawn from the draining storage
};

// Per frame: begin(), add() every source and addCommand() every command (its
// ranges, in painter order), plan(); then the caller, in this order, releases
// releaseSlot() (if any), allocates curSlot() at newCapacity() (if allocate()),
// writes uploads() -- contiguous, [writeBegin, writeEnd) of curSlot() -- and
// draws command i from commandSlot(i) at first(range); finally end(ok).
//
// Storage slots: the CURRENT storage takes every write. When its appended data
// would not fit, it becomes the DRAINING storage and a new current one is
// allocated (rollover): a command keeps drawing from the draining storage
// while all its sources are resident there; it moves to the current storage
// (all its non-resident sources copied) when one of them changed, or, within
// the per-frame migration budget, unchanged. When no command draws from the
// draining storage it is released. A command always draws from ONE storage,
// so rollover never splits a draw; nothing written is ever overwritten. Only
// when the current storage fills while another is still draining, or the GPU
// budget allows no second storage, does a frame rewrite everything into fresh
// storage (full rebuild).
class VertexArenaPlanner {
public:
    explicit VertexArenaPlanner(ArenaPolicy policy = {}) : policy_(policy) {}
    const ArenaPolicy& policy() const { return policy_; }
    void setPolicy(const ArenaPolicy& p) { policy_ = p; }

    void begin(uint64_t frame, uint64_t nowMs) {
        frame_ = frame; nowMs_ = nowMs;
        sources_.clear(); ranges_.clear(); commands_.clear(); uploads_.clear();
        stats_ = {}; mode_ = ArenaMode::Stream; writeBegin_ = writeEnd_ = 0;
        maxSlot_ = 0; planned_ = false; allocate_ = false; releaseSlot_ = -1;
    }
    uint32_t add(const ArenaSource& s) {
        sources_.push_back(s);
        if (s.slot != kArenaNoSlot) maxSlot_ = std::max<size_t>(maxSlot_, size_t(s.slot) + 1);
        return uint32_t(sources_.size() - 1);
    }
    // Returns the command's index; its ranges get indices [first, first + n) in
    // the order given (first() takes those indices).
    uint32_t addCommand(std::span<const ArenaRange> ranges) {
        commands_.push_back({uint32_t(ranges_.size()), uint32_t(ranges.size()), 0});
        ranges_.insert(ranges_.end(), ranges.begin(), ranges.end());
        return uint32_t(commands_.size() - 1);
    }
    uint32_t commandFirstRange(uint32_t command) const { return commands_[command].first; }

    // allowed: the caller can draw from the arena this frame. budget: the most
    // vertices a NEW storage may hold if one is allocated now (GPU budget).
    ArenaMode plan(bool allowed, size_t budget) {
        planned_ = true;
        const size_t n = sources_.size();
        res_.assign(n * kArenaSlots, 0);
        offs_.assign(n * kArenaSlots, 0);
        sched_.assign(n, kNone);
        stamp_.assign(n, 0);
        keys_.assign(n, 0);
        size_t total = 0, resident = 0, stable = 0;
        for (size_t i = 0; i < n; ++i) {
            const ArenaSource& s = sources_[i];
            total += s.count;
            if (s.slot == kArenaNoSlot) continue;
            const Record* r = s.slot < records_.size() ? &records_[s.slot] : nullptr;
            keys_[i] = contentKey(r, s);
            if (!r) continue;
            bool any = false;
            for (int k = 0; k < kArenaSlots; ++k)
                if (residentAt(*r, s, keys_[i], k)) { res_[i * kArenaSlots + k] = 1; offs_[i * kArenaSlots + k] = r->loc[k].offset; any = true; }
            if (any) { resident += s.count; stable += s.count; }
            else if (isStable(*r, s, keys_[i])) stable += s.count;
        }
        stats_.sources = n; stats_.commands = commands_.size(); stats_.verts = total;
        stats_.residentVerts = resident; stats_.stableVerts = stable;
        auto stream = [&](ArenaStreamReason why) {
            mode_ = ArenaMode::Stream; stats_.mode = mode_; stats_.reason = why;
            return mode_;
        };
        if (!allowed) return stream(ArenaStreamReason::Disallowed);
        if (total == 0) return stream(ArenaStreamReason::Empty);
        if (frame_ < retryFrame_) return stream(ArenaStreamReason::Retry);
        if (double(stable) < policy_.stableFraction * double(total)) return stream(ArenaStreamReason::Unstable);

        int cur = cur_;
        int old = slots_[1 - cur].epoch ? 1 - cur : -1;
        if (!slots_[cur].epoch && old >= 0) { cur = old; old = -1; }   // only a draining storage left: it is current
        // Exact forced writes: sources of commands that can draw from neither
        // storage as they are (a source changed or is new).
        size_t forced = 0;
        for (const Command& c : commands_)
            if (!wholly(c, cur, false) && !(old >= 0 && wholly(c, old, false)))
                for (uint32_t k = c.first; k < c.first + c.count; ++k) {
                    const uint32_t src = ranges_[k].source;
                    if (!res_[src * kArenaSlots + size_t(cur)] && !stamp_[src]) { stamp_[src] = 1; forced += sources_[src].count; }
                }
        const size_t migrate = old >= 0 ? std::max(policy_.migrateMin, size_t(std::ceil(double(total) * policy_.migrateFraction))) : 0;
        const size_t need = size_t(std::ceil(double(total) * policy_.minHeadroom));
        const bool shrink = shrinkDue();
        if (!slots_[cur].epoch || slots_[cur].top + forced + migrate > slots_[cur].capacity || shrink ||
            slots_[cur].capacity < need) {
            const size_t want = std::min(capacityFor(total), budget);
            if (want < need) return stream(ArenaStreamReason::Budget);
            if (slots_[cur].epoch && old < 0) {
                // Rollover: the full storage drains while a new one fills.
                stats_.rollover = true;
                old = cur; cur = 1 - cur;
            } else if (old >= 0) {
                releaseSlot_ = old; old = -1;   // still draining: rewrite everything (full rebuild)
            }
            allocate_ = true; newCapacity_ = want;
            // The fresh storage holds nothing. (After a full rebuild old < 0, so no
            // command is assigned to the released storage.)
            for (size_t i = 0; i < n; ++i) res_[i * kArenaSlots + size_t(cur)] = 0;
        }
        const size_t capacity = allocate_ ? newCapacity_ : slots_[cur].capacity;
        size_t at = allocate_ ? 0 : slots_[cur].top;
        writeBegin_ = at;
        size_t budgetLeft = migrate;
        bool oldUsed = false;
        auto schedule = [&](const Command& c) {
            for (uint32_t k = c.first; k < c.first + c.count; ++k) {
                const uint32_t src = ranges_[k].source;
                if (res_[src * kArenaSlots + size_t(cur)] || sched_[src] != kNone) continue;
                sched_[src] = uint32_t(at);
                uploads_.push_back({src, uint32_t(at)});
                at += sources_[src].count;
            }
        };
        uint32_t command = 0;
        for (Command& c : commands_) {
            ++command;
            if (wholly(c, cur, true)) { c.slot = uint8_t(cur); continue; }
            if (old >= 0 && wholly(c, old, false)) {
                const size_t cost = missing(c, cur, command);
                if (cost <= budgetLeft) { budgetLeft -= cost; stats_.migratedVerts += cost; schedule(c); c.slot = uint8_t(cur); }
                else { c.slot = uint8_t(old); oldUsed = true; ++stats_.oldCommands; }
                continue;
            }
            schedule(c);
            c.slot = uint8_t(cur);
        }
        writeEnd_ = at;
        if (writeEnd_ > capacity) return failPlan();   // cannot happen (reservation above); never write past
        if (old >= 0 && !oldUsed) releaseSlot_ = old;   // drained
        curPlanned_ = cur;
        mode_ = allocate_ ? ArenaMode::Rebuild : ArenaMode::Append;
        stats_.mode = mode_;
        stats_.uploadedVerts = writeEnd_ - writeBegin_;
        return mode_;
    }
    ArenaMode mode() const { return mode_; }
    const ArenaFrameStats& stats() const { return stats_; }
    // Valid after plan() for Append/Rebuild:
    int curSlot() const { return curPlanned_; }
    int releaseSlot() const { return releaseSlot_; }
    bool allocate() const { return allocate_; }
    bool reallocate() const { return allocate_; }
    size_t newCapacity() const { return newCapacity_; }      // vertices (allocate())
    size_t writeBegin() const { return writeBegin_; }
    size_t writeEnd() const { return writeEnd_; }
    std::span<const ArenaUpload> uploads() const { return uploads_; }
    std::span<const ArenaSource> sources() const { return sources_; }
    std::span<const ArenaRange> ranges() const { return ranges_; }
    size_t commandCount() const { return commands_.size(); }
    int commandSlot(uint32_t command) const { return commands_[command].slot; }
    bool written(uint32_t source) const { return sched_[source] != kNone; }   // uploaded this frame
    // Arena vertex of a range of a command, in that command's storage.
    uint32_t first(uint32_t command, uint32_t range) const {
        const ArenaRange& r = ranges_[range];
        const int slot = commands_[command].slot;
        const uint32_t base = slot == curPlanned_ && sched_[r.source] != kNone
            ? sched_[r.source] : offs_[r.source * kArenaSlots + size_t(slot)];
        return base + r.offset;
    }

    // The frame's write succeeded (ok) or failed: storage contents unknown, so
    // nothing is resident and the arena stays off for retryFrames. Records the
    // frame's observations either way (the stable test of the next frame).
    void end(bool ok) {
        if (!planned_) plan(false, 0);
        const bool wrote = mode_ != ArenaMode::Stream && ok;
        if (mode_ != ArenaMode::Stream && !ok) {
            invalidateStorage();
            retryFrame_ = frame_ + policy_.retryFrames;
            ++failures_;
        }
        if (wrote) {
            const int cur = curPlanned_;
            if (releaseSlot_ >= 0) { slots_[releaseSlot_] = {}; ++drains_; }
            if (allocate_) {
                slots_[cur] = {nextEpoch_++, newCapacity_, 0};
                if (stats_.rollover) ++rollovers_; else ++rebuilds_;
            }
            cur_ = cur;
            slots_[cur].top = writeEnd_;
            lastUseFrame_ = frame_; lastUseMs_ = nowMs_;
            const size_t held = slots_[0].capacity + slots_[1].capacity;
            if (held > policy_.minCapacity && double(slots_[cur].capacity) > policy_.shrinkRatio * double(stats_.verts)) {
                if (!shrinkSince_) { shrinkSince_ = true; shrinkSinceFrame_ = frame_; shrinkSinceMs_ = nowMs_; }
            } else shrinkSince_ = false;
            if (allocate_ && !slots_[1 - cur].epoch) trimRecords();
        }
        if (maxSlot_ > records_.size()) records_.resize(maxSlot_);
        for (size_t i = 0; i < sources_.size(); ++i) {
            const ArenaSource& s = sources_[i];
            if (s.slot == kArenaNoSlot) continue;
            Record& r = records_[s.slot];
            const bool byValue = s.revision == 0;
            if (byValue && s.count != kArenaValueVerts) {   // not retainable by value
                r.loc[0] = r.loc[1] = {}; r.seenByValue = false; r.seenFrame = ~uint64_t(0);
                continue;
            }
            if (wrote && sched_[i] != kNone) {
                const int cur = curPlanned_;
                r.loc[cur] = {slots_[cur].epoch, sched_[i], s.count, s.generation, keys_[i], byValue};
            }
            if (byValue) {
                if (values_.size() <= s.slot) values_.resize(size_t(s.slot) + 1);
                auto& v = values_[s.slot];
                if (!(r.seenByValue && std::memcmp(v.data(), s.data, sizeof(v)) == 0)) {
                    std::memcpy(v.data(), s.data, sizeof(v));
                    r.valueSerial = keys_[i];   // the serial this value was keyed with
                }
            }
            r.seenFrame = frame_; r.seenGeneration = s.generation; r.seenRevision = s.revision;
            r.seenCount = s.count; r.seenByValue = byValue;
        }
        planned_ = false;
    }

    // ---- storage lifetime (the caller owns the GL buffers) ----
    // No arena frame for idleFrames and idleMs: release every storage.
    bool shouldRelease(uint64_t frame, uint64_t nowMs) const {
        return capacity() > 0 && frame >= lastUseFrame_ + policy_.idleFrames &&
               nowMs >= lastUseMs_ + policy_.idleMs;
    }
    // Every storage was released or lost (reset, context loss, idle, budget).
    void release() { invalidateStorage(); trimRecords(); }
    size_t capacity() const { return slots_[0].capacity + slots_[1].capacity; }   // vertices held (0 = none)
    size_t slotCapacity(int slot) const { return slots_[slot].capacity; }
    bool slotLive(int slot) const { return slots_[slot].epoch != 0; }
    size_t top() const { return slots_[cur_].top; }   // vertices written to the current storage
    uint64_t rebuilds() const { return rebuilds_; }
    uint64_t rollovers() const { return rollovers_; }
    uint64_t drains() const { return drains_; }
    uint64_t failures() const { return failures_; }
    size_t cpuBytes() const {
        return records_.capacity() * sizeof(Record) + values_.capacity() * sizeof(values_[0]) +
               sources_.capacity() * sizeof(ArenaSource) + ranges_.capacity() * sizeof(ArenaRange) +
               commands_.capacity() * sizeof(Command) + uploads_.capacity() * sizeof(ArenaUpload) +
               res_.capacity() + offs_.capacity() * sizeof(uint32_t) +
               (sched_.capacity() + stamp_.capacity()) * sizeof(uint32_t) +
               keys_.capacity() * sizeof(uint64_t);
    }
    size_t recordCount() const { return records_.size(); }

private:
    static constexpr uint32_t kNone = ~0u;
    struct Location {
        uint32_t epoch = 0, offset = 0, count = 0, generation = 0;
        uint64_t key = 0;   // revision, or the value serial for by-value sources
        bool byValue = false;
    };
    struct Record {
        Location loc[kArenaSlots];
        uint64_t valueSerial = 0;   // advanced whenever the last seen value changes
        // Last observation, resident or not: the stable test.
        bool seenByValue = false;
        uint32_t seenGeneration = 0, seenCount = 0;
        uint64_t seenRevision = 0, seenFrame = ~uint64_t(0);
    };
    struct Slot { uint32_t epoch = 0; size_t capacity = 0, top = 0; };
    struct Command { uint32_t first, count; uint8_t slot; };

    // The content key of a source this frame: its revision, or for a by-value
    // source the serial of its value (the last seen serial when unchanged).
    uint64_t contentKey(const Record* r, const ArenaSource& s) const {
        if (s.revision) return s.revision;
        if (s.count != kArenaValueVerts) return 0;
        const bool same = r && r->seenByValue && s.slot < values_.size() &&
                          std::memcmp(values_[s.slot].data(), s.data, sizeof(values_[0])) == 0;
        return r ? (same ? r->valueSerial : r->valueSerial + 1) : 1;
    }
    bool residentAt(const Record& r, const ArenaSource& s, uint64_t key, int k) const {
        const Location& l = r.loc[k];
        if (!slots_[k].epoch || l.epoch != slots_[k].epoch || l.count != s.count) return false;
        if (s.revision == 0) return s.count == kArenaValueVerts && l.byValue && l.key == key;
        return !l.byValue && l.generation == s.generation && l.key == key;
    }
    bool isStable(const Record& r, const ArenaSource& s, uint64_t key) const {
        if (r.seenFrame + 1 != frame_ || r.seenCount != s.count) return false;
        if (s.revision == 0) return r.seenByValue && s.count == kArenaValueVerts && key == r.valueSerial;
        return !r.seenByValue && r.seenGeneration == s.generation && r.seenRevision == s.revision;
    }
    // Every source of the command resident in `slot` (with `scheduled`: or
    // already written there this frame).
    bool wholly(const Command& c, int slot, bool scheduled) const {
        for (uint32_t k = c.first; k < c.first + c.count; ++k) {
            const uint32_t src = ranges_[k].source;
            if (!res_[src * kArenaSlots + size_t(slot)] && !(scheduled && sched_[src] != kNone)) return false;
        }
        return true;
    }
    // Vertices the command would copy into `cur` (each source once).
    size_t missing(const Command& c, int cur, uint32_t command) {
        size_t n = 0;
        for (uint32_t k = c.first; k < c.first + c.count; ++k) {
            const uint32_t src = ranges_[k].source;
            if (res_[src * kArenaSlots + size_t(cur)] || sched_[src] != kNone || stamp_[src] == command + 1) continue;
            stamp_[src] = command + 1;
            n += sources_[src].count;
        }
        return n;
    }
    size_t capacityFor(size_t total) const {
        size_t want = size_t(std::ceil(double(total) * policy_.headroom));
        want = (want + policy_.granule - 1) / policy_.granule * policy_.granule;
        return std::clamp(want, policy_.minCapacity, std::max(policy_.minCapacity, policy_.maxCapacity));
    }
    ArenaMode failPlan() {
        uploads_.clear(); allocate_ = false; releaseSlot_ = -1;
        mode_ = ArenaMode::Stream; stats_.mode = mode_; stats_.reason = ArenaStreamReason::Budget;
        return mode_;
    }
    bool shrinkDue() const {
        return shrinkSince_ && frame_ >= shrinkSinceFrame_ + policy_.shrinkFrames &&
               nowMs_ >= shrinkSinceMs_ + policy_.shrinkMs;
    }
    void invalidateStorage() {
        slots_[0] = slots_[1] = {}; cur_ = 0; shrinkSince_ = false;
    }
    // Records past the highest slot of this frame describe entries not drawn now.
    void trimRecords() {
        if (records_.size() > maxSlot_) records_.resize(maxSlot_);
        if (values_.size() > maxSlot_) values_.resize(maxSlot_);
        if (records_.capacity() > 2 * records_.size() + 256) records_.shrink_to_fit();
        if (values_.capacity() > 2 * values_.size() + 256) values_.shrink_to_fit();
    }

    ArenaPolicy policy_;
    std::vector<Record> records_;                                    // by slot (entry handle index)
    std::vector<std::array<SDL_Vertex, kArenaValueVerts>> values_;  // by slot: last seen by-value content
    std::vector<ArenaSource> sources_;
    std::vector<ArenaRange> ranges_;
    std::vector<Command> commands_;
    std::vector<ArenaUpload> uploads_;
    std::vector<uint8_t> res_;      // per source x storage: resident
    std::vector<uint32_t> offs_;    // per source x storage: resident offset
    std::vector<uint32_t> sched_;   // per source: offset written this frame (current storage), kNone if not
    std::vector<uint32_t> stamp_;   // per source: dedupe marks (forced pre-pass, per-command cost)
    std::vector<uint64_t> keys_;    // per source: content key this frame
    Slot slots_[kArenaSlots];
    int cur_ = 0, curPlanned_ = 0, releaseSlot_ = -1;
    ArenaFrameStats stats_;
    ArenaMode mode_ = ArenaMode::Stream;
    uint64_t frame_ = 0, nowMs_ = 0;
    size_t newCapacity_ = 0, writeBegin_ = 0, writeEnd_ = 0, maxSlot_ = 0;
    uint32_t nextEpoch_ = 1;   // epochs are never reused; 0 = no storage
    uint64_t lastUseFrame_ = 0, lastUseMs_ = 0, retryFrame_ = 0;
    bool shrinkSince_ = false, planned_ = false, allocate_ = false;
    uint64_t shrinkSinceFrame_ = 0, shrinkSinceMs_ = 0;
    uint64_t rebuilds_ = 0, rollovers_ = 0, drains_ = 0, failures_ = 0;
};

// The arena's storage: one GL buffer object (or, simulated, CPU memory for
// tests and the software renderer). Render thread only. Writes come from a
// caller-owned CPU copy (GameView stages them in the tail of its assembled
// body buffer, so they need no memory of their own) and go up with one
// glBufferSubData per frame.
//
// GPU memory: the storage is counted in gpuvram while it lives; when it is
// orphaned (reallocated) or released, its bytes stay counted for
// kInFlightFrames more frames -- the driver keeps an orphaned store until the
// draws that read it have executed. That is an estimate (the driver's own
// retention is not observable); it is never undercounted for a frame in which
// the GPU may still read it.
class VertexArenaBuffer {
public:
    static constexpr uint32_t kInFlightFrames = 3;
    VertexArenaBuffer() = default;
    VertexArenaBuffer(const VertexArenaBuffer&) = delete;
    VertexArenaBuffer& operator=(const VertexArenaBuffer&) = delete;
    ~VertexArenaBuffer() { release(); dropInFlight(); }

    // Simulated storage (debug / software renderer / tests): CPU memory, never
    // counted as GPU memory. Changing the mode releases the storage.
    void setSimulated(bool on) { if (on != simulated_) { release(); dropInFlight(); simulated_ = on; } }
    bool simulated() const { return simulated_; }

    // Can the arena run now? The current GL context has buffer objects. A new
    // context (context loss) forgets the old buffer without touching it.
    bool ready() {
        if (simulated_) return true;
        const SDL_GLContext context = SDL_GL_GetCurrentContext();
        if (!context) return false;
        if (!checked_ || context != context_) { forget(); checked_ = true; context_ = context; available_ = load(); }
        return available_;
    }
    // (Re)allocate the storage at `bytes`. The previous storage is orphaned:
    // pending draws keep reading it, nothing written from now on can reach it.
    bool allocate(size_t bytes) {
        retire();
        if (simulated_) {
            storage_.assign(bytes / sizeof(SDL_Vertex), SDL_Vertex{});
            capacity_ = bytes; ++allocations_;
            return true;
        }
        if (!ready()) return false;
        if (!buffer_) genBuffers_(1, &buffer_);
        if (!buffer_) return false;
        while (getError_() != GL_NO_ERROR) {}
        GLint previous = 0; getInteger_(GL_ARRAY_BUFFER_BINDING, &previous);
        bindBuffer_(GL_ARRAY_BUFFER, buffer_);
        bufferData_(GL_ARRAY_BUFFER, GLsizeiptr(bytes), nullptr, GL_DYNAMIC_DRAW);
        GLint allocated = 0; getBufferParameter_(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &allocated);
        const bool failed = getError_() == GL_OUT_OF_MEMORY || size_t(allocated) != bytes;
        bindBuffer_(GL_ARRAY_BUFFER, GLuint(previous));
        if (failed) {
            deleteBuffers_(1, &buffer_); buffer_ = 0;
            gpuvram::noteFail(); ++allocationFailures_;
            return false;
        }
        capacity_ = bytes; gpuvram::addBuffer(bytes); ++allocations_;
        return true;
    }
    // Write `count` vertices at vertex `first`. The planner only writes past
    // what the storage holds since its last (re)allocation, so no pending draw
    // reads the range and the upload needs no synchronization. false: the
    // storage contents are lost (upload error) -- the caller must not draw
    // from it and releases it.
    bool write(size_t first, const SDL_Vertex* data, size_t count) {
        const size_t offset = first * sizeof(SDL_Vertex), bytes = count * sizeof(SDL_Vertex);
        if (offset + bytes > capacity_) return false;
        if (!count) return true;
        ++writes_;
        if (simulated_) { std::memcpy(storage_.data() + first, data, bytes); return true; }
        if (!buffer_) return false;
        while (getError_() != GL_NO_ERROR) {}
        GLint previous = 0; getInteger_(GL_ARRAY_BUFFER_BINDING, &previous);
        bindBuffer_(GL_ARRAY_BUFFER, buffer_);
        bufferSubData_(GL_ARRAY_BUFFER, GLintptr(offset), GLsizeiptr(bytes), data);
        const bool ok = getError_() == GL_NO_ERROR;
        bindBuffer_(GL_ARRAY_BUFFER, GLuint(previous));
        return ok;
    }
    // Release the storage (its bytes stay counted while draws may be in flight).
    void release() {
        if (simulated_) { std::vector<SDL_Vertex>().swap(storage_); capacity_ = 0; return; }
        retire();
        if (buffer_ && SDL_GL_GetCurrentContext() == context_ && deleteBuffers_) deleteBuffers_(1, &buffer_);
        buffer_ = 0;
    }
    // The context is gone (device reset / new context): its objects died with
    // it, so nothing is deleted and nothing stays counted.
    void forget() {
        gpuvram::removeBuffer(capacity_); capacity_ = 0; buffer_ = 0; dropInFlight();
        std::vector<SDL_Vertex>().swap(storage_);
    }
    // Once per frame: in-flight bytes of orphaned/released storage expire.
    void endFrame() {
        size_t kept = 0;
        for (auto& r : retired_) {
            if (--r.framesLeft == 0) gpuvram::removeBuffer(r.bytes);
            else retired_[kept++] = r;
        }
        retired_.resize(kept);
    }

    GLuint id() const { return buffer_; }
    const SDL_Vertex* simulatedData() const { return storage_.data(); }
    size_t capacityBytes() const { return capacity_; }
    size_t inFlightBytes() const { size_t b = 0; for (const auto& r : retired_) b += r.bytes; return b; }
    uint64_t allocations() const { return allocations_; }
    uint64_t allocationFailures() const { return allocationFailures_; }
    uint64_t writes() const { return writes_; }

private:
    struct Retired { size_t bytes; uint32_t framesLeft; };
    void retire() {
        if (!simulated_ && capacity_) retired_.push_back({capacity_, kInFlightFrames});
        capacity_ = 0;
    }
    void dropInFlight() {
        for (const auto& r : retired_) gpuvram::removeBuffer(r.bytes);
        retired_.clear();
    }
    template <class T> static bool proc(T& out, const char* name) {
        out = reinterpret_cast<T>(SDL_GL_GetProcAddress(name)); return out != nullptr;
    }
    bool load() {
        if (!proc(genBuffers_, "glGenBuffers") || !proc(deleteBuffers_, "glDeleteBuffers") ||
            !proc(bindBuffer_, "glBindBuffer") || !proc(bufferData_, "glBufferData") ||
            !proc(bufferSubData_, "glBufferSubData") || !proc(getBufferParameter_, "glGetBufferParameteriv") ||
            !proc(getInteger_, "glGetIntegerv") || !proc(getError_, "glGetError"))
            return false;
        return true;
    }

    bool simulated_ = false, checked_ = false, available_ = false;
    SDL_GLContext context_ = nullptr;
    GLuint buffer_ = 0;
    size_t capacity_ = 0;
    std::vector<SDL_Vertex> storage_;
    std::vector<Retired> retired_;
    uint64_t allocations_ = 0, allocationFailures_ = 0, writes_ = 0;
    void (APIENTRY* genBuffers_)(GLsizei, GLuint*) = nullptr;
    void (APIENTRY* deleteBuffers_)(GLsizei, const GLuint*) = nullptr;
    void (APIENTRY* bindBuffer_)(GLenum, GLuint) = nullptr;
    void (APIENTRY* bufferData_)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
    void (APIENTRY* bufferSubData_)(GLenum, GLintptr, GLsizeiptr, const void*) = nullptr;
    void (APIENTRY* getBufferParameter_)(GLenum, GLenum, GLint*) = nullptr;
    void (APIENTRY* getInteger_)(GLenum, GLint*) = nullptr;
    GLenum (APIENTRY* getError_)() = nullptr;
};

// Merge a command's arena ranges: consecutive ranges that continue each other
// become one (a rebuilt arena holds a run of painter-ordered sources in one
// piece). Appends to firsts/counts; returns the number of merged ranges added.
inline size_t appendArenaRange(std::vector<GLint>& firsts, std::vector<GLsizei>& counts, size_t commandBegin,
                               uint32_t first, uint32_t count) {
    if (count == 0) return 0;
    if (firsts.size() > commandBegin && size_t(firsts.back()) + size_t(counts.back()) == first) {
        counts.back() += GLsizei(count);
        return 0;
    }
    firsts.push_back(GLint(first)); counts.push_back(GLsizei(count));
    return 1;
}

}  // namespace tak::render
