#pragma once

// A self-contained Options overlay, hosted by BOTH the main menu (MainMenu) and the
// in-game Esc menu (GameView). It edits a ta::Settings live and calls onChange()
// after every change so the host can apply it (audio / window / camera). It owns no
// game state; volumes/speeds/scale are all local display prefs (never hashed).

#include <SDL.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "client/settings.h"

namespace ta {

// The process-wide output-channel count (1/2/4/6/8) of the chosen device (or the
// system default), probed once and cached. Shared by SoundBank (which requests it) and
// the Options per-speaker sliders, so they always agree.
int detectOutputChannels();

// Snapshot the output devices' honest channel layouts. MUST be called once at startup,
// BEFORE any audio stream (menu music, door video, SoundBank) opens -- see main() -- so
// the 5.1 default sink isn't already collapsed to stereo when we read it. Idempotent.
void initAudioCaps();

// --- Output device selection -------------------------------------------------------
// Names of the current output devices (for the Options picker); [] entry is implicitly
// "System Default".
std::vector<std::string> listAudioDevices();
// Choose the output device by name ("" = system default). Validated: an absent name
// falls back to system default. Call once at startup from Settings::audioDevice, before
// any audio is opened.
void setAudioDevice(const std::string& name);
// The device currently in effect ("" = system default).
const std::string& currentAudioDevice();
// Open the chosen output device (or system default), falling back to system default --
// permanently -- if the chosen one is missing or won't open. ALL audio opens go
// through this so a stale saved device can never wedge sound. Mirrors
// SDL_OpenAudioDevice's (iscapture, want, got, allowed_changes) signature.
SDL_AudioDeviceID openAudioDevice(int iscapture, const SDL_AudioSpec* want,
                                  SDL_AudioSpec* got, int allowed);

class OptionsScreen {
public:
    // ren: the app renderer. s: the settings to edit (mutated in place). onChange:
    // invoked after any value change so the host applies it live. onSave: invoked when
    // the user clicks SAVE (persist to disk). audioChannels: number of output channels
    // to expose per-channel sliders for (0 = auto-detect the default device).
    // onAudioDevice: invoked when the user picks a DIFFERENT output device, so the host can
    // re-open its audio streams (SoundBank / menu music) on it live -- no restart. The
    // speaker-slider row set is rebuilt for the new channel count automatically.
    OptionsScreen(SDL_Renderer* ren, Settings& s, std::function<void()> onChange,
                  std::function<void()> onSave, int audioChannels = 0,
                  std::function<void()> onHotkeys = {}, std::function<void()> onAudioDevice = {});

    // Feed one SDL event. Returns true when the user leaves (Esc / BACK); the host
    // should then stop showing the screen. Persisting is explicit (the SAVE button
    // fires onSave); BACK does not save.
    bool input(const SDL_Event& e, int winW, int winH);

    // Draw the overlay (call after the host's own frame so it sits on top).
    void render(int winW, int winH);

private:
    struct Control {
        enum Kind { Section, Slider, Toggle, Button, Dropdown } kind;
        std::string label;
        float lo = 0, hi = 1;
        std::function<float()> get;              // Dropdown: the selected option index
        std::function<void(float)> set;          // Dropdown: select option `index`
        std::function<std::string(float)> fmt;   // value -> display text
        std::function<void()> action;            // Button: click handler
        std::function<std::vector<std::string>()> options;   // Dropdown: the choices
        SDL_FRect row{};                          // filled by layout()
    };

    void build(int audioChannels);
    void layout(int winW, int winH);
    void commit(Control& c, float mx);            // set a slider from a mouse x

    // Rebuild the control list (called deferred, outside the input/render loops) after a
    // device change so the SPEAKER sliders match the new device's channel count.
    void applyPendingRebuild();

    // Snapshot the output-device list into devSnapshot_ ([0] = "" = system default). Taken
    // ONCE at build and again each time the dropdown opens -- never per frame: SDL re-runs
    // detection on every SDL_GetNumAudioDevices() and can return a different order/count
    // each call, so scanning live made the open list churn ("keep re-sorting").
    void refreshDevices();

    // Width of dropdown `c`'s chip + pop-up list: as wide as the row allows (minus a
    // label column), clamped, so long device names aren't truncated. One source of truth
    // shared by the closed chip, the open list, and hit-testing so they line up.
    float dropWidth(const Control& c) const {
        return std::clamp(c.row.w - 150.0f * u_, 220.0f * u_, 560.0f * u_);
    }

    // Geometry of the open dropdown `c`'s scrollable option list: its top `y0`, per-item
    // height `itemH`, and visible viewport height `viewH` (capped to the panel so a long
    // device list never runs past the footer). Also clamps dropScroll_ to the overflow,
    // so input hit-testing, the wheel handler, and render() all agree frame-to-frame.
    void dropViewport(const Control& c, int nOpts, float& y0, float& itemH, float& viewH);

    // True when every setting DEFAULTS would reset already equals its default value
    // (playerName / accountName / lastMap are preserved by DEFAULTS, so they're
    // excluded).
    // "At defaults" and "what DEFAULTS does" are the same question, so they share one
    // definition -- see ta::preferenceDefaults. They used to be two hand-maintained
    // lists that kept drifting apart.
    bool atDefaults() const { return s_ == ta::preferenceDefaults(s_); }

    SDL_Renderer* ren_;
    Settings& s_;
    std::function<void()> onChange_;
    std::function<void()> onSave_;
    std::function<void()> onHotkeys_;   // opens the hotkey config screen (host-provided)
    std::function<void()> onAudioDevice_;   // host re-opens audio on a live device switch
    bool pendingRebuild_ = false;       // a device change asked for a deferred control rebuild
    std::vector<Control> ctls_;
    bool dirty_ = false;    // unsaved changes since the last save/open -> SAVE enabled
    float scroll_ = 0;      // content scroll offset (px)
    float contentH_ = 0;    // total laid-out content height
    int drag_ = -1;         // index of the slider being dragged, or -1
    int openDrop_ = -1;     // index of the open Dropdown control (-1 = none)
    float dropScroll_ = 0;  // scroll offset (px) INSIDE the open dropdown's option list
    std::vector<std::string> devSnapshot_;   // output devices ([0]="" system default, rest A-Z)
    float u_ = 1.0f;        // layout unit (scaled to window)
    SDL_FRect panel_{};     // the panel rect (for scroll clamping)
    SDL_FRect defaultsRect_{}, saveRect_{}, backRect_{};   // footer buttons (filled by layout())
};

}  // namespace ta
