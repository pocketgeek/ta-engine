#include "client/options.h"

#include "client/blockfont.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace tak {

namespace {

// The user-chosen output device NAME ("" = system default). Set once at startup from
// Settings::audioDevice (validated), and cleared back to system default if a chosen
// device turns out to be missing or fails to open. Process-global so SoundBank and the
// menu/briefing audio all open the same device.
std::string g_audioDevice;
int g_channelCache = -1;   // detectOutputChannels() memo; -1 = recompute

// HONEST channel-layout snapshot, captured ONCE at startup before ANY audio stream opens
// (see initAudioCaps). We cannot query this live: PipeWire/PulseAudio collapse a 5.1
// sink's ADVERTISED layout to stereo while a stereo stream (e.g. the menu music) is
// playing on it, so SDL_GetDefaultAudioInfo/SDL_GetAudioDeviceSpec report 2 mid-menu even
// for a real 5.1 device. The startup snapshot (no streams yet) is the only moment they
// tell the truth, so detectOutputChannels reads from here, not from a live query.
std::map<std::string, int> g_devCaps;   // device name -> advertised channels (1..8)
int g_defaultCaps = 0;                  // system-default advertised channels (0 = unknown)
bool g_capsReady = false;

// Index of an output device by name (-1 if absent), for SDL_GetAudioDeviceSpec.
int deviceIndex(const char* name) {
    if (!name) return -1;
    int n = SDL_GetNumAudioDevices(0);
    for (int i = 0; i < n; ++i)
        if (const char* dn = SDL_GetAudioDeviceName(i, 0); dn && !std::strcmp(dn, name)) return i;
    return -1;
}

// Speaker label per channel count (mirrors SoundBank::channelRole / channelGains).
const char* role(int count, int i) {
    switch (count) {
        case 2: { static const char* r[] = {"LEFT", "RIGHT"}; return i < 2 ? r[i] : ""; }
        case 4: { static const char* r[] = {"FRONT L", "FRONT R", "REAR L", "REAR R"}; return i < 4 ? r[i] : ""; }
        case 6: { static const char* r[] = {"FRONT L", "FRONT R", "CENTER", "SUB", "REAR L", "REAR R"}; return i < 6 ? r[i] : ""; }
        case 8: { static const char* r[] = {"FRONT L", "FRONT R", "CENTER", "SUB", "REAR L", "REAR R", "SIDE L", "SIDE R"}; return i < 8 ? r[i] : ""; }
        default: return "MONO";
    }
}

std::string pctOf(float v, float full) { return std::to_string(int(v * 100 / full + 0.5f)) + "%"; }
std::string timesFmt(float v) { char b[16]; std::snprintf(b, sizeof b, "%.2fX", v); return b; }

}  // namespace

// Snapshot every output device's HONEST channel layout, ONCE, before any stream opens.
// MUST be called at startup (see main()) ahead of the menu music / door videos -- once a
// stereo stream is live on the default 5.1 sink, PipeWire reports it as 2-channel and the
// snapshot would be wrong. Idempotent.
void initAudioCaps() {
    if (g_capsReady) return;
    g_capsReady = true;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return;
    SDL_AudioSpec spec{};
    if (SDL_GetDefaultAudioInfo(nullptr, &spec, 0) == 0 && spec.channels >= 1)
        g_defaultCaps = std::clamp(int(spec.channels), 1, 8);
    int n = SDL_GetNumAudioDevices(0);
    for (int i = 0; i < n; ++i)
        if (const char* dn = SDL_GetAudioDeviceName(i, 0)) {
            SDL_AudioSpec ds{};
            if (SDL_GetAudioDeviceSpec(i, 0, &ds) == 0 && ds.channels >= 1)
                g_devCaps[dn] = std::clamp(int(ds.channels), 1, 8);
        }
    std::fprintf(stderr, "audio: capability snapshot -- default=%d, %d device(s)\n",
                 g_defaultCaps, int(g_devCaps.size()));
}

// The one true output-channel count for this process (of the chosen device, or the
// system default), so SoundBank and the Options per-speaker sliders always agree.
// Computed once and cached; recomputed after a device change. Reads the startup snapshot
// (g_devCaps / g_defaultCaps) rather than a live query, which under-reports surround
// while the menu music holds the sink in stereo.
int detectOutputChannels() {
    if (g_channelCache >= 0) return g_channelCache;
    initAudioCaps();   // no-op if already snapshotted at startup (the normal path)
    int adv = 0;
    if (g_audioDevice.empty()) {
        adv = g_defaultCaps;
    } else if (auto it = g_devCaps.find(g_audioDevice); it != g_devCaps.end()) {
        adv = it->second;
    } else if (int idx = deviceIndex(g_audioDevice.c_str()); idx >= 0) {
        // Chosen device wasn't in the startup snapshot (hot-plugged since) -- best-effort
        // live spec; may read low if a stereo stream is currently on it.
        SDL_AudioSpec spec{};
        if (SDL_GetAudioDeviceSpec(idx, 0, &spec) == 0 && spec.channels >= 1)
            adv = std::clamp(int(spec.channels), 1, 8);
    }
    if (adv <= 0) adv = 2;   // unknown -> safe stereo (never fabricate surround)
    g_channelCache = std::clamp(adv, 1, 8);
    std::fprintf(stderr, "audio: %d output channels on %s\n", g_channelCache,
                 g_audioDevice.empty() ? "system default" : g_audioDevice.c_str());
    return g_channelCache;
}

std::vector<std::string> listAudioDevices() {
    std::vector<std::string> out;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return out;
    int n = SDL_GetNumAudioDevices(0);
    for (int i = 0; i < n; ++i)
        if (const char* dn = SDL_GetAudioDeviceName(i, 0)) out.emplace_back(dn);
    return out;
}

void setAudioDevice(const std::string& name) {
    // Ensure the audio subsystem is up (and the device snapshot taken) BEFORE validating:
    // at startup this runs right after SDL_Init(VIDEO), so without this SDL_GetNumAudioDevices
    // would see zero devices and EVERY saved device would falsely read as "not present".
    initAudioCaps();
    std::string prev = g_audioDevice;
    g_audioDevice.clear();
    if (!name.empty()) {
        if (deviceIndex(name.c_str()) >= 0) g_audioDevice = name;
        else std::fprintf(stderr, "audio: chosen device '%s' not present -- using system default\n",
                          name.c_str());
    }
    if (g_audioDevice != prev) g_channelCache = -1;  // recompute on next detectOutputChannels
}

const std::string& currentAudioDevice() { return g_audioDevice; }

SDL_AudioDeviceID openAudioDevice(int iscapture, const SDL_AudioSpec* want,
                                  SDL_AudioSpec* got, int allowed) {
    if (!g_audioDevice.empty()) {
        SDL_AudioDeviceID d = SDL_OpenAudioDevice(g_audioDevice.c_str(), iscapture, want, got, allowed);
        if (d) return d;
        // Missing / inaccessible at open time -> permanently fall back to system default.
        std::fprintf(stderr, "audio: device '%s' failed to open -- falling back to system default\n",
                     g_audioDevice.c_str());
        g_audioDevice.clear();
        g_channelCache = -1;
    }
    return SDL_OpenAudioDevice(nullptr, iscapture, want, got, allowed);
}

OptionsScreen::OptionsScreen(SDL_Renderer* ren, Settings& s, std::function<void()> onChange,
                             std::function<void()> onSave, int audioChannels,
                             std::function<void()> onHotkeys, std::function<void()> onAudioDevice)
    : ren_(ren), s_(s), onChange_(std::move(onChange)), onSave_(std::move(onSave)),
      onHotkeys_(std::move(onHotkeys)), onAudioDevice_(std::move(onAudioDevice)) {
    build(audioChannels > 0 ? audioChannels : detectOutputChannels());
}

// Rebuild the speaker sliders (etc.) after a device change. Deferred to input()/render()
// so it never runs while a caller still holds a reference into ctls_.
void OptionsScreen::applyPendingRebuild() {
    if (!pendingRebuild_) return;
    pendingRebuild_ = false;
    openDrop_ = -1; drag_ = -1; dropScroll_ = 0;   // transient control state -> invalid after rebuild
    build(detectOutputChannels());
}

void OptionsScreen::build(int channels) {
    ctls_.clear();   // rebuildable: a live device switch re-runs this with a new channel count
    std::fprintf(stderr, "Options: %d speaker sliders\n", channels);
    auto section = [&](const char* label) { ctls_.push_back({Control::Section, label, 0, 0, {}, {}, {}, {}, {}}); };
    auto slider = [&](const char* label, float lo, float hi, std::function<float()> get,
                      std::function<void(float)> set, std::function<std::string(float)> fmt) {
        ctls_.push_back({Control::Slider, label, lo, hi, std::move(get), std::move(set), std::move(fmt), {}, {}});
    };
    auto toggle = [&](const char* label, std::function<float()> get, std::function<void(float)> set) {
        ctls_.push_back({Control::Toggle, label, 0, 1, std::move(get), std::move(set), {}, {}, {}});
    };
    // (The Options screen no longer hosts a button row -- hotkey config moved to the
    // SETTINGS menu; Control::Button still exists for future use, just unused here.)
    // A dropdown: `options()` lists the display choices, get() is the selected index,
    // set(index) applies the choice. Rendered closed as a chip + arrow; clicking opens
    // a pop-up list.
    auto dropdown = [&](const char* label, std::function<std::vector<std::string>()> options,
                        std::function<float()> get, std::function<void(float)> set) {
        ctls_.push_back({Control::Dropdown, label, 0, 1, std::move(get), std::move(set),
                         {}, {}, std::move(options)});
    };

    section("AUDIO");
    // Output device: "System Default" plus every current output device. On startup a
    // saved-but-missing device auto-falls-back to system (see setAudioDevice); here the
    // user picks one from the list.
    refreshDevices();                                // one A-Z snapshot; re-taken on each open
    auto deviceValues = [this] { return devSnapshot_; };   // stored values ("" = system default)
    dropdown("SOUND DEVICE",
        [deviceValues] {                             // display labels
            std::vector<std::string> labels;
            for (auto& v : deviceValues()) labels.push_back(v.empty() ? "System Default" : v);
            return labels;
        },
        [this, deviceValues] {                       // selected index (0 = system default)
            auto v = deviceValues();
            for (size_t k = 0; k < v.size(); ++k) if (v[k] == s_.audioDevice) return float(k);
            return 0.0f;
        },
        [this, deviceValues](float idx) {            // apply
            auto v = deviceValues();
            size_t i = size_t(idx < 0 ? 0 : idx);
            std::string chosen = i < v.size() ? v[i] : std::string();
            if (chosen == s_.audioDevice) return;    // same device -> nothing to switch
            s_.audioDevice = chosen;
            setAudioDevice(s_.audioDevice);          // resets the channel-count cache
            pendingRebuild_ = true;                  // re-lay the SPEAKER sliders for the new count
            if (onAudioDevice_) onAudioDevice_();     // host re-opens its streams on the new device
            if (onChange_) onChange_();               // re-apply volumes to the re-opened device
        });
    slider("MASTER VOLUME", 0, 256, [&] { return float(s_.masterVol); },
           [&](float v) { s_.masterVol = int(v + 0.5f); }, [](float v) { return pctOf(v, 256); });
    slider("MUSIC VOLUME", 0, 256, [&] { return float(s_.bgmVol); },
           [&](float v) { s_.bgmVol = int(v + 0.5f); }, [](float v) { return pctOf(v, 256); });
    slider("SOUND EFFECTS", 0, 256, [&] { return float(s_.sfxVol); },
           [&](float v) { s_.sfxVol = int(v + 0.5f); }, [](float v) { return pctOf(v, 256); });
    for (int i = 0; i < channels && i < 8; ++i) {
        std::string label = std::string("SPEAKER ") + role(channels, i);
        slider(label.c_str(), 0, 1, [this, i] { return s_.chanGain[i]; },
               [this, i](float v) { s_.chanGain[i] = v; }, [](float v) { return pctOf(v, 1); });
    }

    // DISPLAY: the window / output device itself.
    section("DISPLAY");
    toggle("FULLSCREEN", [&] { return s_.fullscreen ? 1.0f : 0.0f; },
           [&](float v) { s_.fullscreen = v > 0.5f; });
    toggle("VSYNC", [&] { return s_.vsync ? 1.0f : 0.0f; }, [&](float v) { s_.vsync = v > 0.5f; });
    slider("MAX FPS", 30, 240, [&] { return float(s_.maxFps); },
           [&](float v) { s_.maxFps = int(v + 0.5f); }, [](float v) { return std::to_string(int(v + 0.5f)); });

    // GRAPHICS: how the scene is rendered (quality / performance trade-offs).
    section("GRAPHICS");
    // Supersampling AA. The 4x level was always implemented (main.cpp picks a 2.0x
    // linear scale for it, against 1.4142x for 2x) but the row here was a TOGGLE that
    // could only ever write 0 or 2, so it was unreachable without hand-editing the ini.
    // Measured at 7680x2160 with ~1200 units: off 60 fps, 2x 57, 4x 57 -- the frame is
    // geometry-bound, not fill-bound, so the bigger target is close to free. It is not
    // free in VRAM (a 14336x4033 target is ~231 MB against ~133 MB for 2x), which is why
    // it stays opt-in rather than becoming the default.
    slider("ANTI-ALIASING", 0, 2, [&] { return float(s_.antiAlias >= 4 ? 2 : s_.antiAlias >= 2 ? 1 : 0); },
           [&](float v) { const int l = std::clamp(int(v + 0.5f), 0, 2);
                          s_.antiAlias = l >= 2 ? 4 : l >= 1 ? 2 : 0; },
           [](float v) { int l = int(v + 0.5f);
                         return std::string(l >= 2 ? "4X" : l >= 1 ? "2X" : "OFF"); });
    // Retail's video option: smooth terrain + feature scaling (off = crisp pixels).
    toggle("BILINEAR FILTERING", [&] { return s_.bilinear ? 1.0f : 0.0f; },
           [&](float v) { s_.bilinear = v > 0.5f; });
    // Projected unit shadows. Retail's Glide path casts these, so ON is the faithful
    // setting -- but it is the largest single cost in a crowded frame (measured ~3.2ms
    // of a ~12ms draw at ~1180 visible units), which is worth a switch on a slow machine.
    toggle("UNIT SHADOWS", [&] { return s_.unitShadows ? 1.0f : 0.0f; },
           [&](float v) { s_.unitShadows = v > 0.5f; });
    // Edge-directed 2x upscale of the static art, done when it is BUILT -- so it costs
    // load time and VRAM and nothing per frame. Art already loaded keeps whatever it was
    // built with, hence the caption.
    toggle("SMOOTH GUI ART (RESTART)", [&] { return s_.smoothArt ? 1.0f : 0.0f; },
           [&](float v) { s_.smoothArt = v > 0.5f; });
    toggle("TREES SWAY IN WIND", [&] { return s_.treeSway ? 1.0f : 0.0f; },
           [&](float v) { s_.treeSway = v > 0.5f; });

    // INTERFACE: on-screen HUD -- its size, and the game overlays.
    section("INTERFACE");
    slider("UI SCALE", 0.75f, 2.0f, [&] { return s_.uiScale; },
           [&](float v) { s_.uiScale = v; }, [](float v) { return pctOf(v, 1); });
    slider("HEALTH BARS", 0, 2, [&] { return float(s_.healthBars); },
           [&](float v) { s_.healthBars = std::clamp(int(v + 0.5f), 0, 2); },
           [](float v) { int l = int(v + 0.5f); return std::string(l >= 2 ? "ALWAYS" : l >= 1 ? "DAMAGED" : "OFF"); });
    // Where the in-game conjure/build icon row sits along the bottom of the screen.
    slider("BUILD MENU", 0, 2, [&] { return float(s_.buildBarAlign); },
           [&](float v) { s_.buildBarAlign = std::clamp(int(v + 0.5f), 0, 2); },
           [](float v) { int l = int(v + 0.5f); return std::string(l >= 2 ? "RIGHT" : l >= 1 ? "CENTER" : "LEFT"); });
    // Extra scale for the build icon row, applied ON TOP of UI SCALE (same range).
    slider("BUILD MENU SCALE", 0.75f, 2.0f, [&] { return s_.buildBarScale; },
           [&](float v) { s_.buildBarScale = v; }, [](float v) { return pctOf(v, 1); });

    section("CAMERA");
    slider("MOUSE ZOOM SPEED", 0.25f, 4.0f, [&] { return s_.mouseZoomSpeed; },
           [&](float v) { s_.mouseZoomSpeed = v; }, [](float v) { return timesFmt(v); });
    toggle("EDGE SCROLLING", [&] { return s_.edgeScroll ? 1.0f : 0.0f; },
           [&](float v) { s_.edgeScroll = v > 0.5f; });
    slider("EDGE SCROLL SPEED", 0.25f, 4.0f, [&] { return s_.edgeScrollSpeed; },
           [&](float v) { s_.edgeScrollSpeed = v; }, [](float v) { return timesFmt(v); });
    // Mouse-cursor size as an 8-stop slider: 1X..8X.
    slider("CURSOR SIZE", 1, 8, [&] { return float(s_.cursorScale); },
           [&](float v) { s_.cursorScale = int(v + 0.5f); },
           [](float v) { return std::to_string(int(v + 0.5f)) + "X"; });
    // OS-tracked cursor: the pointer keeps moving smoothly even when a heavy frame
    // stalls the render loop (the retail Direct3D "hardware cursor" option).
    toggle("HARDWARE CURSOR", [&] { return s_.hardwareCursor ? 1.0f : 0.0f; },
           [&](float v) { s_.hardwareCursor = v > 0.5f; });
    // Glide units between the 30Hz sim ticks instead of stepping them -- smooth motion
    // above 30fps, at the cost of ~one tick (33ms) of visual latency.
    toggle("SMOOTH MOTION", [&] { return s_.smoothMotion ? 1.0f : 0.0f; },
           [&](float v) { s_.smoothMotion = v > 0.5f; });

    // (Hotkey rebinding moved OUT of Options: it's now a CONTROLS entry in the settings
    // menu -- the in-game Esc menu and the title screen's menu button. See mainmenu.cpp
    // and the in-game GAME MENU. onHotkeys_ is retained for compatibility but unused here.)
    // SAVE / BACK are drawn as a fixed footer (see layout()/render()), not list rows.
}

void OptionsScreen::layout(int winW, int winH) {
    u_ = std::clamp(std::min(winW / 1280.0f, winH / 720.0f), 1.0f, 3.0f);
    auto rowH = [&](Control::Kind k) { return (k == Control::Section) ? 30 * u_ : 36 * u_; };
    float panelW = std::min(660 * u_, winW * 0.72f);
    float titleH = 3.4f * 7 * u_ + 26 * u_;
    float footerH = 52 * u_;    // SAVE / BACK buttons, fixed at the panel bottom
    contentH_ = 0;
    for (auto& c : ctls_) contentH_ += rowH(c.kind);
    float viewH = std::min(contentH_, winH * 0.92f - titleH - footerH);
    float panelH = titleH + viewH + footerH;
    panel_ = {(winW - panelW) / 2, (winH - panelH) / 2, panelW, panelH};

    float maxScroll = std::max(0.0f, contentH_ - viewH);
    scroll_ = std::clamp(scroll_, 0.0f, maxScroll);

    float vy = panel_.y + titleH;    // viewport top
    float y = vy - scroll_;
    for (auto& c : ctls_) {
        float h = rowH(c.kind);
        c.row = {panel_.x + 26 * u_, y, panelW - 52 * u_, h};
        y += h;
    }
    float bw = 150 * u_, bh = 32 * u_, gap = 18 * u_;
    float total = 3 * bw + 2 * gap;
    float x0 = panel_.x + (panel_.w - total) / 2;
    float by = panel_.y + panel_.h - footerH + (footerH - bh) / 2;
    defaultsRect_ = {x0, by, bw, bh};
    saveRect_ = {x0 + bw + gap, by, bw, bh};
    backRect_ = {x0 + 2 * (bw + gap), by, bw, bh};
}

void OptionsScreen::commit(Control& c, float mx) {
    float frac = std::clamp((mx - c.row.x) / std::max(1.0f, c.row.w), 0.0f, 1.0f);
    float v = c.lo + frac * (c.hi - c.lo);
    if (c.set) c.set(v);
    dirty_ = true;
    if (onChange_) onChange_();
}

void OptionsScreen::refreshDevices() {
    auto devs = listAudioDevices();
    std::sort(devs.begin(), devs.end(),                  // stable A-Z (case-insensitive)
              [](const std::string& a, const std::string& b) {
                  return std::lexicographical_compare(
                      a.begin(), a.end(), b.begin(), b.end(),
                      [](unsigned char x, unsigned char y) {
                          return std::tolower(x) < std::tolower(y);
                      });
              });
    devSnapshot_.assign(1, std::string());               // [0] = "" = system default (always first)
    for (auto& d : devs) devSnapshot_.push_back(d);
}

void OptionsScreen::dropViewport(const Control& c, int nOpts, float& y0, float& itemH,
                                 float& viewH) {
    itemH = 24 * u_;
    y0 = c.row.y + 5 * u_ + 24 * u_;                     // just below the closed chip
    float footerH = 52 * u_;
    float maxBottom = panel_.y + panel_.h - footerH - 6 * u_;   // keep clear of the footer
    float fullH = itemH * float(std::max(nOpts, 1));
    viewH = std::clamp(maxBottom - y0, itemH, fullH);    // at least one row, at most the list
    float maxScroll = std::max(0.0f, fullH - viewH);
    dropScroll_ = std::clamp(dropScroll_, 0.0f, maxScroll);
}

bool OptionsScreen::input(const SDL_Event& e, int winW, int winH) {
    applyPendingRebuild();   // fold in a pending device-change rebuild before this event
    layout(winW, winH);
    if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
        if (openDrop_ >= 0) { openDrop_ = -1; return false; }   // close the dropdown, not the screen
        return true;
    }
    if (e.type == SDL_MOUSEWHEEL) {
        // While a dropdown is open, the wheel scrolls ITS list, not the panel underneath --
        // otherwise the panel slides and the pop-up runs out from under the cursor.
        if (openDrop_ >= 0 && openDrop_ < int(ctls_.size()) &&
            ctls_[size_t(openDrop_)].kind == Control::Dropdown) {
            const Control& c = ctls_[size_t(openDrop_)];
            int n = c.options ? int(c.options().size()) : 0;
            float y0, itemH, viewH;
            dropScroll_ -= e.wheel.y * 42 * u_;
            dropViewport(c, n, y0, itemH, viewH);        // clamps dropScroll_
            return false;
        }
        scroll_ -= e.wheel.y * 42 * u_;
        return false;
    }

    auto in = [](const SDL_FRect& r, float mx, float my) {
        return mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h;
    };
    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        float bmx = float(e.button.x), bmy = float(e.button.y);
        // An open dropdown grabs the click: an option selects + closes; the dropdown's
        // own chip toggles it shut; anywhere else just closes it (then falls through so
        // the click still lands on whatever is under it).
        if (openDrop_ >= 0 && openDrop_ < int(ctls_.size()) &&
            ctls_[size_t(openDrop_)].kind == Control::Dropdown) {
            Control& c = ctls_[size_t(openDrop_)];
            auto opts = c.options ? c.options() : std::vector<std::string>{};
            float cw = dropWidth(c), y0, itemH, viewH;
            dropViewport(c, int(opts.size()), y0, itemH, viewH);
            float x = c.row.x + c.row.w - cw;
            // Only clicks inside the scrolled viewport count (items scroll under a clip).
            if (bmx >= x && bmx <= x + cw && bmy >= y0 && bmy <= y0 + viewH) {
                int i = int((bmy - y0 + dropScroll_) / itemH);
                if (i >= 0 && i < int(opts.size())) {
                    if (c.set) c.set(float(i));
                    dirty_ = true; openDrop_ = -1;
                    return false;
                }
            }
            openDrop_ = -1;
            if (in(c.row, bmx, bmy)) return false;   // clicked its own chip -> just close
        }
        float mx = float(e.button.x), my = float(e.button.y);
        if (in(backRect_, mx, my)) return true;                     // close, no save
        if (in(saveRect_, mx, my)) {                                // persist (if dirty)
            if (dirty_ && onSave_) { onSave_(); dirty_ = false; }
            return false;
        }
        if (in(defaultsRect_, mx, my)) {                            // reset to defaults
            if (atDefaults()) return false;         // already default -> disabled, ignore
            s_ = tak::preferenceDefaults(s_);   // one definition, shared with atDefaults()
            dirty_ = true;
            if (onChange_) onChange_();
            return false;
        }
        float titleH = 3.4f * 7 * u_ + 26 * u_, footerH = 52 * u_;  // scrollable viewport
        if (my < panel_.y + titleH || my > panel_.y + panel_.h - footerH) return false;
        for (size_t i = 0; i < ctls_.size(); ++i) {
            Control& c = ctls_[i];
            if (!in(c.row, mx, my)) continue;
            if (c.kind == Control::Toggle) {
                c.set(c.get() > 0.5f ? 0.0f : 1.0f); dirty_ = true; if (onChange_) onChange_(); return false;
            }
            if (c.kind == Control::Slider) { drag_ = int(i); commit(c, mx); return false; }
            if (c.kind == Control::Button) { if (c.action) c.action(); return false; }
            if (c.kind == Control::Dropdown) { refreshDevices(); openDrop_ = int(i); dropScroll_ = 0; return false; }   // fresh A-Z list
        }
        return false;
    }
    if (e.type == SDL_MOUSEMOTION && drag_ >= 0 && (e.motion.state & SDL_BUTTON_LMASK)) {
        layout(winW, winH);   // rows may have shifted if window resized mid-drag
        if (drag_ < int(ctls_.size())) commit(ctls_[size_t(drag_)], float(e.motion.x));
        return false;
    }
    if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) drag_ = -1;
    return false;
}

void OptionsScreen::render(int winW, int winH) {
    applyPendingRebuild();   // in case the change came between input() and this frame
    layout(winW, winH);
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);

    // Dim the whole screen, then the panel.
    SDL_SetRenderDrawColor(ren_, 0, 0, 0, 175);
    SDL_FRect dim{0, 0, float(winW), float(winH)}; SDL_RenderFillRectF(ren_, &dim);
    SDL_SetRenderDrawColor(ren_, 24, 26, 34, 245); SDL_RenderFillRectF(ren_, &panel_);
    SDL_SetRenderDrawColor(ren_, 120, 130, 160, 255); SDL_RenderDrawRectF(ren_, &panel_);

    const char* title = "OPTIONS";
    float tpx = 3.4f * u_;
    drawBlockText(ren_, title, panel_.x + (panel_.w - blockTextWidth(title, tpx)) / 2,
                  panel_.y + 13 * u_, tpx, {235, 225, 180, 255});

    // Clip the scrollable region to between the title and the footer so rows don't
    // spill onto the footer buttons.
    float titleH = 3.4f * 7 * u_ + 26 * u_, footerH = 52 * u_;
    SDL_Rect clip{int(panel_.x), int(panel_.y + titleH), int(panel_.w),
                  int(panel_.h - titleH - footerH)};
    SDL_RenderSetClipRect(ren_, &clip);

    float fpx = 2.0f * u_;
    for (auto& c : ctls_) {
        if (c.row.y + c.row.h < clip.y || c.row.y > clip.y + clip.h) continue;   // off-screen
        if (c.kind == Control::Section) {
            drawBlockText(ren_, c.label, c.row.x, c.row.y + 12 * u_, 2.2f * u_, {150, 200, 235, 255});
            SDL_SetRenderDrawColor(ren_, 70, 78, 96, 255);
            SDL_FRect ln{c.row.x, c.row.y + c.row.h - 3 * u_, c.row.w, 1.5f * u_};
            SDL_RenderFillRectF(ren_, &ln);
        } else if (c.kind == Control::Toggle) {
            bool on = c.get() > 0.5f;
            drawBlockText(ren_, c.label, c.row.x, c.row.y + 12 * u_, fpx, {225, 230, 240, 255});
            SDL_FRect pill{c.row.x + c.row.w - 78 * u_, c.row.y + 7 * u_, 78 * u_, 20 * u_};
            SDL_SetRenderDrawColor(ren_, on ? 60 : 44, on ? 120 : 48, on ? 90 : 58, 255);
            SDL_RenderFillRectF(ren_, &pill);
            SDL_SetRenderDrawColor(ren_, 130, 140, 170, 255); SDL_RenderDrawRectF(ren_, &pill);
            const char* t = on ? "ON" : "OFF";
            drawBlockText(ren_, t, pill.x + (pill.w - blockTextWidth(t, 2.0f * u_)) / 2,
                          pill.y + (pill.h - 7 * 2.0f * u_) / 2, 2.0f * u_,
                          on ? SDL_Color{210, 240, 215, 255} : SDL_Color{170, 175, 185, 255});
        } else if (c.kind == Control::Button) {
            drawBlockText(ren_, c.label, c.row.x, c.row.y + 12 * u_, fpx, {225, 230, 240, 255});
            SDL_FRect chip{c.row.x + c.row.w - 96 * u_, c.row.y + 5 * u_, 96 * u_, 24 * u_};
            SDL_SetRenderDrawColor(ren_, 52, 60, 82, 255);
            SDL_RenderFillRectF(ren_, &chip);
            SDL_SetRenderDrawColor(ren_, 130, 140, 170, 255); SDL_RenderDrawRectF(ren_, &chip);
            const char* t = "OPEN >";
            drawBlockText(ren_, t, chip.x + (chip.w - blockTextWidth(t, 2.0f * u_)) / 2,
                          chip.y + (chip.h - 7 * 2.0f * u_) / 2, 2.0f * u_, {215, 225, 245, 255});
        } else if (c.kind == Control::Dropdown) {
            // Closed state: label on the left, a wide chip with the current value and a
            // downward arrow on the right. The open pop-up list is drawn in a second
            // pass below (un-clipped, on top of everything).
            drawBlockText(ren_, c.label, c.row.x, c.row.y + 12 * u_, fpx, {225, 230, 240, 255});
            float cw = dropWidth(c);
            SDL_FRect chip{c.row.x + c.row.w - cw, c.row.y + 5 * u_, cw, 24 * u_};
            SDL_SetRenderDrawColor(ren_, 52, 60, 82, 255); SDL_RenderFillRectF(ren_, &chip);
            SDL_SetRenderDrawColor(ren_, 130, 140, 170, 255); SDL_RenderDrawRectF(ren_, &chip);
            auto opts = c.options ? c.options() : std::vector<std::string>{};
            int sel = int(c.get ? c.get() : 0.0f);
            std::string t = (sel >= 0 && sel < int(opts.size())) ? opts[size_t(sel)] : std::string();
            float dpx = 1.6f * u_;
            int maxCh = int((chip.w - 30 * u_) / (6 * dpx));   // block glyph = 6px; leave room for the arrow
            if (int(t.size()) > maxCh && maxCh > 0) t = t.substr(0, size_t(maxCh));
            drawBlockText(ren_, t, chip.x + 6 * u_, chip.y + (chip.h - 7 * dpx) / 2, dpx,
                          {215, 225, 245, 255});
            // Down-caret: a filled triangle at the chip's right edge (a real glyph, not "V").
            float aw = 9 * u_, ah = 5 * u_;
            float cx = chip.x + chip.w - 13 * u_, ty = chip.y + (chip.h - ah) / 2;
            SDL_SetRenderDrawColor(ren_, 170, 180, 210, 255);
            for (float r = 0; r < ah; r += 1) {
                float half = (aw / 2) * (1 - r / ah);
                SDL_FRect span{cx - half, ty + r, 2 * half, 1};
                SDL_RenderFillRectF(ren_, &span);
            }
        } else if (c.kind == Control::Slider) {
            float val = c.get();
            drawBlockText(ren_, c.label, c.row.x, c.row.y + 4 * u_, fpx, {225, 230, 240, 255});
            if (c.fmt) {
                std::string vs = c.fmt(val);
                drawBlockText(ren_, vs, c.row.x + c.row.w - blockTextWidth(vs, fpx),
                              c.row.y + 4 * u_, fpx, {150, 195, 235, 255});
            }
            // track + fill + handle
            float ty = c.row.y + 22 * u_, th = 7 * u_;
            SDL_FRect track{c.row.x, ty, c.row.w, th};
            SDL_SetRenderDrawColor(ren_, 48, 52, 66, 255); SDL_RenderFillRectF(ren_, &track);
            float frac = std::clamp((val - c.lo) / std::max(0.0001f, c.hi - c.lo), 0.0f, 1.0f);
            SDL_FRect fill{c.row.x, ty, c.row.w * frac, th};
            SDL_SetRenderDrawColor(ren_, 80, 140, 225, 255); SDL_RenderFillRectF(ren_, &fill);
            SDL_FRect handle{c.row.x + c.row.w * frac - 4 * u_, ty - 4 * u_, 8 * u_, th + 8 * u_};
            SDL_SetRenderDrawColor(ren_, 205, 215, 240, 255); SDL_RenderFillRectF(ren_, &handle);
        }
    }
    SDL_RenderSetClipRect(ren_, nullptr);

    // Scrollbar hint (viewport is the panel minus the title and the footer).
    float viewH = panel_.h - titleH - footerH;
    if (contentH_ > viewH) {
        float th = viewH * viewH / contentH_;
        float ty = panel_.y + titleH + (scroll_ / (contentH_ - viewH)) * (viewH - th);
        SDL_SetRenderDrawColor(ren_, 110, 120, 150, 220);
        SDL_FRect bar{panel_.x + panel_.w - 5 * u_, ty, 3 * u_, th}; SDL_RenderFillRectF(ren_, &bar);
    }

    // Footer buttons. BACK is always active; SAVE only when there are unsaved changes
    // (dirty_), and DEFAULTS only when the settings aren't already at their defaults --
    // so each button's enabled state shows whether it would do anything.
    auto button = [&](const SDL_FRect& r, const char* label, bool on) {
        SDL_SetRenderDrawColor(ren_, on ? 60 : 34, on ? 66 : 38, on ? 86 : 46, 255);
        SDL_RenderFillRectF(ren_, &r);
        SDL_SetRenderDrawColor(ren_, on ? 130 : 70, on ? 140 : 76, on ? 170 : 92, 255);
        SDL_RenderDrawRectF(ren_, &r);
        float bpx = 2.2f * u_;
        drawBlockText(ren_, label, r.x + (r.w - blockTextWidth(label, bpx)) / 2,
                      r.y + (r.h - 7 * bpx) / 2, bpx,
                      on ? SDL_Color{228, 232, 242, 255} : SDL_Color{110, 115, 125, 255});
    };
    button(defaultsRect_, "DEFAULTS", !atDefaults());   // disabled when already default
    button(saveRect_, "SAVE", dirty_);
    button(backRect_, "BACK", true);

    // Open dropdown pop-up: drawn LAST so it sits above every control and the footer.
    if (openDrop_ >= 0 && openDrop_ < int(ctls_.size()) &&
        ctls_[size_t(openDrop_)].kind == Control::Dropdown) {
        const Control& c = ctls_[size_t(openDrop_)];
        auto opts = c.options ? c.options() : std::vector<std::string>{};
        int sel = int(c.get ? c.get() : 0.0f);
        float cw = dropWidth(c), dpx = 1.6f * u_, y0, itemH, viewH;
        dropViewport(c, int(opts.size()), y0, itemH, viewH);
        float x = c.row.x + c.row.w - cw;
        SDL_FRect bg{x, y0, cw, viewH};
        SDL_SetRenderDrawColor(ren_, 30, 34, 46, 255); SDL_RenderFillRectF(ren_, &bg);
        SDL_SetRenderDrawColor(ren_, 140, 150, 185, 255); SDL_RenderDrawRectF(ren_, &bg);
        // Clip the rows to the viewport so a list taller than the panel scrolls under it.
        SDL_Rect clip{int(x), int(y0), int(cw) + 1, int(viewH) + 1};
        SDL_RenderSetClipRect(ren_, &clip);
        int maxCh = int((cw - 16 * u_) / (6 * dpx));
        for (size_t i = 0; i < opts.size(); ++i) {
            float iy = y0 + float(i) * itemH - dropScroll_;
            if (iy + itemH < y0 || iy > y0 + viewH) continue;   // fully scrolled out
            SDL_FRect it{x, iy, cw, itemH};
            if (int(i) == sel) { SDL_SetRenderDrawColor(ren_, 52, 74, 96, 255); SDL_RenderFillRectF(ren_, &it); }
            std::string t = opts[i];
            if (int(t.size()) > maxCh && maxCh > 0) t = t.substr(0, size_t(maxCh));
            drawBlockText(ren_, t, it.x + 6 * u_, it.y + (itemH - 7 * dpx) / 2, dpx,
                          int(i) == sel ? SDL_Color{215, 235, 245, 255} : SDL_Color{200, 205, 220, 255});
        }
        SDL_RenderSetClipRect(ren_, nullptr);
    }
}

}  // namespace tak
