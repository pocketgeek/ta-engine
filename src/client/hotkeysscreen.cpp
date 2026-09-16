#include "client/hotkeysscreen.h"

#include "client/blockfont.h"

#include <algorithm>

namespace ta {

HotkeysScreen::HotkeysScreen(SDL_Renderer* ren, Settings& s, std::function<void()> onChange,
                             std::function<void()> onSave)
    : ren_(ren), s_(s), onChange_(std::move(onChange)), onSave_(std::move(onSave)) {
    build();
}

void HotkeysScreen::build() {
    std::string cur;
    for (const auto& d : hotkeyDefs()) {
        if (cur != d.section) {   // section header row on each group change
            cur = d.section;
            rows_.push_back({true, cur, Act::Count, {}});
        }
        rows_.push_back({false, d.label, d.act, {}});
    }
}

void HotkeysScreen::layout(int winW, int winH) {
    u_ = std::clamp(std::min(winW / 1280.0f, winH / 720.0f), 1.0f, 3.0f);
    auto rowH = [&](bool section) { return (section ? 30.0f : 34.0f) * u_; };
    float panelW = std::min(660 * u_, winW * 0.72f);
    float titleH = 3.4f * 7 * u_ + 26 * u_;
    float footerH = 52 * u_;
    contentH_ = 0;
    for (auto& r : rows_) contentH_ += rowH(r.section);
    float viewH = std::min(contentH_, winH * 0.92f - titleH - footerH);
    float panelH = titleH + viewH + footerH;
    panel_ = {(winW - panelW) / 2, (winH - panelH) / 2, panelW, panelH};

    float maxScroll = std::max(0.0f, contentH_ - viewH);
    scroll_ = std::clamp(scroll_, 0.0f, maxScroll);

    float y = panel_.y + titleH - scroll_;
    for (auto& r : rows_) {
        float h = rowH(r.section);
        r.rect = {panel_.x + 26 * u_, y, panelW - 52 * u_, h};
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

bool HotkeysScreen::input(const SDL_Event& e, int winW, int winH) {
    layout(winW, winH);

    // While a row is armed, the NEXT key press binds it (Esc cancels capture, not the
    // screen). Bare modifier presses are ignored so the user can hold Ctrl/Shift/Alt
    // and then hit the real key.
    if (capture_ >= 0 && e.type == SDL_KEYDOWN) {
        SDL_Keycode k = e.key.keysym.sym;
        if (k == SDLK_ESCAPE) { capture_ = -1; return false; }   // cancel this capture
        if (k == SDLK_LCTRL || k == SDLK_RCTRL || k == SDLK_LSHIFT || k == SDLK_RSHIFT ||
            k == SDLK_LALT || k == SDLK_RALT || k == SDLK_LGUI || k == SDLK_RGUI)
            return false;   // wait for the non-modifier key
        Act a = rows_[size_t(capture_)].act;
        KeyChord c{int32_t(k), normMod(e.key.keysym.mod)};
        // No two actions share a chord: if this chord is already bound elsewhere, unbind
        // that one (it shows as UNBOUND, ready to reassign) rather than double-firing.
        for (const auto& d : hotkeyDefs())
            if (d.act != a && effectiveChord(s_.hotkeys, d.act) == c)
                setChordOverride(s_.hotkeys, d.act, KeyChord{});
        setChordOverride(s_.hotkeys, a, c);
        capture_ = -1;
        dirty_ = true;
        if (onChange_) onChange_();
        return false;
    }

    if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) return true;   // leave
    if (e.type == SDL_MOUSEWHEEL) { scroll_ -= e.wheel.y * 42 * u_; return false; }

    auto in = [](const SDL_FRect& r, float mx, float my) {
        return mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h;
    };
    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        float mx = float(e.button.x), my = float(e.button.y);
        if (in(backRect_, mx, my)) return true;
        if (in(saveRect_, mx, my)) { if (dirty_ && onSave_) { onSave_(); dirty_ = false; } return false; }
        if (in(defaultsRect_, mx, my)) {
            if (atDefaults()) return false;
            s_.hotkeys.clear();     // drop every override -> back to factory chords
            dirty_ = true;
            if (onChange_) onChange_();
            return false;
        }
        // Right-click on a binding row clears it (unbind); left-click arms capture.
        float titleH = 3.4f * 7 * u_ + 26 * u_, footerH = 52 * u_;
        if (my < panel_.y + titleH || my > panel_.y + panel_.h - footerH) return false;
        for (size_t i = 0; i < rows_.size(); ++i) {
            if (rows_[i].section || !in(rows_[i].rect, mx, my)) continue;
            capture_ = int(i);
            return false;
        }
        return false;
    }
    // Right-click a row: unbind it outright.
    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT) {
        float mx = float(e.button.x), my = float(e.button.y);
        for (auto& r : rows_) {
            if (r.section || !in(r.rect, mx, my)) continue;
            setChordOverride(s_.hotkeys, r.act, KeyChord{});
            dirty_ = true;
            if (onChange_) onChange_();
            return false;
        }
    }
    return false;
}

void HotkeysScreen::render(int winW, int winH) {
    layout(winW, winH);
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);

    SDL_SetRenderDrawColor(ren_, 0, 0, 0, 175);
    SDL_FRect dim{0, 0, float(winW), float(winH)}; SDL_RenderFillRectF(ren_, &dim);
    SDL_SetRenderDrawColor(ren_, 24, 26, 34, 245); SDL_RenderFillRectF(ren_, &panel_);
    SDL_SetRenderDrawColor(ren_, 120, 130, 160, 255); SDL_RenderDrawRectF(ren_, &panel_);

    const char* title = "HOTKEYS";
    float tpx = 3.4f * u_;
    drawBlockText(ren_, title, panel_.x + (panel_.w - blockTextWidth(title, tpx)) / 2,
                  panel_.y + 13 * u_, tpx, {235, 225, 180, 255});

    float titleH = 3.4f * 7 * u_ + 26 * u_, footerH = 52 * u_;
    SDL_Rect clip{int(panel_.x), int(panel_.y + titleH), int(panel_.w),
                  int(panel_.h - titleH - footerH)};
    SDL_RenderSetClipRect(ren_, &clip);

    float fpx = 2.0f * u_;
    for (size_t i = 0; i < rows_.size(); ++i) {
        Row& r = rows_[i];
        if (r.rect.y + r.rect.h < clip.y || r.rect.y > clip.y + clip.h) continue;
        if (r.section) {
            drawBlockText(ren_, r.label, r.rect.x, r.rect.y + 12 * u_, 2.2f * u_, {150, 200, 235, 255});
            SDL_SetRenderDrawColor(ren_, 70, 78, 96, 255);
            SDL_FRect ln{r.rect.x, r.rect.y + r.rect.h - 3 * u_, r.rect.w, 1.5f * u_};
            SDL_RenderFillRectF(ren_, &ln);
            continue;
        }
        drawBlockText(ren_, r.label, r.rect.x, r.rect.y + 10 * u_, fpx, {225, 230, 240, 255});
        // The binding chip on the right: the current chord, or "PRESS A KEY" while armed.
        bool armed = capture_ == int(i);
        KeyChord c = effectiveChord(s_.hotkeys, r.act);
        std::string val = armed ? "PRESS A KEY" : chordName(c);
        float chipW = std::max(120 * u_, blockTextWidth(val, fpx) + 20 * u_);
        SDL_FRect chip{r.rect.x + r.rect.w - chipW, r.rect.y + 4 * u_, chipW, 24 * u_};
        if (armed) SDL_SetRenderDrawColor(ren_, 90, 80, 40, 255);
        else if (!c.bound()) SDL_SetRenderDrawColor(ren_, 60, 40, 44, 255);   // unbound: reddish
        else SDL_SetRenderDrawColor(ren_, 44, 52, 66, 255);
        SDL_RenderFillRectF(ren_, &chip);
        SDL_SetRenderDrawColor(ren_, armed ? 220 : 120, armed ? 200 : 130, armed ? 90 : 160, 255);
        SDL_RenderDrawRectF(ren_, &chip);
        SDL_Color tc = armed ? SDL_Color{245, 225, 150, 255}
                     : !c.bound() ? SDL_Color{225, 150, 155, 255}
                                  : SDL_Color{175, 205, 235, 255};
        drawBlockText(ren_, val, chip.x + (chip.w - blockTextWidth(val, fpx)) / 2,
                      chip.y + (chip.h - 7 * fpx) / 2, fpx, tc);
    }
    SDL_RenderSetClipRect(ren_, nullptr);

    // Scrollbar hint.
    float viewH = panel_.h - titleH - footerH;
    if (contentH_ > viewH) {
        float th = viewH * viewH / contentH_;
        float ty = panel_.y + titleH + (scroll_ / (contentH_ - viewH)) * (viewH - th);
        SDL_SetRenderDrawColor(ren_, 110, 120, 150, 220);
        SDL_FRect bar{panel_.x + panel_.w - 5 * u_, ty, 3 * u_, th}; SDL_RenderFillRectF(ren_, &bar);
    }

    // A hint line above the footer: how to unbind.
    {
        const char* hint = "CLICK TO REBIND   RIGHT-CLICK TO CLEAR";
        float hpx = 1.5f * u_;
        drawBlockText(ren_, hint, panel_.x + (panel_.w - blockTextWidth(hint, hpx)) / 2,
                      panel_.y + panel_.h - footerH - 16 * u_, hpx, {130, 140, 160, 255});
    }

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
    button(defaultsRect_, "DEFAULTS", !atDefaults());
    button(saveRect_, "SAVE", dirty_);
    button(backRect_, "BACK", true);
}

}  // namespace ta
