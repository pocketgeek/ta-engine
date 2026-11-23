// The menu Benchmark runs with no mod packs of any kind. A scratch data folder holds
// a ticked and an always-enabled pack of each kind (game packs carrying unit data
// and AI profiles); a launch that may use every mod (the menu's default) must, once
// applyBenchmarkMods runs, mount none of them, host none, ask its room for no game
// mods, and keep the retail gameplay hash.
#include "client/benchmarkmods.h"
#include "net/crypto.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include "util/u8path.h"
using namespace tak;
int main() {
    auto root = std::filesystem::temp_directory_path() / ("tak-benchmods-" + crypto::toHex(crypto::randomVec(8)));
    int failures = 0;
    auto check = [&](bool value, const char* label) { if (!value) { std::cerr << "FAIL: " << label << '\n'; ++failures; } };
    auto put = [&](const std::string& path, const std::string& value) {
        auto p = root / tak::u8path(path);
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << value;
    };
    const std::string unit = "[UNITINFO]\n{\nUnitName=modded;\nMaxDamage=5;\n}\n";
    const char* packFiles[] = {"sounds/tick.wav", "sounds/always.wav", "units/tick.fbi", "units/always.fbi",
                               "ai/default.txt", "ai/takengine.txt"};
    try {
        put("mods/cosmetic/Tick/sounds/tick.wav", "c");
        put("mods/cosmetic/always_enabled/AlwaysC/sounds/always.wav", "ca");
        put("mods/game/Tick/units/tick.fbi", unit);
        put("mods/game/Tick/ai/default.txt", "ai");
        put("mods/game/always_enabled/AlwaysG/units/always.fbi", unit);
        put("mods/game/always_enabled/AlwaysG/ai/takengine.txt", "ai");
        const std::vector<std::string> tickedCosmetic{"Tick"};
        const uint64_t retailHash = hpi::gameplayHash(hpi::mountRetailRoot(root));

        // A menu launch: every kind of mod allowed, the player's game pack ticked.
        net::MpClient mp;
        mp.setMapRoot(root);
        hpi::ModUse mods;
        mp.setGameModsAllowed(mods.game);
        mp.setGameModPacks({"Tick"});
        check(mp.hostGameModPacks() == std::vector<std::string>({"AlwaysG", "Tick"}),
              "setup: an ordinary launch hosts the ticked and always-enabled game packs");
        {
            auto vfs = hpi::mountPlayVfs(root, hpi::modFiles(root, hpi::ModKind::Game, mp.hostGameModPacks()),
                                         tickedCosmetic, mods.cosmetic);
            for (const auto* p : packFiles) check(vfs.has(p), "setup: an ordinary game mounts every pack");
        }

        applyBenchmarkMods(mods, &mp);
        check(!mods.game && !mods.cosmetic, "the Benchmark uses no mod kind");
        check(mp.hostGameModPacks().empty() && mp.gameModPacks().empty() && mp.alwaysGameModPacks().empty(),
              "the Benchmark's room hosts no game pack");
        net::GameOptions o;
        o.gameMods = mods.game ? 1 : 0;   // as mpAutoStep builds the Benchmark room
        check(o.gameMods == 0, "the Benchmark room asks for no game mods");
        // What remountMods mounts for the game (the room has no game mods).
        const auto cosmetic = mods.cosmetic ? tickedCosmetic : std::vector<std::string>{};
        std::vector<std::string> skipped;
        auto vfs = mp.modVfs(cosmetic, mods.cosmetic, &skipped);
        for (const auto* p : packFiles) check(!vfs.has(p), "the Benchmark mounts no pack file");
        check(hpi::gameplayHash(vfs) == retailHash, "the Benchmark keeps the retail gameplay hash");
        // A room this launch hosts would build its map from the retail data alone.
        check(!hpi::mountRetailRoot(root, mp.hostGameModPacks()).has("units/always.fbi"),
              "the Benchmark's map data has no game pack");

        // The player's own choices are untouched: the next launch from the menu
        // starts from its own ModUse and MpClient, with every pack again.
        net::MpClient next;
        next.setMapRoot(root);
        hpi::ModUse nextMods;
        next.setGameModsAllowed(nextMods.game);
        next.setGameModPacks({"Tick"});
        check(nextMods.game && nextMods.cosmetic && next.hostGameModPacks().size() == 2,
              "the next game uses the player's mods again");
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        ++failures;
    }
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    if (failures) return 1;
    std::cout << "benchmark_mods: OK\n";
    return 0;
}
