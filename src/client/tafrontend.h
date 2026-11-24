#pragma once

// Total Annihilation's front end, driven by the retail .GUI screens
// (docs/retail-gui.md section 2): MAINMENU -> SINGLE -> SKIRMISH (+ SELMAP), with
// the backdrops, buttons and defaults of TotalA.exe. Returns what the player chose;
// main.cpp launches it (a skirmish on a private local server, seated and started
// from the SkirmishSetup).

#include <SDL.h>

#include "client/cursors.h"
#include "client/settings.h"

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

// A Total Annihilation campaign in play (docs/retail-campaign.md section 1):
// camps/<name>.tdf's missions, the mission being played, the difficulty and the
// 25-slot U/W/L result string. Retail keeps no persistent unlock; neither do we.
struct TaCampaign {
    struct Mission { std::string file, name; };   // missionfile (with .ota), missionname
    std::string name;                             // camps/<name>.tdf stem
    std::vector<Mission> missions;
    int side = 0, cur = 0, difficulty = 1;
    std::string results = std::string(25, 'U');
    bool active() const { return !missions.empty(); }
    // The mission spec setupMission takes ("<missionfile>:<difficulty>") and its map name.
    std::string spec() const;
    std::string mapName() const;
};
// camps/*.tdf sorted by name whose [HEADER] campaignside is the side's name or ALL (0x476a60).
std::vector<std::string> taCampaignList(const hpi::Vfs& vfs, const std::string& sideName);
// MISSION0.. up to the first gap; missionname per language falls back to the plain key.
bool loadTaCampaign(const hpi::Vfs& vfs, const std::string& name, TaCampaign& out);

class TaFrontEnd {
public:
    // EngineSingle / EngineMulti: the title screen's SINGLE and MULTI doors open the
    // engine's own screens (the single-player lobby, the CONNECT screen); Options
    // opens the engine's SETTINGS menu.
    enum class Choice { Exit, Skirmish, Multiplayer, Campaign, Options, Menu, EngineSingle, EngineMulti,
                        EngineCampaign };
    TaFrontEnd(SDL_Renderer* ren, SDL_Window* win, const hpi::Vfs& vfs);
    // Runs until a choice is made. `setup` is filled for Skirmish (and kept between
    // calls, like retail's registry-backed skirmish settings). Campaign: NEWGAME ->
    // MSNBRIEF Start, with `camp` holding the mission to play.
    Choice run(SkirmishSetup& setup, TaCampaign& camp);
    // After a campaign mission (outcome +1 won / -1 lost): records the result, then
    // ENDMSN. Start -> MSNBRIEF -> Choice::Campaign (camp.cur is the pick); MainMenu
    // -> Choice::Menu.
    Choice endMission(SkirmishSetup& setup, TaCampaign& camp, int outcome);
    // A won mission's glamour picture and sound (docs/retail-campaign.md 4.3).
    void showGlamour(const std::string& glamour, const std::string& sound);
    // A launch that failed: reopen the screen it was picked from (SELPROV for a
    // multiplayer connect, NEWGAME for a campaign, else the main menu) with the reason.
    void setFailure(int failTo, std::string message);
    // MULTI -> SELPROV -> TCP: the server address typed ("" = search the local network).
    const std::string& chosenServer() const { return server_; }

private:
    enum class Page { Main, Single, Skirmish, SelectMap, SelProv, NewGame, Brief, EndMission };
    Page start_ = Page::Main;
    CursorSet cursors_;              // TA's pointer (anims/cursors.gaf)
    Settings cursorSettings_;        // cursorScale / hardwareCursor
    bool cursorsLoaded_ = false;
    std::string failure_;
    int outcome_ = 0;
    std::string server_;
    Page page_ = Page::Main;
    SDL_Renderer* ren_;
    SDL_Window* win_;
    const hpi::Vfs& vfs_;
};

// Retail's first-run skirmish defaults (docs/retail-gui.md "Skirmish defaults").
SkirmishSetup defaultSkirmish(const hpi::Vfs& vfs);

}  // namespace ta
