#pragma once

// The retail loading screen (`guis/loadscreen.gui`): the LoadingBG plate, the map name,
// a status line, a percentage, one overall progress bar, and a per-player name + bar
// column down the left so you can see who the game is still waiting on.
//
// Used as a scoped object around the heavy part of starting a game: construct it, call
// step() as each phase finishes, and let it fall out of scope when the first tick runs.
// Each step() draws and presents a frame itself, because the work it brackets blocks
// the render loop.

#include <SDL.h>

#include <array>
#include <string>

#include <memory>
#include <vector>

#include "client/guiart.h"
#include "video/bink.h"

namespace ta::hpi { class Vfs; }

namespace ta {

struct Settings;

class LoadScreen {
public:
    LoadScreen(SDL_Renderer* ren, const hpi::Vfs& vfs, const std::string& mapName,
               Settings* settings = nullptr);
    ~LoadScreen();

    LoadScreen(const LoadScreen&) = delete;
    LoadScreen& operator=(const LoadScreen&) = delete;

    // Name the players in the game, in slot order; an empty name means the slot is
    // open/closed and gets no row. `you` is the local slot (its bar tracks our own
    // progress); pass -1 when spectating.
    void setPlayers(const std::array<std::string, 8>& names, int you);

    // Mark a remote slot finished (its bar snaps to full). Driven by the server's
    // per-slot loaded status once every peer reports in.
    void setSlotDone(int slot);

    // Advance to `pct` (0..100) with `status` under the bar, then draw and present.
    void step(const std::string& status, int pct);

    // True under the dummy video driver: the screen draws nothing, so a harness run
    // should skip it entirely rather than hold the frame.
    bool headless() const { return headless_; }

    // Draw the current state into whatever target is bound, WITHOUT presenting --
    // for the render loop, which owns the present (and the AA resolve before it).
    void draw();

private:
    // Draw and present immediately, bypassing the render loop's AA target. Used by
    // step(), which runs inside blocking load work that never reaches a present.
    void present();

    SDL_Renderer* ren_ = nullptr;
    const hpi::Vfs* vfs_ = nullptr;
    Settings* settings_ = nullptr;
    SDL_Texture* bg_ = nullptr;
    // Loadscreen.bik plays in the plate's arch, exactly as retail does. Decoded one
    // frame per present(), so a long load actually animates instead of sitting still.
    video::BinkVideo movie_;
    SDL_Texture* movieTex_ = nullptr;
    std::vector<uint8_t> movieRgba_;
    uint64_t movieStartMs_ = 0;
    int movieFrame_ = -1;
    std::string map_, status_;
    int pct_ = 0;
    std::array<std::string, 8> names_{};
    std::array<bool, 8> done_{};
    int you_ = -1;
    bool headless_ = false;
};

}  // namespace ta
