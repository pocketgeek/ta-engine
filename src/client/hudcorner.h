#pragma once

// The map view's top-left corner. Several overlays want it: the stream status banner
// (client/streaming.cpp, drawn after the stream's capture so it never reaches the
// broadcast), the legacy mana readout, the GAME TIME clock, the F4 scoreboard, the
// mission objectives panel and the voice-chat speakers. Each used to claim the corner
// at a fixed position, so any two shown together overlapped (the banner covered the
// scoreboard's header). Now they share one Column: each overlay asks for the top it
// always had and gets the first free y at or below it, in draw order. With nothing
// above an overlay its position is exactly what it was. Display only: nothing here
// touches the simulation or the network.

#include <algorithm>

namespace tak::hudcorner {

// Clear space between two stacked overlays.
inline constexpr float kGap = 4.0f;

struct Rect { float x = 0, y = 0, w = 0, h = 0; };

// The stream status banner ("STREAM LIVE - ... - F9"): a fixed 30 px box at (10, 10),
// as wide as its text plus padding but never wider than the window.
inline Rect streamBanner(float textW, int winW) {
    return {10, 10, std::max(0.0f, std::min(float(winW - 20), textW + 20)), 30};
}

class Column {
public:
    // `reserved`: the first y no overlay may use (the stream banner's bottom plus kGap).
    explicit Column(float reserved = 0) : next_(reserved) {}
    // An overlay whose box wants its top at `want` and is `height` tall: where its top goes.
    float place(float want, float height) {
        const float top = std::max(want, next_);
        next_ = top + height + kGap;
        return top;
    }
    float next() const { return next_; }
private:
    float next_;
};

// The F4 scoreboard: Retail's table, (484 + 64 if teams) x scale wide, a header row
// plus one 32 x scale row per player, 12 x scale in from the corner. It keeps the
// requested scale and shrinks only when the complete table cannot fit -- across
// `areaW` (the map view's width, a 12 x scale margin each side), or between its top
// and `floor` (the information bar's top). `minTop` is the column's next free y; the
// table goes below it.
struct Board { float scale = 1, top = 0; };
inline Board scoreboard(float requested, bool teams, int players, float areaW, float floor, float minTop) {
    const float tableW = 484.0f + (teams ? 64.0f : 0.0f);
    const float rowsH = 32.0f * float(players + 1);
    floor = std::max(1.0f, floor);
    float scale = std::max(0.1f, std::min({requested, areaW / (tableW + 24.0f), floor / (24.0f + rowsH)}));
    float top = std::max(12 * scale, minTop);
    if (top + (rowsH + 12) * scale > floor) {   // pushed down: fit what is left
        scale = std::max(0.1f, (floor - top) / (rowsH + 12));
        top = std::max(12 * scale, minTop);
    }
    return {scale, top};
}

}  // namespace tak::hudcorner
