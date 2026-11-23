#pragma once
#include <SDL.h>
#include <functional>
#include <string>
#include "hpi/hpi.h"
namespace tak {
class MenuMusic;
struct Settings;
class BriefingScreen {
public:
    // Native Briefing.gui is a paused overlay on the loaded campaign. The caller
    // holds the initial ready barrier until dismissal. Background is borrowed;
    // idle services the connection and returns false if the session ends.
    // `note` (e.g. the always-on cosmetic mods) follows the objectives, wrapped.
    static bool run(SDL_Renderer*, const hpi::Vfs&, const std::string& stem,
                    const std::string& title, Settings* = nullptr, MenuMusic* = nullptr,
                    SDL_Texture* background = nullptr, std::function<bool()> idle = {},
                    int chapter = 0, const std::string& note = {});
};
}
