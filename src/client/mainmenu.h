#pragma once

// The retail Total Annihilation: Kingdoms front-end: the three-door main menu.
// Layout is data-driven from guis/mainmenu.gui (background, door + button rects,
// GAF art); each door plays its Bink (.bik) hover video (idle -> hover-in -> loop
// -> hover-out) when FFmpeg is available, falling back to the GAF door art. Returns
// the player's choice so the app can enter single-player, multiplayer, etc.

#include <SDL.h>

#include <string>

#include "hpi/hpi.h"

namespace tak {

class MenuMusic;
struct Settings;

class MainMenu {
public:
    enum class Choice { None, SinglePlayer, Campaign, Multiplayer, Options, Benchmark,
                        Credits, Replay, Exit };

    // ren: the app renderer. vfs: the mounted retail root (for guis/anims). install:
    // the retail install dir on disk (the door .bik videos are loose files under
    // Movies/Gui/, not in the VFS).
    MainMenu(SDL_Renderer* ren, const hpi::Vfs& vfs, std::string install);
    ~MainMenu();
    MainMenu(const MainMenu&) = delete;
    MainMenu& operator=(const MainMenu&) = delete;

    // Run the front-end loop until the user picks a door/button or closes the
    // window. If shotPath is non-empty, render a single frame there and return None
    // (headless screenshot for tests). On a Multiplayer choice, *serverOut (if given)
    // receives the chosen server address (empty = default/localhost). `music` (if
    // given) is polled each frame so the shared background track keeps looping.
    // A failed multiplayer connect loops back here: run() reopens the CONNECT
    // dropdown with this message shown in red, instead of the app exiting.
    void setConnectError(const std::string& msg);

    Choice run(const std::string& shotPath = "", std::string* serverOut = nullptr,
               MenuMusic* music = nullptr, Settings* settings = nullptr);

    // After run() returns Choice::Campaign, the campaign mission stem the player picked
    // (e.g. "takmission01_mt") and the campaign id it belongs to; empty otherwise.
    const std::string& chosenMission() const;
    const std::string& chosenCampaign() const;
    // After run() returns Choice::Multiplayer, the account the player signed in
    // with. chosenPassword() is live only until the connect has used it -- call
    // clearPassword() the moment it has, so it is not sitting in memory while the
    // game runs. It is never written to disk (only the NAME is remembered).
    const std::string& chosenAccount() const;
    const std::string& chosenPassword() const;
    void clearPassword();

    // After run() returns Choice::Benchmark, the picked intensity level (1=Low..5=Absurd).
    int chosenBenchmarkLevel() const;
    // Full path of the .takrep the user picked (Choice::Replay). Empty otherwise.
    const std::string& chosenReplay() const;

    // Play a fullscreen intro clip (Movies/<nameLower>, e.g. "logo.bik") once, scaled
    // to fill the window (letterboxed, linear-filtered). Returns when the clip ends or
    // the user presses any key / clicks / closes the window. No-op if the clip or
    // FFmpeg is missing. Video only -- the clip's audio track is not played.
    static void playIntro(SDL_Renderer* ren, const std::string& install,
                          const char* nameLower = "logo.bik");

private:
    struct Impl;
    Impl* d_;
};

}  // namespace tak
