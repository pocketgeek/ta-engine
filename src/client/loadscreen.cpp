#include "client/loadscreen.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "util/png.h"
#include "client/appquit.h"
#include "client/artscale.h"
#include "client/dev.h"
#include "client/gpuvram.h"
#include "client/guiart.h"
#include "client/tabeamdraw.h"
#include "client/taguiview.h"
#include "gaf/gaf.h"
#include "hpi/hpi.h"

namespace ta {

namespace {

// TotalA.exe 0x4984dd..0x498c79: per part, the label at (90, y) and a bar from x 205,
// 20 px high, 3.5 px per percent (pct * 7 / 2), with LIGHTBAR frame 0 over it at
// (205, y). The y of each part, in LoadPart order.
constexpr int kPartY[kLoadParts] = {135, 177, 218, 262, 304, 347};
constexpr const char* kPartLabel[kLoadParts] = {"Textures", "Terrain", "Units",
                                                "Animation", "3D Data", "Explosions"};
constexpr float kLabelX = 90, kBarX = 205, kBarH = 20;

}  // namespace

// TotalA.exe's GAF text (0x4a50e0): the current GAF font -- anims\hattfont12.gaf, made
// current at startup (0x491558..0x491592) -- in its own colours; the colour the caller
// set applies only to the .FNT fallback. A non-zero `light` draws each opaque pixel as
// PALETTE.LHT[light][pixel] (0x4b8310 -> 0x4cc3d0), the 32-row light table loaded at
// startup (0x42e260). Glyphs are kept as palette indices and turned into textures per
// light row on first use.
struct LitFont {
    struct Glyph { int w = 0, h = 0, xoff = 0, yoff = 0; std::vector<int16_t> idx; };   // -1 transparent
    std::array<Glyph, 256> glyphs;
    std::vector<uint8_t> lht;   // 32 x 256
    gaf::Palette pal{};
    std::array<std::array<SDL_Texture*, 256>, 32> tex{};
    int baseline = 12;          // the font loader's yoff shift: 'I''s height (0x4aee56)

    ~LitFont() {
        for (auto& row : tex)
            for (auto* t : row)
                if (t) gpuvram::destroy(t);
    }
    bool load(const hpi::Vfs& vfs, const gaf::Palette& screen) {
        gaf::Palette index{};
        for (int i = 0; i < 256; ++i) {
            index.rgba[i][0] = uint8_t(i); index.rgba[i][1] = 0;
            index.rgba[i][2] = 0; index.rgba[i][3] = 255;
        }
        const auto seqs = gaf::load(vfs.read("anims/hattfont12.gaf"), index, -1, "anims/hattfont12.gaf");
        if (seqs.empty()) return false;
        const auto& s = seqs.front();
        for (size_t ch = 0; ch < s.frames.size() && ch < 256; ++ch) {
            const auto& f = s.frames[ch];
            Glyph& g = glyphs[ch];
            g.w = f.width; g.h = f.height; g.xoff = f.xoff; g.yoff = f.yoff;
            g.idx.resize(size_t(f.width) * size_t(f.height));
            for (size_t p = 0; p < g.idx.size(); ++p)
                g.idx[p] = f.rgba[p * 4 + 3] ? int16_t(f.rgba[p * 4]) : int16_t(-1);
        }
        if (glyphs['I'].h > 0) baseline = glyphs['I'].h;
        try { lht = vfs.read("palettes/palette.lht"); } catch (...) { lht.clear(); }
        if (lht.size() < 32 * 256) {   // no table: every row is the identity (no flash)
            lht.resize(32 * 256);
            for (size_t p = 0; p < lht.size(); ++p) lht[p] = uint8_t(p & 255);
        }
        pal = screen;
        return true;
    }
    SDL_Texture* glyph(SDL_Renderer* ren, unsigned char c, int light) {
        light = std::clamp(light, 0, 31);
        SDL_Texture*& t = tex[size_t(light)][c];
        const Glyph& g = glyphs[c];
        if (t || g.w <= 0 || g.h <= 0) return t;
        std::vector<uint8_t> rgba(g.idx.size() * 4, 0);
        for (size_t p = 0; p < g.idx.size(); ++p) {
            if (g.idx[p] < 0) continue;
            const uint8_t i = light ? lht[size_t(light) * 256 + size_t(g.idx[p])] : uint8_t(g.idx[p]);
            rgba[p * 4] = pal.rgba[i][0]; rgba[p * 4 + 1] = pal.rgba[i][1];
            rgba[p * 4 + 2] = pal.rgba[i][2]; rgba[p * 4 + 3] = 255;
        }
        t = art::makeTexture(ren, rgba, g.w, g.h);
        return t;
    }
    int width(const std::string& s) const {
        int w = 0;
        for (unsigned char c : s) if (c >= 0x20) w += glyphs[c].w;
        return w;
    }
    // y is the top of the line: each glyph at (x - xoff, y + baseline - yoff).
    void draw(SDL_Renderer* ren, const GuiLayout& L, float x, float y, const std::string& s, int light) {
        for (unsigned char c : s) {
            if (c < 0x20) continue;
            const Glyph& g = glyphs[c];
            if (c != ' ')
                if (SDL_Texture* t = glyph(ren, c, light)) {
                    const SDL_FRect r = L.rect(x - float(g.xoff), y + float(baseline - g.yoff), float(g.w), float(g.h));
                    SDL_RenderCopyF(ren, t, nullptr, &r);
                }
            x += float(g.w);
        }
    }
};

LoadScreen::LoadScreen(SDL_Renderer* ren, const hpi::Vfs& vfs, const std::string& mapName,
                       Settings* settings)
    : ren_(ren), vfs_(&vfs), settings_(settings), map_(mapName) {
    const char* drv = SDL_GetCurrentVideoDriver();
    headless_ = drv && !std::strcmp(drv, "dummy") && !devEnv("TA_SHOT_LOAD");
    part_.fill(0);
    if (headless_) return;
    // 0x498170: loadgame2bg.pcx is the backdrop, and in 8-bit its palette is the
    // screen's: the guipal colour map (0x4ac7d0) and LIGHTBAR resolve through it.
    gaf::Palette pal{};
    bool havePal = false;
    try {
        const std::string pcx = "bitmaps/loadgame2bg.pcx";
        const auto bytes = vfs.read(pcx);
        const auto img = gaf::loadPcx(bytes, pcx);
        bg_ = art::makeTexture(ren, img.rgba, img.width, img.height);
        pal = gaf::Palette::fromBytes(bytes, pcx);
        havePal = true;
    } catch (...) {}
    if (havePal) {
        try {
            const auto gui = gaf::Palette::fromBytes(vfs.read("palettes/guipal.pal"), "palettes/guipal.pal");
            const auto map = taGuiColourMap(gui.rgba, pal.rgba);
            auto colour = [&](int i) {
                const auto& c = pal.rgba[map[size_t(i)]];
                return SDL_Color{c[0], c[1], c[2], 255};
            };
            darkRed_ = colour(4);
            green_ = colour(10);
            red_ = colour(12);
            white_ = colour(15);
        } catch (...) {}
        try {
            for (auto& sq : gaf::load(vfs.read("anims/commongui.gaf"), pal, -1, "anims/commongui.gaf")) {
                if (sq.name != "LIGHTBAR" || sq.frames.empty()) continue;
                const auto& f = sq.frames[0];
                if (f.width > 0 && f.height > 0) {
                    lightbar_ = art::makeTexture(ren, f.rgba, f.width, f.height);
                    lightW_ = f.width;
                    lightH_ = f.height;
                }
                break;
            }
        } catch (...) {}
    }
    // Text goes through 0x4a50e0: hattfont12.gaf while there is a current GAF font
    // (always, in retail). fonts\smlfont.fnt (0x42a375, selected at 0x498603) is the
    // .FNT font; its height places the map line, and it draws if the GAF is missing.
    lit_ = std::make_unique<LitFont>();
    try { if (!havePal || !lit_->load(vfs, pal)) lit_.reset(); } catch (...) { lit_.reset(); }
    try {
        const auto fnt = vfs.read("fonts/smlfont.fnt");
        if (fnt.size() >= 2) fntHeight_ = fnt[0] | (fnt[1] << 8);
        if (!lit_) {
            font_ = std::make_unique<TaFont>();
            if (!font_->loadFnt(ren, fnt, SDL_Color{255, 255, 255, 255})) font_.reset();
        }
    } catch (...) { font_.reset(); }
    if (!fntHeight_) fntHeight_ = lit_ ? lit_->baseline : 12;
}

LoadScreen::~LoadScreen() {
    if (bg_) gpuvram::destroy(bg_);
    if (lightbar_) gpuvram::destroy(lightbar_);
}

void LoadScreen::setPlayers(const std::array<std::string, ta::net::kMaxSlots>& names, int you,
                            bool network) {
    names_ = names;
    you_ = you;
    network_ = network;
}

void LoadScreen::setSlotPercent(int slot, int pct) {
    if (slot >= 0 && slot < ta::net::kMaxSlots)
        slotPct_[size_t(slot)] = std::max(slotPct_[size_t(slot)], std::clamp(pct, 0, 100));
}

void LoadScreen::setPartQuiet(LoadPart part, int pct) {
    part_[size_t(part)] = std::clamp(pct, 0, 100);
    partsShown_ = true;
    if (const int now = localPercent(); now != reported_ && onProgress) {
        reported_ = now;
        onProgress(now);
    }
}

void LoadScreen::setPart(LoadPart part, int pct) {
    setPartQuiet(part, pct);
    present();
}

void LoadScreen::useLobbyParts(bool showStatus) {
    labels_ = {"Game Data", "Server", "Sign In", "Game Room", "Map List", "Maps"};
    showStatus_ = showStatus;
    for (int i = 0; i < kLoadParts; ++i) setPartQuiet(LoadPart(i), 0);
}

void LoadScreen::step(const std::string& status) {
    status_ = status;
    present();
}

// 0x456de0: a player's own progress is the mean of the six parts.
int LoadScreen::localPercent() const {
    int sum = 0;
    for (int p : part_) sum += p;
    return sum / kLoadParts;
}

void LoadScreen::present() {
    if (headless_ || termRequested()) return;   // nothing to show, and no pump to run
    // Keep the window responsive (and un-flagged by the compositor) while the load
    // blocks the main loop. Everything is swallowed: the screen takes no input.
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    // The caller may be mid-frame with a render target and scale of its own bound (the
    // load blocks before its present). Draw straight to the window at 1:1 instead, then
    // hand the target and scale back exactly as we found them.
    SDL_Texture* prevTarget = SDL_GetRenderTarget(ren_);
    float sx = 1, sy = 1;
    SDL_RenderGetScale(ren_, &sx, &sy);
    SDL_SetRenderTarget(ren_, nullptr);
    SDL_RenderSetScale(ren_, 1.0f, 1.0f);
    draw();
    // TA_SHOT_LOAD=<png>: the screen as last presented; TA_SHOT_LOAD_AT=<n> keeps the
    // n-th present (1-based) instead, to see a part mid-load.
    ++presents_;
    const char* at = devEnv("TA_SHOT_LOAD_AT");
    if (const char* sp = devEnv("TA_SHOT_LOAD"); sp && (!at || std::atoi(at) == presents_)) {
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
    if (headless_) return;
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(ren_, &w, &h);
    // Retail switches to 640x480 for the load (0x49802b); we scale that screen up.
    GuiLayout lay(w, h);

    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
    SDL_RenderClear(ren_);
    if (bg_) {
        const SDL_FRect r = lay.rect(0, 0, 640, 480);
        SDL_RenderCopyF(ren_, bg_, nullptr, &r);
    }
    auto fill = [&](float x, float y, float fw, float fh, SDL_Color c) {
        if (fw <= 0 || fh <= 0) return;
        SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, 255);
        const SDL_FRect r = lay.rect(x, y, fw, fh);
        SDL_RenderFillRectF(ren_, &r);
    };
    auto text = [&](const std::string& s, float x, float y, SDL_Color c, int light = 0) {
        if (s.empty()) return;
        if (lit_) lit_->draw(ren_, lay, x, y, s, light);
        else if (font_) font_->draw(ren_, lay, x, y, s, c);
    };
    auto textWidth = [&](const std::string& s) {
        return lit_ ? lit_->width(s) : font_ ? font_->width(s) : 0;
    };

    // The flash clock: one -2 step a frame at most, when the 30 Hz tick has moved.
    if (const uint64_t now = SDL_GetTicks64() * 30 / 1000; flashTick_ < now) {
        flashTick_ = now;
        for (auto& f : flash_) if (f) f = uint8_t(f - 2);
    }

    // The six parts. Label and bar share one colour: guipal 12 (red) while the part
    // loads, guipal 10 (green) once it reaches 100.
    if (partsShown_) {
        for (int i = 0; i < kLoadParts; ++i) {
            const int pct = part_[size_t(i)];
            const SDL_Color c = pct < 100 ? red_ : green_;
            const float y = float(kPartY[i]);
            // A part reaching 100 lights its label from row 30 (0x498786..0x498796).
            if (pct == 100 && shownPct_[size_t(i)] != 100) flash_[size_t(i)] = 30;
            shownPct_[size_t(i)] = pct;
            text(labels_[size_t(i)] ? labels_[size_t(i)] : kPartLabel[i], kLabelX, y, c, flash_[size_t(i)]);
            fill(kBarX, y, float(pct * 7 / 2), kBarH, c);
            if (lightbar_) {
                const SDL_FRect r = lay.rect(kBarX, y, float(lightW_), float(lightH_));
                SDL_RenderCopyF(ren_, lightbar_, nullptr, &r);
            }
        }
    }

    // 0x498647: "Map: <name>", centred, 1.5 font heights above the bottom, guipal 15.
    if (!map_.empty()) {
        const std::string s = "Map: " + map_;
        text(s, float((640 - textWidth(s)) / 2), 480.0f - 1.5f * float(fntHeight_), white_);
    }

    // 0x497ce0, network games: a cell per player across 620 px from x 11, y 420..435,
    // guipal 4 with the player's progress in guipal 10 and the name over it; above
    // it at (10, 400) how many are ready, or "Synchronization complete".
    std::string line = status_;
    if (network_) {
        int players = 0, ready = 0;
        for (int i = 0; i < ta::net::kMaxSlots; ++i)
            if (!names_[size_t(i)].empty()) ++players;
        if (players > 0) {
            const int cell = 620 / players;
            float x = 11;
            for (int i = 0; i < ta::net::kMaxSlots; ++i) {
                if (names_[size_t(i)].empty()) continue;
                const int pct = i == you_ ? std::max(localPercent(), slotPct_[size_t(i)])
                                          : slotPct_[size_t(i)];
                if (pct >= 100) ++ready;
                fill(x, 420, float(cell - 2), 15, darkRed_);
                fill(x, 420, float(pct * (cell - 2) / 100), 15, green_);
                text(names_[size_t(i)], x, 420, white_);
                x += float(cell);
            }
            if (line.empty())
                line = ready == players ? std::string("Synchronization complete")
                                        : "Waiting for other players.  " + std::to_string(ready) +
                                              (ready == 1 ? " player ready" : " players ready");
        }
    }
    if (showStatus_ || network_) text(line, 10, 400, white_);
    (void)settings_;
}

}  // namespace ta
