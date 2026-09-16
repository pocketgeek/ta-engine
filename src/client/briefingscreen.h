#pragma once

// The pre-mission briefing shown between a campaign mission's intro movie and the
// mission itself: the mission title and its objectives (missions/<stem>.txt), with
// BEGIN / BACK. A self-contained modal loop (own event pump + cursor), drawn with the
// shared block font over a parchment-dark backdrop. See docs/campaign-design.md.

#include <SDL.h>

#include <string>

#include "hpi/hpi.h"

namespace ta {

class MenuMusic;
struct Settings;

class BriefingScreen {
public:
    // Show the briefing for `stem` (loads missions/<stem>.txt), titled `title`. Blocks
    // until the player clicks BEGIN (returns true -> launch the mission) or BACK / Esc
    // (returns false -> back to the picker). `settings` supplies the cursor scale;
    // `music` (if given) keeps the front-end track looping. `install` is the retail
    // dir (unused today; reserved for briefing VO).
    static bool run(SDL_Renderer* ren, const hpi::Vfs& vfs, const std::string& stem,
                    const std::string& title, Settings* settings = nullptr,
                    MenuMusic* music = nullptr);
};

}  // namespace ta
