#pragma once

// The campaign / mission picker overlay, hosted by the main menu's "PlayStory" door.
// Lists the installed campaigns (Book of Darien, The Iron Plague, its alt ending) and,
// for the selected one, its missions -- each marked DONE or PLAY (nothing is ever
// locked), with the first unbeaten mission highlighted as the suggested next. Picking
// a mission closes the overlay with a chosen stem the host launches through the mission
// runner. Completion comes from ta::Settings (campaignCompleted), which the host
// records on victory. Draws with the shared block font; owns no game state.

#include <SDL.h>

#include <string>
#include <vector>

#include "campaign/campaign.h"

namespace ta {

namespace hpi { class Vfs; }
struct Settings;

class CampaignScreen {
public:
    // ren: the app renderer. vfs: the mounted retail root (to load camps/*.tdf).
    // settings: read for per-campaign progress (not mutated here).
    CampaignScreen(SDL_Renderer* ren, const hpi::Vfs& vfs, const Settings& settings,
                   int initialTab = 0);

    // Feed one SDL event. Returns true when the overlay should close (BACK / Esc, or a
    // mission was picked). After it returns true the host checks picked().
    bool input(const SDL_Event& e, int winW, int winH);

    // Draw the overlay (call after the host's own frame so it sits on top).
    void render(int winW, int winH);

    // Set once the user clicks a playable mission.
    bool picked() const { return picked_; }
    const std::string& pickedStem() const { return pickedStem_; }
    const std::string& pickedCampaign() const { return pickedCampaign_; }

private:
    struct Row {
        SDL_FRect rect{}; int mission = -1; bool playable = false;
        std::string title;   // chapter name from translate/missions.tdf ("" = none)
    };
    void layout(int winW, int winH);

    SDL_Renderer* ren_;
    const Settings& settings_;
    std::vector<Campaign> camps_;
    int tab_ = 0;               // selected campaign index
    float scroll_ = 0;          // mission-list scroll (px)
    float contentH_ = 0;
    float u_ = 1.0f;            // layout unit
    SDL_FRect panel_{}, listClip_{}, backRect_{};
    std::vector<SDL_FRect> tabRects_;
    std::vector<Row> rows_;
    bool picked_ = false;
    std::string pickedStem_, pickedCampaign_;
};

}  // namespace ta
