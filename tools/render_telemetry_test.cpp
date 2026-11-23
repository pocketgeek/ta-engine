// Shared renderer vocabulary (client/rendertelemetry.h): generational handles
// must never alias a recycled slot, stats must stay inert when off, and the
// RENDERSTATS line keeps the key names tools/render_benchmark.py parses.
// Also: geometry-miss attribution (client/geometrystages.h) and the worker
// pool's dispatch telemetry (client/threadpool.h), which must not change results.
#include "client/geometrystages.h"
#include "client/rendertelemetry.h"
#include "client/threadpool.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
}

int main() {
    using namespace tak::render;
    {   // Handles: capacity bound, stale-handle rejection, generation on reuse.
        HandleSlots slots(2);
        const RenderHandle a = slots.acquire(), b = slots.acquire();
        check(a.valid() && b.valid() && !(a == b), "two live handles");
        check(!slots.acquire().valid(), "capacity is a hard bound");
        check(slots.release(a) && !slots.valid(a), "released handle is stale");
        check(!slots.release(a), "double release refused");
        const RenderHandle c = slots.acquire();
        check(c.index == a.index && c.generation != a.generation, "recycled index gets a new generation");
        check(slots.valid(c) && !slots.valid(a), "stale handle cannot alias the new occupant");
        check(slots.live() == 2 && slots.allocated() == 2, "live/allocated accounting");
        check(!slots.valid(RenderHandle{}), "default handle invalid");
        check(!slots.valid(RenderHandle{7, 1}), "out-of-range index invalid");
        // Churn one slot many times: generations never repeat or hit zero.
        RenderHandle last = c;
        for (int i = 0; i < 1000; ++i) {
            slots.release(last);
            const RenderHandle next = slots.acquire();
            check(next.generation != last.generation && next.generation != 0, "generation advances");
            last = next;
        }
    }
    {   // Generations start live at 1; 0 is reserved for "never".
        GenerationCounter g;
        check(g.current() == 1 && g.current() != kNoGeneration, "generation starts at 1");
        const Generation before = g.current();
        check(g.bump() == before + 1 && g.current() == before + 1, "bump advances");
    }
    {   // Name tables: complete, unique (the benchmark keys on them).
        std::set<std::string> names;
        size_t total = 0;
        const auto add = [&](const char* prefix, const char* const* table, size_t count) {
            for (size_t i = 0; i < count; ++i) {
                check(table[i] && *table[i], "every enum value has a name");
                if (table[i]) names.insert(std::string(prefix) + table[i]);
                ++total;
            }
        };
        add("t_", kPhaseNames, size_t(Phase::Count));
        add("w_", kWorkNames, size_t(Work::Count));
        add("", kCounterNames, size_t(Counter::Count));
        add("miss_", kMissNames, size_t(MissCause::Count));
        check(names.size() == total, "telemetry keys are unique");
    }
    {   // Off: tallies and phase timers record nothing.
        FrameStats s;
        s.begin(false);
        s.add(Counter::VisibleUnits, 5);
        s.addMiss(MissCause::Cold);
        { ScopedPhase p(s, Phase::GeometryPrep); }
        s.addWorkNs(Work::PoolBusy, 5000000);
        s.addMs(Phase::Terrain, 2.0);
        check(s.get(Counter::VisibleUnits) == 0 && s.miss[0] == 0 && s.ms[size_t(Phase::GeometryPrep)] == 0 &&
              s.work[size_t(Work::PoolBusy)] == 0 && s.ms[size_t(Phase::Terrain)] == 0,
              "disabled stats stay zero");
        // On: values recorded, begin() resets them. A release (NDEBUG) build
        // compiles telemetry out, so enabling it there must still record nothing.
        s.begin(true);
        check(s.enabled() == kTelemetryCompiled, "enabled only where compiled in");
        if (!kTelemetryCompiled) {
            s.add(Counter::VisibleUnits, 5);
            check(s.get(Counter::VisibleUnits) == 0, "release build records nothing");
            std::printf("note: render telemetry compiled out (NDEBUG); enabled-path checks skipped\n");
        } else {
        s.add(Counter::VisibleUnits, 5);
        s.add(Counter::VisibleUnits);
        s.set(Counter::GpuTrackedBytes, 42);
        s.addMiss(MissCause::Owner, 3);
        { ScopedPhase p(s, Phase::BodySubmit); }
        s.addWorkNs(Work::GeometryKey, 1500000);
        s.add(Counter::PoolItems, 7);
        check(s.get(Counter::VisibleUnits) == 6 && s.get(Counter::GpuTrackedBytes) == 42, "enabled tallies");
        check(s.work[size_t(Work::GeometryKey)] == 1.5, "worker nanoseconds folded as ms");
        check(s.ms[size_t(Phase::BodySubmit)] >= 0, "phase recorded");
        char line[16384];
        const int n = s.format(line, sizeof line);
        const std::string text(line, size_t(n));
        check(text.find(" visible_units=6") != std::string::npos, "counter key");
        check(text.find(" gpu_tracked_bytes=42") != std::string::npos, "phase-0 counter key");
        check(text.find(" pool_items=7") != std::string::npos, "last counter key");
        check(text.find(" w_geometry_key=1.5000") != std::string::npos, "worker key");
        check(text.find(" miss_zoom=0") != std::string::npos, "last miss key");
        check(size_t(n) < 8192, "whole line well inside the client's buffer");
        check(text.find(" miss_owner=3") != std::string::npos, "miss key");
        check(text.find(" t_body_submit=") != std::string::npos && text.find(" t_candidates=") != std::string::npos,
              "phase keys");
        s.begin(true);
        check(s.get(Counter::VisibleUnits) == 0, "begin resets");
        char tiny[16];
        s.format(tiny, sizeof tiny);
        check(std::strlen(tiny) < sizeof tiny, "truncated format stays terminated");
        }
    }
    {   // Geometry-miss attribution reads the exact stage keys (workstream D).
        // Local key: 13 fixed words, 2 texture-frame words, piece table + 2 pose words.
        std::vector<uint64_t> base(kLocalKeyFixedWords, 0);
        base[0] = 7;                                  // unit id
        base.insert(base.end(), {3, 4});              // texture frames
        base.insert(base.end(), {0xabc, 10, 11});     // piece table, poses
        const LocalKeyLayout layout{uint32_t(kLocalKeyFixedWords + 2)};
        const auto changes = [&](std::vector<uint64_t> after, LocalKeyLayout afterLayout) {
            return localKeyChanges(base, layout, after, afterLayout);
        };
        const auto with = [&](size_t i, uint64_t v) { auto k = base; k[i] = v; return k; };
        check(changes(with(0, 8), layout) == missBit(MissCause::Owner), "owner change reported alone");
        check(changes(with(12, 1), layout) == missBit(MissCause::Pose), "facing is pose");
        check(changes(with(6, 1), layout) == missBit(MissCause::Pose), "body pitch is pose");
        check(changes(with(9, 1), layout) == missBit(MissCause::Pose), "corpse is pose");
        check(changes(with(4, 1), layout) == missBit(MissCause::Material), "player palette is material");
        check(changes(with(8, 1), layout) == missBit(MissCause::Material), "construction is material");
        check(changes(with(2, 1), layout) == missBit(MissCause::Resource), "model is resource");
        check(changes(with(3, 1), layout) == missBit(MissCause::Resource), "atlas is resource");
        check(changes(with(11, 1), layout) == missBit(MissCause::Settings), "shadow toggle is settings");
        check(changes(with(kLocalKeyFixedWords + 1, 9), layout) == missBit(MissCause::Material),
              "texture frame is material");
        check(changes(with(kLocalKeyFixedWords + 3, 99), layout) == missBit(MissCause::Pose), "piece pose");
        check(changes(with(kLocalKeyFixedWords + 2, 1), layout) == missBit(MissCause::Resource),
              "piece table is resource");
        auto moved = with(12, 5); moved[kLocalKeyFixedWords + 4] = 1; moved[3] = 1;
        const uint32_t several = changes(moved, layout);
        check(several == (missBit(MissCause::Resource) | missBit(MissCause::Pose)), "every changed category counted");
        check(primaryMissCause(several) == MissCause::Resource, "filed under the more fundamental stage");
        // A frame list of another length moves the pose section.
        std::vector<uint64_t> longer(base.begin(), base.begin() + kLocalKeyFixedWords);
        longer.push_back(5);
        longer.insert(longer.end(), base.begin() + kLocalKeyFixedWords, base.end());
        check(changes(longer, {layout.poseBegin + 1}) == missBit(MissCause::Material), "frame list length is material");
        // Losing the VM pose (corpse / replacement model) removes the pose section.
        auto noPose = base; noPose.resize(layout.poseBegin);
        check(changes(noPose, layout) == missBit(MissCause::Resource), "pose table removed");
        check(changes(base, layout) == 0, "identical local keys claim nothing");
        check(primaryMissCause(0) == MissCause::Changed, "empty mask files as changed");
        check(localKeyChanges(std::vector<uint64_t>(3), {}, base, layout) == missBit(MissCause::Changed),
              "malformed key is unclaimed");
        check(changes(base, {uint32_t(base.size() + 1)}) == missBit(MissCause::Changed), "bad layout is unclaimed");
        // Placement: zoom apart from the anchor, the clip line and the emote bob.
        PlacementKey place{};
        auto placeWith = [&](size_t i) { auto k = place; k[i] = 1; return k; };
        check(placementKeyChanges(place, placeWith(kPlacementZoom)) == missBit(MissCause::Zoom), "zoom");
        for (size_t i : {size_t(kPlacementAnchorX), size_t(kPlacementAnchorY), size_t(kPlacementBob),
                         size_t(kPlacementAltitude), size_t(kPlacementOcclusion)})
            check(placementKeyChanges(place, placeWith(i)) == missBit(MissCause::Placement), "anchor/clip is placement");
        check(placementKeyChanges(place, place) == 0, "same placement");
        MaterialKey material{};
        auto later = material; later[kMaterialClock] = 1;
        check(materialKeyChanges(material, later) == missBit(MissCause::Material), "conjuring clock is material");
        check(materialKeyChanges(material, material) == 0, "same material");
        check(primaryMissCause(missBit(MissCause::Material) | missBit(MissCause::Zoom) |
                               missBit(MissCause::Placement)) == MissCause::Material, "material before placement");
    }
    {   // Worker-pool telemetry: counts and times, identical results either way.
        ThreadPool pool(4);
        ThreadPool::Stats stats;
        const auto run = [&](size_t count, size_t minParallel) {
            std::vector<int> out(count, 0);
            pool.parallelFor(count, [&](size_t b, size_t e) { for (size_t i = b; i < e; ++i) out[i] = int(i * 3); },
                             minParallel);
            for (size_t i = 0; i < count; ++i)
                if (out[i] != int(i * 3)) return false;
            return true;
        };
        check(run(100, 32), "untimed parallel results");
        pool.setStats(&stats);
        check(run(100, 32) && run(10, 32) && run(200, 32), "timed results identical");
        check(stats.dispatches == 2 && stats.serial == 1 && stats.items == 310, "dispatch / serial / item counts");
        // 100 items = 13 chunks: each dispatch wakes min(pool, chunks) - 1 workers; a
        // worker counts as a wake when it ran a chunk (one that arrives after every
        // chunk was taken is an idle wake, folded at a later timed pass).
        check(stats.posted == 2 * (pool.threads() - 1) && stats.chunks == 13 + 25, "workers posted per dispatch");
        check(stats.wakes <= stats.posted, "a wake is recorded only for a posted worker");
        check(stats.busyNs > 0, "participant busy time recorded");
        pool.setStats(nullptr);
        const ThreadPool::Stats before = stats;
        check(run(100, 32), "untimed again");
        check(stats.dispatches == before.dispatches && stats.busyNs == before.busyNs, "detached stats untouched");
    }
    if (failures) return EXIT_FAILURE;
    std::printf("PASS: render telemetry handles, generations, stats, miss attribution and pool telemetry\n");
    return EXIT_SUCCESS;
}
