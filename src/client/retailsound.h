#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace tak {
struct SoundVoice {
    bool active=false, looping=false;
    int priority=0;
    uint64_t started=0;
};

// Native 56ed40/56ea90: use a free voice, otherwise replace a non-looping
// voice with strictly lower priority. Lowest priority wins, then oldest start.
inline int retailSoundVoice(std::span<const SoundVoice> voices,int priority) {
    int victim=-1;
    for (std::size_t i=0;i<voices.size();++i) {
        const auto& voice=voices[i];
        if (!voice.active) return int(i);
        if (voice.looping || voice.priority>=priority) continue;
        if (victim<0 || voice.priority<voices[std::size_t(victim)].priority ||
            (voice.priority==voices[std::size_t(victim)].priority &&
             voice.started<voices[std::size_t(victim)].started)) victim=int(i);
    }
    return victim;
}

constexpr int retailSoundPriority(int32_t flags) { return uint32_t(flags)&7; }
// Native unit PLAY_SOUND host 50def0. Class 7 is global; classes 0/1 also
// require selection. The mixer enforces the free-voice gate for those classes.
constexpr bool retailUnitSoundAudible(int32_t flags,bool visible,bool selected) {
    const int priority=retailSoundPriority(flags);
    return priority==7 || (visible && (priority>1 || selected));
}
// Not a retail rule: a rejoining client or a spectator replays the game's
// history (ticks before the server-declared end, GameView::cmdReplayEnd_) as fast
// as it can. That history's death cries are dropped rather than played at
// fast-forward rate. Live play keeps every cry however late its frame comes,
// like the impact sounds.
constexpr bool unitSoundLive(uint32_t soundTick,uint32_t historyEnd) {
    return soundTick>=historyEnd;
}
} // namespace tak
