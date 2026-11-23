#include "client/hotkeysscreen.h"

#include "client/uitext.h"
#include "client/cliprect.h"

#include <algorithm>

namespace tak {

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

bool HotkeysScreen::rowAtDefault(Act a) const {
    for (const auto& d : hotkeyDefs())
        if (d.act == a) return !s_.hotkeys.count(d.id);
    return true;
}

// One action back to its factory chord. As when rebinding, an action that holds that
// chord meanwhile gives it up (shows as UNBOUND) so no two actions share it.
void HotkeysScreen::restoreDefault(Act a) {
    for (const auto& d : hotkeyDefs()) {
        if (d.act != a) continue;
        if (d.def.bound())
            for (const auto& o : hotkeyDefs())
                if (o.act != a && effectiveChord(s_.hotkeys, o.act) == d.def)
                    setChordOverride(s_.hotkeys, o.act, KeyChord{});
        s_.hotkeys.erase(d.id);
        changed();
        return;
    }
}

void HotkeysScreen::layout(int winW, int winH) {
    u_ = std::clamp(std::min(winW / 1280.0f, winH / 720.0f), 1.0f, 3.0f);
    auto rowH = [&](bool section) { return (section ? 30.0f : 34.0f) * u_; };
    float panelW = std::min(760 * u_, winW * 0.80f);
    float titleH = 3.4f * 7 * u_ + 26 * u_;
    float footerH = 52 * u_;
    float hintH = 22 * u_;   // the hint line's own band, between the rows and the footer
    contentH_ = 0;
    for (auto& r : rows_) contentH_ += rowH(r.section);
    // The rows scroll in a viewport that stops above the hint, so the hint never lies on one.
    float viewH = std::max(0.0f, std::min(contentH_, winH * 0.92f - titleH - hintH - footerH));
    float panelH = titleH + viewH + hintH + footerH;
    panel_ = {(winW - panelW) / 2, (winH - panelH) / 2, panelW, panelH};
    view_ = {panel_.x, panel_.y + titleH, panelW, viewH};
    hintRect_ = {panel_.x, view_.y + viewH, panelW, hintH};

    float maxScroll = std::max(0.0f, contentH_ - viewH);
    scroll_ = std::clamp(scroll_, 0.0f, maxScroll);

    float y = panel_.y + titleH - scroll_;
    for (auto& r : rows_) {
        float h = rowH(r.section);
        r.rect = {panel_.x + 26 * u_, y, panelW - 52 * u_, h};
        const float dw = 96 * u_;
        r.defRect = {r.rect.x + r.rect.w - dw, y + 4 * u_, dw, 24 * u_};
        y += h;
    }
    // DEFAULTS at the left edge and BACK at the right, in line with the rows.
    float bw = 150 * u_, bh = 32 * u_, inset = 26 * u_;
    float by = panel_.y + panel_.h - footerH + (footerH - bh) / 2;
    defaultsRect_ = {panel_.x + inset, by, bw, bh};
    backRect_ = {panel_.x + panel_.w - inset - bw, by, bw, bh};
}

bool HotkeysScreen::input(const SDL_Event& e, int winW, int winH) {
    // Tell the global key handlers (toggleFullscreenKey) while a row waits for its key.
    struct CaptureFlag {
        const int& capture;
        ~CaptureFlag() { hotkeyCaptureActive = capture >= 0; }
    } captureFlag{capture_};
    layout(winW, winH);

    if (defaultsPrompt_.open) {   // RESET TO DEFAULTS? takes every event while it is up
        if (defaultsPrompt_.input(e) == 1) {
            s_.hotkeys.clear();     // drop every override -> back to factory chords
            changed();
        }
        return false;
    }

    // While a row is armed, the NEXT key press binds it (Esc cancels capture, not the
    // screen). Bare modifier presses are ignored so the user can hold Ctrl/Shift/Alt
    // and then hit the real key.
    if (capture_ >= 0 && e.type == SDL_KEYDOWN) {
        SDL_Keycode k = e.key.keysym.sym;
        if (k == SDLK_ESCAPE) { capture_ = -1; refused_.clear(); return false; }   // cancel this capture
        if (k == SDLK_LCTRL || k == SDLK_RCTRL || k == SDLK_LSHIFT || k == SDLK_RSHIFT ||
            k == SDLK_LALT || k == SDLK_RALT || k == SDLK_LGUI || k == SDLK_RGUI)
            return false;   // wait for the non-modifier key
        Act a = rows_[size_t(capture_)].act;
        if (a == Act::PushToTalk && pushToTalkReserved(int32_t(k))) {   // stay armed, say why
            refused_ = "ENTER, 0-9 AND THE ARROWS ARE GAME KEYS - CHOOSE ANOTHER FOR PUSH TO TALK";
            return false;
        }
        refused_.clear();
        KeyChord c{int32_t(k), normMod(e.key.keysym.mod)};
        // No two actions share a chord: if this chord is already bound elsewhere, unbind
        // that one (it shows as UNBOUND, ready to reassign) rather than double-firing.
        for (const auto& d : hotkeyDefs())
            if (d.act != a && effectiveChord(s_.hotkeys, d.act) == c)
                setChordOverride(s_.hotkeys, d.act, KeyChord{});
        setChordOverride(s_.hotkeys, a, c);
        capture_ = -1;
        changed();
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
        if (in(defaultsRect_, mx, my)) {   // asks first (see the prompt above)
            if (!atDefaults()) { capture_ = -1; refused_.clear(); defaultsPrompt_.open = true; }
            return false;
        }
        // Right-click on a binding row clears it (unbind); left-click arms capture, or on
        // the row's DEFAULT button puts that one key back.
        if (!in(view_, mx, my)) return false;   // the title, the hint and the footer are not rows
        for (size_t i = 0; i < rows_.size(); ++i) {
            if (rows_[i].section || !in(rows_[i].rect, mx, my)) continue;
            if (in(rows_[i].defRect, mx, my)) {
                capture_ = -1; refused_.clear();
                if (!rowAtDefault(rows_[i].act)) restoreDefault(rows_[i].act);
                return false;
            }
            capture_ = int(i); refused_.clear();
            return false;
        }
        return false;
    }
    // Right-click a row: unbind it outright.
    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT) {
        float mx = float(e.button.x), my = float(e.button.y);
        if (!in(view_, mx, my)) return false;   // a row scrolled out of sight is not a target
        for (auto& r : rows_) {
            if (r.section || !in(r.rect, mx, my)) continue;
            setChordOverride(s_.hotkeys, r.act, KeyChord{});
            changed();
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
    uiText(ren_, title, panel_.x + (panel_.w - uiTextWidth(title, tpx)) / 2,
                  panel_.y + 13 * u_, tpx, {235, 225, 180, 255});

    SDL_Rect clip{int(view_.x), int(view_.y), int(view_.w), int(view_.h)};
    // No room (a tiny window): no rows -- on Metal they would draw unclipped.
    const bool rowsShow = tak::cliprect::set(ren_, clip);

    float fpx = 2.0f * u_;
    for (size_t i = 0; rowsShow && i < rows_.size(); ++i) {
        Row& r = rows_[i];
        if (r.rect.y + r.rect.h < clip.y || r.rect.y > clip.y + clip.h) continue;
        if (r.section) {
            // Caps end 7 * 2.2 = 15.4 below y, the divider 3 px above the row's bottom:
            // y + 9 leaves a clear gap (descenders included) above it, as in Options.
            uiText(ren_, r.label, r.rect.x, r.rect.y + 9 * u_, 2.2f * u_, {150, 200, 235, 255});
            SDL_SetRenderDrawColor(ren_, 70, 78, 96, 255);
            SDL_FRect ln{r.rect.x, r.rect.y + r.rect.h - 3 * u_, r.rect.w, 1.5f * u_};
            SDL_RenderFillRectF(ren_, &ln);
            continue;
        }
        uiText(ren_, r.label, r.rect.x, r.rect.y + 10 * u_, fpx, {225, 230, 240, 255});
        // The binding chip on the right: the current chord, or "PRESS A KEY" while armed.
        bool armed = capture_ == int(i);
        KeyChord c = effectiveChord(s_.hotkeys, r.act);
        std::string val = armed ? "PRESS A KEY" : chordName(c);
        float chipW = std::max(120 * u_, uiTextWidth(val, fpx) + 20 * u_);
        SDL_FRect chip{r.defRect.x - 10 * u_ - chipW, r.rect.y + 4 * u_, chipW, 24 * u_};
        if (armed) SDL_SetRenderDrawColor(ren_, 90, 80, 40, 255);
        else if (!c.bound()) SDL_SetRenderDrawColor(ren_, 60, 40, 44, 255);   // unbound: reddish
        else SDL_SetRenderDrawColor(ren_, 44, 52, 66, 255);
        SDL_RenderFillRectF(ren_, &chip);
        SDL_SetRenderDrawColor(ren_, armed ? 220 : 120, armed ? 200 : 130, armed ? 90 : 160, 255);
        SDL_RenderDrawRectF(ren_, &chip);
        SDL_Color tc = armed ? SDL_Color{245, 225, 150, 255}
                     : !c.bound() ? SDL_Color{225, 150, 155, 255}
                                  : SDL_Color{175, 205, 235, 255};
        uiText(ren_, val, chip.x + (chip.w - uiTextWidth(val, fpx)) / 2,
                      chip.y + (chip.h - 7 * fpx) / 2, fpx, tc);
        // The row's DEFAULT: live only while this key differs from its factory one.
        const bool live = !rowAtDefault(r.act);
        SDL_SetRenderDrawColor(ren_, live ? 60 : 34, live ? 66 : 38, live ? 86 : 46, 255);
        SDL_RenderFillRectF(ren_, &r.defRect);
        SDL_SetRenderDrawColor(ren_, live ? 130 : 70, live ? 140 : 76, live ? 170 : 92, 255);
        SDL_RenderDrawRectF(ren_, &r.defRect);
        const float dpx = 1.6f * u_;
        uiText(ren_, "DEFAULT", r.defRect.x + (r.defRect.w - uiTextWidth("DEFAULT", dpx)) / 2,
                      r.defRect.y + (r.defRect.h - 7 * dpx) / 2, dpx,
                      live ? SDL_Color{228, 232, 242, 255} : SDL_Color{110, 115, 125, 255});
    }
    SDL_RenderSetClipRect(ren_, nullptr);

    // Scrollbar hint.
    const float viewH = view_.h;
    if (contentH_ > viewH) {
        float th = viewH * viewH / contentH_;
        float ty = view_.y + (scroll_ / (contentH_ - viewH)) * (viewH - th);
        SDL_SetRenderDrawColor(ren_, 110, 120, 150, 220);
        SDL_FRect bar{panel_.x + panel_.w - 5 * u_, ty, 3 * u_, th}; SDL_RenderFillRectF(ren_, &bar);
    }

    // A hint line in its own band between the rows and the footer: how to unbind -- or why
    // the last key was refused.
    {
        const std::string hint = !refused_.empty() ? refused_ : "CLICK TO REBIND   RIGHT-CLICK TO CLEAR   DEFAULT PUTS ONE KEY BACK";
        float hpx = std::min(1.5f * u_, (panel_.w - 20 * u_) / std::max(1.0f, uiTextWidth(hint, 1)));
        uiText(ren_, hint, hintRect_.x + (hintRect_.w - uiTextWidth(hint, hpx)) / 2,
                      hintRect_.y + (hintRect_.h - 7 * hpx) / 2, hpx,
                      !refused_.empty() ? SDL_Color{235, 190, 110, 255} : SDL_Color{130, 140, 160, 255});
    }

    auto button = [&](const SDL_FRect& r, const char* label, bool on) {
        SDL_SetRenderDrawColor(ren_, on ? 60 : 34, on ? 66 : 38, on ? 86 : 46, 255);
        SDL_RenderFillRectF(ren_, &r);
        SDL_SetRenderDrawColor(ren_, on ? 130 : 70, on ? 140 : 76, on ? 170 : 92, 255);
        SDL_RenderDrawRectF(ren_, &r);
        float bpx = 2.2f * u_;
        uiText(ren_, label, r.x + (r.w - uiTextWidth(label, bpx)) / 2,
                      r.y + (r.h - 7 * bpx) / 2, bpx,
                      on ? SDL_Color{228, 232, 242, 255} : SDL_Color{110, 115, 125, 255});
    };
    button(defaultsRect_, "DEFAULTS", !atDefaults());
    button(backRect_, "BACK", true);
    defaultsPrompt_.render(ren_, panel_, u_, winW, winH, "RESET TO DEFAULTS?",
                           {"EVERY KEY GOES BACK TO ITS DEFAULT."}, "YES", "NO");
}

}  // namespace tak
