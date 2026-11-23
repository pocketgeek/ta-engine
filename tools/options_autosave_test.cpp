// options_autosave_test -- Options, Voice Chat and Controls save every change as it is
// made; there is no SAVE button. Driven with real SDL events on a software renderer,
// with the hosts' save callback (saveSettings) writing a private settings file
// (setSettingsPath), and the file read back after each step:
//   - a toggle, a dropdown pick and a rebound key are in the file at once;
//   - a slider writes once, when it is let go -- not per drag step;
//   - the player name writes when its field loses focus (Enter, or the screen closing),
//     not per keystroke;
//   - DEFAULTS asks first (RESET TO DEFAULTS? YES / NO): NO changes nothing, YES resets
//     and saves -- in Options and in Controls;
//   - the Options DEFAULTS puts the sound device back to the system default and re-opens
//     audio on it at once, only when the device in use changes;
//   - SIMULATION THREADS (GAME page) is a whole-step slider from AUTO at its left end to
//     twice the CPU threads this process may use, coloured by the kind of CPU a count
//     reaches (red above the computer's threads), with lines under it that say what the
//     computer has and what the choice comes to; saved when let go, and the GAME page's
//     DEFAULTS is live only while it is off AUTO;
//   - PIN SIMULATION THREADS under it is OFF by default, which hands no CPU to any thread;
//     a click turns it ON, saved at once, the line under it follows it, DEFAULTS turns it
//     back OFF, and where the system cannot pin (macOS) the row is greyed, reads OFF and
//     keeps the stored choice;
//   - the Controls hint line has a band of its own: at every window size it lies below the
//     rows' viewport (never over a row, scrolled or not) and above the footer buttons.
//     TAK_LAYOUT_SHOT_DIR=<dir> also writes each case as a BMP, and the GAME page with
//     its RESET TO DEFAULTS? prompt open.

#include "client/hotkeys.h"
#include "client/hotkeysscreen.h"
#include "client/options.h"
#include "client/settings.h"
#include "sim/simcpus.h"

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace tak;

static int g_fail = 0, g_checks = 0;
static void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) { ++g_fail; std::printf("  FAIL %s\n", what.c_str()); }
}

static constexpr int W = 1280, H = 720;

template <typename Screen>
static bool send(Screen& s, Uint32 type, float x, float y, Uint32 state = 0) {
    SDL_Event e{};
    e.type = type;
    if (type == SDL_MOUSEMOTION) { e.motion.x = int(x); e.motion.y = int(y); e.motion.state = state; }
    else { e.button.button = SDL_BUTTON_LEFT; e.button.x = int(x); e.button.y = int(y); }
    const bool leave = s.input(e, W, H);
    s.render(W, H);
    return leave;
}
template <typename Screen>
static bool click(Screen& s, const SDL_FRect& r) {
    const float x = r.x + r.w / 2, y = r.y + r.h / 2;
    const bool leave = send(s, SDL_MOUSEBUTTONDOWN, x, y);
    send(s, SDL_MOUSEBUTTONUP, x, y);
    return leave;
}
template <typename Screen>
static bool key(Screen& s, SDL_Keycode k) {
    SDL_Event e{}; e.type = SDL_KEYDOWN; e.key.keysym.sym = k;
    const bool leave = s.input(e, W, H);
    s.render(W, H);
    return leave;
}

// Scroll Options until the row is in the middle of the panel; nullptr if there is none.
static const SDL_FRect* bring(OptionsScreen& o, const char* label) {
    o.render(W, H);
    const SDL_FRect* r = o.rowRect(label);
    for (int i = 0; i < 80 && r && (r->y + r->h / 2 > H * 0.6f || r->y < H * 0.25f); ++i) {
        SDL_Event e{}; e.type = SDL_MOUSEWHEEL; e.wheel.y = r->y < H * 0.25f ? 1 : -1;
        o.input(e, W, H); o.render(W, H);
        r = o.rowRect(label);
    }
    check(r != nullptr, std::string("Options has a ") + label + " row");
    return r;
}

static void testOptions(SDL_Renderer* ren) {
    std::printf("Options\n");
    Settings s = loadSettings();
    int saves = 0, changes = 0;
    auto save = [&] { ++saves; check(saveSettings(s), "the host's save writes the file"); };
    {
        OptionsScreen o(ren, s, [&] { ++changes; }, save, 2, {}, {}, {}, OptionsPage::Graphics);
        o.render(W, H);

        // A toggle: in the file at once.
        const bool shadows = s.unitShadows;
        if (const SDL_FRect* r = bring(o, "SHADOWS")) click(o, *r);
        check(s.unitShadows != shadows && changes == 1, "SHADOWS toggles live");
        check(saves == 1 && loadSettings().unitShadows == s.unitShadows, "a toggle is saved at once, no SAVE");

        // A slider: live on every drag step, saved once when let go. MASTER VOLUME is on the
        // AUDIO page, with the rest of the sound settings.
        check(o.rowRect("MASTER VOLUME") == nullptr && o.rowRect("UI SCALE") == nullptr && o.rowRect("PLAYER NAME") == nullptr,
              "GRAPHICS holds only its own settings");
        OptionsScreen audio(ren, s, [&] { ++changes; }, save, std::vector<music::SideTracks>{}, 2);
        audio.render(W, H);
        if (const SDL_FRect* r = bring(audio, "MASTER VOLUME")) {
            OptionsScreen& o = audio;
            const int before = loadSettings().masterVol;
            const float y = r->y + r->h / 2;
            send(o, SDL_MOUSEBUTTONDOWN, r->x + r->w * 0.20f, y);
            send(o, SDL_MOUSEMOTION, r->x + r->w * 0.35f, y, SDL_BUTTON_LMASK);
            send(o, SDL_MOUSEMOTION, r->x + r->w * 0.50f, y, SDL_BUTTON_LMASK);
            check(s.masterVol >= 124 && s.masterVol <= 132 && changes >= 4, "MASTER VOLUME follows the drag live");
            check(saves == 1 && loadSettings().masterVol == before, "a slider is not saved while it is held");
            send(o, SDL_MOUSEBUTTONUP, r->x + r->w * 0.50f, y);
            check(saves == 2 && loadSettings().masterVol == s.masterVol, "a slider is saved once, when let go");
            send(o, SDL_MOUSEMOTION, r->x + r->w * 0.70f, y, 0);   // no button: not a drag
            check(saves == 2 && s.masterVol <= 132, "moving the mouse after the release changes nothing");
        }
        o.render(W, H);

        // A dropdown pick: saved at once.
        if (const SDL_FRect* r = bring(o, "GPU MEMORY LIMIT")) {
            click(o, SDL_FRect{r->x + r->w - 20, r->y, 10, r->h});
            const SDL_FRect item = o.dropItemRect(0);
            check(item.w > 0, "the GPU MEMORY LIMIT list opens");
            send(o, SDL_MOUSEBUTTONDOWN, item.x + item.w / 2, item.y + item.h / 2);
            check(s.gpuMemoryLimit == kGpuMemoryLimits[0], "the pick applies");
            check(saves == 3 && loadSettings().gpuMemoryLimit == kGpuMemoryLimits[0], "a dropdown pick is saved at once");
        }

        // The player name (on GAME): typed live, saved when its field loses focus.
        OptionsScreen game(ren, s, [&] { ++changes; }, save, 2, {}, {}, {}, OptionsPage::Game);
        game.render(W, H);
        if (const SDL_FRect* r = bring(game, "PLAYER NAME")) {
            OptionsScreen& o = game;
            click(o, *r);
            check(o.editingText(), "clicking the name field focuses it");
            SDL_Event t{}; t.type = SDL_TEXTINPUT; SDL_strlcpy(t.text.text, "Aldric", sizeof t.text.text);
            o.input(t, W, H);
            check(s.playerName == "Aldric", "the name is typed");
            check(saves == 3 && loadSettings().playerName != "Aldric", "the name is not saved per keystroke");
            key(o, SDLK_RETURN);
            check(!o.editingText() && saves == 4 && loadSettings().playerName == "Aldric", "Enter saves the name");
            click(o, *r);
            SDL_Event t2{}; t2.type = SDL_TEXTINPUT; SDL_strlcpy(t2.text.text, "Brand", sizeof t2.text.text);
            o.input(t2, W, H);
            check(saves == 4 && loadSettings().playerName == "Aldric", "the second name waits for the focus to go");
        }
    }   // the screen closes with the name still focused
    check(saves == 5 && loadSettings().playerName == "Brand", "closing the screen saves the name being typed");

    // DEFAULTS asks first, and resets only its page (GRAPHICS here).
    saves = 0;
    {
        OptionsScreen o(ren, s, [&] { ++changes; }, save, 2, {}, {}, {}, OptionsPage::Graphics);
        o.render(W, H);
        const Settings before = s;
        check(!(s == graphicsDefaults(s)), "the profile is off its defaults");
        click(o, o.defaultsRect());
        check(s == before && saves == 0, "DEFAULTS alone changes nothing");
        const SDL_FRect no = o.defaultsNoRect(), yes = o.defaultsYesRect();
        check(no.w > 0 && yes.w > 0, "DEFAULTS asks RESET TO DEFAULTS? YES / NO");
        check(!click(o, no) && s == before && saves == 0 && o.defaultsYesRect().w == 0, "NO keeps everything");
        click(o, o.defaultsRect());
        key(o, SDLK_ESCAPE);
        check(s == before && saves == 0 && o.defaultsYesRect().w == 0, "Esc answers NO, and stays in Options");
        click(o, o.defaultsRect());
        click(o, o.defaultsYesRect());
        check(s == graphicsDefaults(before) && s.playerName == "Brand", "YES resets this page, keeping the player name");
        check(saves == 1 && loadSettings() == s, "YES saves the defaults");
        check(click(o, o.defaultsRect()) == false && o.defaultsYesRect().w == 0, "at defaults, DEFAULTS does nothing");
    }

    // The AUDIO page's DEFAULTS puts the sound device back to the system default AND
    // re-opens audio on it now, as picking it in the SOUND DEVICE list does -- only if the
    // device really changes.
    {
        const auto devs = listAudioDevices(0);
        check(!devs.empty(), "the dummy audio driver lists an output device");
        if (!devs.empty()) {
            int reopens = 0;
            auto reset = [&](OptionsScreen& o) { click(o, o.defaultsRect()); click(o, o.defaultsYesRect()); };
            s = preferenceDefaults(s);
            s.audioDevice = devs[0];
            setAudioDevice(s.audioDevice);
            check(currentAudioDevice() == devs[0], "the chosen device is the one in use");
            OptionsScreen o(ren, s, [&] { ++changes; }, save, std::vector<music::SideTracks>{}, 2, [&] { ++reopens; });
            o.render(W, H);
            reset(o);
            check(s.audioDevice.empty() && currentAudioDevice().empty(), "DEFAULTS puts the device in use back to the system default");
            check(reopens == 1, "DEFAULTS re-opens audio on the new device, once");
            // A saved device that is not plugged in is not in use: resetting it re-opens nothing.
            s.audioDevice = "No Such Speakers";
            setAudioDevice(s.audioDevice);
            check(currentAudioDevice().empty(), "a missing device is not in use");
            reset(o);
            check(s.audioDevice.empty() && reopens == 1, "DEFAULTS does not re-open when the device in use is unchanged");
            // The Voice Chat page's DEFAULTS never touches the game's device.
            s.audioDevice = devs[0];
            setAudioDevice(s.audioDevice);
            OptionsScreen v(ren, s, [] {}, save, VoicePageHooks{});
            v.render(W, H);
            s.voiceEnabled = true;
            reset(v);
            check(!s.voiceEnabled && s.audioDevice == devs[0] && currentAudioDevice() == devs[0] && reopens == 1,
                  "the Voice Chat DEFAULTS leaves the sound device alone");
            setAudioDevice("");
        }
    }
}

[[maybe_unused]] static void putEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// A row's wrapped lines as one text, continuation indents dropped.
static std::string joined(const std::vector<std::string>& lines) {
    std::string all;
    for (const std::string& l : lines) all += l.substr(std::min(l.size(), l.find_first_not_of(' '))) + " ";
    return all;
}

// The screen as drawn, for TAK_LAYOUT_SHOT_DIR.
static void shoot(SDL_Renderer* ren, OptionsScreen& o, const std::string& path) {
    SDL_SetRenderDrawColor(ren, 12, 12, 16, 255); SDL_RenderClear(ren);   // the panel is translucent
    o.render(W, H);
    if (SDL_Surface* shot = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888)) {
        if (SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888, shot->pixels, shot->pitch) == 0)
            SDL_SaveBMP(shot, path.c_str());
        SDL_FreeSurface(shot);
    }
}

// PIN SIMULATION THREADS, on the GAME page `o` at its defaults: OFF, under the slider's
// lines, and OFF hands no CPU to any thread (sim::SimPool::handOut); a click turns it ON
// and saves at once, the line under it follows, and DEFAULTS turns it back OFF.
static void testPinToggle(OptionsScreen& o, Settings& s, int& saves, const sim::CpuTopology& topology) {
    const SDL_FRect* r = bring(o, "PIN SIMULATION THREADS");
    const SDL_FRect* info = o.rowRect("SIMULATION THREADS INFO");
    const SDL_FRect* line = o.rowRect("PIN SIMULATION THREADS INFO");
    if (!r || !info || !line) { check(false, "the PIN SIMULATION THREADS row and its line are there"); return; }
    check(r->y >= info->y + info->h && line->y >= r->y + r->h, "the toggle is under the slider's lines, its own line under it");
    if (!topology.canPin) return;   // macOS: greyed (the debug-build case below)
    const int before = saves;
    check(!s.simPin && o.rowEnabled("PIN SIMULATION THREADS") && !o.rowOn("PIN SIMULATION THREADS"), "it is OFF by default");
    check(joined(o.rowLines("PIN SIMULATION THREADS INFO")).find("THREADS ARE NOT PINNED: THE SYSTEM PLACES THEM.") != std::string::npos,
          "OFF: the line says threads are not pinned");
    check(joined(o.rowHelp("PIN SIMULATION THREADS")).find("APPLIES FROM THE NEXT GAME YOU OPEN.") != std::string::npos,
          "its help says it applies from the next game opened");
    const auto session = sim::SimPool::sessionThreads(s.simThreads, topology, true);
    const auto off = sim::SimPool::handOut(topology.order, session, true, s.simPin);
    check(off.game.empty() && off.server.empty(), "OFF reaches the hand-out as nothing pinned");
    click(o, *r);
    check(s.simPin && o.rowOn("PIN SIMULATION THREADS") && saves == before + 1 && loadSettings().simPin, "a click turns it ON, saved at once");
    const std::string pinned = joined(o.rowLines("PIN SIMULATION THREADS INFO"));
    check(pinned.find("EACH THREAD IS PINNED TO ITS OWN CORE: ") != std::string::npos &&
          pinned.find(topology.eff ? "PERFORMANCE CORES FIRST, THEN EFFICIENCY CORES, THEN HYPERTHREADS." : "CORES FIRST, THEN HYPERTHREADS.") !=
              std::string::npos &&
          pinned.find("THREADS BEYOND THE COMPUTER'S " + std::to_string(topology.threads()) + " ARE NOT PINNED.") != std::string::npos,
          "ON: the line says each thread is pinned, in which order, and that threads beyond the computer's are not");
    bool fits = true;
    for (const std::string& l : o.rowLines("PIN SIMULATION THREADS INFO")) fits = fits && uiTextWidth(l, 1.3f) <= line->w;
    check(fits, "its line fits the row");
    check(!sim::SimPool::handOut(topology.order, session, true, s.simPin).game.empty(), "ON hands the game its CPUs");
    check(o.rowBand("SIMULATION THREADS") == 0, "the slider keeps its colours");
    click(o, o.defaultsRect());
    check(o.defaultsYesRect().w > 0, "ON, DEFAULTS is live");
    click(o, o.defaultsYesRect());
    check(!s.simPin && s.simThreads == 0 && saves == before + 2 && !loadSettings().simPin && !o.rowOn("PIN SIMULATION THREADS"),
          "DEFAULTS turns it back OFF");
}

// SIMULATION THREADS: the GAME page's slider (PERFORMANCE, after PLAYER and GAME DATA).
// Position 0 is AUTO, then 1 (serial) .. 2N in whole steps, N being every CPU thread this
// process may use (sim::detectCpus: 1 on a one-CPU test machine, where the slider has
// AUTO, 1 and 2). Its colour is the kind of CPU the count reaches (sim::SimPool::band),
// red above N; the lines under it say what the computer has and what the choice comes
// to with a local server (half each) and without (all of it). PIN SIMULATION THREADS is
// the toggle under those.
static void testSimThreads(SDL_Renderer* ren) {
    std::printf("SIMULATION THREADS\n");
    const sim::CpuTopology topology = sim::detectCpus();
    const int cpus = int(topology.threads());
    const int most = int(sim::SimPool::maxChoice(unsigned(cpus)));
    Settings s = loadSettings();
    s.simThreads = 0;
    s.simPin = false;
    check(saveSettings(s), "the profile starts at AUTO");
    const std::string name = s.playerName;
    int saves = 0, changes = 0;
    auto save = [&] { ++saves; check(saveSettings(s), "the host's save writes the file"); };
    {
        OptionsScreen o(ren, s, [&] { ++changes; }, save, 2, {}, {}, {}, OptionsPage::Game);
        o.render(W, H);
        const SDL_FRect* r = bring(o, "SIMULATION THREADS");
        if (!r) return;
        const SDL_FRect* folder = o.rowRect("DATA FOLDER");
        check(o.rowRect("PLAYER NAME") && folder && r->y > folder->y && o.rowEnabled("SIMULATION THREADS"),
              "the slider is on the GAME page, below PLAYER and GAME DATA");
        // At AUTO the page has nothing to reset: DEFAULTS is greyed and does nothing.
        check(s == gameDefaults(s), "AUTO is the GAME page's default");
        check(click(o, o.defaultsRect()) == false && o.defaultsYesRect().w == 0, "at AUTO, DEFAULTS does nothing");
        r = o.rowRect("SIMULATION THREADS");
        const float y = r->y + r->h / 2, x0 = r->x, w = r->w;
        auto at = [&](float step) { return x0 + w * step / float(most) + 0.5f; };   // a step's place on the track
        // The lines under it, at AUTO: the computer's threads, half each with a local
        // server, all of them without. They are wrapped to the row: read here as one text.
        auto says = [&](const std::string& part) {
            std::string all;
            for (const std::string& l : o.rowLines("SIMULATION THREADS INFO")) all += l.substr(l.find_first_not_of(' ')) + " ";
            return all.find(part) != std::string::npos;
        };
        const SDL_FRect* info = o.rowRect("SIMULATION THREADS INFO");
        check(info && info->y >= r->y + r->h && info->h > 0, "the info lines are under the slider");
        bool fits = info != nullptr;
        for (const std::string& l : o.rowLines("SIMULATION THREADS INFO")) fits = fits && uiTextWidth(l, 1.3f) <= info->w;
        check(fits && o.rowLines("SIMULATION THREADS INFO").size() >= 3, "each line fits the row");
        // What this computer has: its cores by kind, hyperthreading or not, the threads.
        auto count = [](unsigned n, const char* one, const char* many) { return std::to_string(n) + (n == 1 ? one : many); };
        check(says("THIS COMPUTER HAS " + (topology.eff ? count(topology.perf, " PERFORMANCE CORE", " PERFORMANCE CORES") + " AND " +
                                                              count(topology.eff, " EFFICIENCY CORE", " EFFICIENCY CORES")
                                                        : count(topology.perf, " CORE", " CORES")) +
                   (topology.smt ? " WITH HYPERTHREADING: " : ", NO HYPERTHREADING: ") + count(unsigned(cpus), " THREAD.", " THREADS.")),
              "they say what this computer has: cores by kind, hyperthreading, threads");
        // The counts are sim::SimPool::sessionThreads' (tools/simpool_test.cpp pins it): on a
        // machine with more threads than one simulation runs (kMaxThreads) they stop there.
        auto halves = [&](int choice) {
            const auto t = sim::SimPool::sessionThreads(choice, topology, true);
            return "THE GAME (" + std::to_string(t.game) + "), HALF TO ITS LOCAL SERVER (" + std::to_string(t.server) + ").";
        };
        auto whole = [&](int choice) {
            return "ALL OF THEM (" + std::to_string(sim::SimPool::sessionThreads(choice, topology, false).game) + ").";
        };
        check(says("SINGLE PLAYER, A LOCAL SERVER YOU HOST AND THE BENCHMARK: HALF") && says(halves(0)),
              "they say a game with a local server gives half to each");
        check(says("A GAME ON SOMEONE ELSE'S SERVER: THE GAME USES " + whole(0)),
              "they say a game on someone else's server uses them all");
        check(o.rowBand("SIMULATION THREADS") == 0, "AUTO is blue");
        {
            const std::string text = joined(o.rowHelp("SIMULATION THREADS"));
            check(text.find(topology.eff ? "AUTO IS ONE FOR EACH PERFORMANCE CORE." : "AUTO IS ONE FOR EACH CORE.") != std::string::npos &&
                  text.find("BLUE") != std::string::npos && text.find("RED IS MORE THREADS THAN THIS COMPUTER HAS.") != std::string::npos &&
                  (text.find("GREEN") != std::string::npos) == (topology.eff > 0) &&
                  (text.find("YELLOW") != std::string::npos) == (topology.smt > 0),
                  "the hover help says what AUTO is and names the colours this computer shows");
        }
        // The right end is 2N (two simulations of kMaxThreads at most); held, it is live
        // but not in the file yet.
        send(o, SDL_MOUSEBUTTONDOWN, at(float(most)), y);
        check(s.simThreads == most && most == 2 * std::min(cpus, int(sim::SimPool::kMaxThreads)) && changes == 1,
              "the right end is twice this machine's threads");
        check(saves == 0 && loadSettings().simThreads == 0, "the slider is not saved while it is held");
        check(says(halves(most)) && says(whole(most)), "the info lines follow the slider");
        // Every whole step down to the left end, which is AUTO; each in the colour of the
        // CPUs it reaches: blue to the performance cores, green to the efficiency cores,
        // yellow to the hyperthreads, red above N only.
        bool steps = true, bands = true;
        for (int n = most; n >= 0; --n) {
            send(o, SDL_MOUSEMOTION, at(float(n)), y, SDL_BUTTON_LMASK);
            steps = steps && s.simThreads == n;
            const int band = int(topology.perf) >= n ? 0 : int(topology.perf + topology.eff) >= n ? 1 : n <= cpus ? 2 : 3;
            bands = bands && o.rowBand("SIMULATION THREADS") == band;
        }
        check(steps && s.simThreads == 0, "it moves in whole steps, 1 to 2N, with AUTO at the left end");
        check(bands, "its colour is the kind of CPU the count reaches, red above this machine's threads only");
        if (most >= 2) {   // between two steps: the nearer one
            send(o, SDL_MOUSEMOTION, at(1.3f), y, SDL_BUTTON_LMASK);
            check(s.simThreads == 1, "a place between two steps is the nearer step");
            send(o, SDL_MOUSEMOTION, at(1.7f), y, SDL_BUTTON_LMASK);
            check(s.simThreads == 2, "a place between two steps is the nearer step (upper)");
        }
        send(o, SDL_MOUSEMOTION, at(1), y, SDL_BUTTON_LMASK);
        check(saves == 0, "no save during the drag");
        send(o, SDL_MOUSEBUTTONUP, at(1), y);
        check(s.simThreads == 1 && saves == 1 && loadSettings().simThreads == 1, "let go on 1 (serial): saved once");
        // Off AUTO, the GAME page's DEFAULTS is live: it asks, and YES puts AUTO back.
        click(o, o.defaultsRect());
        check(o.defaultsYesRect().w > 0 && s.simThreads == 1 && saves == 1, "off AUTO, DEFAULTS asks first");
        if (const char* dir = std::getenv("TAK_LAYOUT_SHOT_DIR")) {   // the prompt, to look at
            SDL_SetRenderDrawColor(ren, 12, 12, 16, 255); SDL_RenderClear(ren);   // the panel is translucent
            o.render(W, H);
            if (SDL_Surface* shot = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888)) {
                if (SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888, shot->pixels, shot->pitch) == 0)
                    SDL_SaveBMP(shot, (std::string(dir) + "/game-defaults-prompt.bmp").c_str());
                SDL_FreeSurface(shot);
            }
        }
        click(o, o.defaultsNoRect());
        check(s.simThreads == 1 && saves == 1, "NO keeps the choice");
        click(o, o.defaultsRect());
        click(o, o.defaultsYesRect());
        check(s.simThreads == 0 && s.playerName == name && saves == 2 && loadSettings().simThreads == 0,
              "YES puts SIMULATION THREADS back to AUTO, keeps the player name, and saves");
        check(click(o, o.defaultsRect()) == false && o.defaultsYesRect().w == 0, "back at AUTO, DEFAULTS does nothing again");
        testPinToggle(o, s, saves, topology);
    }
#ifndef NDEBUG
    {
        // A system that cannot pin (macOS; here a debug build's TAK_CPU_NOPIN): the row is
        // greyed, reads OFF, takes no click and keeps the stored choice (ON here), and says why.
        putEnv("TAK_CPU_NOPIN", "1");
        s.simPin = true;
        check(saveSettings(s), "the stored choice is ON");
        OptionsScreen o(ren, s, [&] { ++changes; }, save, 2, {}, {}, {}, OptionsPage::Game);
        o.render(W, H);
        const int savesBefore = saves;
        if (const SDL_FRect* r = bring(o, "PIN SIMULATION THREADS")) {
            check(s.simPin && !o.rowEnabled("PIN SIMULATION THREADS") && !o.rowOn("PIN SIMULATION THREADS"),
                  "where the system cannot pin, the row is greyed and reads OFF");
            click(o, *r);
            check(s.simPin && saves == savesBefore && loadSettings().simPin, "a click there changes nothing");
            check(joined(o.rowHelp("PIN SIMULATION THREADS")).find("MACOS DOES NOT LET A GAME PIN THREADS") != std::string::npos &&
                  joined(o.rowLines("PIN SIMULATION THREADS INFO")).find("MACOS DOES NOT LET A GAME PIN THREADS, SO THE SYSTEM PLACES THEM.") !=
                      std::string::npos, "its help and the line under it say macOS does not let a game pin threads");
            if (const char* dir = std::getenv("TAK_LAYOUT_SHOT_DIR")) shoot(ren, o, std::string(dir) + "/game-pin-cannot.bmp");
        } else check(false, "the PIN SIMULATION THREADS row is there");
        putEnv("TAK_CPU_NOPIN", "0");
        s.simPin = false;
        check(saveSettings(s), "the profile is back at OFF");
    }
#endif
    // A profile from a bigger machine: a saved count above this machine's 2N is shown (and
    // run) as 2N, and stays in the file until the player moves the slider.
    if (most < int(sim::SimPool::kMaxChoice)) {
        s.simThreads = int(sim::SimPool::kMaxChoice);
        check(saveSettings(s), "a bigger machine's choice is in the file");
        OptionsScreen o(ren, s, [&] { ++changes; }, save, 2, {}, {}, {}, OptionsPage::Game);
        o.render(W, H);
        const int savesBefore = saves;
        if (const SDL_FRect* r = bring(o, "SIMULATION THREADS")) {
            send(o, SDL_MOUSEMOTION, r->x + r->w / 2, r->y + r->h / 2);   // hover: the help box, nothing else
            check(s.simThreads == int(sim::SimPool::kMaxChoice) && saves == savesBefore &&
                  loadSettings().simThreads == int(sim::SimPool::kMaxChoice), "showing the page does not rewrite it");
            check(int(sim::SimPool::configured(s.simThreads, topology)) == most, "it runs as this machine's most");
            // Pressing the right end is the same place the handle already shows: 2N.
            send(o, SDL_MOUSEBUTTONDOWN, r->x + r->w, r->y + r->h / 2);
            send(o, SDL_MOUSEBUTTONUP, r->x + r->w, r->y + r->h / 2);
            check(s.simThreads == most, "moving the slider stores this machine's own count");
        }
        s.simThreads = 0;
        check(saveSettings(s), "the profile is back at AUTO");
    }
}

static void testVoice(SDL_Renderer* ren) {
    std::printf("VOICE CHAT\n");
    Settings s = loadSettings();
    int saves = 0;
    OptionsScreen o(ren, s, [] {}, [&] { ++saves; check(saveSettings(s), "the host's save writes the file"); },
                    VoicePageHooks{});
    o.render(W, H);
    if (const SDL_FRect* r = o.rowRect("VOICE CHAT")) click(o, *r);
    check(s.voiceEnabled && saves == 1 && loadSettings().voiceEnabled, "the VOICE CHAT switch is saved at once");
    click(o, o.defaultsRect());
    check(s.voiceEnabled && saves == 1, "the page's DEFAULTS asks first");
    click(o, o.defaultsYesRect());
    check(!s.voiceEnabled && saves == 2 && !loadSettings().voiceEnabled, "YES resets the voice settings and saves");
}

static void testControls(SDL_Renderer* ren) {
    std::printf("CONTROLS\n");
    Settings s = loadSettings();
    s.hotkeys.clear();
    check(saveSettings(s), "start from the default keys");
    int saves = 0, changes = 0;
    HotkeysScreen hk(ren, s, [&] { ++changes; }, [&] { ++saves; check(saveSettings(s), "the host's save writes the file"); });
    hk.render(W, H);
    const Act act = hotkeyDefs().front().act;
    const SDL_FRect row = hk.rowRect(act);
    check(row.w > 0, "CONTROLS has a row for the first action");
    click(hk, row);   // arm
    key(hk, SDLK_F7);
    check(effectiveChord(s.hotkeys, act) == KeyChord{SDLK_F7, 0} && changes == 1, "a key press rebinds the row");
    check(saves == 1 && loadSettings().hotkeys == s.hotkeys && !s.hotkeys.empty(), "a rebind is saved at once, no SAVE");

    const auto bound = s.hotkeys;
    click(hk, hk.defaultsRect());
    check(s.hotkeys == bound && saves == 1, "DEFAULTS alone changes nothing");
    check(hk.defaultsNoRect().w > 0, "DEFAULTS asks RESET TO DEFAULTS? YES / NO");
    click(hk, hk.defaultsNoRect());
    check(s.hotkeys == bound && saves == 1 && loadSettings().hotkeys == bound, "NO keeps the keys");
    click(hk, hk.defaultsRect());
    click(hk, hk.defaultsYesRect());
    check(s.hotkeys.empty() && saves == 2 && loadSettings().hotkeys.empty(), "YES resets the keys and saves");

    // A row's own DEFAULT: one key back, and an action holding that key gives it up.
    const HotkeyDef *mine = nullptr, *other = nullptr;   // one with a plain default key, and another
    for (const auto& d : hotkeyDefs())
        if (!mine && d.def.bound() && d.def.mod == 0) mine = &d;
        else if (mine && !other && d.def.bound()) other = &d;
    const Act act1 = mine->act;
    const KeyChord actDef = mine->def;
    click(hk, hk.rowDefaultRect(act1));
    check(s.hotkeys.empty() && saves == 2, "a row's DEFAULT at its default changes nothing");
    click(hk, hk.rowRect(act1));
    key(hk, SDLK_F7);
    click(hk, hk.rowRect(other->act));
    key(hk, SDL_Keycode(actDef.key));   // the first action's default key, taken
    check(effectiveChord(s.hotkeys, other->act) == actDef, "another row takes the first action's key");
    click(hk, hk.rowDefaultRect(act1));
    check(effectiveChord(s.hotkeys, act1) == actDef && !effectiveChord(s.hotkeys, other->act).bound() &&
          loadSettings().hotkeys == s.hotkeys, "a row's DEFAULT puts its key back, takes it from the other, saves");
    click(hk, hk.rowDefaultRect(other->act));
    check(s.hotkeys.empty() && loadSettings().hotkeys.empty(), "the other row's DEFAULT: every key default again");
}

// The Controls screen at one window size, on a software renderer of its own: the hint
// line's band must lie below the rows' viewport and above the footer, inside the panel, and
// the last row must be reachable. Rows are clipped to the viewport, so a hint outside it
// can never lie on a row.
// Renderers are kept to the end of the run: uiText caches its textures by renderer address,
// and a destroyed renderer's address is reused by the next one.
static std::vector<std::pair<SDL_Renderer*, SDL_Surface*>> g_kept;

static void layoutCase(int w, int h) {
    const std::string tag = std::to_string(w) + "x" + std::to_string(h);
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Renderer* ren = surf ? SDL_CreateSoftwareRenderer(surf) : nullptr;
    if (!ren) { check(false, "a software renderer at " + tag); if (surf) SDL_FreeSurface(surf); return; }
    Settings s;
    HotkeysScreen hk(ren, s, [] {}, [] {});
    auto shot = [&](const char* what) {
        const char* dir = std::getenv("TAK_LAYOUT_SHOT_DIR");
        if (dir) SDL_SaveBMP(surf, (std::string(dir) + "/controls-" + tag + "-" + what + ".bmp").c_str());
    };
    auto verify = [&](const char* what) {
        SDL_SetRenderDrawColor(ren, 12, 12, 16, 255); SDL_RenderClear(ren);   // the panel is translucent
        hk.render(w, h);
        const std::string at = tag + " " + what + ": ";
        const SDL_FRect v = hk.viewRect(), hint = hk.hintRect(), p = hk.panelRect(), d = hk.defaultsRect();
        check(hint.h > 0, at + "the hint line has a band of its own");
        check(hint.y >= v.y + v.h - 0.01f, at + "the hint lies below the rows' viewport");
        check(hint.y + hint.h <= d.y + 0.01f, at + "the hint lies above the footer buttons");
        check(v.y >= p.y && hint.y + hint.h <= p.y + p.h + 0.01f, at + "the viewport and hint are inside the panel");
        shot(what);
    };
    verify("top");
    // To the end: the last row ends at the viewport's bottom, so it is not under the hint.
    for (int i = 0; i < 200; ++i) {
        SDL_Event e{}; e.type = SDL_MOUSEWHEEL; e.wheel.y = -1;
        hk.input(e, w, h);
    }
    verify("end");
    const SDL_FRect last = hk.rowRect(hotkeyDefs().back().act), v = hk.viewRect();
    check(std::fabs((last.y + last.h) - (v.y + v.h)) < 1.5f, tag + ": scrolled to the end, the last row is the viewport's last");
    g_kept.emplace_back(ren, surf);
}

// A click on the hint band is not a click on a row scrolled under it.
static void testHintBandClicks() {
    const int w = 1600, h = 1100;
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Renderer* ren = surf ? SDL_CreateSoftwareRenderer(surf) : nullptr;
    if (!ren) { check(false, "a software renderer for the hint band clicks"); return; }
    Settings s;
    HotkeysScreen hk(ren, s, [] {}, [] {});
    hk.render(w, h);
    bool tried = false;
    for (int i = 0; i < 40 && !tried; ++i) {
        const SDL_FRect hint = hk.hintRect();
        for (const auto& d : hotkeyDefs()) {
            const SDL_FRect r = hk.rowRect(d.act);
            if (r.y < hint.y + hint.h / 2 && r.y + r.h > hint.y + hint.h / 2) {   // a row sits under the hint
                SDL_Event e{}; e.type = SDL_MOUSEBUTTONDOWN; e.button.button = SDL_BUTTON_RIGHT;
                e.button.x = int(r.x + r.w / 2); e.button.y = int(hint.y + hint.h / 2);
                hk.input(e, w, h);
                tried = true;
                break;
            }
        }
        SDL_Event wh{}; wh.type = SDL_MOUSEWHEEL; wh.wheel.y = -1;
        hk.input(wh, w, h); hk.render(w, h);
    }
    check(tried, "a scroll position has a row under the hint band");
    check(s.hotkeys.empty(), "a right-click on the hint band unbinds nothing");
    g_kept.emplace_back(ren, surf);
}

int main() {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { std::printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
    const fs::path dir = fs::temp_directory_path() / ("options_autosave_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);
    setSettingsPath((dir / "settings.ini").string());
    SDL_Window* win = SDL_CreateWindow("autosave", 0, 0, W, H, SDL_WINDOW_HIDDEN);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE) : nullptr;
    if (!ren) { std::printf("renderer: %s\n", SDL_GetError()); return 1; }
    testOptions(ren);
    testSimThreads(ren);
    testVoice(ren);
    testControls(ren);
    for (auto [w, h] : {std::pair{1280, 720}, {1600, 1100}, {1920, 1080}, {3840, 2160}, {800, 600}, {640, 360}, {400, 300}})
        layoutCase(w, h);
    testHintBandClicks();
    for (auto& [r, sf] : g_kept) { SDL_DestroyRenderer(r); SDL_FreeSurface(sf); }
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    setSettingsPath("");
    std::error_code ec;
    fs::remove_all(dir, ec);
    SDL_Quit();
    std::printf("options_autosave_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
