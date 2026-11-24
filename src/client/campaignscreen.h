#pragma once

#include <SDL.h>
#include <filesystem>
#include <memory>
#include <functional>
#include <string>

namespace ta {
namespace hpi { class Vfs; }
struct Settings;
class TaGuiView;

// Retail's BOD book, chapter illustration and previous/next/play controls.
// Every installed chapter remains selectable; completion suggests a starting page.
// Not retail: with <install> given, the left page names the always-enabled
// cosmetic packs (mods/cosmetic/always_enabled/) the mission will load. Campaigns
// offer no mod selection: game mods (always-enabled ones too) and ticked cosmetic
// packs are never used there.
// Mission opponents always play the Classic AI (retail's own); there is no choice.
//
// On Total Annihilation data (no guis/bod.gui) the picker is TA's any-mission screen
// instead: NEWGAME.GUI on its any-mission backdrop playanygame4 (docs/retail-gui.md,
// "New campaign"; docs/retail-campaign.md 1.2). The side buttons and the Choose
// Campaign list are the campaign tabs (camps/*.tdf whose campaignside is the side or
// ALL); Choose Game lists the campaign's missions. Not retail: each mission row
// carries DONE (Settings::campaignCompleted) or PLAY (the next-up mission, which is
// highlighted and selected on entry). Every mission stays playable. Start (or Enter,
// or a double-click on a row) picks the selected mission: pickedCampaign() is then
// "ta:<camps stem>", pickedStem() the TaCampaign spec "<missionfile>:<difficulty>",
// pickedMission() its index and pickedDifficulty() the Set Difficulty stage.
class CampaignScreen {
public:
    CampaignScreen(SDL_Renderer*, const hpi::Vfs&, const Settings&, int initialTab=0,
                   std::function<void(const std::string&)> playSound={},
                   const std::filesystem::path& install={});
    ~CampaignScreen();
    bool input(const SDL_Event&, int winW, int winH);
    void render(int winW, int winH);
    bool picked() const;
    const std::string& pickedStem() const;
    const std::string& pickedCampaign() const;
    // TA data only (see above); 0 / 1 / nullptr otherwise.
    bool taMode() const { return ta_ != nullptr; }
    int pickedMission() const;
    int pickedSide() const;          // the side whose campaigns were showing
    int pickedDifficulty() const;
    void setDifficulty(int stage);   // Easy 0, Medium 1, Hard 2
    // The TA screen's gadgets, for scripted front-end clicks (TA_FRONT_SCRIPT).
    TaGuiView* taView();
    // Press a gadget by name as a click would (scripted runs); true when the picker
    // is done (picked, or backed out), as input() returns.
    bool press(const std::string& gadget);
private:
    struct Impl;
    struct TaPicker;
    std::unique_ptr<Impl> d_;
    std::unique_ptr<TaPicker> ta_;
};
} // namespace ta
