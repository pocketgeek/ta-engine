// client/speechqueue.h: TotalA.exe's unit speech queue (0x47fad0, 0x47f680).
#include "client/speechqueue.h"

#include <cstdio>

namespace {

bool check(bool condition, const char* what) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", what);
    return condition;
}

ta::SpeechEntry entry(int event, int priority) {
    ta::SpeechEntry e;
    e.event = event; e.priority = priority; e.unitName = "u" + std::to_string(event);
    return e;
}

} // namespace

int main() {
    bool ok = true;
    ta::SpeechQueue q;
    // The spacing counts from tick 0: nothing is voiced before tick 30.
    q.request(entry(2, 9));
    auto turn = q.next(10);
    ok &= check(turn && !turn->speak && turn->entry.event == 2, "before the first spacing: text only");
    ok &= check(q.lastSpoken == 0, "a text-only turn leaves the spoken tick");
    ok &= check(!q.next(11), "an empty queue plays nothing");
    q.request(entry(2, 9));
    turn = q.next(30);
    ok &= check(turn && turn->speak && q.lastSpoken == 30, "spacing reached: voiced, tick kept");
    // One entry a frame, one voice per 30 ticks.
    q.request(entry(1, 5));
    q.request(entry(2, 9));
    ok &= check(!q.request(entry(2, 9)), "a queued event is not queued twice");
    ok &= check(q.entries.size() == 2 && q.entries[0].event == 2, "higher priority first");
    turn = q.next(40);
    ok &= check(turn && turn->entry.event == 2 && !turn->speak, "within the spacing: text only");
    turn = q.next(60);
    ok &= check(turn && turn->entry.event == 1 && turn->speak && q.lastSpoken == 60, "next frame past the spacing: voiced");
    // Equal priorities keep their order; a full queue plays its last entry text-only.
    for (int e = 0; e < 8; ++e) q.request(entry(10 + e, 3));
    ok &= check(q.entries.size() == 8 && q.entries.front().event == 10 && q.entries.back().event == 17,
                "equal priorities first come, first served");
    const auto evicted = q.request(entry(30, 7));
    ok &= check(evicted && evicted->entry.event == 17 && !evicted->speak, "a full queue drops its last, text only");
    ok &= check(q.entries.size() == 8 && q.entries.front().event == 30, "the new entry by priority");
    if (ok) std::puts("PASS: unit speech queue");
    return ok ? 0 : 1;
}
