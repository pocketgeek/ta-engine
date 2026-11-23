#pragma once

// Push-to-talk (and, below, voice activation) as a pure state machine (no SDL), so the rules are tested directly.
// The host feeds it the presses and releases of the push-to-talk chord, and every frame
// whether talking is allowed at all, whether a text field has keyboard focus, whether
// the window has it, and whether the key is still physically down.
//
//  - A press starts talking only when talking is allowed and nothing is being typed:
//    a Space typed into the chat box, a lobby field or a name entry is text, not voice,
//    and the press is NOT consumed so the field gets it.
//  - A held key never leaks: the moment a text field takes focus, the window loses
//    focus, talking stops being allowed (left the room, voice switched off, the CONTROLS
//    screen is capturing a key) or the key is found up without our having seen the
//    release, the transmission ends. It restarts only on a fresh press.

namespace tak::voice {

class PushToTalk {
public:
    // A key-down of the chord. Returns true when consumed (talking, or a repeat while
    // talking); false leaves the event to whoever else wants it.
    bool press(bool repeat, bool allowed, bool typing) {
        if (repeat) return talking_;
        if (!allowed || typing) return false;
        talking_ = true;
        return true;
    }
    // A key-up of the chord's key. Returns true when it ended a transmission.
    bool release() {
        const bool was = talking_;
        talking_ = false;
        return was;
    }
    void frame(bool allowed, bool typing, bool focused, bool keyDown) {
        if (talking_ && (!allowed || typing || !focused || !keyDown)) talking_ = false;
    }
    bool talking() const { return talking_; }

private:
    bool talking_ = false;
};

// Voice activation (TALK MODE), as a pure state machine fed one 20 ms frame's peak at a
// time. A frame at or above the threshold opens the gate; it stays open kHoldFrames after
// the last such frame, so the pauses between words do not chop a sentence into pieces.
// The engine sends kPreFrames of the audio from just before the gate opened, so the first
// syllable is not clipped.
class VoiceGate {
public:
    static constexpr int kHoldFrames = 20;   // 400 ms
    static constexpr int kPreFrames = 2;     // 40 ms
    // One frame's peak (0..1, after MIC GAIN). Returns whether this frame is sent.
    bool frame(float peak, float threshold) {
        if (peak >= threshold) hold_ = kHoldFrames;
        else if (hold_ > 0) --hold_;
        return hold_ > 0;
    }
    void reset() { hold_ = 0; }
    bool open() const { return hold_ > 0; }

private:
    int hold_ = 0;
};

}  // namespace tak::voice
