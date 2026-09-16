#pragma once

// The hotkey-rebinding overlay, styled like OptionsScreen and hosted the same way
// (main menu + in-game Esc menu). It edits Settings::hotkeys live -- click a row to
// arm capture, then the next key press binds it; the DEFAULTS / SAVE / BACK footer
// matches Options (BACK closes without persisting; SAVE writes; DEFAULTS clears all
// overrides back to the factory chords). onChange() lets the host rebuild its live
// Hotkeys so a rebind takes effect immediately.

#include <SDL.h>

#include <functional>
#include <string>
#include <vector>

#include "client/hotkeys.h"
#include "client/settings.h"

namespace ta {

class HotkeysScreen {
public:
    HotkeysScreen(SDL_Renderer* ren, Settings& s, std::function<void()> onChange,
                  std::function<void()> onSave);

    // Feed one SDL event. Returns true when the user leaves (BACK, or Esc while not
    // capturing). While a row is armed, the next key press is captured as its binding.
    bool input(const SDL_Event& e, int winW, int winH);
    void render(int winW, int winH);

private:
    struct Row {
        bool section = false;
        std::string label;
        Act act = Act::Count;   // valid when !section
        SDL_FRect rect{};       // filled by layout()
    };

    void build();
    void layout(int winW, int winH);
    bool atDefaults() const { return s_.hotkeys.empty(); }   // no overrides == all default

    SDL_Renderer* ren_;
    Settings& s_;
    std::function<void()> onChange_;
    std::function<void()> onSave_;
    std::vector<Row> rows_;
    int capture_ = -1;      // index of the row awaiting a key press, or -1
    bool dirty_ = false;
    float scroll_ = 0;
    float contentH_ = 0;
    float u_ = 1.0f;
    SDL_FRect panel_{};
    SDL_FRect defaultsRect_{}, saveRect_{}, backRect_{};
};

}  // namespace ta
