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

enum class ResultChoice { Menu, Retry, Next, NewGame, GameSelect };

// One row of the statistics table. Filled from the sim's per-player counters.
struct ResultRow {
    std::string name;
    int colorSlot = 0;    // team-colour frame for the row's faction emblem
    int32_t score = 0;    // TA: econ::plateScore (the map's killmul / timemul)
    int built = 0;        // units put into the field
    int kills = 0;        // enemy units destroyed
    int losses = 0;       // own units destroyed
    // TA's plate columns (TotalA.exe 0x41dc20): lifetime energy / metal produced and
    // wasted, each _ftol'd from the player's qword total.
    int32_t energyProduced = 0, metalProduced = 0, energyWasted = 0, metalWasted = 0;
    int timeSec = 0;      // seconds survived (match length for a survivor)
    bool defeated = false;
    bool isLocal = false;
    std::string side; // Faction emblem; colorSlot selects its player-color frame.
};

// The plate for a side with none of its own: the shared TA plate (bitmaps/outcome0.pcx).
inline constexpr int kPlateFallback = -1;

// Everything the screen needs beyond the win/lose flag.
struct ResultStats {
    std::vector<ResultRow> rows;
    int faction = kPlateFallback;   // local player's side (ta::sides index: 0 ARM, 1 CORE)
                                    // -- a data set's victory<prefix>.gui plate, if it
                                    // ships one; a watcher's is its winner's
                                    // (GameView::resultStats)
    int matchSec = 0;
    // A watcher's plate (GameView::resultStats): the line naming the winner, drawn above
    // the title band where a campaign shows the mission name, and the band's word when
    // nobody won ("Game Over"; empty keeps Victory / Defeat).
    std::string watchLine, bandWord;
};

class ResultScreen {
public:
    // Show VICTORY (victory=true) or DEFEAT for `title`. `hasNext` enables the NEXT
    // MISSION button (victory + a mission after this one). `stats` may be empty, in
    // which case the table is omitted. Returns the player's choice. `settings` supplies
    // the cursor scale; `music` keeps the front-end track looping. `skirmishNext`
    // (NewGame or GameSelect, title empty) is where a skirmish's right door leads.
    // menuTip / proceedTip name where the doors really lead when the host knows better
    // (a LAN game's go back to Connect to Server); empty keeps the usual words.
    static ResultChoice run(SDL_Renderer* ren, const hpi::Vfs& vfs, bool victory,
                            const std::string& title, bool hasNext,
                            Settings* settings = nullptr, MenuMusic* music = nullptr,
                            const ResultStats* stats = nullptr,
                            ResultChoice skirmishNext = ResultChoice::Menu,
                            const std::string& menuTip = {}, const std::string& proceedTip = {});
};

}  // namespace ta
