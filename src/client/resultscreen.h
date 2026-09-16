#pragma once

// The post-game result screen: retail's `victory<race>.gui` / `defeat.gui` plate with
// its per-player statistics table (Player / Units / Kills / Losses / Time / Score),
// then the player's next step. A self-contained modal (own pump + cursor), shown after
// a campaign mission resolves and after a skirmish/multiplayer game ends.
// See docs/campaign-design.md.

#include <SDL.h>

#include <string>
#include <vector>

#include "hpi/hpi.h"

namespace ta {

class MenuMusic;
struct Settings;

enum class ResultChoice { Menu, Retry, Next };

// One row of the statistics table. Filled from the sim's per-player counters.
struct ResultRow {
    std::string name;
    int colorSlot = 0;    // team-colour palette slot, for the row's logo swatch
    int built = 0;        // units put into the field
    int kills = 0;        // enemy units destroyed
    int losses = 0;       // own units destroyed
    int timeSec = 0;      // seconds survived (match length for a survivor)
    bool defeated = false;
    bool isLocal = false;
};

// Everything the screen needs beyond the win/lose flag.
struct ResultStats {
    std::vector<ResultRow> rows;
    int faction = 0;      // local player's faction (0=ara 1=tar 2=ver 3=zon 4=cre)
                          // -- picks which victory<race>.gui plate is shown
    int matchSec = 0;
};

class ResultScreen {
public:
    // Show VICTORY (victory=true) or DEFEAT for `title`. `hasNext` enables the NEXT
    // MISSION button (victory + a mission after this one). `stats` may be empty, in
    // which case the table is omitted. Returns the player's choice. `settings` supplies
    // the cursor scale; `music` keeps the front-end track looping.
    static ResultChoice run(SDL_Renderer* ren, const hpi::Vfs& vfs, bool victory,
                            const std::string& title, bool hasNext,
                            Settings* settings = nullptr, MenuMusic* music = nullptr,
                            const ResultStats* stats = nullptr);
};

}  // namespace ta
