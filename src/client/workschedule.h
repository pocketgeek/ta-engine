#pragma once

// Estimated-cost plans for the renderer's worker-pool passes (workstream I).
//
// A pass adds its items in their natural order with an estimate of each item's
// cost in nanoseconds (dirty vertices, points or copy bytes times a calibrated
// rate). The plan cuts the items into contiguous chunks of roughly equal
// estimated cost and decides how many threads the pass is worth:
//
//   - below `serialBelowNs` of estimated work the caller runs the pass alone
//     (waking a worker costs more than the work);
//   - otherwise one participant per `workerNs` of estimated work, capped by
//     the pool, the chunk count and the policy's own cap (none of the renderer
//     passes sets one: a 4-thread cap on the body copy measured slower);
//   - chunks of about `chunkNs` (a fraction of workerNs) so that participants
//     that wake late, and mispredicted items, still balance through claiming.
//
// The plan depends only on the estimates, never on timing, and it never decides
// WHAT a pass computes: every item still writes only its own outputs, so the
// result equals the serial loop whichever thread runs which chunk.
//
// Header-only, no SDL; tested by tools/workschedule_test.cpp.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tak::render {

struct SchedulePolicy {
    double serialBelowNs = 30000;   // whole-pass estimate under which the caller runs alone
    double workerNs = 40000;        // estimated work that justifies one more participant
    double chunkNs = 10000;         // target estimate per chunk
    unsigned maxParticipants = 0;   // 0 = only the pool and the chunk count limit it
};

class WorkPlan {
public:
    // Upper bound on chunks; beyond it the last chunk absorbs the rest.
    static constexpr size_t kMaxChunks = 65536;

    void begin(const SchedulePolicy& policy) {
        policy_ = policy;
        bounds_.clear();
        bounds_.push_back(0);
        count_ = 0;
        acc_ = total_ = 0;
        participants_ = 1;
    }
    // Next item, in pass order. A non-positive estimate is a free item (it still
    // belongs to the chunk it falls in).
    void add(double ns) {
        ++count_;
        if (ns > 0) { acc_ += ns; total_ += ns; }
        if (acc_ >= policy_.chunkNs && bounds_.size() <= kMaxChunks) {
            bounds_.push_back(uint32_t(count_));
            acc_ = 0;
        }
    }
    // Many items at once, each estimated as units[i] * unitNs (e.g. the packed
    // per-entry geometry estimates): the same plan as add() per item, in
    // integer units, without a floating-point step per item.
    void addUnits(std::span<const uint16_t> units, double unitNs) {
        if (unitNs <= 0) { for (size_t i = 0; i < units.size(); ++i) add(0); return; }
        const double chunkUnits = std::ceil(policy_.chunkNs / unitNs);
        const uint64_t limit = chunkUnits > 0 ? uint64_t(chunkUnits) : 1;
        uint64_t acc = uint64_t(std::llround(std::max(acc_, 0.0) / unitNs)), total = 0;
        for (const uint16_t u : units) {
            ++count_;
            acc += u; total += u;
            if (acc >= limit && bounds_.size() <= kMaxChunks) {
                bounds_.push_back(uint32_t(count_));
                acc = 0;
            }
        }
        acc_ = double(acc) * unitNs;
        total_ += double(total) * unitNs;
    }
    // Close the plan for a pool of `poolThreads` threads (the caller included).
    void finish(unsigned poolThreads) {
        if (bounds_.back() != count_) bounds_.push_back(uint32_t(count_));
        const size_t chunks = bounds_.size() - 1;
        if (chunks == 0 || total_ < policy_.serialBelowNs || policy_.workerNs <= 0) {
            participants_ = 1;
            return;
        }
        const double wanted = std::ceil(total_ / policy_.workerNs);
        size_t cap = std::min<size_t>(std::max(poolThreads, 1u), chunks);
        if (policy_.maxParticipants) cap = std::min<size_t>(cap, policy_.maxParticipants);
        participants_ = unsigned(std::clamp<double>(wanted, 1.0, double(cap)));
    }

    std::span<const uint32_t> bounds() const { return bounds_; }
    size_t chunks() const { return bounds_.empty() ? 0 : bounds_.size() - 1; }
    size_t items() const { return count_; }
    unsigned participants() const { return participants_; }
    double estimateNs() const { return total_; }
    const SchedulePolicy& policy() const { return policy_; }
    size_t usedBytes() const { return bounds_.size() * sizeof(uint32_t); }
    size_t capacityBytes() const { return bounds_.capacity() * sizeof(uint32_t); }

private:
    SchedulePolicy policy_;
    std::vector<uint32_t> bounds_{0};
    size_t count_ = 0;
    double acc_ = 0, total_ = 0;
    unsigned participants_ = 1;
};

// Estimated cost of one unit's geometry build (GameView::buildUnitGeom), from
// what the build redid: the exact key check (every unit, every frame) plus the
// stages that ran. Rates fitted to serial per-unit timings on the reference
// machine (render optimization Experiment 11); they only
// steer chunking and the participant count, never what is computed.
struct GeometryCostModel {
    // Fitted to 952k serial per-unit samples over eight matrix scenes (median
    // relative error: key 13%, local 21%, placement 13%, material 9%).
    static constexpr double kKeyBaseNs = 580;        // interpolation, lift, fixed key words
    static constexpr double kKeyPerWordNs = 2.3;     // per local key word (frames, piece poses)
    static constexpr double kLocalBaseNs = 750;      // piece transforms, collection setup
    static constexpr double kLocalPerVertNs = 31.5;  // collect + sort + place + colour, per body vertex
    static constexpr double kLocalPerShadowNs = 5;   // silhouette collection, per shadow point (body built too)
    static constexpr double kShadowOnlyPerPointNs = 15;   // silhouette without the body (G deferral; not fitted)
    static constexpr double kPlaceBaseNs = 350;
    static constexpr double kPlacePerVertNs = 2.5;   // re-placement, per body vertex
    static constexpr double kPlacePerShadowNs = 0.9; // ... per shadow point
    static constexpr double kMaterialPerVertNs = 8;  // re-colouring, per body vertex
    static constexpr double kColdNs = 16000;         // an entry with no history (mean full build)

    // stages / bodyStages: tak::render::StageBit sets (local 1, placement 2,
    // material 4) of the whole build and of its body half (workstream G may hold
    // the body back); sizes are the entry's body vertices and shadow points.
    static double estimateNs(uint8_t stages, uint8_t bodyStages, size_t keyWords, size_t bodyVerts,
                             size_t shadowPoints) {
        double ns = kKeyBaseNs + kKeyPerWordNs * double(keyWords);
        if (stages & 1) {
            ns += kLocalBaseNs;
            if (bodyStages & 1) ns += kLocalPerVertNs * double(bodyVerts) + kLocalPerShadowNs * double(shadowPoints);
            else ns += kShadowOnlyPerPointNs * double(shadowPoints);
            return ns;
        }
        if (stages & 2) ns += kPlaceBaseNs + kPlacePerShadowNs * double(shadowPoints);
        if (bodyStages & 2) ns += kPlacePerVertNs * double(bodyVerts);
        if (bodyStages & 4) ns += kMaterialPerVertNs * double(bodyVerts);
        return ns;
    }
    // Compact per-entry form (UnitGeom::nextCost): units of kUnitNs, saturating.
    static constexpr double kUnitNs = 64;
    static uint16_t pack(double ns) {
        const double units = std::ceil(std::max(ns, 0.0) / kUnitNs);
        return uint16_t(std::min(units, 65535.0));
    }
    static double unpack(uint16_t units) { return double(units) * kUnitNs; }
};

// Per-pass policies. workerNs ~ the work that pays for one more woken thread
// (wake latency ~10-50 us when the CPUs are busy); chunkNs a quarter of it.
struct RenderSchedules {
    SchedulePolicy geometry{.serialBelowNs = 30000, .workerNs = 40000, .chunkNs = 10000};
    SchedulePolicy shadowCheck{.serialBelowNs = 30000, .workerNs = 40000, .chunkNs = 10000};
    SchedulePolicy shadowPrep{.serialBelowNs = 30000, .workerNs = 40000, .chunkNs = 10000};
    SchedulePolicy bodyCopy{.serialBelowNs = 30000, .workerNs = 40000, .chunkNs = 10000};
    // Fitted to serial per-pass timings (Experiment 11): a point check stops at
    // the first differing point, so the rate is the full-scan (kept tile) cost.
    static constexpr double kShadowCheckPerPointNs = 2;     // sameTilePoints, per point
    static constexpr double kShadowPrepPerPointNs = 1.6;    // tilePoint transform, per opaque point
    static constexpr double kShadowPrepPerMaskedNs = 3.2;   // ... per masked vertex (20-byte copy)
    static constexpr double kCopyPerByteNs = 0.1;           // memcpy, ~10 GB/s per thread
    static constexpr double kCopyPerTaskNs = 40;            // per copy range (cache misses at its ends)
};

}  // namespace tak::render
