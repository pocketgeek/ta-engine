#pragma once

// The front-end's button clicks, the SDL-free part (client/menusfx.h plays them).
//
// Every retail menu button names its own click in its .gui: the first ".wav" among
// the gadget's state strings (mainmenu.gui's doors skirmish.wav / single.wav /
// multi.wav, its Options menubutton.wav, bod.gui's ok.wav / cancel.wav, and the
// victory<race>.gui / defeat.gui plates' MainMenu cancel.wav and Proceed ok.wav).
// The shipped clicks are unsigned 8-bit mono 11025 Hz, so the level is applied
// around 128 (silence).

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include "gui/gui.h"

namespace tak::menuclick {

// The format every click is played in (the shipped WAVs' own; others are converted).
constexpr int kRate = 11025;

// The clicks retail's .gui files give each kind of button, for our own panels that
// have no .gui (the SETTINGS, LOAD REPLAY, BENCHMARK and server pages): a button
// that opens another page (options.gui Interface/Music/Sound/Visual, selectgame.gui
// HostGame), one that confirms (Ok, Join, Yes, LoadGame), one that backs out
// (Cancel, MainMenu, No) and an on/off box (visualoptions.gui's toggles).
inline constexpr const char* kForward = "menubutton.wav";
inline constexpr const char* kOk = "ok.wav";
inline constexpr const char* kBack = "cancel.wav";
inline constexpr const char* kToggle = "toggle.wav";

// A gadget's click sound, lowercased; "" when its states name none.
inline std::string soundOf(const gui::Gadget& g) {
    for (const auto& s : g.states) {
        if (s.size() < 4) continue;
        std::string v = s;
        for (char& c : v) c = char(std::tolower((unsigned char)c));
        if (v.compare(v.size() - 4, 4, ".wav") == 0) return v;
    }
    return {};
}

// The played PCM: the click at a UI level that sits above the (soft, ~45/128) menu
// music, under the Options MASTER and SOUND EFFECTS sliders (0..256 each, both full
// by default, which leaves the UI level as it always was).
inline std::vector<uint8_t> level(const std::vector<uint8_t>& u8, int master, int sfx) {
    master = std::clamp(master, 0, 256);
    sfx = std::clamp(sfx, 0, 256);
    std::vector<uint8_t> out(u8.size());
    for (size_t i = 0; i < u8.size(); ++i) {
        const int64_t d = int64_t(int(u8[i]) - 128) * 100 * master * sfx / (int64_t(128) * 256 * 256);
        out[i] = uint8_t(std::clamp<int64_t>(128 + d, 0, 255));
    }
    return out;
}

}  // namespace tak::menuclick
