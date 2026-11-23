#pragma once

// The hotkey-rebinding overlay, styled like OptionsScreen and hosted the same way
// (main menu + in-game Esc menu). It edits Settings::hotkeys live -- click a row to
// arm capture, then the next key press binds it; the DEFAULTS / BACK footer matches
// Options (DEFAULTS asks first, then clears all overrides back to the factory chords);
// each row's own DEFAULT button puts just that action back.
// onChange() lets the host rebuild its live Hotkeys so a rebind takes effect
// immediately, and onSave() writes the settings file after every change: there is no
// SAVE button.

#include <SDL.h>

#include <functional>
#include <string>
#include <vector>

#include "client/hotkeys.h"
#include "client/promptbox.h"
#include "client/settings.h"

namespace tak {

class HotkeysScreen {
public:
    HotkeysScreen(SDL_Renderer* ren, Settings& s, std::function<void()> onChange,
                  std::function<void()> onSave);
    ~HotkeysScreen() { hotkeyCaptureActive = false; }

    // Feed one SDL event. Returns true when the user leaves (BACK, or Esc while not
    // capturing). While a row is armed, the next key press is captured as its binding.
    bool input(const SDL_Event& e, int winW, int winH);
    void render(int winW, int winH);

    // For the in-process UI tests: an action's row as last laid out (empty if none), the
    // footer's DEFAULTS button, and the DEFAULTS prompt's buttons (empty while closed).
    SDL_FRect rowRect(Act a) const {
        for (const auto& r : rows_) if (!r.section && r.act == a) return r.rect;
        return {};
    }
    SDL_FRect rowDefaultRect(Act a) const {   // the row's own DEFAULT button
        for (const auto& r : rows_) if (!r.section && r.act == a) return r.defRect;
        return {};
    }
    SDL_FRect defaultsRect() const { return defaultsRect_; }
    SDL_FRect viewRect() const { return view_; }       // where the rows scroll
    SDL_FRect hintRect() const { return hintRect_; }   // the hint line's band, below the rows
    SDL_FRect panelRect() const { return panel_; }
    SDL_FRect backRect() const { return backRect_; }
    SDL_FRect defaultsYesRect() const { return defaultsPrompt_.open ? defaultsPrompt_.first : SDL_FRect{}; }
    SDL_FRect defaultsNoRect() const { return defaultsPrompt_.open ? defaultsPrompt_.second : SDL_FRect{}; }

private:
    struct Row {
        bool section = false;
        std::string label;
        Act act = Act::Count;   // valid when !section
        SDL_FRect rect{};       // filled by layout()
        SDL_FRect defRect{};    // its DEFAULT button, at the row's right end
    };

    void build();
    void layout(int winW, int winH);
    bool atDefaults() const { return s_.hotkeys.empty(); }   // no overrides == all default
    bool rowAtDefault(Act a) const;
    void restoreDefault(Act a);
    void changed() { if (onChange_) onChange_(); if (onSave_) onSave_(); }   // apply live, then save

    SDL_Renderer* ren_;
    Settings& s_;
    std::function<void()> onChange_;
    std::function<void()> onSave_;
    std::vector<Row> rows_;
    int capture_ = -1;      // index of the row awaiting a key press, or -1
    std::string refused_;   // why the armed row refused the last key (shown in the hint line)
    PromptBox defaultsPrompt_;   // DEFAULTS asks first: RESET TO DEFAULTS? YES / NO
    float scroll_ = 0;
    float contentH_ = 0;
    float u_ = 1.0f;
    SDL_FRect panel_{};
    SDL_FRect view_{}, hintRect_{};   // the rows' viewport, and the hint line's band under it
    SDL_FRect defaultsRect_{}, backRect_{};
};

}  // namespace tak
