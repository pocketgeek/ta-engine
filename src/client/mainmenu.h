#pragma once

// The retail Total Annihilation: Kingdoms front-end: the three-door main menu.
// Layout is data-driven from guis/mainmenu.gui (background, door + button rects,
// GAF art); each door plays its Bink (.bik) hover video (idle -> hover-in -> loop
// -> hover-out) when FFmpeg is available, falling back to the GAF door art. Returns
// the player's choice so the app can enter single-player, multiplayer, etc.

#include <SDL.h>

#include <string>

#include "client/options.h"
#include "client/sessionfailure.h"
#include "hpi/hpi.h"

namespace tak {

class VoiceEngine;

class MenuMusic;
class Streaming;
struct Settings;

class MainMenu {
public:
    // Restart: Options changed the GAME DATA FOLDER and the player chose RESTART NOW;
    // main() leaves its loop, tears down and relaunches (client/datafolder.h).
    enum class Choice { None, SinglePlayer, Campaign, Multiplayer, Options, Benchmark,
                        Credits, Replay, Exit, Restart };

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
    // A launch that failed loops back here: the next run() reopens the screen the
    // launch was picked from (failure.to: the CONNECT dropdown, the replay picker,
    // the benchmark or campaign picker, or the bare title screen) and shows
    // failure.message there, instead of the app exiting. One channel for every
    // launch, so a failure cannot reopen the wrong screen -- answering a refused
    // replay or a dead single-player server with the multiplayer sign-in dialog
    // has shipped before.
    void setFailure(const SessionFailure& failure);
    // Open on the CONNECT screen with `server` picked, at the top of run().
    void reopenConnect(const std::string& server);

    // The Options screen's GAME DATA FOLDER row: the folder this launch mounted, and why
    // it cannot be changed (a --data or launch-folder override), if it cannot.
    void setDataFolder(DataFolderRow row);

    // Voice chat (client/voice.h): main()'s engine, so the SETTINGS menu's VOICE CHAT
    // screen can show the microphone level. Pumped every frame while the menu runs.
    void setVoice(VoiceEngine* voice);

    Choice run(const std::string& shotPath = "", std::string* serverOut = nullptr,
               MenuMusic* music = nullptr, Settings* settings = nullptr, Streaming* streaming = nullptr);

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
    // How that Multiplayer choice connects: a server (with an account), this player's
    // own LOCAL SERVER to start and host on, or a LAN game heard on the network (the
    // name only; *serverOut is its tcp:// address) to play in or to watch.
    enum class Connect { Server, LanHost, LanJoin, LanSpectate };
    Connect chosenConnect() const;

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
