// scn_truth -- run a harvested situation (.scn with a `truth` line) on THIS build's sim and print how many of
// its bodies sit within 2 cells of the recording's positions `truth` ticks in. No observer, no baseline, no
// Retail comparison: this file exists so the same fidelity check runs on the OLD build that recorded the replay
// (legion-r9 on 02aa55a, legion-r10 on 7aae704), where tools/legion_scenario does not build. The harvest
// (tools/scenarios/situation-*.scn) is committed only if this reports 90% or more there.
//
//   scn_truth <file.scn> --data <install> [--mode legion|retail] [--offset N] [--detail]
//
// Output: one line per `truth` line of the file:
//   `truth t=T n=.. within2_permille=.. moved_n=.. moved_within2_permille=.. commands=..`.
// Exit 0 ok, 2 usage or a bad file, 77 needs --data.
#include "legion_scn.h"

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    const char* file = nullptr;
    const char* data = nullptr;
    auto mode = tak::sim::PathfindingMode::Legion;
    int offset = 0;
    bool detail = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--data") && i + 1 < argc) data = argv[++i];
        else if (!std::strcmp(argv[i], "--mode") && i + 1 < argc) mode = !std::strcmp(argv[++i], "retail") ? tak::sim::PathfindingMode::Retail : tak::sim::PathfindingMode::Legion;
        else if (!std::strcmp(argv[i], "--offset") && i + 1 < argc) offset = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--detail")) detail = true;
        else if (argv[i][0] != '-' && !file) file = argv[i];
        else { std::fprintf(stderr, "usage: scn_truth <file.scn> --data DIR [--mode legion|retail] [--offset N] [--detail]\n"); return 2; }
    }
    if (!file) { std::fprintf(stderr, "usage: scn_truth <file.scn> --data DIR\n"); return 2; }
    try {
        const auto s = tak::scn::load(file);
        tak::scn::BuildOptions bo;
        bo.mode = mode;
        bo.serial = true;
        bo.offset = offset;
        bo.data = data;
        auto b = tak::scn::build(s, bo);
        if (!b->skipped.empty()) { std::fprintf(stderr, "%s\n", b->skipped.c_str()); return 77; }
        if (s.truths.empty()) { std::fprintf(stderr, "%s has no truth line\n", file); return 2; }
        const auto start = tak::scn::startPositions(s, *b);
        tak::scn::OrderFeed feed(s, *b);
        int64_t commands = 0;
        for (uint32_t t = 0; t < s.truths.back().tick; ++t) {
            commands += int64_t(feed.apply(t));
            b->world->tick(1.f / 30);
            for (const auto& tr : s.truths) {
                if (t + 1 != tr.tick) continue;
                const auto r = tak::scn::truthReport(s, *b, tr, start, offset, detail ? "" : nullptr);
                std::printf("truth t=%u n=%lld within2_permille=%lld moved_n=%lld moved_within2_permille=%lld commands=%lld\n",
                            tr.tick, (long long)r.n, (long long)r.within2Permille(), (long long)r.movedN,
                            (long long)r.movedWithin2Permille(), (long long)commands);
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "scn_truth: %s\n", e.what());
        return 2;
    }
    return 0;
}
