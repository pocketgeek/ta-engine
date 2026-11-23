#pragma once

// Total Annihilation's front end, driven by the retail .GUI screens
// (docs/retail-gui.md section 2): MAINMENU -> SINGLE -> SKIRMISH (+ SELMAP), with
// the backdrops, buttons and defaults of TotalA.exe. Returns what the player chose;
// main.cpp launches it (a skirmish on a private local server, seated and started
// from the SkirmishSetup).

#include <SDL.h>

#include <string>
#include <vector>

namespace ta::hpi { class Vfs; }

namespace ta {

struct SkirmishSeat {
    int controller = 0;   // 0 Open, 1 Player, 2 Computer (Player<n>Controller)
    int side = 0;         // sidedata index (alternating Arm/Core by default)
    int color = 0;        // 0..9
    int ally = 5;         // ally group 0..4, 5 = none
    int metal = 1000, energy = 1000;
};

struct SkirmishSetup {
    std::string map;                 // map name (findMap)
    bool commanderEnds = true;       // CommanderDeath "Game ends|Continues"
    bool fixedStarts = true;         // StartLocation "Fixed|Random"
    bool mapped = false;             // Mapping "Unmapped|Mapped"
    int lineOfSight = 1;             // 0 Permanent, 1 True, 2 Circular
    int difficulty = 1;              // 0 Easy, 1 Medium, 2 Hard
    std::vector<SkirmishSeat> seats; // NumSkirmishPlayers rows
};

class TaFrontEnd {
public:
    enum class Choice { Exit, Skirmish, Multiplayer, Campaign, Options };
    TaFrontEnd(SDL_Renderer* ren, SDL_Window* win, const hpi::Vfs& vfs);
    // Runs until a choice is made. `setup` is filled for Skirmish (and kept between
    // calls, like retail's registry-backed skirmish settings).
    Choice run(SkirmishSetup& setup);

private:
    enum class Page { Main, Single, Skirmish, SelectMap };
    Page page_ = Page::Main;
    SDL_Renderer* ren_;
    SDL_Window* win_;
    const hpi::Vfs& vfs_;
};

// Retail's first-run skirmish defaults (docs/retail-gui.md "Skirmish defaults").
SkirmishSetup defaultSkirmish(const hpi::Vfs& vfs);

}  // namespace ta
