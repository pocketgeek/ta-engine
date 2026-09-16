#include "client/mainmenu.h"
#include "client/videofilter.h"
#include "client/runtimesettings.h"
#include "client/artscale.h"
#include "client/gpuvram.h"

#include "gaf/gaf.h"
#include "gui/gui.h"
#include "net/auth.h"
#include "net/crypto.h"
#include "util/png.h"
#include "version.h"
#include "video/bink.h"
#include "client/campaignscreen.h"
#include "client/resultscreen.h"
#include "client/cursors.h"
#include "client/menumusic.h"
#include "client/hotkeys.h"
#include "client/hotkeysscreen.h"
#include "client/options.h"
#include "client/settings.h"
#include "client/dev.h"
#include "client/appquit.h"

#include <memory>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace ta {

namespace {

// Resolve a subpath under `base` case-insensitively, segment by segment. Retail
// folders (Movies, Movies/Gui, Music, ...) may be ANY case on a case-sensitive
// filesystem -- a fixed "Movies/Gui" silently finds nothing if the install uses
// "movies/gui". Returns empty if any segment is missing.
fs::path ciResolve(fs::path base, std::initializer_list<const char*> parts) {
    std::error_code ec;
    auto low = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        return s;
    };
    if (!fs::exists(base, ec)) return {};
    for (const char* want : parts) {
        std::string wl = low(want);
        fs::path found;
        for (auto& e : fs::directory_iterator(base, ec)) {
            if (low(e.path().filename().string()) == wl) { found = e.path(); break; }
        }
        if (found.empty()) return {};
        base = std::move(found);
    }
    return base;
}

// Per-door hover video state: hold the idle clip (`*4`), play hover-in (`*5`) once,
// loop (`*6`) while hovered, then play hover-out (`*7`) back to idle.
enum class DoorState { Idle, In, Loop, Out };

struct Door {
    std::string name;                 // gui gadget id
    SDL_Rect rect{};                  // 640x480 layout space
    std::string vbase;                // video basename: "machine"/"girl"/"knight"
    std::string sound;                // click sound (gui states, e.g. "skirmish.wav")
    std::string tip;                  // help caption (gui cmd, e.g. "Play the Machine")
    MainMenu::Choice action = MainMenu::Choice::None;

    SDL_Texture* gaf = nullptr;       // static fallback (GAF state-0 art)
    video::BinkVideo vid;             // current clip
    SDL_Texture* vtex = nullptr;      // streaming frame texture (video size)
    int vw = 0, vh = 0;
    double fps = 30.0, accum = 0.0;
    DoorState state = DoorState::Idle;
    bool videoOk = false;
    bool hover = false;
    std::vector<uint8_t> rgba;
};

struct Button {
    std::string name;
    SDL_Rect rect{};
    SDL_Texture* tex[3] = {nullptr, nullptr, nullptr};   // normal / hover / pressed
    int texFac[3] = {1, 1, 1};   // 2 when smooth-art built that texture at 2x. Carried
                                 // per texture, NOT read back off the global: the 2x
                                 // allocation can fail under VRAM pressure and fall back
                                 // to 1x, and laying out a 1x texture as if it were 2x
                                 // would draw the button at half size.
    std::string sound;                                   // click sound (gui states)
    std::string tip;                                     // help caption (gui cmd)
    MainMenu::Choice action = MainMenu::Choice::None;
    bool hover = false;
};

// A menu gadget's click sound is the first ".wav" entry in its gui state strings
// (the others are "Default" or empty). Case-insensitive on the extension.
inline std::string clickSound(const gui::Gadget& g) {
    for (const auto& s : g.states) {
        if (s.size() < 4) continue;
        std::string ext = s.substr(s.size() - 4);
        for (char& c : ext) c = char(std::tolower((unsigned char)c));
        if (ext == ".wav") return s;
    }
    return {};
}

}  // namespace

struct MainMenu::Impl {
    SDL_Renderer* ren;
    const hpi::Vfs& vfs;
    std::string install;

    gui::Gui gui;
    SDL_Texture* bg = nullptr;
    std::vector<Door> doors;
    std::vector<Button> buttons;
    std::unordered_map<std::string, std::string> bikByLower;   // lowercased name -> path

    // Click SFX: one queue-driven device (all menu click WAVs share a format,
    // u8/11025/mono), and each sound's volume-scaled PCM keyed by filename. This is
    // separate from the shared MenuMusic device so a click mixes over the music.
    SDL_AudioDeviceID sfxDev_ = 0;
    std::unordered_map<std::string, std::vector<uint8_t>> sfxPcm_;

    // The Options overlay, opened from the SETTINGS menu (see run()).
    std::unique_ptr<OptionsScreen> options_;
    std::unique_ptr<HotkeysScreen> hotkeys_;   // opened from the SETTINGS menu -> CONTROLS
    // The SETTINGS menu overlay -- the lower-right menu button opens THIS (OPTIONS / CONTROLS)
    // instead of jumping straight into Options, mirroring the in-game Esc GAME MENU.
    bool settingsMenu_ = false;
    SDL_FRect setBtnRect_[4]{};        // OPTIONS / CONTROLS / BENCHMARK / LOAD REPLAY (set each render)
    bool benchMenu_ = false;          // benchmark intensity submenu (opened from SETTINGS)
    bool replayMenu_ = false;         // replay picker (opened from SETTINGS)
    int replayScroll_ = 0;            // first listed row
    std::vector<std::string> replayFiles_;   // full paths, newest first
    std::string chosenReplay_;
    std::string pendingReplayError;   // set via setReplayError before run()
    std::string replayError_;         // shown on the picker
    SDL_FRect replayBtnRect_[10]{};   // one per listed row (set each render)
    SDL_FRect benchBtnRect_[6]{};      // LOW..EXTRA ABSURD hit-rects (set each render)
    int chosenBenchmark_ = 0;         // picked benchmark level 1..6 (0 = none)

    // The campaign / mission picker, opened from the PlayStory door (see run()). When
    // the player picks a mission it closes and chosenMission_ names the bundle stem.
    std::unique_ptr<CampaignScreen> campaign_;
    std::string chosenMission_;    // set when a campaign mission is picked (bundle stem)
    std::string chosenCampaign_;   // ...and which campaign it belongs to (id)

    // The retail mouse cursor on the front-end, same art as in-game. Loaded once on the
    // first run(). We never restore the OS arrow on teardown (see run()).
    CursorSet cursors_;
    bool cursorsInit_ = false;

    Impl(SDL_Renderer* r, const hpi::Vfs& v, std::string in)
        : ren(r), vfs(v), install(std::move(in)) {}
    ~Impl() {
        if (bg) gpuvram::destroy(bg);
        for (auto& d : doors) { if (d.gaf) gpuvram::destroy(d.gaf);
                                if (d.vtex) gpuvram::destroy(d.vtex); }
        for (auto& b : buttons) for (auto* t : b.tex) if (t) gpuvram::destroy(t);
        if (sfxDev_) SDL_CloseAudioDevice(sfxDev_);
    }

    // ---- click SFX ------------------------------------------------------------

    // Load one click WAV from sounds/ and cache its PCM. Opens the shared SFX
    // device from the first WAV (all four share u8/11025/mono, so no conversion).
    void loadSfx(const std::string& name) {
        if (name.empty() || sfxPcm_.count(name)) return;
        std::vector<uint8_t> raw;
        try { raw = vfs.read("sounds/" + name); } catch (...) { return; }
        if (raw.empty()) return;
        SDL_RWops* rw = SDL_RWFromConstMem(raw.data(), int(raw.size()));
        SDL_AudioSpec spec{}; Uint8* buf = nullptr; Uint32 len = 0;
        if (!rw || !SDL_LoadWAV_RW(rw, 1, &spec, &buf, &len)) return;
        if (!sfxDev_) {
            SDL_InitSubSystem(SDL_INIT_AUDIO);
            SDL_AudioSpec want = spec, have{};
            want.callback = nullptr;   // queue-driven
            // Small buffer so a queued click plays with minimal latency. The whole
            // click is queued at once (never a partial fill), so a short device
            // period can't underrun it. ~128/11025 ~= 12ms vs ~46ms at 512.
            want.samples = 128;
            sfxDev_ = ta::openAudioDevice(0, &want, &have, 0);
            if (sfxDev_) SDL_PauseAudioDevice(sfxDev_, 0);
        }
        // Scale to a UI level that sits above the (soft, ~45/128) menu music.
        // These WAVs are unsigned 8-bit (silence == 128), so scale around 128.
        std::vector<uint8_t> pcm(len);
        for (Uint32 i = 0; i < len; ++i) {
            int s = 128 + (int(buf[i]) - 128) * 100 / 128;
            pcm[i] = uint8_t(s < 0 ? 0 : s > 255 ? 255 : s);
        }
        SDL_FreeWAV(buf);
        sfxPcm_[name] = std::move(pcm);
    }

    void playSfx(const std::string& name) {
        if (!sfxDev_ || name.empty()) return;
        auto it = sfxPcm_.find(name);
        if (it == sfxPcm_.end()) return;
        SDL_ClearQueuedAudio(sfxDev_);   // retrigger cleanly on rapid clicks
        SDL_QueueAudio(sfxDev_, it->second.data(), Uint32(it->second.size()));
    }

    // Play the click sound of whatever door/button is currently hovered.
    void playHoveredSound() {
        for (auto& d : doors) if (d.hover) { playSfx(d.sound); return; }
        for (auto& b : buttons) if (b.hover) { playSfx(b.sound); return; }
    }

    // A click that transitions away destroys this MainMenu (closing the SFX
    // device), so let the short click finish first -- keep rendering so the menu
    // doesn't freeze. Bounded well above the longest click (~140ms).
    void flushSfx(int winW, int winH) {
        if (!sfxDev_) return;
        for (int i = 0; i < 30 && SDL_GetQueuedAudioSize(sfxDev_) > 0; ++i) {
            render(winW, winH);
            SDL_RenderPresent(ren);
            SDL_Delay(10);
        }
    }

    // ---- asset loading --------------------------------------------------------

    gaf::Palette palette(const std::string& gaf) {
        std::string pp = "anims/" + gaf + ".pcx";
        try { return gaf::Palette::fromBytes(vfs.read(pp), pp); } catch (...) {}
        try { return gaf::Palette::fromBytes(vfs.read("palettes/guipal.pal"),
                                             "palettes/guipal.pal"); } catch (...) {}
        return {};
    }

    // Turn a GAF sequence/frame into an SDL texture (same pipeline as the in-game HUD).
    SDL_Texture* gafTex(const std::string& gafName, const std::string& seq, int frame,
                        int* appliedFactor = nullptr) {
        if (gafName.empty() || seq.empty()) return nullptr;
        std::string base = gafName;
        if (base.size() >= 4 && base.substr(base.size() - 4) == ".gaf")
            base = base.substr(0, base.size() - 4);
        std::string gp = "anims/" + base + ".gaf";
        try {
            auto pal = palette(base);
            for (auto& sq : gaf::load(vfs.read(gp), pal, -1, gp)) {
                if (sq.name != seq) continue;
                if (frame < 0 || size_t(frame) >= sq.frames.size()) frame = 0;
                if (sq.frames.empty()) return nullptr;
                auto& f = sq.frames[size_t(frame)];
                if (f.width == 0 || f.height == 0) return nullptr;
                return ta::art::makeTexture(ren, f.rgba, f.width, f.height, appliedFactor);
            }
        } catch (...) {}
        return nullptr;
    }

    // Index the loose door videos under <install>/Movies/Gui by lowercased name so a
    // "machine5.bik" / "GIRL5.BIK" mix resolves on a case-sensitive filesystem.
    void indexVideos() {
        std::error_code ec;
        fs::path dir = ciResolve(fs::path(install), {"Movies", "Gui"});
        if (dir.empty()) {
            std::fprintf(stderr, "menu: no Movies/Gui under %s -- door videos disabled "
                         "(doors stay on their static art)\n", install.c_str());
            return;
        }
        for (auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file()) continue;
            std::string n = e.path().filename().string();
            std::string low = n;
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            bikByLower[low] = e.path().string();
        }
        std::fprintf(stderr, "menu: indexed %zu door video(s) from %s\n",
                     bikByLower.size(), dir.string().c_str());
    }

    std::string findBik(const std::string& base, int n) const {
        auto it = bikByLower.find(base + std::to_string(n) + ".bik");
        return it == bikByLower.end() ? std::string() : it->second;
    }

    // ---- door video state machine --------------------------------------------

    void setDoorTex(Door& d) {
        // Exactly once per DECODED frame: setDoorTex is only ever called straight after a
        // successful nextFrame(). Filtering at the upload instead would re-filter a held
        // frame every time it was re-uploaded, smearing it a little more each pass.
        if (ta::video::g_deblock) ta::video::deblock(d.rgba, d.vw, d.vh, 3);
        if (d.vw <= 0 || d.vh <= 0 || d.rgba.empty()) return;
        if (!d.vtex) {
            d.vtex = gpuvram::create(ren, SDL_PIXELFORMAT_ABGR8888,
                                       SDL_TEXTUREACCESS_STREAMING, d.vw, d.vh);
            // Bink frames are opaque -- ignore any decoder alpha. Linear filtering
            // smooths the low-res door clips (~150-220px) when scaled to the window.
            SDL_SetTextureBlendMode(d.vtex, SDL_BLENDMODE_NONE);
            SDL_SetTextureScaleMode(d.vtex, SDL_ScaleModeLinear);
        }
        SDL_UpdateTexture(d.vtex, nullptr, d.rgba.data(), d.vw * 4);
    }

    // Open clip <base><n>.bik and show its first frame; returns false if unavailable.
    bool startClip(Door& d, int n, DoorState st) {
        std::string path = findBik(d.vbase, n);
        if (path.empty()) return false;
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
        if (!d.vid.open(std::move(bytes))) return false;
        d.vw = d.vid.width(); d.vh = d.vid.height();
        d.fps = d.vid.fps() > 1.0 ? d.vid.fps() : 30.0;
        d.accum = 0.0;
        d.state = st;
        if (d.vid.nextFrame(d.rgba)) { setDoorTex(d); return true; }
        return false;
    }

    void updateDoor(Door& d, double dt) {
        if (!d.videoOk) return;
        if (d.hover && (d.state == DoorState::Idle || d.state == DoorState::Out))
            startClip(d, 5, DoorState::In);
        else if (!d.hover && (d.state == DoorState::In || d.state == DoorState::Loop))
            startClip(d, 7, DoorState::Out);

        if (d.state == DoorState::Idle) return;   // hold the idle frame
        d.accum += dt;
        double spf = 1.0 / std::max(1.0, d.fps);
        int guard = 0;
        while (d.accum >= spf && guard++ < 8) {
            d.accum -= spf;
            if (d.vid.nextFrame(d.rgba)) { setDoorTex(d); continue; }
            // clip ended -> advance the state machine
            if (d.state == DoorState::In) startClip(d, 6, DoorState::Loop);
            else if (d.state == DoorState::Loop) { d.vid.rewind();
                if (d.vid.nextFrame(d.rgba)) setDoorTex(d); }
            else if (d.state == DoorState::Out) { startClip(d, 4, DoorState::Idle); break; }
        }
    }

    // ---- setup ----------------------------------------------------------------

    void load() {
        try { gui = gui::parse(vfs.read("guis/mainmenu.gui"), "guis/mainmenu.gui"); }
        catch (...) {}
        indexVideos();

        // Background: the root gadget's first image (MainScreen.gaf/MainBG).
        if (!gui.gadgets.empty() && !gui.gadgets[0].imgs.empty()) {
            auto& im = gui.gadgets[0].imgs[0];
            bg = gafTex(im.gaf, im.seq, im.frame);
            // The background is the opaque base layer -- ignore any palette-index-0
            // "transparency" in the map art so it doesn't punch through to black.
            if (bg) SDL_SetTextureBlendMode(bg, SDL_BLENDMODE_NONE);
        }

        struct DoorSpec { const char* gadget; const char* vbase; Choice act; };
        const DoorSpec specs[] = {
            {"PlayComputer", "machine", Choice::SinglePlayer},
            {"PlayStory",    "girl",    Choice::Campaign},
            {"PlayPlayer",   "knight",  Choice::Multiplayer},
            // The fourth door. Its gadget is in mainmenu.gui and its hover videos
            // ship as SNORT4..7.BIK; we simply never wired it up, so the credits
            // were unreachable from the front end.
            {"Credits",      "snort",   Choice::Credits},
        };
        for (auto& s : specs) {
            const gui::Gadget* g = gui.find(s.gadget);
            if (!g) continue;
            Door d;
            d.name = s.gadget;
            d.rect = {g->x, g->y, g->w, g->h};
            d.vbase = s.vbase;
            d.action = s.act;
            d.sound = clickSound(*g);
            d.tip = g->cmd;   // gui cmd doubles as the hover help caption
            loadSfx(d.sound);
            if (!g->imgs.empty()) d.gaf = gafTex(g->imgs[0].gaf, g->imgs[0].seq, g->imgs[0].frame);
            doors.push_back(std::move(d));
        }
        // Open each door's idle clip so it rests on the animated idle frame.
        for (auto& d : doors) d.videoOk = video::BinkVideo::available() && startClip(d, 4, DoorState::Idle);

        struct BtnSpec { const char* gadget; Choice act; };
        const BtnSpec btns[] = {{"Options", Choice::Options}, {"Exit", Choice::Exit}};
        for (auto& b : btns) {
            const gui::Gadget* g = gui.find(b.gadget);
            if (!g) continue;
            Button bt;
            bt.name = b.gadget;
            bt.rect = {g->x, g->y, g->w, g->h};
            bt.action = b.act;
            bt.sound = clickSound(*g);
            bt.tip = g->cmd;   // gui cmd doubles as the hover help caption
            if (b.act == Choice::Exit) bt.tip = "Exit";   // retail's cmd is "Exit to Windows"
            if (b.act == Choice::Options) bt.tip = "Settings";   // opens the SETTINGS menu now
            loadSfx(bt.sound);
            for (int i = 0; i < 3 && i < int(g->imgs.size()); ++i)
                bt.tex[i] = gafTex(g->imgs[size_t(i)].gaf, g->imgs[size_t(i)].seq,
                                   g->imgs[size_t(i)].frame, &bt.texFac[i]);
            buttons.push_back(std::move(bt));
        }
    }

    // ---- render ---------------------------------------------------------------

    // Uniform 640x480 -> window scale with letterbox centering.
    void layout(int winW, int winH, float& scale, float& offX, float& offY) const {
        scale = std::min(winW / 640.0f, winH / 480.0f);
        offX = (winW - 640.0f * scale) * 0.5f;
        offY = (winH - 480.0f * scale) * 0.5f;
    }
    SDL_FRect toScreen(const SDL_Rect& r, float s, float ox, float oy) const {
        return {ox + r.x * s, oy + r.y * s, r.w * s, r.h * s};
    }

    void render(int winW, int winH) {
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        float s, ox, oy; layout(winW, winH, s, ox, oy);
        if (bg) { SDL_FRect r{ox, oy, 640 * s, 480 * s}; SDL_RenderCopyF(ren, bg, nullptr, &r); }
        // TA_NODOORVID=1 forces the static GAF door art instead of the clips, so
        // the two can be compared directly -- retail authored both, so the art is
        // the closest thing to a reference for how bright a door should look.
        static const bool kNoDoorVid = ta::devEnv("TA_NODOORVID") != nullptr;
        for (auto& d : doors) {
            // A door with no gui art of its own has nothing to show at rest: the gui
            // gives the other three a GAF state image (singlemachine/bodgirl/
            // multiknight) but gives Credits three empty image refs, and MainBG
            // already has that door's carved "K" painted into it. So its resting
            // look IS the background, and clip 4 -- a Bink-compressed copy of the
            // same carving -- must not be pinned over it. Measured against MainBG at
            // 1:1, SNORT4's thin gold strokes come back 24 luma short (neutrals match
            // to ~2), which is exactly the rectangle that cut through the letter.
            // The hover clips still play; only the idle still is suppressed.
            const bool idleNoArt = (d.state == DoorState::Idle && !d.gaf);
            if (d.videoOk && d.vtex && !kNoDoorVid && !idleNoArt) {
                // Like the buttons, the door video is authored bigger than its gui
                // hotspot and anchored at the gadget origin -- draw it at native size,
                // NOT stretched to the (smaller) hotspot. Stretching squished it badly:
                // knight is 221x250 vs a 161-wide hotspot, and its 221px width is
                // authored to run from x=419 to the 640px screen edge.
                SDL_Rect nat{d.rect.x, d.rect.y, d.vw, d.vh};
                SDL_FRect r = toScreen(nat, s, ox, oy);
                SDL_RenderCopyF(ren, d.vtex, nullptr, &r);
            } else if (d.gaf) {
                SDL_FRect r = toScreen(d.rect, s, ox, oy);
                SDL_RenderCopyF(ren, d.gaf, nullptr, &r);
            }
        }
        for (auto& b : buttons) {
            const int ti = (b.hover && b.tex[1]) ? 1 : 0;
            SDL_Texture* t = b.tex[ti];
            if (!t) continue;
            // The button art is bigger than its gui hotspot rect and is authored to
            // exactly cover MainBG's button-footprint box; draw it at native size
            // from the gadget origin, not stretched to the (smaller) hotspot rect.
            int tw = 0, th = 0; SDL_QueryTexture(t, nullptr, nullptr, &tw, &th);
            // The texture may have been built at 2x by the smooth-art option; the LAYOUT
            // is in 1x gui units, so divide it back out. Without this the button art
            // would silently render at double size with smoothing on.
            const int fac = b.texFac[ti];
            SDL_Rect nat{b.rect.x, b.rect.y, tw / fac, th / fac};
            SDL_FRect r = toScreen(nat, s, ox, oy);
            SDL_RenderCopyF(ren, t, nullptr, &r);
        }

        // Bottom-centre placards, in the retail gui's 640x480 coordinates:
        // the "Version" static (174,408,295,14) shows the build version, and its
        // "HelpText" sibling (172,441,296,31) shows the hovered gadget's caption --
        // a fixed help/status line, the retail convention, not a floating tooltip.
        // (blockText covers A-Z/0-9/.:- ; spaces render as a gap.)
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        auto placard = [&](const std::string& text, int dx, int dw, int dy, float dpx, SDL_Color c) {
            float tw = float(text.size()) * 6 * dpx;                        // design-space width
            shadowText(text, ox + (dx + (dw - tw) / 2) * s, oy + dy * s, dpx * s, c);
        };
        placard(std::string("VERSION ") + ta::kVersion, 174, 295, 409, 2.0f, {185, 180, 160, 220});
        const std::string* tip = nullptr;
        for (auto& d : doors) if (d.hover && !d.tip.empty()) { tip = &d.tip; break; }
        if (!tip) for (auto& b : buttons) if (b.hover && !b.tip.empty()) { tip = &b.tip; break; }
        if (tip) placard(*tip, 172, 296, 448, 2.5f, {240, 238, 245, 235});
    }

    // ---- minimal block font + multiplayer server-select overlay ---------------
    // A dropdown: the default server on top, then every server that has connected
    // successfully before (Settings::knownServers), then CUSTOM with a text field.
    static constexpr const char* kDefaultServer = "ta.pgnet.us";
    bool serverSelect = false;
    std::vector<std::string> serverItems;   // dropdown rows (default + remembered)
    int serverSel = 0;                      // selected row; == serverItems.size() -> CUSTOM
    std::string serverText = "127.0.0.1";   // custom-entry text
    std::vector<SDL_FRect> serverRects;     // row hit-rects (rebuilt each render)
    SDL_FRect serverBoxRect{};              // custom text-box hit-rect
    SDL_FRect serverConnectRect{}, serverBackRect{};   // button hit-rects
    std::string serverError;                // shown in red after a failed connect
    std::string pendingConnectError;        // set via setConnectError before run()
    // Account login. Servers require a name and password; an unused name is
    // registered on the spot, so there is no separate sign-up screen (the server
    // decides which of the two happened and says so). The password lives here
    // only until main.cpp has used it, and is wiped the moment it has.
    std::string loginUser, loginPass;
    SDL_FRect loginUserRect{}, loginPassRect{};
    // Which text field has the caret: 0 = none (the server list), 1 = the CUSTOM
    // address box, 2 = username, 3 = password.
    int field = 2;

    void openServerSelect(const Settings* settings) {
        serverItems.clear();
        serverItems.push_back(kDefaultServer);
        auto ieq = [](const std::string& a, const std::string& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
                    return false;
            return true;
        };
        if (settings)
            for (const auto& sv : settings->knownServers) {
                bool dup = false;
                for (const auto& it : serverItems) if (ieq(it, sv)) { dup = true; break; }
                if (!dup && serverItems.size() < 7) serverItems.push_back(sv);
            }
        serverSel = 0;
        serverError.clear();
        // The name is remembered between sessions; the password never is.
        if (settings && loginUser.empty()) loginUser = settings->accountName;
        loginPass.clear();
        field = loginUser.empty() ? 2 : 3;   // land on whichever still needs typing
        serverSelect = true;
        SDL_StartTextInput();
    }
    bool serverCustom() const { return serverSel == int(serverItems.size()); }
    void setServerSel(int i) {
        bool wasCustom = serverCustom();
        serverSel = std::clamp(i, 0, int(serverItems.size()));
        // Picking CUSTOM puts the caret in the address box; leaving it hands the
        // caret back to the account fields rather than dropping it entirely.
        if (serverCustom() && !wasCustom) field = 1;
        if (!serverCustom() && wasCustom && field == 1) field = 2;
    }
    // Tab order: the address box only participates while CUSTOM is selected.
    void cycleField(int dir) {
        const int lo = serverCustom() ? 1 : 2;
        field = field < lo ? lo : field;
        field = lo + ((field - lo + dir) % (4 - lo) + (4 - lo)) % (4 - lo);
    }
    std::string* activeField() {
        switch (field) {
            case 1: return serverCustom() ? &serverText : nullptr;
            case 2: return &loginUser;
            case 3: return &loginPass;
            default: return nullptr;
        }
    }
    std::string serverChoice() const {
        return serverCustom() ? serverText : serverItems[size_t(serverSel)];
    }

    static const uint8_t* glyph5x7(char c) {
        static const std::unordered_map<char, std::array<uint8_t, 5>> F = {
            {'0',{0x3E,0x51,0x49,0x45,0x3E}},{'1',{0x00,0x42,0x7F,0x40,0x00}},
            {'2',{0x42,0x61,0x51,0x49,0x46}},{'3',{0x21,0x41,0x45,0x4B,0x31}},
            {'4',{0x18,0x14,0x12,0x7F,0x10}},{'5',{0x27,0x45,0x45,0x45,0x39}},
            {'6',{0x3C,0x4A,0x49,0x49,0x30}},{'7',{0x01,0x71,0x09,0x05,0x03}},
            {'8',{0x36,0x49,0x49,0x49,0x36}},{'9',{0x06,0x49,0x49,0x29,0x1E}},
            {'A',{0x7E,0x11,0x11,0x11,0x7E}},{'B',{0x7F,0x49,0x49,0x49,0x36}},
            {'C',{0x3E,0x41,0x41,0x41,0x22}},{'D',{0x7F,0x41,0x41,0x22,0x1C}},
            {'E',{0x7F,0x49,0x49,0x49,0x41}},{'F',{0x7F,0x09,0x09,0x09,0x01}},
            {'G',{0x3E,0x41,0x49,0x49,0x7A}},{'H',{0x7F,0x08,0x08,0x08,0x7F}},
            {'I',{0x00,0x41,0x7F,0x41,0x00}},{'J',{0x20,0x40,0x41,0x3F,0x01}},
            {'K',{0x7F,0x08,0x14,0x22,0x41}},{'L',{0x7F,0x40,0x40,0x40,0x40}},
            {'M',{0x7F,0x02,0x0C,0x02,0x7F}},{'N',{0x7F,0x04,0x08,0x10,0x7F}},
            {'O',{0x3E,0x41,0x41,0x41,0x3E}},{'P',{0x7F,0x09,0x09,0x09,0x06}},
            {'Q',{0x3E,0x41,0x51,0x21,0x5E}},{'R',{0x7F,0x09,0x19,0x29,0x46}},
            {'S',{0x46,0x49,0x49,0x49,0x31}},{'T',{0x01,0x01,0x7F,0x01,0x01}},
            {'U',{0x3F,0x40,0x40,0x40,0x3F}},{'V',{0x1F,0x20,0x40,0x20,0x1F}},
            {'W',{0x7F,0x20,0x18,0x20,0x7F}},{'X',{0x63,0x14,0x08,0x14,0x63}},
            {'Y',{0x07,0x08,0x70,0x08,0x07}},{'Z',{0x61,0x51,0x49,0x45,0x43}},
            {'.',{0x00,0x60,0x60,0x00,0x00}},{':',{0x00,0x36,0x36,0x00,0x00}},
            {'-',{0x08,0x08,0x08,0x08,0x08}},
            // Account names allow '_', and the login messages need sentence
            // punctuation; without these they render as gaps.
            {'_',{0x40,0x40,0x40,0x40,0x40}},{',',{0x00,0x50,0x30,0x00,0x00}},
            {'!',{0x00,0x00,0x5F,0x00,0x00}},{'\'',{0x00,0x00,0x03,0x00,0x00}},
            {'?',{0x02,0x01,0x51,0x09,0x06}},{'(',{0x00,0x1C,0x22,0x41,0x00}},
            {')',{0x00,0x41,0x22,0x1C,0x00}},{'/',{0x20,0x10,0x08,0x04,0x02}},
            {'*',{0x14,0x08,0x3E,0x08,0x14}},{'+',{0x08,0x08,0x3E,0x08,0x08}},
        };
        auto it = F.find(c);
        return it == F.end() ? nullptr : it->second.data();
    }
    void blockText(const std::string& str, float x, float y, float px, SDL_Color c) {
        SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
        float cx = x;
        for (char ch : str) {
            const uint8_t* cols = glyph5x7(char(std::toupper((unsigned char)ch)));
            if (!cols) { cx += 4 * px; continue; }
            for (int col = 0; col < 5; ++col)
                for (int row = 0; row < 7; ++row)
                    if (cols[col] & (1 << row)) {
                        SDL_FRect r{cx + col * px, y + row * px, px, px};
                        SDL_RenderFillRectF(ren, &r);
                    }
            cx += 6 * px;
        }
    }
    // Block text with a 1px dark drop-shadow so it stays legible over any menu art.
    void shadowText(const std::string& s, float x, float y, float px, SDL_Color c) {
        float o = std::max(1.0f, px * 0.4f);
        blockText(s, x + o, y + o, px, {0, 0, 0, 170});
        blockText(s, x, y, px, c);
    }

    void renderServerSelect(int winW, int winH) {
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 160);
        SDL_FRect dim{0, 0, float(winW), float(winH)}; SDL_RenderFillRectF(ren, &dim);
        const int rows = int(serverItems.size()) + 1;   // + CUSTOM
        const float rowH = 40, pw = 560;
        const float errH = serverError.empty() ? 0 : 26;
        // + the two account fields (label + box each) and the sign-up hint line.
        float ph = 78 + errH + rows * rowH + (serverCustom() ? 56 : 0) + 152 + 64;
        float x0 = (winW - pw) / 2, y0 = (winH - ph) / 2;
        SDL_SetRenderDrawColor(ren, 28, 30, 40, 245);
        SDL_FRect panel{x0, y0, pw, ph}; SDL_RenderFillRectF(ren, &panel);
        SDL_SetRenderDrawColor(ren, 150, 150, 175, 255); SDL_RenderDrawRectF(ren, &panel);
        blockText("CONNECT TO SERVER", x0 + 30, y0 + 24, 3.0f, {210, 205, 160, 255});
        if (!serverError.empty()) {
            // Server messages ("that password is not right") are sentences, so wrap
            // rather than truncating mid-word at the panel edge.
            std::string e = serverError.substr(0, 96);
            size_t cut = e.size() > 46 ? e.rfind(' ', 46) : std::string::npos;
            blockText(e.substr(0, cut == std::string::npos ? 46 : cut), x0 + 30, y0 + 50, 1.8f,
                      {235, 120, 110, 255});
            if (cut != std::string::npos)
                blockText(e.substr(cut + 1), x0 + 30, y0 + 64, 1.8f, {235, 120, 110, 255});
        }
        // Dropdown rows: the default server, remembered servers, then CUSTOM.
        serverRects.clear();
        float ry = y0 + 66 + errH;
        for (int i = 0; i < rows; ++i) {
            SDL_FRect r{x0 + 30, ry, pw - 60, rowH - 6};
            bool sel = (i == serverSel);
            SDL_SetRenderDrawColor(ren, sel ? 52 : 16, sel ? 66 : 18, sel ? 96 : 26, 255);
            SDL_RenderFillRectF(ren, &r);
            SDL_SetRenderDrawColor(ren, sel ? 160 : 90, sel ? 185 : 100, sel ? 235 : 130, 255);
            SDL_RenderDrawRectF(ren, &r);
            std::string label = i < int(serverItems.size()) ? serverItems[size_t(i)]
                                                            : std::string("CUSTOM...");
            blockText(label, r.x + 14, r.y + 9, 2.4f,
                      sel ? SDL_Color{235, 240, 250, 255} : SDL_Color{175, 180, 195, 255});
            if (i == 0)   // mark the official default server
                blockText("DEFAULT", r.x + r.w - 110, r.y + 13, 1.6f, {150, 165, 145, 255});
            serverRects.push_back(r);
            ry += rowH;
        }
        // CUSTOM selected: the address entry field below the list.
        serverBoxRect = {0, 0, 0, 0};
        if (serverCustom()) {
            SDL_SetRenderDrawColor(ren, 16, 18, 26, 255);
            SDL_FRect box{x0 + 30, ry + 4, pw - 60, 46}; SDL_RenderFillRectF(ren, &box);
            SDL_SetRenderDrawColor(ren, 120, 140, 180, 255); SDL_RenderDrawRectF(ren, &box);
            blockText(serverText, box.x + 12, box.y + 14, 3.0f, {230, 235, 245, 255});
            if ((SDL_GetTicks() / 500) % 2 == 0) {   // blinking caret
                float cx = box.x + 12 + float(serverText.size()) * 6 * 3.0f;
                SDL_SetRenderDrawColor(ren, 230, 235, 245, 255);
                SDL_FRect car{cx, box.y + 12, 3, 22}; SDL_RenderFillRectF(ren, &car);
            }
            serverBoxRect = box;
            ry += 56;
        }
        // ---- account: name + password -------------------------------------
        // One pair of fields does both jobs. A name the server has never seen is
        // registered as you sign in, so there is no separate create-account step
        // -- which also means the hint below has to say so, or a new player will
        // sit here looking for a SIGN UP button that does not exist.
        ry += 10;
        auto textField = [&](const char* label, const std::string& text, bool mask,
                             bool focused, SDL_FRect& out) {
            blockText(label, x0 + 30, ry, 1.8f, {170, 178, 195, 255});
            SDL_FRect box{x0 + 30, ry + 18, pw - 60, 42};
            SDL_SetRenderDrawColor(ren, 16, 18, 26, 255);
            SDL_RenderFillRectF(ren, &box);
            SDL_SetRenderDrawColor(ren, focused ? 150 : 90, focused ? 175 : 100,
                                   focused ? 225 : 130, 255);
            SDL_RenderDrawRectF(ren, &box);
            const float px = 2.8f, adv = 6 * px;
            float tx = box.x + 12;
            if (mask) {
                // Draw the password as filled pips rather than glyphs: a real
                // character would be readable over someone's shoulder, and the
                // block font has no bullet.
                SDL_SetRenderDrawColor(ren, 215, 220, 235, 255);
                for (size_t i = 0; i < text.size(); ++i) {
                    SDL_FRect pip{tx + float(i) * adv + 1, box.y + 17, px * 3, px * 3};
                    SDL_RenderFillRectF(ren, &pip);
                }
            } else {
                blockText(text, tx, box.y + 12, px, {230, 235, 245, 255});
            }
            if (focused && (SDL_GetTicks() / 500) % 2 == 0) {
                float cx = tx + float(text.size()) * adv;
                SDL_SetRenderDrawColor(ren, 230, 235, 245, 255);
                SDL_FRect car{cx, box.y + 10, 3, 22}; SDL_RenderFillRectF(ren, &car);
            }
            out = box;
            ry += 70;
        };
        textField("ACCOUNT NAME", loginUser, false, field == 2, loginUserRect);
        textField("PASSWORD", loginPass, true, field == 3, loginPassRect);
        blockText("A NEW NAME IS REGISTERED WHEN YOU SIGN IN", x0 + 30, ry - 2, 1.6f,
                  {140, 152, 138, 255});
        ry += 10;
        // CONNECT / BACK buttons (clicking a row only SELECTS -- connecting is
        // always an explicit action, so a stray click can't fire a connection).
        auto button = [&](float bx, const char* label, SDL_FRect& out, bool bright) {
            SDL_FRect b{bx, ry + 12, 150, 36};
            SDL_SetRenderDrawColor(ren, bright ? 46 : 30, bright ? 66 : 34, bright ? 100 : 46, 255);
            SDL_RenderFillRectF(ren, &b);
            SDL_SetRenderDrawColor(ren, bright ? 160 : 110, bright ? 185 : 120,
                                   bright ? 235 : 150, 255);
            SDL_RenderDrawRectF(ren, &b);
            float tw = float(std::strlen(label)) * 6 * 2.2f;
            blockText(label, b.x + (b.w - tw) / 2 + 2, b.y + 11, 2.2f,
                      bright ? SDL_Color{230, 238, 250, 255} : SDL_Color{185, 190, 205, 255});
            out = b;
        };
        button(x0 + 30, "CONNECT", serverConnectRect, true);
        button(x0 + 196, "BACK", serverBackRect, false);
    }

    // The SETTINGS menu overlay: OPTIONS / CONTROLS, styled like the in-game GAME MENU.
    // Records the two hit-rects in setBtnRect_ for the click handler in run().
    void renderSettingsMenu(int winW, int winH) {
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_FRect dim{0, 0, float(winW), float(winH)};
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 150);
        SDL_RenderFillRectF(ren, &dim);
        const int nBtn = 4;
        const float bw = 320, bh = 54, gap = 16, pad = 34, titlePx = 3.2f;
        const float titleH = 7 * titlePx + 22;
        const float pw = bw + pad * 2;
        const float ph = pad * 2 + titleH + nBtn * bh + (nBtn - 1) * gap + 28;
        const float px0 = (winW - pw) / 2, py0 = (winH - ph) / 2;
        SDL_FRect panel{px0, py0, pw, ph};
        SDL_SetRenderDrawColor(ren, 26, 28, 36, 240); SDL_RenderFillRectF(ren, &panel);
        SDL_SetRenderDrawColor(ren, 120, 130, 160, 255); SDL_RenderDrawRectF(ren, &panel);
        // blockText advances 6*px per glyph (5 + 1 gap); estimate width for centring.
        auto tw = [](const std::string& s, float px) { return s.empty() ? 0.0f : (s.size() * 6.0f - 1.0f) * px; };
        shadowText("SETTINGS", px0 + (pw - tw("SETTINGS", titlePx)) / 2, py0 + pad, titlePx, {235, 225, 180, 255});
        int mx = 0, my = 0; SDL_GetMouseState(&mx, &my);
        { float lx, ly; SDL_RenderWindowToLogical(ren, mx, my, &lx, &ly); mx = int(lx); my = int(ly); }
        const char* labels[nBtn] = {"OPTIONS", "CONTROLS", "BENCHMARK", "LOAD REPLAY"};
        float by = py0 + pad + titleH;
        for (int i = 0; i < nBtn; ++i) {
            SDL_FRect r{px0 + pad, by, bw, bh};
            setBtnRect_[i] = r;
            bool hot = mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h;
            SDL_SetRenderDrawColor(ren, hot ? 90 : 60, hot ? 110 : 66, hot ? 150 : 86, 255);
            SDL_RenderFillRectF(ren, &r);
            SDL_SetRenderDrawColor(ren, hot ? 180 : 90, hot ? 200 : 100, hot ? 240 : 130, 255);
            SDL_RenderDrawRectF(ren, &r);
            float lpx = 2.5f, lw = tw(labels[i], lpx);
            blockText(labels[i], r.x + (bw - lw) / 2, r.y + (bh - 7 * lpx) / 2, lpx, {228, 232, 242, 255});
            by += bh + gap;
        }
        shadowText("ESC - BACK", px0 + pad, by + 2, 1.8f, {150, 155, 175, 255});
    }

    // Benchmark intensity submenu: 5 spawn-rate levels. Records benchBtnRect_ for run().
    // Scan the user's config directory for saved replays, newest first. That is where
    // every human player's client writes its own .tarep (see saveReplayFile) -- beside
    // settings.ini -- so the picker lists exactly what this machine has recorded.
    void scanReplays() {
        replayFiles_.clear();
        replayScroll_ = 0;
        std::string dir = ta::settingsPath();
        const size_t cut = dir.find_last_of("/\\");
        if (cut == std::string::npos) return;
        dir.erase(cut + 1);
        std::vector<std::pair<std::filesystem::file_time_type, std::string>> found;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!e.is_regular_file(ec)) continue;
            if (e.path().extension() != ".tarep") continue;
            found.push_back({e.last_write_time(ec), e.path().string()});
        }
        // Newest first: the replay you just played is the one you want to watch.
        std::sort(found.begin(), found.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        for (auto& f : found) replayFiles_.push_back(std::move(f.second));
    }

    // The replay picker. Same shape as the benchmark submenu, but its rows come from
    // the filesystem, so it scrolls and can be empty.
    void renderReplayMenu(int winW, int winH) {
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_FRect dim{0, 0, float(winW), float(winH)};
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 150);
        SDL_RenderFillRectF(ren, &dim);
        const int kRows = 10;
        const int shown = std::min(kRows, int(replayFiles_.size()) - replayScroll_);
        const int nBtn = std::max(1, shown);
        const float bw = 620, bh = 44, gap = 10, pad = 34, titlePx = 3.2f;
        const float titleH = 7 * titlePx + 22;
        const float pw = bw + pad * 2;
        const float ph = pad * 2 + titleH + nBtn * bh + (nBtn - 1) * gap + 28;
        const float px0 = (winW - pw) / 2, py0 = (winH - ph) / 2;
        SDL_FRect panel{px0, py0, pw, ph};
        SDL_SetRenderDrawColor(ren, 26, 28, 36, 240); SDL_RenderFillRectF(ren, &panel);
        SDL_SetRenderDrawColor(ren, 120, 130, 160, 255); SDL_RenderDrawRectF(ren, &panel);
        auto tw = [](const std::string& t, float px) { return t.empty() ? 0.0f : (t.size() * 6.0f - 1.0f) * px; };
        shadowText("LOAD REPLAY", px0 + (pw - tw("LOAD REPLAY", titlePx)) / 2, py0 + pad,
                   titlePx, {235, 225, 180, 255});
        int mx = 0, my = 0; SDL_GetMouseState(&mx, &my);
        { float lx, ly; SDL_RenderWindowToLogical(ren, mx, my, &lx, &ly); mx = int(lx); my = int(ly); }
        for (auto& r : replayBtnRect_) r = SDL_FRect{0, 0, 0, 0};
        float by = py0 + pad + titleH;
        if (replayFiles_.empty()) {
            // Say WHERE they would be, so an empty list is an answer rather than a dead end.
            const std::string m1 = "NO REPLAYS SAVED YET";
            const std::string m2 = "FINISH A GAME AND ITS REPLAY IS SAVED AUTOMATICALLY";
            blockText(m1, px0 + (pw - tw(m1, 2.2f)) / 2, by + 6, 2.2f, {225, 215, 175, 255});
            blockText(m2, px0 + (pw - tw(m2, 1.5f)) / 2, by + 34, 1.5f, {150, 155, 175, 255});
        }
        for (int i = 0; i < shown; ++i) {
            const std::string& full = replayFiles_[size_t(replayScroll_ + i)];
            SDL_FRect r{px0 + pad, by, bw, bh};
            replayBtnRect_[i] = r;
            bool hot = mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h;
            SDL_SetRenderDrawColor(ren, hot ? 90 : 60, hot ? 110 : 66, hot ? 150 : 86, 255);
            SDL_RenderFillRectF(ren, &r);
            SDL_SetRenderDrawColor(ren, hot ? 180 : 90, hot ? 200 : 100, hot ? 240 : 130, 255);
            SDL_RenderDrawRectF(ren, &r);
            std::string name = std::filesystem::path(full).stem().string();
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return char(std::toupper(c)); });
            float lpx = 1.9f;
            if (tw(name, lpx) > bw - 20) name = name.substr(0, size_t((bw - 20) / (6 * lpx)));
            blockText(name, r.x + 12, r.y + (bh - 7 * lpx) / 2, lpx, {228, 232, 242, 255});
            by += bh + gap;
        }
        if (!replayError_.empty()) {
            std::string e = replayError_;
            std::transform(e.begin(), e.end(), e.begin(),
                           [](unsigned char c) { return char(std::toupper(c)); });
            if (tw(e, 1.6f) > pw - pad * 2) e = e.substr(0, size_t((pw - pad * 2) / (6 * 1.6f)));
            blockText(e, px0 + (pw - tw(e, 1.6f)) / 2, by + 2, 1.6f, {235, 130, 110, 255});
            by += 22;
        }
        std::string foot = "ESC - BACK";
        if (int(replayFiles_.size()) > kRows)
            foot += "     WHEEL - SCROLL  (" + std::to_string(replayScroll_ + 1) + "-" +
                    std::to_string(replayScroll_ + shown) + " OF " +
                    std::to_string(replayFiles_.size()) + ")";
        shadowText(foot, px0 + pad, by + 2, 1.8f, {150, 155, 175, 255});
    }

    void renderBenchMenu(int winW, int winH) {
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_FRect dim{0, 0, float(winW), float(winH)};
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 150);
        SDL_RenderFillRectF(ren, &dim);
        const int nBtn = 6;
        const float bw = 560, bh = 50, gap = 13, pad = 34, titlePx = 3.2f;
        const float titleH = 7 * titlePx + 22;
        const float pw = bw + pad * 2;
        const float ph = pad * 2 + titleH + nBtn * bh + (nBtn - 1) * gap + 28;
        const float px0 = (winW - pw) / 2, py0 = (winH - ph) / 2;
        SDL_FRect panel{px0, py0, pw, ph};
        SDL_SetRenderDrawColor(ren, 26, 28, 36, 240); SDL_RenderFillRectF(ren, &panel);
        SDL_SetRenderDrawColor(ren, 120, 130, 160, 255); SDL_RenderDrawRectF(ren, &panel);
        auto tw = [](const std::string& s, float px) { return s.empty() ? 0.0f : (s.size() * 6.0f - 1.0f) * px; };
        shadowText("BENCHMARK", px0 + (pw - tw("BENCHMARK", titlePx)) / 2, py0 + pad, titlePx, {235, 225, 180, 255});
        int mx = 0, my = 0; SDL_GetMouseState(&mx, &my);
        { float lx, ly; SDL_RenderWindowToLogical(ren, mx, my, &lx, &ly); mx = int(lx); my = int(ly); }
        const char* labels[nBtn] = {
            "LOW  -  1 UNIT / FACTION / 1S", "MEDIUM  -  EVERY 0.5S", "HIGH  -  EVERY 0.25S",
            "VERY HIGH  -  EVERY 0.125S", "ABSURD  -  EVERY 0.0625S",
            "EXTRA ABSURD +WTH  -  EVERY 0.03125S"};
        float by = py0 + pad + titleH;
        for (int i = 0; i < nBtn; ++i) {
            SDL_FRect r{px0 + pad, by, bw, bh};
            benchBtnRect_[i] = r;
            bool hot = mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h;
            SDL_SetRenderDrawColor(ren, hot ? 90 : 60, hot ? 110 : 66, hot ? 150 : 86, 255);
            SDL_RenderFillRectF(ren, &r);
            SDL_SetRenderDrawColor(ren, hot ? 180 : 90, hot ? 200 : 100, hot ? 240 : 130, 255);
            SDL_RenderDrawRectF(ren, &r);
            float lpx = 1.9f, lw = tw(labels[i], lpx);
            blockText(labels[i], r.x + (bw - lw) / 2, r.y + (bh - 7 * lpx) / 2, lpx, {228, 232, 242, 255});
            by += bh + gap;
        }
        shadowText("ESC - BACK", px0 + pad, by + 2, 1.8f, {150, 155, 175, 255});
    }

    void screenshot(int winW, int winH, const std::string& path) {
        std::vector<uint8_t> px(size_t(winW) * size_t(winH) * 4);
        if (SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ABGR8888, px.data(), winW * 4) == 0)
            png::write(path, winW, winH, px);
    }

    // ---- input ----------------------------------------------------------------

    void updateHover(int mx, int my, int winW, int winH) {
        float s, ox, oy; layout(winW, winH, s, ox, oy);
        float gx = (mx - ox) / s, gy = (my - oy) / s;   // to 640x480 space
        auto in = [&](const SDL_Rect& r) {
            return gx >= r.x && gx < r.x + r.w && gy >= r.y && gy < r.y + r.h;
        };
        for (auto& d : doors) d.hover = in(d.rect);
        for (auto& b : buttons) b.hover = in(b.rect);
    }

    Choice clicked() const {
        for (auto& d : doors) if (d.hover) return d.action;
        for (auto& b : buttons) if (b.hover) return b.action;
        return Choice::None;
    }
};

MainMenu::MainMenu(SDL_Renderer* ren, const hpi::Vfs& vfs, std::string install)
    : d_(new Impl(ren, vfs, std::move(install))) { d_->load(); }
MainMenu::~MainMenu() { delete d_; }

void MainMenu::setConnectError(const std::string& msg) { d_->pendingConnectError = msg; }
void MainMenu::setReplayError(const std::string& msg) { d_->pendingReplayError = msg; }

MainMenu::Choice MainMenu::run(const std::string& shotPath, std::string* serverOut,
                               MenuMusic* music, Settings* settings) {
    if (settings) ta::video::setDeblock(settings->videoDeblock);
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(d_->ren, &w, &h);

    if (!shotPath.empty()) {
        for (auto& dr : d_->doors) d_->updateDoor(dr, 0.0);
        d_->render(w, h);
        // Debug: TA_SHOT_CAMPAIGN captures the campaign picker overlay for tests.
        if (const char* sc = settings ? ta::devEnv("TA_SHOT_CAMPAIGN") : nullptr) {
            int tab = std::atoi(sc);   // TA_SHOT_CAMPAIGN=1 -> Iron Plague tab
            Settings tmp = *settings;
            // Mark a mix of Iron Plague missions completed so the shot shows DONE + PLAY
            // rows and the next-up highlight (nothing is locked either way).
            if (tab == 1)
                for (int i = 0; i < 12; ++i) tmp.campaignCompleted["the iron plague"].insert(i);
            CampaignScreen cs(d_->ren, d_->vfs, tmp, tab);
            cs.render(w, h);
        }
        // Debug: TA_SHOT_SERVER captures the CONNECT dropdown (2 = CUSTOM selected,
        // showing the address field; 3 = failed-connect error shown); seeds sample
        // remembered servers if none saved.
        if (settings && ta::devEnv("TA_SHOT_SERVER")) {
            Settings tmp = *settings;
            if (tmp.knownServers.empty())
                tmp.knownServers = {"192.168.1.50:7677", "example.dyndns.org"};
            d_->openServerSelect(&tmp);
            int m = std::atoi(ta::devEnv("TA_SHOT_SERVER"));
            if (m == 2) d_->setServerSel(int(d_->serverItems.size()));   // CUSTOM view
            if (m == 3) d_->serverError = "COULD NOT CONNECT TO TA.PGNET.US";
            if (m == 4) {   // signed-in state: name filled, password part-typed
                d_->loginUser = "curtis";
                d_->loginPass = "hunter2hunter";
                d_->field = 3;
            }
            if (m == 5) {   // the server refused the password
                d_->loginUser = "curtis";
                d_->serverError = "that password is not right";
                d_->field = 3;
            }
            d_->renderServerSelect(w, h);
        }
        d_->screenshot(w, h, shotPath);
        // Debug: TA_SHOT_RESULT captures the victory result screen (saves to its path).
        if (settings && ta::devEnv("TA_SHOT_RESULT"))
            ResultScreen::run(d_->ren, d_->vfs, true, "MISSION 1", true, settings, nullptr);
        return Choice::None;
    }

    // Drop any mouse events queued by the previous screen. Returning here from the
    // lobby's MAIN MENU button (which fires on mouse-DOWN) leaves its matching
    // mouse-UP in the queue; without this it would land as a phantom click on a
    // door and snap straight back into the game.
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP);

    // Front-end mouse cursor: the same retail art as in-game. Load once, then hide the OS
    // arrow while the menu draws its own; if the art is missing, show the OS arrow as a
    // fallback (a prior screen may have hidden it). We deliberately never restore the OS
    // arrow on teardown -- both the menu and the game keep it hidden and draw a custom
    // cursor, so restoring on a transition only flashes the arrow during the next
    // screen's load; window destruction returns the desktop cursor at app exit.
    // The menu owns its OWN CursorSet -- the game's warmCursors() does nothing for it --
    // so this load must happen here, before the loop, and hand over the settings so the
    // hardware reconstruction does not land on the first menu frame.
    if (!d_->cursorsInit_) {
        d_->cursorsInit_ = true;
        d_->cursors_.load(d_->ren, d_->vfs, settings);
    }
    SDL_ShowCursor(d_->cursors_.ok() ? SDL_DISABLE : SDL_ENABLE);

    // Returning from a failed connect: reopen the dropdown with the error shown,
    // so the player lands back where they were instead of at a bare menu.
    if (!d_->pendingConnectError.empty()) {
        d_->openServerSelect(settings);
        d_->serverError = d_->pendingConnectError;
        d_->pendingConnectError.clear();
    }
    // Same idea for a refused replay: land back on the PICKER with the reason, not at
    // a bare menu and certainly not in the server dialog.
    if (!d_->pendingReplayError.empty()) {
        d_->scanReplays();
        d_->replayMenu_ = true;
        d_->replayError_ = d_->pendingReplayError;
        d_->pendingReplayError.clear();
    }

    Uint64 prev = SDL_GetPerformanceCounter();
    const double freq = double(SDL_GetPerformanceFrequency());
    for (;;) {
        if (ta::termRequested()) return Choice::Exit;   // SIGTERM/SIGINT -> quit the app
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) continue;   // ignore the WM close button; use the Exit door
            // Mouse events arrive in window points; our hit-rects are in output pixels.
            // Map with SDL_RenderWindowToLogical -- reliable on Wayland fractional scaling,
            // where the size getters report window==drawable yet pointer events are in a
            // smaller logical space. Without this, after a game re-commits the surface at a
            // non-1:1 scale, what's drawn no longer matches where clicks land. Mirrors the
            // in-game input path (main.cpp).
            if (e.type == SDL_MOUSEMOTION) {
                float lx, ly;
                SDL_RenderWindowToLogical(d_->ren, e.motion.x, e.motion.y, &lx, &ly);
                e.motion.x = int(lx); e.motion.y = int(ly);
            } else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
                float lx, ly;
                SDL_RenderWindowToLogical(d_->ren, e.button.x, e.button.y, &lx, &ly);
                e.button.x = int(lx); e.button.y = int(ly);
            }

            if (d_->serverSelect) {   // multiplayer: server + account sign-in
                auto connect = [&]() -> bool {
                    std::string sv = d_->serverChoice();
                    if (sv.empty()) return false;   // empty custom field: stay
                    // Check the account locally before spending a round trip on it,
                    // and say the same things the server would.
                    std::string why;
                    if (!ta::auth::validUsername(d_->loginUser, &why)) {
                        d_->serverError = d_->loginUser.empty() ? "enter an account name" : why;
                        d_->field = 2;
                        return false;
                    }
                    if (d_->loginPass.empty()) {
                        d_->serverError = "enter your password";
                        d_->field = 3;
                        return false;
                    }
                    SDL_StopTextInput();
                    if (serverOut) *serverOut = sv;
                    return true;
                };
                if (e.type == SDL_TEXTINPUT) {
                    std::string* f = d_->activeField();
                    for (const char* p = e.text.text; f && *p; ++p) {
                        unsigned char ch = (unsigned char)*p;
                        // Each field takes only what it can legitimately hold: an
                        // address, an account name (auth.h's rules), or a password
                        // (any printable character, since nothing parses it).
                        if (d_->field == 1) {
                            if (f->size() < 64 &&
                                (std::isalnum(ch) || ch == '.' || ch == ':' || ch == '-'))
                                *f += char(ch);
                        } else if (d_->field == 2) {
                            if (f->size() < ta::auth::kMaxUsername &&
                                (std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.'))
                                *f += char(ch);
                        } else {
                            if (f->size() < ta::auth::kMaxPassword && ch >= 0x20 && ch < 0x7f)
                                *f += char(ch);
                        }
                    }
                } else if (e.type == SDL_KEYDOWN) {
                    SDL_Keycode k = e.key.keysym.sym;
                    const bool shift = (e.key.keysym.mod & KMOD_SHIFT) != 0;
                    if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                        if (connect()) return Choice::Multiplayer;
                    }
                    // UP/DOWN still walk the server list; TAB walks the text fields,
                    // which is what a keyboard user reaches for between name and
                    // password.
                    if (k == SDLK_UP) d_->setServerSel(d_->serverSel - 1);
                    if (k == SDLK_DOWN) d_->setServerSel(d_->serverSel + 1);
                    if (k == SDLK_TAB) d_->cycleField(shift ? -1 : 1);
                    if (k == SDLK_ESCAPE) {
                        d_->serverSelect = false;
                        ta::crypto::wipe(d_->loginPass);
                        SDL_StopTextInput();
                    }
                    if (k == SDLK_BACKSPACE) {
                        if (std::string* f = d_->activeField(); f && !f->empty()) f->pop_back();
                    }
                } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                    float mx = float(e.button.x), my = float(e.button.y);
                    auto in = [&](const SDL_FRect& r) {
                        return r.w > 0 && mx >= r.x && mx <= r.x + r.w &&
                               my >= r.y && my <= r.y + r.h;
                    };
                    // Rows only SELECT; connecting is the explicit button (or ENTER).
                    for (size_t i = 0; i < d_->serverRects.size(); ++i)
                        if (in(d_->serverRects[i])) { d_->setServerSel(int(i)); break; }
                    if (in(d_->serverBoxRect)) {
                        d_->setServerSel(int(d_->serverItems.size()));   // focus the field
                        d_->field = 1;
                    }
                    if (in(d_->loginUserRect)) d_->field = 2;
                    if (in(d_->loginPassRect)) d_->field = 3;
                    if (in(d_->serverConnectRect)) {
                        if (connect()) return Choice::Multiplayer;
                    }
                    if (in(d_->serverBackRect)) {
                        d_->serverSelect = false;
                        ta::crypto::wipe(d_->loginPass);
                        SDL_StopTextInput();
                    }
                }
                continue;   // swallow everything else while the dropdown is open
            }

            if (d_->hotkeys_) {   // hotkey overlay sits on top of Options
                if (d_->hotkeys_->input(e, w, h)) d_->hotkeys_.reset();   // BACK / Esc -> Options
                continue;
            }
            if (d_->options_) {   // Options overlay is up: route everything to it
                if (d_->options_->input(e, w, h)) d_->options_.reset();   // BACK / Esc (SAVE is explicit)
                continue;
            }
            if (d_->settingsMenu_) {   // the SETTINGS menu (OPTIONS / CONTROLS) is up
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) { d_->settingsMenu_ = false; }
                else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                    float fx = float(e.button.x), fy = float(e.button.y);
                    auto hit = [&](const SDL_FRect& r) { return fx >= r.x && fx <= r.x + r.w && fy >= r.y && fy <= r.y + r.h; };
                    SDL_Renderer* ren = d_->ren;
                    if (hit(d_->setBtnRect_[0])) {          // OPTIONS -> the audio/display screen
                        d_->settingsMenu_ = false;
                        d_->options_ = std::make_unique<OptionsScreen>(ren, *settings,
                            [ren, music, settings, fsWas = settings->fullscreen,
                             vsWas = settings->vsync]() mutable {
                                if (music) music->setVolume(settings->masterVol, settings->bgmVol);
                                ta::applyRuntimeSettings(*settings);   // incl. DEFAULTS
                                // onChange fires on EVERY control tweak (a volume-slider drag
                                // fires it many times a second). ANY window/renderer reconfigure
                                // here re-commits the Wayland surface -- which rescales it and
                                // drops pointer-button events, so later clicks miss or die. Touch
                                // the surface ONLY when that display setting actually changed since
                                // this Options screen opened. (Compare the VALUE, not
                                // SDL_GetWindowFlags, which is unreliable on Wayland.)
                                if (settings->fullscreen != fsWas) {
                                    if (SDL_Window* wnd = SDL_RenderGetWindow(ren))
                                        SDL_SetWindowFullscreen(wnd, settings->fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                                    fsWas = settings->fullscreen;
                                }
                                if (settings->vsync != vsWas) {
                                    SDL_RenderSetVSync(ren, settings->vsync ? 1 : 0);
                                    vsWas = settings->vsync;
                                }
                            },
                            [settings] { saveSettings(*settings); }, 0, [] {},
                            [music] { if (music) music->reopen(); });   // live output-device switch
                    } else if (hit(d_->setBtnRect_[1])) {   // CONTROLS -> hotkey rebinding
                        d_->settingsMenu_ = false;
                        d_->hotkeys_ = std::make_unique<HotkeysScreen>(ren, *settings,
                            [] {}, [settings] { saveSettings(*settings); });
                    } else if (hit(d_->setBtnRect_[3])) {   // LOAD REPLAY -> the picker
                        d_->settingsMenu_ = false;
                        d_->scanReplays();                 // rescan on every open
                        d_->replayMenu_ = true;
                    } else if (hit(d_->setBtnRect_[2])) {   // BENCHMARK -> intensity submenu
                        d_->settingsMenu_ = false;
                        d_->benchMenu_ = true;
                    }
                }
                continue;
            }
            if (d_->replayMenu_) {   // replay picker
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
                    d_->replayMenu_ = false;
                    d_->settingsMenu_ = true;   // back to SETTINGS, not out of the menu
                } else if (e.type == SDL_MOUSEWHEEL) {
                    const int maxTop = std::max(0, int(d_->replayFiles_.size()) - 10);
                    d_->replayScroll_ = std::clamp(d_->replayScroll_ - e.wheel.y, 0, maxTop);
                } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                    float fx = float(e.button.x), fy = float(e.button.y);
                    auto hit = [&](const SDL_FRect& r) {
                        return r.w > 0 && fx >= r.x && fx <= r.x + r.w && fy >= r.y && fy <= r.y + r.h;
                    };
                    for (int i = 0; i < 10; ++i)
                        if (hit(d_->replayBtnRect_[i])) {
                            const size_t k = size_t(d_->replayScroll_ + i);
                            if (k >= d_->replayFiles_.size()) break;
                            d_->chosenReplay_ = d_->replayFiles_[k];
                            d_->replayError_.clear();
                            d_->replayMenu_ = false;
                            d_->flushSfx(w, h);
                            return Choice::Replay;
                        }
                }
                continue;
            }

            if (d_->benchMenu_) {   // benchmark intensity submenu (LOW..ABSURD)
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) { d_->benchMenu_ = false; }
                else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                    float fx = float(e.button.x), fy = float(e.button.y);
                    auto hit = [&](const SDL_FRect& r) { return fx >= r.x && fx <= r.x + r.w && fy >= r.y && fy <= r.y + r.h; };
                    for (int i = 0; i < 6; ++i)
                        if (hit(d_->benchBtnRect_[i])) {
                            d_->chosenBenchmark_ = i + 1;   // 1=Low .. 6=Extra Absurd
                            d_->benchMenu_ = false;
                            d_->flushSfx(w, h);
                            return Choice::Benchmark;
                        }
                }
                continue;
            }

            if (d_->campaign_) {   // campaign picker is up: route everything to it
                if (d_->campaign_->input(e, w, h)) {
                    bool picked = d_->campaign_->picked();
                    if (picked) { d_->chosenMission_ = d_->campaign_->pickedStem();
                                  d_->chosenCampaign_ = d_->campaign_->pickedCampaign(); }
                    d_->campaign_.reset();
                    if (picked) { d_->flushSfx(w, h); return Choice::Campaign; }
                }
                continue;
            }

            // Esc does nothing at the title screen -- quitting is only via the Exit
            // door (the WM close button is ignored too). Esc never exits the game.
            if (e.type == SDL_MOUSEMOTION)
                d_->updateHover(e.motion.x, e.motion.y, w, h);
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                // Doors/buttons act on the PRESS: the sound and the action both fire
                // here, so the menu feels immediate.
                d_->updateHover(e.button.x, e.button.y, w, h);
                d_->playHoveredSound();
                Choice c = d_->clicked();
                if (c != Choice::None) {
                    // We acted on the press -- drop this click's matching release so
                    // it can't land as a phantom click on the overlay/lobby/game we're
                    // about to switch to (the mirror of the entry flush above).
                    SDL_PumpEvents();
                    SDL_FlushEvents(SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP);
                }
                if (c == Choice::Multiplayer) d_->openServerSelect(settings);
                else if (c == Choice::Options && settings) {
                    // The lower-right menu button opens the SETTINGS menu (OPTIONS / CONTROLS),
                    // NOT the Options screen directly -- see the settingsMenu_ router above.
                    d_->settingsMenu_ = true;
                }
                else if (c == Choice::Campaign && settings) {
                    // The story intro plays HERE, on the PlayStory door itself, the
                    // first time it is opened in a session -- which is exactly what
                    // retail does (KINGDOMS.icd 0x4a5120 dispatches on the gadget
                    // name "PlayStory" and plays Movies\intro.bik behind a flag it
                    // sets straight after, 0x62d680). That flag is a plain global
                    // with one reader and one writer and is never persisted, so
                    // retail replays the intro once per LAUNCH, not once ever.
                    static bool introPlayed = false;
                    if (!introPlayed) {
                        introPlayed = true;
                        // Hush the front-end track under the movie's own audio, the
                        // same way the campaign briefing movie does.
                        if (music) music->setVolume(0, 0);
                        playIntro(d_->ren, d_->install, "intro.bik");
                        if (music) music->setVolume(settings->masterVol, settings->bgmVol);
                    }
                    // Open the campaign / mission picker in place; a picked mission
                    // returns Choice::Campaign (handled by the campaign_ router above).
                    d_->campaign_ = std::make_unique<CampaignScreen>(d_->ren, d_->vfs, *settings);
                }
                else if (c != Choice::None && c != Choice::Campaign) {
                    d_->flushSfx(w, h);   // let the click sound finish before we tear down
                    return c;
                }
            }
        }
        SDL_GetRendererOutputSize(d_->ren, &w, &h);
        Uint64 now = SDL_GetPerformanceCounter();
        double dt = double(now - prev) / freq;
        prev = now;
        for (auto& dr : d_->doors) d_->updateDoor(dr, dt);
        if (music) music->poll();
        d_->render(w, h);
        if (d_->serverSelect) d_->renderServerSelect(w, h);
        if (d_->settingsMenu_) d_->renderSettingsMenu(w, h);
        if (d_->replayMenu_) d_->renderReplayMenu(w, h);
        if (d_->benchMenu_) d_->renderBenchMenu(w, h);
        if (d_->options_) d_->options_->render(w, h);
        if (d_->hotkeys_) d_->hotkeys_->render(w, h);   // above Options
        if (d_->campaign_) d_->campaign_->render(w, h);
        // Draw the cursor last so it sits above the doors and the overlays. With the
        // hardware-cursor option the OS tracks the pointer (smooth under load); otherwise
        // hide the OS arrow and draw our own into the frame.
        if (d_->cursors_.ok()) {
            int sc = settings ? settings->cursorScale : 1;
            if (settings && settings->hardwareCursor &&
                d_->cursors_.applyHardware(CursorId::Normal, sc)) {
                SDL_ShowCursor(SDL_ENABLE);
            } else {
                SDL_ShowCursor(SDL_DISABLE);
                int mx = 0, my = 0; SDL_GetMouseState(&mx, &my);
                float lmx, lmy; SDL_RenderWindowToLogical(d_->ren, mx, my, &lmx, &lmy);
                d_->cursors_.draw(d_->ren, CursorId::Normal, int(lmx), int(lmy), sc);
            }
        }
        SDL_RenderPresent(d_->ren);
        SDL_Delay(1);
    }
}

const std::string& MainMenu::chosenMission() const { return d_->chosenMission_; }
const std::string& MainMenu::chosenAccount() const { return d_->loginUser; }
const std::string& MainMenu::chosenPassword() const { return d_->loginPass; }
void MainMenu::clearPassword() { ta::crypto::wipe(d_->loginPass); }

int MainMenu::chosenBenchmarkLevel() const { return d_->chosenBenchmark_; }
const std::string& MainMenu::chosenCampaign() const { return d_->chosenCampaign_; }
const std::string& MainMenu::chosenReplay() const { return d_->chosenReplay_; }

void MainMenu::playIntro(SDL_Renderer* ren, const std::string& install, const char* nameLower) {
    if (!video::BinkVideo::available() || install.empty() || !ren) return;
    // Headless / dummy video: no display to play to (and no input to skip it), so a
    // movie would just block for its full length. Skip it.
    if (const char* drv = SDL_GetCurrentVideoDriver(); drv && !std::strcmp(drv, "dummy")) return;
    // Locate <install>/Movies/<name> case-insensitively (retail ships LOGO.BIK etc.).
    std::string path;
    std::error_code ec;
    fs::path moviesDir = ciResolve(fs::path(install), {"Movies"});
    if (moviesDir.empty()) return;
    for (auto& e : fs::directory_iterator(moviesDir, ec)) {
        if (!e.is_regular_file()) continue;
        std::string low = e.path().filename().string();
        std::transform(low.begin(), low.end(), low.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        if (low == nameLower) { path = e.path().string(); break; }
    }
    if (path.empty()) return;
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    video::BinkVideo vid;
    if (!vid.open(std::move(bytes))) return;
    const int vw = vid.width(), vh = vid.height();
    if (vw <= 0 || vh <= 0) return;
    SDL_Texture* tex = gpuvram::create(ren, SDL_PIXELFORMAT_ABGR8888,
                                         SDL_TEXTUREACCESS_STREAMING, vw, vh);
    if (!tex) return;
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);   // smooth when scaled to the window
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
    // Play the clip's soundtrack (if it has one) on a queue-driven device -- the video
    // decode buffers its audio, and we keep it fed each frame. The video is paced by
    // wall clock (below), and the audio is decoded to the same timestamps, so they
    // stay in sync without an explicit audio clock.
    SDL_AudioDeviceID adev = 0;
    if (vid.audioChannels() > 0 && vid.audioRate() > 0) {
        SDL_InitSubSystem(SDL_INIT_AUDIO);
        SDL_AudioSpec want{}, have{};
        want.freq = vid.audioRate();
        want.format = AUDIO_S16SYS;
        want.channels = uint8_t(vid.audioChannels());
        want.samples = 1024;
        want.callback = nullptr;   // queue-driven
        adev = ta::openAudioDevice(0, &want, &have, 0);
        if (adev) SDL_PauseAudioDevice(adev, 0);
    }
    const double fps = vid.fps() > 1.0 ? vid.fps() : 30.0;
    const double aBytesPerSec = adev ? double(vid.audioRate()) * vid.audioChannels() * 2.0 : 0.0;
    std::vector<uint8_t> rgba, apcm;
    // Hide the OS arrow for the fullscreen movie (restored below; the menu then hides
    // it again for its own cursor). Only around real playback -- the early returns above
    // for "no video" never touch the cursor state.
    SDL_ShowCursor(SDL_DISABLE);
    const Uint64 start = SDL_GetTicks64();
    long queuedTotal = 0;   // total audio bytes ever queued (for the audio clock)
    // Trailing-black trim: the retail LOGO.BIK fades out ~3s before its stream ends,
    // then just holds pure black. Cut to the menu once a few frames in a row are pure
    // black instead of sitting on the dead tail. The fade's last visible frames decode
    // to ~8/255, so a low threshold trims only the black hold, not the fade; and the
    // lone black fade-in frame at the start can't reach the run length.
    auto meanBrightness = [](const std::vector<uint8_t>& px, int w, int h) {
        if (int(px.size()) < w * h * 4) return 255;
        long sum = 0; int cnt = 0;
        for (int y = 0; y < h; y += 16)
            for (int x = 0; x < w; x += 16) {
                const uint8_t* p = &px[(size_t(y) * w + x) * 4];
                sum += p[0] + p[1] + p[2]; cnt += 3;
            }
        return cnt ? int(sum / cnt) : 255;
    };
    constexpr int kBlackLevel = 2;      // per-channel mean at/below this counts as black
    constexpr int kBlackEndFrames = 4;  // this many black frames in a row => end the clip
    int blackRun = 0;
    int frame = 0;
    bool skip = false;
    for (;;) {
        if (ta::termRequested()) break;   // SIGTERM/SIGINT -> abandon the intro
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_KEYDOWN || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_QUIT)
                skip = true;   // any key / click / close skips straight to the menu
        if (skip) break;
        // Pace the video off the AUDIO clock when there's sound (how much has actually
        // played), else off the wall clock. Bink front-loads audio packets, so the
        // decoded audio runs ahead of the video -- following the playback position
        // keeps them in sync. Decode up to that frame (dropping frames if behind).
        double t;
        if (adev && aBytesPerSec > 0.0)
            t = double(queuedTotal - long(SDL_GetQueuedAudioSize(adev))) / aBytesPerSec;
        else
            t = double(SDL_GetTicks64() - start) / 1000.0;
        const int want = int(t * fps);
        bool ended = false;
        bool decoded = false;   // did THIS iteration produce a new frame?
        while (frame <= want) {
            if (!vid.nextFrame(rgba)) { ended = true; break; }
            ++frame;
            decoded = true;
            if (meanBrightness(rgba, vw, vh) <= kBlackLevel) {
                if (++blackRun >= kBlackEndFrames) { ended = true; break; }   // trailing black -> done
            } else {
                blackRun = 0;
            }
        }
        // Keep the audio device fed with whatever this iteration decoded.
        apcm.clear();
        vid.drainAudio(apcm);
        if (adev && !apcm.empty()) { SDL_QueueAudio(adev, apcm.data(), Uint32(apcm.size()));
                                     queuedTotal += long(apcm.size()); }
        if (ended || rgba.empty()) break;
        // ONLY a newly decoded frame. The clip runs at ~15 fps while this loop runs at
        // display rate, so most iterations decode nothing and `rgba` is the frame already
        // on screen -- filtering it again each time would smooth the same pixels over and
        // over, and by an amount that depended on the machine's frame rate.
        if (decoded && ta::video::g_deblock) ta::video::deblock(rgba, vw, vh, 3);
        SDL_UpdateTexture(tex, nullptr, rgba.data(), vw * 4);
        int ww = 0, wh = 0;
        SDL_GetRendererOutputSize(ren, &ww, &wh);
        const float sc = std::min(float(ww) / vw, float(wh) / vh);   // fit + letterbox, no distortion
        SDL_FRect dst{(ww - vw * sc) * 0.5f, (wh - vh * sc) * 0.5f, vw * sc, vh * sc};
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderCopyF(ren, tex, nullptr, &dst);
        SDL_RenderPresent(ren);
        SDL_Delay(4);
    }
    if (adev) SDL_CloseAudioDevice(adev);
    gpuvram::destroy(tex);
    // Leave the OS arrow hidden -- the menu that follows keeps its cursor hidden and draws
    // the custom one, so restoring here would only flash the arrow before the menu appears.
    // Drop the skip key/click so it doesn't leak as a phantom press into the menu.
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_KEYDOWN, SDL_MOUSEBUTTONUP);
}

}  // namespace ta
