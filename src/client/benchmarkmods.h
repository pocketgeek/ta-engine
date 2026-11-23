#pragma once
// The menu Benchmark measures the plain retail game, so its run uses no mod pack of
// any kind: no cosmetic packs (ticked or mods/cosmetic/always_enabled/), no game
// packs (ticked or mods/game/always_enabled/), so no mod-supplied unit data, AI
// profiles (ai/*.txt) or maps either. The room it creates then asks for no game
// mods, and the server keeps a benchmark room at the retail data whatever it is
// sent. Only this launch's GameView and MpClient change: the player's own mod
// choices (settings, --mods) are untouched and apply again back at the menu.
#include "hpi/hpi.h"
#include "net/client.h"

namespace tak {

inline constexpr hpi::ModUse kBenchmarkModUse{false, false};

// Apply the Benchmark's no-mods rule to one launch: `mods` is that GameView's mod
// use (what remountMods mounts and the room's gameMods byte), `mp` its connection
// (the game packs a hosted room would send). Call before the room is created.
inline void applyBenchmarkMods(hpi::ModUse& mods, net::MpClient* mp) {
    mods = kBenchmarkModUse;
    if (mp) mp->setGameModsAllowed(false);   // drops ticked AND always-enabled game packs
}

}  // namespace tak
