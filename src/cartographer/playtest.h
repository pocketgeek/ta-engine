#pragma once
#include "cartographer/document.h"
#include <filesystem>
#include <optional>
#include <string>

namespace cart {
// Owns the temporary snapshot while the external game runs. Polling never waits
// for the game; closing the editor leaves an already-running test undisturbed.
class Playtest {
public:
    ~Playtest();
    Playtest() = default;
    Playtest(const Playtest&) = delete;
    Playtest& operator=(const Playtest&) = delete;
    static std::filesystem::path clientPath(const std::filesystem::path& executableDirectory);
    // `gameMod` (an AI mod's TEST IN GAME) ticks that mods/game pack for the test room
    // only: takclient --game-mod <pack>, part of the --play-map handoff.
    bool start(const std::filesystem::path& client, const std::filesystem::path& data,
               const DocumentFiles& files, std::string& error, const std::string& gameMod = {});
    std::optional<int> poll();
    bool running() const { return process_ != 0; }
    const std::filesystem::path& snapshot() const { return snapshot_; }
private:
    intptr_t process_ = 0;
    std::filesystem::path directory_, snapshot_;
    void cleanup();
};
}
