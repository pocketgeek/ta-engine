#include "client/taguiview.h"

#include "hpi/hpi.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace ta {

namespace {

std::string up(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string low(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

SDL_Texture* rgbaTexture(SDL_Renderer* ren, const std::vector<uint8_t>& rgba, int w, int h) {
    if (w <= 0 || h <= 0 || rgba.size() < size_t(w) * size_t(h) * 4) return nullptr;
    SDL_Texture* t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return nullptr;
    SDL_UpdateTexture(t, nullptr, rgba.data(), w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

gaf::Palette loadPalette(const hpi::Vfs& vfs, const std::string& path) {
    try { return gaf::Palette::fromBytes(vfs.read(path), path); } catch (...) {}
    gaf::Palette p{};
    for (int i = 0; i < 256; ++i) { p.rgba[i][0] = p.rgba[i][1] = p.rgba[i][2] = uint8_t(i); p.rgba[i][3] = 255; }
    return p;
}

// BUTTONS0's 4-frame groups (section 1.4): base frame -> size.
struct Group { int base, w, h; };
constexpr Group kButtons0[] = {{0, 16, 16}, {4, 321, 20}, {8, 120, 20}, {12, 96, 20},
                               {16, 112, 20}, {20, 80, 20}, {24, 96, 31}};

}  // namespace

// ---------------------------------------------------------------- fonts

TaFont::~TaFont() {
    for (auto& g : glyphs_) if (g.tex) SDL_DestroyTexture(g.tex);
}

bool TaFont::loadFnt(SDL_Renderer* ren, const std::vector<uint8_t>& d, SDL_Color color) {
    // u16 height, u16 unknown, u16 offset[255] for chars 1..255; glyph = u8 width +
    // height*width bits MSB-first, continuous (docs/retail-gui.md section 1.5).
    if (d.size() < 4 + 255 * 2) return false;
    height_ = d[0] | (d[1] << 8);
    // The offset table is indexed by the character itself (checked on console.fnt:
    // indexing by ch - 1 draws "1000" as "0///").
    for (int ch = 0; ch < 255; ++ch) {
        const size_t o = size_t(d[4 + ch * 2] | (d[5 + ch * 2] << 8));
        if (!o || o >= d.size()) continue;
        const int w = d[o];
        Glyph g;
        g.w = w; g.h = height_; g.advance = w;
        if (w > 0 && height_ > 0) {
            std::vector<uint8_t> rgba(size_t(w) * size_t(height_) * 4, 0);
            size_t bit = 0;
            for (int y = 0; y < height_; ++y)
                for (int x = 0; x < w; ++x, ++bit) {
                    const size_t byte = o + 1 + bit / 8;
                    if (byte >= d.size()) break;
                    if (d[byte] & (0x80 >> (bit % 8))) {
                        uint8_t* p = &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
                        p[0] = color.r; p[1] = color.g; p[2] = color.b; p[3] = 255;
                    }
                }
            g.tex = rgbaTexture(ren, rgba, w, height_);
        }
        glyphs_[ch] = g;
    }
    gafFont_ = false;
    return true;
}

bool TaFont::loadGaf(SDL_Renderer* ren, const hpi::Vfs& vfs, const std::string& gafName, const gaf::Palette& pal) {
    try {
        const std::string path = "anims/" + low(gafName) + ".gaf";
        auto seqs = gaf::load(vfs.read(path), pal, -1, path);
        if (seqs.empty()) return false;
        const auto& s = seqs.front();
        for (size_t ch = 0; ch < s.frames.size() && ch < 256; ++ch) {
            const auto& f = s.frames[ch];
            Glyph g;
            g.w = f.width; g.h = f.height; g.xoff = f.xoff; g.yoff = f.yoff; g.advance = f.width;
            g.tex = rgbaTexture(ren, f.rgba, f.width, f.height);
            height_ = std::max(height_, f.height);
            glyphs_[ch] = g;
        }
        gafFont_ = true;
        return true;
    } catch (...) { return false; }
}

int TaFont::width(const std::string& s) const {
    int w = 0;
    for (unsigned char c : s) w += glyphs_[c].advance + (gafFont_ ? 0 : 1);
    return w;
}

void TaFont::draw(SDL_Renderer* ren, const GuiLayout& L, float x, float y, const std::string& s, SDL_Color tint) const {
    for (unsigned char c : s) {
        const Glyph& g = glyphs_[c];
        if (g.tex && c != ' ') {   // a space only advances
            // GAF glyphs carry their own colours; 1-bpp .FNT glyphs take the tint.
            if (gafFont_) SDL_SetTextureColorMod(g.tex, 255, 255, 255);
            else SDL_SetTextureColorMod(g.tex, tint.r, tint.g, tint.b);
            const float gy = gafFont_ ? y + float(height_ - g.yoff - 1) : y;
            SDL_FRect r = L.rect(x - (gafFont_ ? float(g.xoff) : 0), gy, float(g.w), float(g.h));
            SDL_RenderCopyF(ren, g.tex, nullptr, &r);
        }
        x += float(g.advance + (gafFont_ ? 0 : 1));
    }
}

// ---------------------------------------------------------------- view

TaGuiView::TaGuiView(SDL_Renderer* ren, const hpi::Vfs& vfs, tagui::Screen screen, bool menuPalette)
    : ren_(ren), vfs_(vfs), screen_(std::move(screen)) {
    pal_ = loadPalette(vfs_, menuPalette ? "palettes/guipal.pal" : "palettes/palette.pal");
    font_ = std::make_unique<TaFont>();
    font11_ = std::make_unique<TaFont>();
    // Front-end text is the GAF font hattfont12 (fontnumber 0) / hattfont11 (1);
    // a type-7 FONT gadget names the .FNT used for that screen's buttons.
    std::string fnt;
    for (const auto& g : screen_.gadgets)
        if (g.type == tagui::Type::Font && !g.filename.empty()) fnt = g.filename;
    bool ok = false;
    if (!fnt.empty()) {
        for (const std::string& p : {"fonts/" + low(fnt) + ".fnt", "guis/" + low(fnt) + ".fnt"})
            if (vfs_.has(p)) { ok = font_->loadFnt(ren_, vfs_.read(p), SDL_Color{255, 255, 255, 255}); if (ok) break; }
    }
    if (!ok) font_->loadGaf(ren_, vfs_, "hattfont12", pal_);
    font11_->loadGaf(ren_, vfs_, "hattfont11", pal_);
    for (const auto& g : screen_.gadgets) {
        auto& s = st(g.name);
        if (up(g.name) == "DEBUGSTRING") s.visible = false;   // retail debug builds only
        s.pressed = g.status != 0 && g.type == tagui::Type::Button && !(g.attribs & tagui::kAttrNoPressOffset);
        s.stage = g.stages > 0 ? std::clamp(g.status, 0, std::max(0, g.stages - 1)) : 0;
    }
    place();
}

TaGuiView::~TaGuiView() {
    for (auto& [k, f] : seqs_) for (auto* t : f.tex) if (t) SDL_DestroyTexture(t);
    if (backdrop_) SDL_DestroyTexture(backdrop_);
}

TaGuiView::State& TaGuiView::st(const std::string& name) { return state_[up(name)]; }
const TaGuiView::State* TaGuiView::stc(const std::string& name) const {
    auto it = state_.find(up(name));
    return it == state_.end() ? nullptr : &it->second;
}

void TaGuiView::setBackdrop(const std::string& pcx) {
    if (backdrop_) { SDL_DestroyTexture(backdrop_); backdrop_ = nullptr; }
    try {
        const auto bytes = vfs_.read(pcx);
        auto img = gaf::loadPcx(bytes, pcx);
        backdrop_ = rgbaTexture(ren_, img.rgba, img.width, img.height);
        backW_ = img.width; backH_ = img.height;
        // Retail runs the front end in 8-bit: the backdrop's palette becomes the
        // screen palette, and every GAF on the page is drawn through it (the
        // MAINMENU Credits logo is noise through guipal, right through frontendx).
        pal_ = gaf::Palette::fromBytes(bytes, pcx);
        for (auto& [k, f] : seqs_) for (auto* t : f.tex) if (t) SDL_DestroyTexture(t);
        seqs_.clear();
        gafs_.clear();
        font_ = std::make_unique<TaFont>();
        font11_ = std::make_unique<TaFont>();
        font_->loadGaf(ren_, vfs_, "hattfont12", pal_);
        font11_->loadGaf(ren_, vfs_, "hattfont11", pal_);
    } catch (...) {}
}

void TaGuiView::place(int W, int H) {
    const auto& c = screen_.container();
    ox_ = c.x; oy_ = c.y;
    if (c.x == -1) ox_ = (W - c.w) / 2;
    if (c.y == -1) oy_ = (H - c.h) / 2;
    else if (c.y < 0) oy_ = c.y + H;            // anchored to the bottom (0x4a84f2)
    if (ox_ + c.w > W || oy_ + c.h > H) {        // does not fit: re-centre
        ox_ = std::max(0, (W - c.w) / 2);
        oy_ = std::max(0, (H - c.h) / 2);
    }
}

const TaGuiView::Frames* TaGuiView::sequence(const std::string& gafName, const std::string& seqName) {
    const std::string key = low(gafName) + "/" + low(seqName);
    if (auto it = seqs_.find(key); it != seqs_.end()) return it->second.tex.empty() ? nullptr : &it->second;
    Frames out;
    auto git = gafs_.find(low(gafName));
    if (git == gafs_.end()) {
        std::vector<gaf::Sequence> seqs;
        const std::string path = "anims/" + low(gafName) + ".gaf";
        try { if (vfs_.has(path)) seqs = gaf::load(vfs_.read(path), pal_, -1, path); } catch (...) {}
        git = gafs_.emplace(low(gafName), std::move(seqs)).first;
    }
    for (const auto& s : git->second) {
        if (low(s.name) != low(seqName)) continue;
        for (const auto& f : s.frames) {
            out.tex.push_back(rgbaTexture(ren_, f.rgba, f.width, f.height));
            out.geo.push_back({f.xoff, f.yoff, f.width, f.height});
        }
        break;
    }
    auto& slot = seqs_[key] = std::move(out);
    return slot.tex.empty() ? nullptr : &slot;
}

const TaGuiView::Frames* TaGuiView::buttonArt(const tagui::Gadget& g, int& base) {
    base = 0;
    // anims\<name>_gadget.gaf, sequence <name>.
    if (const Frames* f = sequence(g.name + "_gadget", g.name)) return f;
    if (const Frames* f = sequence(screen_.name, g.name)) return f;
    if (const Frames* f = sequence("commongui", g.name)) return f;
    if (g.attribs & tagui::kAttrCheckbox) return sequence("commongui", "CHECKBOX");
    if (g.stages > 0) {
        const int n = (g.stages == 1 || g.text == "Off|On") ? 1 : std::min(g.stages, 4);
        return sequence("commongui", "stagebuttn" + std::to_string(n));
    }
    const Frames* f = sequence("commongui", "BUTTONS0");
    if (!f) return nullptr;
    auto it = resized_.find(g.index);
    if (it == resized_.end()) {
        int best = 0, bestD = 1 << 30;
        for (const auto& grp : kButtons0) {
            const int dd = std::abs(grp.w - g.w) + std::abs(grp.h - g.h);
            if (dd < bestD) { bestD = dd; best = grp.base; }
        }
        it = resized_.emplace(g.index, best).first;
        for (const auto& grp : kButtons0)   // the gadget takes the frame's size
            if (grp.base == best) {
                auto* m = screen_.find(g.name);
                if (m) { m->w = grp.w; m->h = grp.h; }
            }
    }
    base = it->second;
    return f;
}

TaGuiView::Rect TaGuiView::rectOf(const tagui::Gadget& g) const {
    if (g.index == 0) return {float(ox_), float(oy_), float(g.w), float(g.h)};
    return {float(ox_ + g.x), float(oy_ + g.y), float(g.w), float(g.h)};
}

void TaGuiView::drawFrame(const Frames& f, int frame, const SDL_FRect& r, const GuiLayout& L, bool anchor) {
    if (f.tex.empty()) return;
    frame = std::clamp(frame, 0, int(f.tex.size()) - 1);
    if (!f.tex[size_t(frame)]) return;
    const auto& g = f.geo[size_t(frame)];
    // Retail blits GUI art at the gadget origin (frame offsets apply to sprites only).
    SDL_FRect dst = L.rect(r.x - (anchor ? float(g.x) : 0), r.y - (anchor ? float(g.y) : 0), float(g.w), float(g.h));
    SDL_RenderCopyF(ren_, f.tex[size_t(frame)], nullptr, &dst);
}

void TaGuiView::drawText(const tagui::Gadget& g, const std::string& s, float x, float y, bool centre,
                         const GuiLayout& L) {
    if (s.empty()) return;
    const TaFont& f = (g.fontNumber == 1 && font11_->height()) ? *font11_ : *font_;
    float tx = x;
    if (centre) tx = x + (float(g.w) - float(f.width(s))) / 2;
    const float ty = y + (float(g.h) - float(f.height())) / 2;
    SDL_Color c{255, 255, 255, 255};
    if (g.colorf > 0 && g.colorf < 256) c = SDL_Color{pal_.rgba[g.colorf][0], pal_.rgba[g.colorf][1], pal_.rgba[g.colorf][2], 255};
    f.draw(ren_, L, tx, ty, s, c);
}

void TaGuiView::draw(const GuiLayout& L) {
    const auto& c = screen_.container();
    const Rect cr = rectOf(c);
    if (backdrop_) {
        SDL_FRect r = L.rect(cr.x, cr.y, float(backW_), float(backH_));
        SDL_RenderCopyF(ren_, backdrop_, nullptr, &r);
    } else if (!c.panel.empty()) {
        const Frames* f = sequence(screen_.name, c.panel);
        if (!f) f = sequence("commongui", c.panel);
        if (f) drawFrame(*f, 0, {cr.x, cr.y, cr.w, cr.h}, L, false);
        else if (const Frames* t = sequence("commongui", "BackTile")) {
            const auto& g = t->geo[0];
            for (float y = cr.y; y < cr.y + cr.h; y += float(std::max(1, g.h)))
                for (float x = cr.x; x < cr.x + cr.w; x += float(std::max(1, g.w)))
                    drawFrame(*t, 0, {x, y, 0, 0}, L, false);
        }
    }
    for (auto& g : screen_.gadgets) {
        if (g.index == 0) continue;
        const State* s = stc(g.name);
        if (s && !s->visible) continue;
        const Rect r = rectOf(g);
        const SDL_FRect fr{r.x, r.y, r.w, r.h};
        switch (g.type) {
        case tagui::Type::BackgroundPic: {
            const Frames* f = sequence(screen_.name, g.name);
            if (!f) f = sequence("commongui", g.name);
            if (f) drawFrame(*f, 0, fr, L, false);
            break;
        }
        case tagui::Type::Button: {
            int base = 0;
            const Frames* f = buttonArt(g, base);
            const bool down = s && (s->pressed || s->down);
            int frame = base;
            if (g.stages > 0 && f) {
                const int n = int(f->tex.size());
                frame = s ? s->stage : 0;
                if (down && !(g.attribs & tagui::kAttrNoPressOffset)) frame = std::max(0, n - 2);
                if (g.grayedOut && (g.attribs & tagui::kAttrGrayLastFrame)) frame = n - 1;
            } else if (f) {
                const int n = int(f->tex.size());
                const int status = down ? 1 : 0;
                frame = g.grayedOut ? base + std::min(status + 2, std::max(0, n - 1 - base))
                                    : base + status;
            }
            if (f) drawFrame(*f, frame, fr, L, false);
            const Rect rr = rectOf(g);   // may have been resized
            std::string label = g.text;
            if (g.stages > 0) {
                auto labels = g.stageLabels();
                const int stg = s ? s->stage : 0;
                label = stg < int(labels.size()) ? labels[size_t(stg)] : "";
            }
            drawText(g, label, rr.x, rr.y + (down ? 1.0f : 0.0f), true, L);
            break;
        }
        case tagui::Type::Label:
            drawText(g, g.text, r.x, r.y, false, L);
            break;
        case tagui::Type::TextField: {
            if (const Frames* f = sequence("commongui", "TEXTINPUT"); f && f->tex.size() >= 3) {
                drawFrame(*f, 0, {r.x, r.y, 0, 0}, L, false);
                const float lw = float(f->geo[0].w), rw = float(f->geo[2].w);
                SDL_FRect mid = L.rect(r.x + lw, r.y, std::max(0.0f, r.w - lw - rw), float(f->geo[1].h));
                if (f->tex[1]) SDL_RenderCopyF(ren_, f->tex[1], nullptr, &mid);
                drawFrame(*f, 2, {r.x + r.w - rw, r.y, 0, 0}, L, false);
            }
            drawText(g, g.text, r.x + 4, r.y, false, L);
            break;
        }
        case tagui::Type::ListBox: {
            if (!s) break;
            const int ih = std::max(1, g.itemHeight ? g.itemHeight : font_->height() + 2);
            const int rows = std::max(1, g.h / ih);
            for (int i = 0; i < rows && s->scroll + i < int(s->items.size()); ++i) {
                const int idx = s->scroll + i;
                if (idx == s->sel) {
                    SDL_FRect hl = L.rect(r.x, r.y + float(i * ih), r.w, float(ih));
                    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(ren_, 60, 60, 160, 160);
                    SDL_RenderFillRectF(ren_, &hl);
                }
                tagui::Gadget row = g;
                row.h = ih;
                drawText(row, s->items[size_t(idx)], r.x + 2, r.y + float(i * ih), false, L);
            }
            break;
        }
        case tagui::Type::Surface:
            if (drawSurface) drawSurface(g, L.rect(r.x, r.y, r.w, r.h));
            break;
        default:
            break;
        }
    }
}

const tagui::Gadget* TaGuiView::at(float mx, float my, const GuiLayout& L) const {
    for (auto it = screen_.gadgets.rbegin(); it != screen_.gadgets.rend(); ++it) {
        const auto& g = *it;
        if (g.index == 0) continue;
        if (g.type != tagui::Type::Button && g.type != tagui::Type::ListBox &&
            g.type != tagui::Type::Surface && g.type != tagui::Type::TextField && g.type != tagui::Type::Label)
            continue;
        const State* s = stc(g.name);
        if (s && !s->visible) continue;
        const Rect r = rectOf(g);
        if (L.hit(r.x, r.y, r.w, r.h, mx, my)) return &g;
    }
    return nullptr;
}

std::string TaGuiView::hoverHelp(float mx, float my, const GuiLayout& L) const {
    const tagui::Gadget* g = at(mx, my, L);
    if (!g) return {};
    if (g->type == tagui::Type::Label && !g->link.empty())
        if (const auto* t = screen_.find(g->link)) return t->help;
    return g->help;
}

std::string TaGuiView::handle(const SDL_Event& e, const GuiLayout& L) {
    auto activate = [&](tagui::Gadget& g) -> std::string {
        if (g.grayedOut) return {};
        auto& s = st(g.name);
        if (g.type == tagui::Type::Button) {
            if (g.stages > 0) s.stage = (s.stage + 1) % std::max(1, g.stages == 1 ? 2 : g.stages);
            if ((g.attribs & 0xff) == tagui::kAttrRadio && g.assoc) {
                for (auto& o : screen_.gadgets)
                    if (o.assoc == g.assoc && o.type == tagui::Type::Button) st(o.name).pressed = false;
                s.pressed = true;
            } else if (g.attribs & (tagui::kAttrTab | tagui::kAttrLatched)) {
                s.pressed = !s.pressed;
            }
        }
        return g.name;
    };
    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        const tagui::Gadget* g = at(float(e.button.x), float(e.button.y), L);
        if (!g) return {};
        if (g->type == tagui::Type::ListBox) {
            auto& s = st(g->name);
            const Rect r = rectOf(*g);
            const int ih = std::max(1, g->itemHeight ? g->itemHeight : font_->height() + 2);
            const float ly = (float(e.button.y) - L.py(r.y)) / L.scale;
            const int idx = s.scroll + int(ly) / ih;
            if (idx >= 0 && idx < int(s.items.size())) s.sel = idx;
            return g->name;
        }
        if (g->type == tagui::Type::Button && !g->grayedOut) { st(g->name).down = true; downGadget_ = g->name; }
        if (g->type == tagui::Type::Surface) return g->name;
        return {};
    }
    if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
        std::string fired;
        if (!downGadget_.empty()) {
            st(downGadget_).down = false;
            const tagui::Gadget* g = at(float(e.button.x), float(e.button.y), L);
            if (g && up(g->name) == up(downGadget_))
                if (auto* m = screen_.find(downGadget_)) fired = activate(*m);
            downGadget_.clear();
        }
        return fired;
    }
    if (e.type == SDL_MOUSEWHEEL) {
        int mx = 0, my = 0;
        SDL_GetMouseState(&mx, &my);
        if (const tagui::Gadget* g = at(float(mx), float(my), L); g && g->type == tagui::Type::ListBox) {
            auto& s = st(g->name);
            s.scroll = std::clamp(s.scroll - e.wheel.y, 0, std::max(0, int(s.items.size()) - 1));
        }
        return {};
    }
    if (e.type == SDL_KEYDOWN) {
        const SDL_Keycode k = e.key.keysym.sym;
        std::string target;
        if (k == SDLK_RETURN || k == SDLK_KP_ENTER) target = screen_.enterDefault();
        else if (k == SDLK_ESCAPE) target = screen_.container().escDefault;
        if (target.empty())
            for (const auto& g : screen_.gadgets) {
                if (g.type != tagui::Type::Button || !g.quickKey) continue;
                const int want = (k >= 'a' && k <= 'z') ? std::toupper(int(k)) : int(k);
                if (g.quickKey == want || (g.quickKey == 13 && k == SDLK_RETURN) || (g.quickKey == 27 && k == SDLK_ESCAPE)) {
                    target = g.name; break;
                }
            }
        if (!target.empty())
            if (auto* m = screen_.find(target)) return activate(*m);
    }
    return {};
}

void TaGuiView::drawSequence(const std::string& gaf, const std::string& seq, int frame, float x, float y,
                             const GuiLayout& L) {
    if (const Frames* f = sequence(gaf, seq)) drawFrame(*f, frame, {x, y, 0, 0}, L, false);
}

std::string TaGuiView::activate(const std::string& name) {
    tagui::Gadget* g = screen_.find(name);
    if (!g || g->grayedOut) return {};
    auto& s = st(g->name);
    if (g->type == tagui::Type::Button) {
        if (g->stages > 0) s.stage = (s.stage + 1) % std::max(1, g->stages == 1 ? 2 : g->stages);
        if ((g->attribs & 0xff) == tagui::kAttrRadio && g->assoc) {
            for (auto& o : screen_.gadgets)
                if (o.assoc == g->assoc && o.type == tagui::Type::Button) st(o.name).pressed = false;
            s.pressed = true;
        } else if (g->attribs & (tagui::kAttrTab | tagui::kAttrLatched)) {
            s.pressed = !s.pressed;
        }
    }
    return g->name;
}

void TaGuiView::setText(const std::string& n, const std::string& t) { if (auto* g = screen_.find(n)) g->text = t; }
void TaGuiView::setGrayed(const std::string& n, bool v) { if (auto* g = screen_.find(n)) g->grayedOut = v; }
void TaGuiView::setStage(const std::string& n, int s) { st(n).stage = s; }
int TaGuiView::stage(const std::string& n) const { const State* s = stc(n); return s ? s->stage : 0; }
void TaGuiView::setPressed(const std::string& n, bool v) { st(n).pressed = v; }
bool TaGuiView::pressed(const std::string& n) const { const State* s = stc(n); return s && s->pressed; }
void TaGuiView::setVisible(const std::string& n, bool v) { st(n).visible = v; }
void TaGuiView::setList(const std::string& n, std::vector<std::string> items) {
    auto& s = st(n);
    s.items = std::move(items);
    if (s.sel >= int(s.items.size())) s.sel = s.items.empty() ? -1 : 0;
    s.scroll = 0;
}
int TaGuiView::selection(const std::string& n) const { const State* s = stc(n); return s ? s->sel : -1; }
void TaGuiView::setSelection(const std::string& n, int i) { st(n).sel = i; }

}  // namespace ta
