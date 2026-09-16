#include "client/loadscreen.h"
#include "client/videofilter.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <vector>

#include "util/png.h"
#include "client/appquit.h"
#include "client/dev.h"
#include "client/blockfont.h"
#include "client/font.h"
#include "client/gpuvram.h"
#include "client/appglobals.h"
#include "client/settings.h"
#include "gui/gui.h"
#include "hpi/hpi.h"

namespace ta {

namespace {

// The gadget rects we care about, in the .gui's 640x480 space. Parsed from
// guis/loadscreen.gui at construction; these are the shipped values, used when the
// file is missing (a cosmetic-overrides-only install, say) so the screen still works.
struct Rect { float x, y, w, h; };

struct Geom {
    Rect loadText{17, 422, 490, 20};
    Rect loadMap{401, 4, 227, 20};
    Rect percent{520, 422, 94, 20};
    Rect mainBar{19, 444, 597, 10};
    Rect name[7] = {{8, 4, 102, 20},  {8, 42, 102, 20},  {8, 80, 102, 20}, {8, 118, 102, 20},
                    {8, 156, 102, 20}, {8, 194, 102, 20}, {8, 232, 102, 20}};
    Rect bar[7] = {{10, 26, 137, 10},  {10, 64, 137, 10},  {10, 102, 137, 10}, {10, 140, 137, 10},
                   {10, 178, 137, 10}, {10, 216, 137, 10}, {10, 254, 137, 10}};
    std::string bgGaf = "loadingbg", bgSeq = "LoadingBG";
};

Geom loadGeom(const hpi::Vfs& vfs) {
    Geom g;
    try {
        gui::Gui ui = gui::parse(vfs.read("guis/loadscreen.gui"), "guis/loadscreen.gui");
        auto take = [&](const char* n, Rect& r) {
            if (const gui::Gadget* w = ui.find(n))
                r = {float(w->x), float(w->y), float(w->w), float(w->h)};
        };
        take("LoadText", g.loadText);
        take("LoadMap", g.loadMap);
        take("Percent", g.percent);
        take("MainProgress", g.mainBar);
        for (int i = 0; i < 7; ++i) {
            take(("PlayerName" + std::to_string(i)).c_str(), g.name[i]);
            take(("PlayerProgress" + std::to_string(i)).c_str(), g.bar[i]);
        }
        if (!ui.gadgets.empty() && !ui.gadgets[0].imgs.empty()) {
            const gui::ImgRef& im = ui.gadgets[0].imgs[0];
            std::string base = im.gaf;
            if (base.size() >= 4 && base.substr(base.size() - 4) == ".gaf")
                base = base.substr(0, base.size() - 4);
            g.bgGaf = base;
            g.bgSeq = im.seq;
        }
    } catch (...) {}
    return g;
}

const Geom& geom(const hpi::Vfs& vfs) {
    static Geom g = loadGeom(vfs);
    return g;
}

// The retail bars are plain filled rectangles over the plate art. Two tones so an
// empty bar still reads as a bar rather than a hole in the background.
void drawBar(SDL_Renderer* ren, const SDL_FRect& r, float frac) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 18, 14, 8, 190);
    SDL_RenderFillRectF(ren, &r);
    SDL_FRect fill = r;
    fill.w = r.w * std::clamp(frac, 0.0f, 1.0f);
    SDL_SetRenderDrawColor(ren, 214, 176, 92, 255);
    SDL_RenderFillRectF(ren, &fill);
    SDL_SetRenderDrawColor(ren, 92, 74, 40, 255);
    SDL_RenderDrawRectF(ren, &r);
}

// The plate's arch aperture, measured from LoadingBG frame 0 (the stone frames a
// 422x351 window). Loadscreen.bik is authored at exactly that size.
constexpr float kArchX = 168, kArchY = 45, kArchW = 422, kArchH = 351;

// Case-insensitive lookup of <install>/Movies/Gui/<name>: the retail folder ships
// a mix of "Loadscreen.bik" and "SNORT4.BIK", and Linux cares.
std::filesystem::path findMovie(const std::string& name) {
    namespace fs = std::filesystem;
    if (gInstallRoot.empty()) return {};
    std::error_code ec;
    auto lower = [](std::string v) {
        for (char& c : v) c = char(std::tolower((unsigned char)c));
        return v;
    };
    fs::path dir = fs::path(gInstallRoot);
    for (const char* part : {"Movies", "Gui"}) {
        fs::path found;
        for (auto& e : fs::directory_iterator(dir, ec))
            if (lower(e.path().filename().string()) == lower(part)) { found = e.path(); break; }
        if (found.empty()) return {};
        dir = found;
    }
    for (auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file() && lower(e.path().filename().string()) == lower(name))
            return e.path();
    return {};
}

}  // namespace

LoadScreen::LoadScreen(SDL_Renderer* ren, const hpi::Vfs& vfs, const std::string& mapName,
                       Settings* settings)
    : ren_(ren), vfs_(&vfs), settings_(settings), map_(mapName) {
    if (settings_) ta::video::setDeblock(settings_->videoDeblock);
    const Geom& g = geom(vfs);
    bg_ = gafTexture(ren, vfs, g.bgGaf, g.bgSeq, 0, /*keyBlack=*/true);
    const char* drv = SDL_GetCurrentVideoDriver();
    headless_ = drv && !std::strcmp(drv, "dummy");
    done_.fill(false);
    if (!headless_) {
        if (std::filesystem::path mv = findMovie("Loadscreen.bik"); !mv.empty()) {
            std::ifstream f(mv, std::ios::binary);
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
            if (!data.empty() && movie_.open(std::move(data))) {
                movieTex_ = gpuvram::create(ren, SDL_PIXELFORMAT_RGBA32,
                                            SDL_TEXTUREACCESS_STREAMING,
                                            movie_.width(), movie_.height());
                if (movieTex_) SDL_SetTextureBlendMode(movieTex_, SDL_BLENDMODE_NONE);
                movieStartMs_ = SDL_GetTicks64();
            }
        }
    }
}

LoadScreen::~LoadScreen() {
    if (bg_) gpuvram::destroy(bg_);
    if (movieTex_) gpuvram::destroy(movieTex_);
}

void LoadScreen::setPlayers(const std::array<std::string, 8>& names, int you) {
    names_ = names;
    you_ = you;
}

void LoadScreen::setSlotDone(int slot) {
    if (slot >= 0 && slot < 8) done_[size_t(slot)] = true;
}

void LoadScreen::step(const std::string& status, int pct) {
    status_ = status;
    pct_ = std::clamp(pct, 0, 100);
    present();
}

void LoadScreen::present() {
    if (headless_ || termRequested()) return;   // nothing to show, and no pump to run
    // Keep the window responsive (and un-flagged by the compositor) while the load
    // blocks the main loop. Everything is swallowed: the screen takes no input.
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    // The caller is mid-frame inside the whole-frame AA supersample target, which it
    // will never resolve (the load blocks before the present). Draw straight to the
    // window instead, then hand the target back exactly as we found it.
    SDL_Texture* prevTarget = SDL_GetRenderTarget(ren_);
    float sx = 1, sy = 1;
    SDL_RenderGetScale(ren_, &sx, &sy);
    SDL_SetRenderTarget(ren_, nullptr);
    SDL_RenderSetScale(ren_, 1.0f, 1.0f);
    draw();
    if (const char* sp = devEnv("TA_SHOT_LOAD")) {
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(ren_, &w, &h);
        std::vector<uint8_t> px(size_t(w) * size_t(h) * 4);
        if (SDL_RenderReadPixels(ren_, nullptr, SDL_PIXELFORMAT_ABGR8888, px.data(), w * 4) == 0)
            png::write(sp, w, h, px);
    }
    SDL_RenderPresent(ren_);
    SDL_SetRenderTarget(ren_, prevTarget);
    SDL_RenderSetScale(ren_, sx, sy);
}

void LoadScreen::draw() {
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(ren_, &w, &h);
    GuiLayout lay(w, h);
    const Geom& g = geom(*vfs_);

    SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
    SDL_RenderClear(ren_);
    // The clip goes in the arch first; the stone plate then draws over it, so the
    // rounded aperture masks the rectangular video exactly the way retail's did.
    if (movieTex_ && movie_.isOpen()) {
        // The clip is driven by BOTH the wall clock and the load progress, taking
        // whichever is further along:
        //   * wall clock, so it plays at its own speed while there is time;
        //   * progress, so a FAST load still shows the whole arc of the clip
        //     instead of its first second. The screen only advances the movie when
        //     it is drawn, and a load fires as few as four step() calls, so a quick
        //     start used to get about four frames of video.
        // It never runs backwards, so the two can only ever pull it forward.
        double fps = movie_.fps() > 1 ? movie_.fps() : 15.0;
        int want = int(double(SDL_GetTicks64() - movieStartMs_) * fps / 1000.0);
        if (int total = movie_.frameCount(); total > 1) {
            int byPct = int(double(std::clamp(pct_, 0, 100)) / 100.0 * double(total - 1));
            want = std::max(want, byPct);
        }
        // Decoding is forward-only, so catching up on a big jump costs real time.
        // Measured on the shipped Loadscreen.bik (422x351, 234 frames, 20fps =
        // 11.7s of video): 0.39 ms/frame, the whole clip in 91 ms. 96 frames is
        // therefore ~37 ms of catch-up in the worst draw, and four step() calls
        // can cover the entire clip -- while still bounding the cost if someone
        // drops a much longer video in.
        constexpr int kMaxCatchUp = 96;
        if (want > movieFrame_ + kMaxCatchUp) want = movieFrame_ + kMaxCatchUp;
        while (movieFrame_ < want) {
            if (!movie_.nextFrame(movieRgba_)) { movie_.rewind(); movieStartMs_ = SDL_GetTicks64();
                                                 movieFrame_ = -1; want = 0; continue; }
            // Filter the freshly DECODED frame only. The upload below runs every frame,
            // including ones where no new frame was decoded, so filtering there would
            // smear a held frame progressively.
            if (ta::video::g_deblock)
                ta::video::deblock(movieRgba_, movie_.width(), movie_.height(), 3);
            ++movieFrame_;
        }
        if (!movieRgba_.empty())
            SDL_UpdateTexture(movieTex_, nullptr, movieRgba_.data(), movie_.width() * 4);
        SDL_FRect r = lay.rect(kArchX, kArchY, kArchW, kArchH);
        SDL_RenderCopyF(ren_, movieTex_, nullptr, &r);
    }
    if (bg_) {
        SDL_FRect r = lay.rect(0, 0, 640, 480);
        SDL_RenderCopyF(ren_, bg_, nullptr, &r);
    }

    // The retail screen sets every label in Times New Roman; fall back to the built-in
    // block font so a missing font GAF costs legibility, not the whole screen.
    static Font font;
    static bool fontTried = false;
    if (!fontTried) {
        fontTried = true;
        try { font = Font(ren_, *vfs_, "fonts/b_times new roman (100).gaf"); } catch (...) {}
        if (!font.ok())
            try { font = Font(ren_, *vfs_, "fonts/times new roman (100).gaf"); } catch (...) {}
    }
    // Font::draw lifts each glyph by its own y-offset, so a naive y clips the text
    // off the top of a gadget sitting at the screen edge. Centre it in the rect using
    // the font's real vertical bounds instead.
    auto label = [&](const std::string& s, const Rect& r, SDL_Color c) {
        if (s.empty()) return;
        if (font.ok()) {
            float sc = lay.scale * (r.h / 20.0f);
            float top = 0, th = 0;
            font.vbounds(s, sc, top, th);
            float y = lay.py(r.y) + (r.h * lay.scale - th) / 2 - top;
            font.draw(ren_, s, lay.px(r.x), y, sc, c);
        } else {
            float px = std::max(1.0f, lay.scale * 1.6f);
            drawBlockText(ren_, s, lay.px(r.x), lay.py(r.y) + (r.h * lay.scale - 7 * px) / 2, px, c);
        }
    };

    const SDL_Color gold{236, 214, 160, 255};
    label(map_, g.loadMap, gold);
    label(status_, g.loadText, gold);
    label(std::to_string(pct_) + "%", g.percent, gold);
    drawBar(ren_, lay.rect(g.mainBar.x, g.mainBar.y, g.mainBar.w, g.mainBar.h), pct_ / 100.0f);

    // One row per player in the game (retail's plate has seven), so a multiplayer
    // start shows exactly who is still loading. Our own row tracks the real
    // percentage; a peer's fills when the server says they reported in. Rows pack
    // upward -- an open slot in the middle doesn't leave a gap.
    int row = 0;
    for (int i = 0; i < 8 && row < 7; ++i) {
        if (names_[size_t(i)].empty()) continue;   // open/closed slot: no row
        label(names_[size_t(i)], g.name[row], gold);
        float frac = (i == you_) ? pct_ / 100.0f : (done_[size_t(i)] ? 1.0f : 0.0f);
        drawBar(ren_, lay.rect(g.bar[row].x, g.bar[row].y, g.bar[row].w, g.bar[row].h), frac);
        ++row;
    }
    (void)settings_;
}

}  // namespace ta
