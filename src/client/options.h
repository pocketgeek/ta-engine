#pragma once

// A self-contained Options overlay, hosted by BOTH the main menu (MainMenu) and the
// in-game Esc menu (GameView). It edits a tak::Settings live and calls onChange()
// after every change so the host can apply it (audio / window / camera), and onSave()
// once each change is finished so the host writes the settings file: there is no SAVE
// button. It owns no game state; volumes/speeds/scale are all local display prefs
// (never hashed).

#include <SDL.h>

#include <algorithm>
#include <functional>
#include <future>
#include <map>
#include <string>
#include <vector>

#include "client/datafolderpick.h"
#include "client/musicfile.h"
#include "client/promptbox.h"
#include "client/settings.h"
#include "client/uitext.h"

namespace tak {

// The GAME DATA FOLDER row (client/datafolder.h). The host says which folder this
// launch mounted and whether the setting may be changed from where Options is open.
struct DataFolderRow {
    std::string inUse;     // the install this process mounted
    std::string locked;    // why it cannot be changed here ("" = it can): in a game or a
                           // lobby, or a --data / launch-folder override that would win again
    DataFolderPicker::Picker picker;   // empty = the native dialog (tests substitute one)
};

// The process-wide output-channel count (1/2/4/6/8) of the chosen device (or the
// system default), probed once and cached. Shared by SoundBank (which requests it) and
// the Options per-speaker sliders, so they always agree.
int detectOutputChannels();

// Snapshot the output devices' honest channel layouts. MUST be called once at startup,
// BEFORE any audio stream (menu music, door video, SoundBank) opens -- see main() -- so
// the 5.1 default sink isn't already collapsed to stereo when we read it. Idempotent.
void initAudioCaps();

// --- Output device selection -------------------------------------------------------
// Names of the current output (iscapture=0) or capture (iscapture=1) devices, for the
// pickers; "System Default" is implicit and not in the list.
std::vector<std::string> listAudioDevices(int iscapture = 0);
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

// Which screen an OptionsScreen shows: GRAPHICS, USER INTERFACE or GAME (the settings
// that used to be one OPTIONS screen; see build()), VOICE CHAT or AUDIO. Each is its own
// page in the same overlay, with its own DEFAULTS.
enum class OptionsPage { Graphics, Interface, Game, Voice, Audio };

// What the VOICE CHAT page reads from the host's voice engine (client/voice.h), so the
// screen itself needs no engine. Every hook is optional.
struct VoicePageHooks {
    std::function<float()> micLevel;          // 0..1, for the MIC LEVEL meter
    std::function<std::string()> micNote;     // device status beside MICROPHONE ("" = fine)
    std::function<std::string()> outNote;     // ... and beside VOICE OUTPUT
    std::function<void(bool)> monitor;        // the page opened (true) / closed (false)
};

class OptionsScreen {
public:
    // ren: the app renderer. s: the settings to edit (mutated in place). onChange:
    // invoked after any value change so the host applies it live. onSave: persist to
    // disk, invoked once per finished change -- at once for a toggle, a dropdown pick or
    // a confirmed DEFAULTS; when a slider is let go (not per drag step); when the player
    // name loses focus (Enter, Tab, Esc, a click elsewhere, or the screen closing).
    // audioChannels: number of output channels
    // to expose per-channel sliders for (0 = auto-detect the default device).
    // onAudioDevice: invoked when the user picks a DIFFERENT output device, so the host can
    // re-open its audio streams (SoundBank / menu music) on it live -- no restart. The
    // speaker-slider row set is rebuilt for the new channel count automatically.
    // dataFolder: the GAME DATA FOLDER row. A valid pick is written to the settings file
    // at once (saveDataFolderSetting) and offers RESTART NOW; see restartRequested().
    OptionsScreen(SDL_Renderer* ren, Settings& s, std::function<void()> onChange,
                  std::function<void()> onSave, int audioChannels = 0,
                  std::function<void()> onHotkeys = {}, std::function<void()> onAudioDevice = {},
                  DataFolderRow dataFolder = {}, OptionsPage page = OptionsPage::Game);

    // The VOICE CHAT page. Its rows edit only the voice fields, and every one of them but
    // the VOICE CHAT switch is greyed out and ignores input while voice chat is off.
    // onHotkeys: the PUSH TO TALK row opens the CONTROLS screen, where the key is bound.
    OptionsScreen(SDL_Renderer* ren, Settings& s, std::function<void()> onChange,
                  std::function<void()> onSave, VoicePageHooks voice,
                  std::function<void()> onHotkeys = {});

    // The AUDIO page: the sound device, volumes and speakers, and CUSTOM MUSIC -- a file of
    // the player's own for any original track (musicSides: each side's tracks, as the
    // install lists them; tak::music::sideTracks).
    OptionsScreen(SDL_Renderer* ren, Settings& s, std::function<void()> onChange,
                  std::function<void()> onSave, std::vector<music::SideTracks> musicSides,
                  int audioChannels = 0, std::function<void()> onAudioDevice = {});

    ~OptionsScreen() {
        flushPending();   // a name being typed, or a slider still held, when the host closes it
        if (voice_.monitor) voice_.monitor(false);
    }

    OptionsPage page() const { return page_; }
    // A text field here has keyboard focus (the player name): push to talk stays off.
    bool editingText() const { return nameEditing_; }
    // For the UI tests: whether the row labelled `label` currently takes input.
    bool rowEnabled(const std::string& label) const {
        for (auto& c : ctls_) if (c.label == label && c.kind != Control::Section) return !c.enabled || c.enabled();
        return false;
    }

    // Feed one SDL event. Returns true when the user leaves (Esc / BACK); the host
    // should then stop showing the screen. Everything is saved by then (see onSave).
    bool input(const SDL_Event& e, int winW, int winH);

    // Draw the overlay (call after the host's own frame so it sits on top).
    void render(int winW, int winH);

    // True once the player chose RESTART NOW after changing the data folder; input()
    // returned true for it. The host then leaves its loop and relaunches the game.
    bool restartRequested() const { return restartRequested_; }

    // For the in-process UI tests: a row's rectangle as last laid out (nullptr if no row
    // has that label), the data-folder row's status line, the footer's DEFAULTS button,
    // and the restart and DEFAULTS prompts' buttons (empty rects while they are closed).
    const SDL_FRect* rowRect(const std::string& label) const {
        for (auto& c : ctls_) if (c.label == label && c.kind != Control::Section) return &c.row;
        return nullptr;
    }
    // Option `index` of the open dropdown, where a click selects it (empty when closed).
    SDL_FRect dropItemRect(int index) {
        if (openDrop_ < 0 || openDrop_ >= int(ctls_.size())) return {};
        const Control& c = ctls_[size_t(openDrop_)];
        const int n = c.options ? int(c.options().size()) : 0;
        float y0, itemH, viewH;
        dropViewport(c, n, y0, itemH, viewH);
        const float w = dropWidth(c);
        return {c.row.x + c.row.w - w, y0 + float(index) * itemH - dropScroll_, w, itemH};
    }
    // A banded slider's band at its value (Control::band; -1 without bands), a toggle as
    // it reads (ON = true), a row's hover help, and an Info row's lines as wrapped and drawn.
    int rowBand(const std::string& label) const {
        for (auto& c : ctls_) if (c.label == label && c.kind == Control::Slider) return c.band ? c.band(c.get()) : -1;
        return -1;
    }
    bool rowOn(const std::string& label) const {
        for (auto& c : ctls_) if (c.label == label && c.kind == Control::Toggle) return c.get() > 0.5f;
        return false;
    }
    std::vector<std::string> rowHelp(const std::string& label) const {
        for (auto& c : ctls_) if (c.label == label && c.help) return c.help();
        return {};
    }
    std::vector<std::string> rowLines(const std::string& label) const {
        for (auto& c : ctls_) if (c.label == label && c.kind == Control::Info) return infoLines(c, c.row.w);
        return {};
    }
    std::string folderStatus() const { return folderStatusText(); }
    bool folderPicking() const { return folderPick_.busy(); }
    SDL_FRect restartNowRect() const { return restartPrompt_.open ? restartPrompt_.first : SDL_FRect{}; }
    SDL_FRect restartLaterRect() const { return restartPrompt_.open ? restartPrompt_.second : SDL_FRect{}; }
    SDL_FRect defaultsRect() const { return defaultsRect_; }
    SDL_FRect defaultsYesRect() const { return defaultsPrompt_.open ? defaultsPrompt_.first : SDL_FRect{}; }
    SDL_FRect defaultsNoRect() const { return defaultsPrompt_.open ? defaultsPrompt_.second : SDL_FRect{}; }

private:
    struct Control {
        enum Kind { Section, Slider, Toggle, Button, Dropdown, PlayerName, Folder, Meter, MusicFile, Info } kind;
        std::string label;
        float lo = 0, hi = 1;
        std::function<float()> get;              // Dropdown: the selected option index
        std::function<void(float)> set;          // Dropdown: select option `index`
        std::function<std::string(float)> fmt;   // value -> display text
        std::function<void()> action;            // Button: click handler
        std::function<std::vector<std::string>()> options;   // Dropdown: the choices; Info: its lines
        std::function<std::string()> note;       // Dropdown: optional status beside the label;
                                                 // Button: the chip text (default "OPEN >")
        std::function<bool()> enabled;           // unset = always; false = greyed, ignores input
        std::function<std::vector<std::string>()> help;   // optional lines shown while hovered
        std::function<float()> mark;             // Meter: a threshold tick, 0..1 (unset or < 0 = none)
        std::function<int(float)> band;          // Slider: a value's colour band, 0 blue, 1 green, 2 yellow, 3 red;
                                                 // set, the track shows every band and the value's colours the handle and text
        SDL_FRect row{};                          // filled by layout()
        int track = 0;                            // MusicFile: the original track it replaces
    };

    bool handle(const SDL_Event& e, int winW, int winH);   // input() less the flush on leaving
    void build(int audioChannels);
    void buildVoice();                            // the VOICE CHAT page's rows
    void buildAudio();                            // the AUDIO page's rows
    void layout(int winW, int winH);
    void commit(Control& c, float mx);            // set a slider from a mouse x
    void persist() { if (onSave_) onSave_(); }    // write the settings file now
    // Finish what is still in progress -- the player name being typed, a slider still
    // held -- and save it if it changed. Called on leaving, and by the destructor.
    void flushPending();
    void endNameEdit();                           // the name field loses focus: save a change
    void endDrag();                               // the held slider is let go: save a change

    // Rebuild the control list (called deferred, outside the input/render loops) after a
    // device change so the SPEAKER sliders match the new device's channel count.
    void applyPendingRebuild();

    // Snapshot the output-device list into devSnapshot_ ([0] = "" = system default). Taken
    // ONCE at build and again each time the dropdown opens -- never per frame: SDL re-runs
    // detection on every SDL_GetNumAudioDevices() and can return a different order/count
    // each call, so scanning live made the open list churn ("keep re-sorting").
    void refreshDevices();

    // Width of dropdown `c`'s chip + pop-up list: as wide as the row allows (minus a
    // label column, widened for a label longer than ~12 block glyphs such as GPU MEMORY
    // LIMIT so the chip never covers it), clamped, so long device names aren't truncated.
    // One source of truth shared by the closed chip, the open list, and hit-testing so
    // they line up.
    // The VOICE CHAT page lines its two device chips and the level meter up on one column,
    // clear of its widest label (voiceLabelW_, measured by buildVoice()).
    float dropWidth(const Control& c) const {
        // Labels draw at 2.0u; keep the chip 18u clear of the label's measured width.
        float label = std::max(150 * u_, uiTextWidth(c.label, 2.0f * u_) + 18 * u_);
        if (page_ == OptionsPage::Voice) label = std::max(label, voiceLabelW_ * u_ + 24 * u_);
        return std::clamp(c.row.w - label, 220.0f * u_, 560.0f * u_);
    }

    // Geometry of the open dropdown `c`'s scrollable option list: its top `y0`, per-item
    // height `itemH`, and visible viewport height `viewH` (capped to the panel so a long
    // device list never runs past the footer). Also clamps dropScroll_ to the overflow,
    // so input hit-testing, the wheel handler, and render() all agree frame-to-frame.
    void dropViewport(const Control& c, int nOpts, float& y0, float& itemH, float& viewH);

    // True when every setting DEFAULTS would reset already equals its default value
    // (playerName / accountName / the remembered room sets are preserved by DEFAULTS,
    // so they're excluded).
    // "At defaults" and "what DEFAULTS does" are the same question, so they share one
    // definition -- see tak::preferenceDefaults. They used to be two hand-maintained
    // lists that kept drifting apart.
    bool atDefaults() const {
        return s_ == pageDefaults();
    }

    SDL_Renderer* ren_;
    Settings& s_;
    std::function<void()> onChange_;
    std::function<void()> onSave_;
    std::function<void()> onHotkeys_;   // opens the hotkey config screen (host-provided)
    std::function<void()> onAudioDevice_;   // host re-opens audio on a live device switch
    bool pendingRebuild_ = false;       // a device change asked for a deferred control rebuild
    std::vector<Control> ctls_;
    bool nameEditing_ = false, nameSelectAll_ = false;
    bool nameChanged_ = false;   // the name was edited since its field took focus (unsaved)
    void appendPlayerName(const std::string& text);
    float scroll_ = 0;      // content scroll offset (px)
    float contentH_ = 0;    // total laid-out content height
    int drag_ = -1;         // index of the slider being dragged, or -1
    float dragFrom_ = 0;    // its value when it was pressed (saved on release if it moved)
    float hoverX_ = -1, hoverY_ = -1;   // last pointer position (help text for the row under it)
    void drawHelp(const Control& c);    // the hovered row's help lines in a box beside it
    // An Info row's lines, each word-wrapped to `width` in the note size (a continuation
    // line is indented): small text that is always shown, under the row it is about.
    std::vector<std::string> infoLines(const Control& c, float width) const;

    // GAME DATA FOLDER (client/datafolder.h): the row, its picker, the outcome of the last
    // pick, and the RESTART NOW / LATER prompt a valid pick opens.
    DataFolderRow folder_;
    DataFolderPicker folderPick_;
    enum class FolderNote { None, NoPicker, Invalid, Saved, SaveFailed } folderNote_ = FolderNote::None;
    std::string folderNoteText_;        // Invalid: the reason; SaveFailed: the folder
    PromptBox restartPrompt_;           // RESTART NOW / LATER
    bool restartRequested_ = false;
    void clickFolder();                 // the row was clicked: start the picker
    void pollFolder();                  // fold in a finished pick (input() and render())
    std::string folderStatusText() const;
    void drawFolderRow(const Control& c);
    SDL_FRect folderButton(const Control& c) const;   // the row's CHANGE button: the only click target
    void drawRestartPrompt(int winW, int winH);
    bool restartPromptInput(const SDL_Event& e);   // true = leave Options (RESTART NOW)
    // CUSTOM MUSIC rows (AUDIO page): CHOOSE runs the file picker on a worker, which also
    // tries the file, so a file that will not play says so at once (musicNote_).
    std::vector<music::SideTracks> musicSides_;
    std::future<std::pair<std::string, std::string>> musicPick_;   // (path, why it will not play)
    int musicPickTrack_ = 0;
    std::map<int, std::string> musicNote_;   // track -> why its file will not play
    float musicLabelW_ = 0;                  // widest track label at 2.0 (per u): the file boxes line up
    void clickMusic(const Control& c, float mx, float my);
    void pollMusicPick();
    void drawMusicRow(const Control& c);
    SDL_FRect musicChooseButton(const Control& c) const;
    SDL_FRect musicClearButton(const Control& c) const;
    PromptBox defaultsPrompt_;          // DEFAULTS asks first: RESET TO DEFAULTS? YES / NO
    void drawDefaultsPrompt(int winW, int winH);
    void defaultsPromptInput(const SDL_Event& e);
    int openDrop_ = -1;     // index of the open Dropdown control (-1 = none)
    float dropScroll_ = 0;  // scroll offset (px) INSIDE the open dropdown's option list
    std::vector<std::string> devSnapshot_;   // output devices ([0]="" system default, rest A-Z)
    std::vector<std::string> capSnapshot_;   // capture devices, the same way (VOICE CHAT page)
    OptionsPage page_ = OptionsPage::Game;
    // This page's DEFAULTS: the settings with only its own fields reset (tak::*Defaults).
    Settings pageDefaults() const {
        switch (page_) {
            case OptionsPage::Graphics: return tak::graphicsDefaults(s_);
            case OptionsPage::Interface: return tak::uiDefaults(s_);
            case OptionsPage::Game: return tak::gameDefaults(s_);
            case OptionsPage::Voice: return tak::voiceDefaults(s_);
            case OptionsPage::Audio: return tak::audioDefaults(s_);
        }
        return s_;
    }
    VoicePageHooks voice_;
    int channels_ = 2;                  // the speaker count build() last laid out
    float voiceLabelW_ = 0;   // widest chip/meter row label on the VOICE CHAT page, at 2.0 (per u)
    float u_ = 1.0f;        // layout unit (scaled to window)
    SDL_FRect panel_{};     // the panel rect (for scroll clamping)
    SDL_FRect defaultsRect_{}, backRect_{};   // footer buttons (filled by layout())
};

}  // namespace tak
