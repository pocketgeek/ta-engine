#pragma once
// TotalA.exe's unit speech queue (the global object at 0x51e68c, made at
// 0x47edea: 0xa9 bytes, entries of 0x11 from +0, count +0x99, last spoken tick
// +0x9d, spacing +0xa1 = 30 ticks, 0x47ee07).
//
// A request (0x47fad0) is dropped while the game tick is below the event's
// next-allowed tick (0x47faf0) or an entry of the same event is queued
// (0x47fb0a). A full queue (8 entries, 0x47fb1a) first plays its LAST entry
// with the speech held back and the text allowed (0x47fb27: 0x47fd70(7, 0, 1))
// and drops it. The new entry goes before the first one of strictly lower
// priority (0x47fbac..0x47fbb8: equal priorities stay first come, first served).
//
// Once a frame (0x4999a6 -> 0x47f680) the head entry is played and removed: with
// its speech allowed only when the game tick has reached the last spoken tick
// plus the spacing (0x47f6bb), which then becomes the last spoken tick whether or
// not anything was heard (0x47f6d7); otherwise text only (0x47f6df). So at most
// one entry a frame and one voice a second; the rest show their lines.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ta {

struct SpeechEntry {
    int event = 0;           // the event table row (0x5086dc, stride 0x18)
    int priority = 0;        // the row's +0 byte, copied to entry +0x10
    int unitId = 0;          // entry +8
    std::string unitName;    // the text's "%s" (0x47ff69: the unit type's name)
    std::string soundClass;  // the unit type's sound class (0x47fd99: +0x20e)
    std::string text;        // the line (0x47f780's text, else the row's; empty: none)
};

struct SpeechQueue {
    static constexpr uint32_t kSpacingTicks = 30;   // +0xa1 (0x47ee07)
    static constexpr size_t kCapacity = 8;          // 0x47fb1a
    struct Turn {
        SpeechEntry entry;
        bool speak = false;   // 0x47fd70's speech argument (the text one is always 1)
    };

    std::vector<SpeechEntry> entries;
    uint32_t lastSpoken = 0;   // +0x9d, 0 at start (0x47ee01)

    bool queued(int event) const {
        for (const auto& entry : entries) if (entry.event == event) return true;
        return false;
    }
    // 0x47fad0, after the caller's next-allowed test. Returns the entry a full
    // queue played text-only to make room, if any.
    std::optional<Turn> request(SpeechEntry entry) {
        if (queued(entry.event)) return std::nullopt;
        std::optional<Turn> evicted;
        if (entries.size() == kCapacity) {
            evicted = Turn{entries.back(), false};
            entries.pop_back();
        }
        size_t at = 0;
        while (at < entries.size() && entries[at].priority >= entry.priority) ++at;
        entries.insert(entries.begin() + std::ptrdiff_t(at), std::move(entry));
        return evicted;
    }
    // 0x47f680: the head entry for this frame, if any.
    std::optional<Turn> next(uint32_t tick) {
        if (entries.empty()) return std::nullopt;
        Turn turn{std::move(entries.front()), tick >= lastSpoken + kSpacingTicks};
        entries.erase(entries.begin());
        if (turn.speak) lastSpoken = tick;
        return turn;
    }
    void clear() { entries.clear(); lastSpoken = 0; }
};

} // namespace ta
