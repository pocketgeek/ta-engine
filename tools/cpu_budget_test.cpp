// Retained-buffer trim policy (client/cpubudget.h, rendering workstream J).
// Simulates a buffer that grows like the renderer's (need + 1/16 when a frame
// exceeds capacity) under scripted demand and checks: no trim before a whole
// window of low demand (time AND frames), never a shrink per frame, spikes and
// oscillation inside the window block it, the factor and floor rules, the
// regrowth backoff, a clock reset, and shrinkVector.
#include "client/cpubudget.h"

#include <cstdio>
#include <functional>
#include <vector>

using tak::render::TrimPolicy;
using tak::render::TrimTracker;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

struct Sim {
    TrimTracker tracker;
    TrimPolicy policy;
    size_t capacity = 0;
    uint64_t frame = 0, ms = 0;
    std::vector<uint64_t> trimFrames;
    size_t grows = 0;
    // Runs `frames` frames of `msPerFrame` with need(frame) bytes.
    void run(uint64_t frames, uint64_t msPerFrame, const std::function<size_t(uint64_t)>& need) {
        for (uint64_t i = 0; i < frames; ++i, ++frame, ms += msPerFrame) {
            const size_t n = need(frame);
            if (n > capacity) { capacity = n + n / 16; ++grows; }
            if (const size_t target = tracker.observe(n, capacity, frame, ms, policy)) {
                check(target < capacity, "a trim shrinks");
                check(target >= n, "a trim keeps this frame's need");
                capacity = target;
                trimFrames.push_back(frame);
            }
        }
    }
};

constexpr size_t MiB = size_t(1) << 20;

void sustainedWindow() {
    Sim s;   // 60 fps: 5 s = 300 frames, the time condition binds
    s.run(1, 16, [](uint64_t) { return 50 * MiB; });
    s.run(1000, 16, [](uint64_t) { return 1 * MiB; });
    check(s.trimFrames.size() == 1, "one trim after the demand fell");
    check(!s.trimFrames.empty() && s.trimFrames[0] >= 300 && s.trimFrames[0] <= 480,
          "the trim waits for a whole window (5 s) and comes within 1.5 windows");
    check(s.capacity == MiB + MiB / 16, "trimmed to the window peak + 1/16");
    // Afterwards nothing more: capacity is within the factor of the need.
    s.run(2000, 16, [](uint64_t) { return 1 * MiB; });
    check(s.trimFrames.size() == 1, "never a second trim at steady demand");
}

void framesBind() {
    Sim s;   // 1 fps: 120 frames = 2 minutes, the frame condition binds
    s.run(1, 1000, [](uint64_t) { return 50 * MiB; });
    s.run(100, 1000, [](uint64_t) { return 1 * MiB; });
    check(s.trimFrames.empty(), "no trim before 120 frames, however much time passed");
    s.run(100, 1000, [](uint64_t) { return 1 * MiB; });
    check(s.trimFrames.size() == 1, "trim once the frames are there too");
}

void spikesAndOscillation() {
    Sim s;
    // A 50 MiB frame every 2 s (e.g. an arena rollover): every window holds one.
    s.run(5000, 16, [](uint64_t f) { return f % 125 == 0 ? 50 * MiB : 3 * MiB; });
    check(s.trimFrames.empty(), "periodic spikes inside the window block the trim");
    check(s.grows == 1, "and nothing regrows");
    Sim o;
    // 3 s busy / 3 s quiet: no 5 s window is ever entirely quiet.
    o.run(6000, 10, [](uint64_t f) { return (f / 300) % 2 ? 1 * MiB : 40 * MiB; });
    check(o.trimFrames.empty(), "oscillation shorter than the window never trims");
}

void factorAndFloor() {
    Sim s;
    s.capacity = 19 * MiB / 10;   // 1.9 x the need: inside the hysteresis band
    s.run(2000, 16, [](uint64_t) { return 1 * MiB; });
    check(s.trimFrames.empty(), "capacity below factor x need is kept");
    Sim t;
    t.capacity = 21 * MiB / 10;
    t.run(2000, 16, [](uint64_t) { return 1 * MiB; });
    check(t.trimFrames.size() == 1, "capacity above factor x need is trimmed");
    Sim f;
    f.capacity = 100 << 10;       // below factor x floor (128 KiB)
    f.run(2000, 16, [](uint64_t) { return 0; });
    check(f.trimFrames.empty(), "small buffers are not worth trimming");
    Sim z;
    z.capacity = 10 * MiB;
    z.run(2000, 16, [](uint64_t) { return 0; });
    check(z.trimFrames.size() == 1 && z.capacity == z.policy.floorBytes, "an idle buffer keeps the floor");
}

void backoff() {
    Sim s;
    // Busy 0.5 s every 9 s: each quiet stretch trims, each burst regrows soon after.
    s.run(30000, 10, [](uint64_t f) { return f % 900 < 50 ? 40 * MiB : 1 * MiB; });
    check(s.tracker.backoff() > 1, "regrowth soon after a trim backs the window off");
    check(s.trimFrames.size() <= 12, "trims thin out instead of one per cycle");
    for (size_t i = 1; i < s.trimFrames.size(); ++i)
        check(s.trimFrames[i] - s.trimFrames[i - 1] >= 500, "never two trims within a window");
    // Demand that then stays low: the trim holds and the backoff relaxes.
    const uint32_t before = s.tracker.backoff();
    s.run(100000, 10, [](uint64_t) { return 1 * MiB; });
    check(s.tracker.backoff() < before, "a trim that holds relaxes the backoff");
}

void clockReset() {
    Sim s;
    s.run(250, 16, [](uint64_t) { return 50 * MiB; });
    s.ms = 0;   // the presentation clock restarted (new game)
    s.run(200, 16, [](uint64_t) { return 1 * MiB; });
    check(s.trimFrames.empty(), "a clock that goes back restarts the window");
}

void shrink() {
    std::vector<int> v(10, 7);
    v.reserve(1000);
    const size_t released = tak::render::shrinkVector(v, 100);
    check(v.size() == 10 && v[9] == 7, "contents kept");
    check(v.capacity() >= 100 && v.capacity() < 1000, "capacity reduced to the target");
    check(released == (1000 - v.capacity()) * sizeof(int), "released bytes");
    check(tak::render::shrinkVector(v, 5) > 0 && v.capacity() >= 10, "never below the size");
    check(tak::render::shrinkVector(v, 5000) == 0, "never grows");
}
} // namespace

int main() {
    sustainedWindow();
    framesBind();
    spikesAndOscillation();
    factorAndFloor();
    backoff();
    clockReset();
    shrink();
    if (failures) { std::fprintf(stderr, "cpu_budget: %d failure(s)\n", failures); return 1; }
    std::printf("cpu_budget: ok\n");
    return 0;
}
