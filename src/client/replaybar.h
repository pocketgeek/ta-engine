#pragma once

// The replay transport: the speed steps, the bar's geometry, the timeline's x <-> tick
// mapping and the keyframe bookkeeping behind rewinding. No SDL and no GameView, so
// replaybar_test pins all of it. Display and seek logic only: what a replay computes
// is decided by the recording, whichever way it is reached.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace tak::replaybar {

inline constexpr int kTicksPerSecond = 30;

// Playback speeds. The table is the replay's own: a live game keeps retail's -9..+10.
inline constexpr std::array<float, 11> kSpeeds = {0.25f, 0.5f, 1, 2, 4, 8, 16, 32, 64, 128, 256};
inline constexpr int kNormalSpeed = 2;   // index of 1x
inline constexpr float kFullEffectsSpeed = 8;   // above this: no sound, display time held here

inline int stepSpeed(int index, int delta) {
    return std::clamp(index + delta, 0, int(kSpeeds.size()) - 1);
}
inline float speedAt(int index) { return kSpeeds[size_t(std::clamp(index, 0, int(kSpeeds.size()) - 1))]; }

// The wall-clock time one frame may spend simulating. Generous when the viewer has asked
// for speed over smoothness, and while a jump is running.
inline double frameBudgetMs(float speed, bool jumping) {
    return jumping || speed > kFullEffectsSpeed ? 40.0 : 14.0;
}

// "8X", "0.25X"; with the rate actually reached once it falls clearly short of the
// request ("64X (27X)", "1X (0.8X)"). `real` <= 0: not measured yet.
inline std::string speedText(float speed, float real) {
    const auto one = [](float value) {
        char text[24];
        if (value >= 1) std::snprintf(text, sizeof text, "%.0fX", double(value));
        else std::snprintf(text, sizeof text, "%gX", double(value));
        return std::string(text);
    };
    std::string out = one(speed);
    if (real > 0 && real < speed * 0.9f)
        out += " (" + one(real >= 1 ? std::round(real) : std::max(0.1f, std::round(real * 10) / 10)) + ")";
    return out;
}

// "3:30"; "1:03:30" from an hour on. `length`: the replay's, so that every time of a
// replay that passes an hour reads the same way ("0:03:30 / 18:00:34").
inline std::string timeText(size_t tick, size_t length = 0) {
    const size_t seconds = tick / kTicksPerSecond;
    char text[48];
    if (std::max(tick, length) / kTicksPerSecond >= 3600)
        std::snprintf(text, sizeof text, "%zu:%02zu:%02zu", seconds / 3600, seconds / 60 % 60, seconds % 60);
    else
        std::snprintf(text, sizeof text, "%zu:%02zu", seconds / 60, seconds % 60);
    return text;
}

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool hit(float px, float py) const { return px >= x && py >= y && px <= x + w && py <= y + h; }
};

// Row 1 is the timeline; row 2 holds << play >>, - speed +, then the time at the right.
struct Layout {
    Rect box, timeline, track;
    Rect back, play, forward, slower, speed, faster;
    float textPx = 1, timeRight = 0;
};

// The bar at the top centre of the map view: nothing else lives there, and a click
// target must not move when a selection brings up the build row or the panel.
inline Layout layout(float mapViewW, float uiScale, float top) {
    constexpr float kRowUnits = 430;   // what row 2 wants at scale 1, its time included
    Layout l;
    const float s = uiScale;
    const float w = std::min(760 * s, std::max(mapViewW - 32, 64.0f));
    const float pad = 8 * s, timelineH = 18 * s, trackH = 8 * s, rowGap = 4 * s;
    // Row 2 shrinks with a bar too narrow for it; the timeline's hit area never does.
    const float r = s * std::min(1.0f, w / (kRowUnits * s));
    const float rowH = 26 * r, button = 34 * r, step = 26 * r, label = 92 * r, gap = 4 * r;
    l.box = {(mapViewW - w) / 2, top, w, pad + timelineH + rowGap + rowH + pad};
    l.timeline = {l.box.x + pad, top + pad, w - 2 * pad, timelineH};
    l.track = {l.timeline.x, l.timeline.y + (timelineH - trackH) / 2, l.timeline.w, trackH};
    const float y = l.timeline.y + timelineH + rowGap;
    float x = l.box.x + pad;
    const auto place = [&](Rect& rect, float width, float after) {
        rect = {x, y, width, rowH};
        x += width + after;
    };
    place(l.back, button, gap);
    place(l.play, button, gap);
    place(l.forward, button, 12 * r);
    place(l.slower, step, gap);
    place(l.speed, label, gap);
    place(l.faster, step, 0);
    l.textPx = 1.5f * r;
    l.timeRight = l.box.x + w - pad;
    return l;
}

// The width row 2 leaves the time, right of the speed controls. A label wider than this
// (hours on a narrow bar) is drawn smaller, as a long speed label is.
inline float timeRoom(const Layout& l) {
    return std::max(0.0f, l.timeRight - (l.faster.x + l.faster.w) - 4 * l.textPx);
}

inline float tickToX(const Layout& l, size_t tick, size_t length) {
    const float fraction = length ? std::min(1.0f, float(double(tick) / double(length))) : 0;
    return l.track.x + l.track.w * fraction;
}
inline size_t xToTick(const Layout& l, float x, size_t length) {
    const float fraction = l.track.w > 0 ? std::clamp((x - l.track.x) / l.track.w, 0.0f, 1.0f) : 0;
    return size_t(std::llround(double(fraction) * double(length)));
}

// ---- keyframes ------------------------------------------------------------------

inline constexpr uint32_t kKeyframeTicks = 900;        // one every 30 s of game
inline constexpr uint32_t kStepTicks = 30 * kTicksPerSecond;      // << >> [ ]
inline constexpr uint32_t kLongStepTicks = 300 * kTicksPerSecond;   // Shift+[ Shift+]
inline constexpr uint32_t kSnapTicks = 15 * kTicksPerSecond;
inline constexpr uint64_t kKeyframeCeilingBytes = uint64_t(4) << 30;

// What keyframes may hold in all: a quarter of the machine, at most 4 GB.
inline uint64_t keyframeBudget(uint64_t systemBytes) {
    return std::min(systemBytes / 4, kKeyframeCeilingBytes);
}

// The index of the latest keyframe at or before `tick`. `ticks` ascends from 0.
inline size_t keyframeAt(const std::vector<uint32_t>& ticks, uint32_t tick) {
    const auto after = std::upper_bound(ticks.begin(), ticks.end(), tick);
    return after == ticks.begin() ? 0 : size_t(after - ticks.begin()) - 1;
}

// Where << lands: on a keyframe within kSnapTicks of the wanted tick when one is behind
// the playhead (the jump is then a copy, however heavy the game), else on the tick.
inline uint32_t snapRewind(const std::vector<uint32_t>& ticks, uint32_t want, uint32_t playhead) {
    uint32_t best = want, bestOff = kSnapTicks + 1;
    for (const uint32_t t : ticks) {
        if (t >= playhead) break;
        const uint32_t off = t > want ? t - want : want - t;
        if (off < bestOff) { bestOff = off; best = t; }
    }
    return best;
}

// Where a press of << or >> aims. It stacks on the jump in hand (`running`, the playhead
// if there is none), but >> never reaches further than a long step past the playhead: a
// heavy game runs a jump far slower than keys can ask for one. A running jump is not
// shortened, however far a timeline click sent it.
inline size_t nudgeTarget(size_t playhead, size_t running, int64_t ticks, size_t length) {
    const int64_t from = int64_t(running);
    const int64_t want = std::clamp(from + ticks, int64_t(0), int64_t(length));
    if (ticks <= 0) return size_t(want);
    return size_t(std::max(from, std::min(want, int64_t(playhead) + int64_t(kLongStepTicks))));
}

// Which keyframe to give up when the budget is full; ticks.size() when there is nothing to
// drop. The first (tick 0) and the last are never dropped.
//
// Only a keyframe whose loss leaves a hole of at most 2 * span / (n - 2) may go: `hole`
// is the distance between its two neighbours, `span` what the n keyframes cover. There
// always is one, as the n - 2 holes add up to less than 2 * span. So thinning never
// opens a gap wider than twice an even spread of the keyframes left, however long the
// replay and however few of them fit (thinGapBound): they stay spread over everything
// played, and a jump back to any time re-simulates at most that much. Among those that
// may go, the one with the smallest hole / age goes, `age` being its distance from the
// playhead: far from it they thin first, near it they stay dense, where << lands.
inline size_t thinChoice(const std::vector<uint32_t>& ticks, uint32_t playhead) {
    const size_t n = ticks.size();
    if (n < 3) return n;
    const uint64_t span = uint64_t(ticks.back()) - ticks.front();
    size_t pick = n;
    uint64_t pickHole = 0, pickAge = 1;
    for (size_t i = 1; i + 1 < n; ++i) {
        const uint64_t hole = uint64_t(ticks[i + 1]) - ticks[i - 1];
        if (hole * (n - 2) > 2 * span) continue;
        const uint64_t age = std::max<uint64_t>(1, ticks[i] > playhead ? ticks[i] - playhead : playhead - ticks[i]);
        if (pick == n || hole * pickAge < pickHole * age) { pick = i; pickHole = hole; pickAge = age; }
    }
    return pick;
}

// The widest gap thinning leaves between neighbours of `kept` keyframes covering `span`
// ticks: twice their even spread. (A gap is never under one capture interval.)
inline uint64_t thinGapBound(uint64_t span, size_t kept) {
    return std::max<uint64_t>(kKeyframeTicks, 2 * span / std::max<size_t>(kept - std::min<size_t>(kept, 1), 1));
}

// Whether a gap between neighbours is wider than thinGapBound allows their number. A gap
// opened while fewer were kept can be: it was within twice the even spread of those, and
// it stays as wide when more fit again, since keyframes are only laid at the playhead.
inline bool thinTooWide(const std::vector<uint32_t>& ticks) {
    if (ticks.size() < 3) return false;
    const uint64_t bound = thinGapBound(uint64_t(ticks.back()) - ticks.front(), ticks.size());
    for (size_t i = 1; i < ticks.size(); ++i)
        if (uint64_t(ticks[i]) - ticks[i - 1] > bound) return true;
    return false;
}

// Room for a keyframe of `incoming` bytes at `tick` within `budget`: which of the held
// keyframes (`ticks` ascending from 0, their `bytes`) to give up, as indices into the
// lists as they stand, in the order thinChoice gives them up. `fits` false: there is no
// room to make, and then nothing is given up -- tick 0 never is, the one being laid is
// not, and when it is itself the one thinChoice would drop, laying it gains nothing.
// The caller lays no keyframe this time and asks again at the next; capture never stops
// for good. `incoming` 0 with `tick` held already: the pass after a keyframe that came
// out larger than the last.
//
// Keyframes go while they are over the budget, and while one of their gaps is too wide
// for their number (thinTooWide). So what a plan leaves, the one being laid among them,
// has no gap past thinGapBound, whatever the sizes and the budget did before. When more
// fit again (keyframes come out smaller, a budget grows), room alone does not raise
// their number: it rises as the replay's length brings the widest gap within the bound
// for one more.
struct ThinPlan {
    bool fits = true;
    std::vector<size_t> drop;
};
inline ThinPlan thinPlan(std::vector<uint32_t> ticks, const std::vector<uint64_t>& bytes, uint64_t budget,
                         uint64_t incoming, uint32_t tick, uint32_t playhead) {
    ThinPlan plan;
    uint64_t held = incoming;
    for (const uint64_t b : bytes) held += b;
    std::vector<size_t> index(ticks.size());   // each entry's place in the caller's lists
    for (size_t i = 0; i < index.size(); ++i) index[i] = i;
    constexpr size_t kLaid = ~size_t(0);
    const auto at = std::lower_bound(ticks.begin(), ticks.end(), tick);
    if (at == ticks.end() || *at != tick) {
        index.insert(index.begin() + (at - ticks.begin()), kLaid);
        ticks.insert(at, tick);
    }
    while (held > budget || thinTooWide(ticks)) {
        const size_t drop = thinChoice(ticks, playhead);
        if (drop >= ticks.size() || index[drop] == kLaid) return {false, {}};
        plan.drop.push_back(index[drop]);
        held -= bytes[index[drop]];
        ticks.erase(ticks.begin() + std::ptrdiff_t(drop));
        index.erase(index.begin() + std::ptrdiff_t(drop));
    }
    return plan;
}

// The first recorded check still ahead of a playhead standing at `tick`: a check taken
// on tick T stands for the state after T + 1 ticks, the moment playback compares it.
template <class Check>
size_t checkAfter(const std::vector<Check>& checks, size_t tick) {
    return size_t(std::upper_bound(checks.begin(), checks.end(), tick, [](size_t at, const Check& c) {
        return at < size_t(c.tick) + 1;
    }) - checks.begin());
}

}  // namespace tak::replaybar
