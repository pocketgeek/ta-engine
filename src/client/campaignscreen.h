#pragma once

#include <SDL.h>
#include <filesystem>
#include <memory>
#include <functional>
#include <string>

namespace tak {
namespace hpi { class Vfs; }
struct Settings;

// Retail's BOD book, chapter illustration and previous/next/play controls.
// Every installed chapter remains selectable; completion suggests a starting page.
// Not retail: with <install> given, the left page names the always-enabled
// cosmetic packs (mods/cosmetic/always_enabled/) the mission will load. Campaigns
// offer no mod selection: game mods (always-enabled ones too) and ticked cosmetic
// packs are never used there.
// Mission opponents always play the Classic AI (retail's own); there is no choice.
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
private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
} // namespace tak
