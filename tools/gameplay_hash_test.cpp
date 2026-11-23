// hpi::gameplayHash covers what the sim loads through the VFS: unit, weapon and side
// data, and each unit's script (scripts/<id>.cob: production, corpse rules) and model
// (objects3d/<id>.3do: production transforms, sight and projectile geometry). Every
// change to those, an addition or a removal, changes the hash; the order an install
// lists them in, and the case or separators of their names, do not. Cosmetic files,
// cosmetic packs and a room's own map resources stay hash-neutral.
#include "hpi/hpi.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>
#include "util/u8path.h"

using namespace tak;
using Entries = std::vector<std::pair<std::string, std::string>>;

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("tak-gameplay-hash-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code e; fs::remove_all(path, e); } } cleanup{root};
    int failures = 0;
    auto check = [&](bool ok, const char* label) { if (!ok) { std::cerr << "FAIL: " << label << '\n'; ++failures; } };
    auto put = [&](const fs::path& path, const std::string& value) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << value;
    };
    auto archive = [&](const fs::path& path, const Entries& entries) {
        std::vector<hpi::PackFile> files;
        for (const auto& [name, value] : entries) files.push_back({name, std::vector<uint8_t>(value.begin(), value.end())});
        const auto bytes = hpi::pack(files);
        put(path, std::string(bytes.begin(), bytes.end()));
    };
    auto stones = [](const std::string& keys) { return "[Stone1]\n{\nfootprintx=2;\nfootprintz=2;\n" + keys + "}\n"; };
    const std::string unit = "[UNITINFO]\n{\nUnitName=knight;\nObjectName=knight;\nMaxDamage=5;\n}\n";
    const Entries base = {
        {"units/knight.fbi", unit},
        {"scripts/knight.cob", "script-a"},
        {"objects3d/knight.3do", "model-a"},
        {"objects3d/rock1.3do", "corpse-a"},
        {"sounds/knight.wav", "sound-a"},
        {"gamedata/sound.tdf", "[KNIGHT]\n{\nselect=a;\n}\n"},
        {"features/stones.tdf", stones("height=20;\nsacredsite=1;\nanimatable=1;\nfilename=stones;\n")},
    };
    // One install per variant: <root>/<name>/data.hpi (plus any extra archive or pack).
    auto install = [&](const std::string& name, Entries entries) {
        archive(root / name / "data.hpi", entries);
        return root / name;
    };
    auto with = [&](Entries entries, const std::string& path, const std::string& value) {
        bool replaced = false;
        for (auto& [name, bytes] : entries) if (name == path) { bytes = value; replaced = true; }
        if (!replaced) entries.push_back({path, value});
        return entries;
    };
    auto without = [&](Entries entries, const std::string& path) {
        std::erase_if(entries, [&](const auto& e) { return e.first == path; });
        return entries;
    };
    auto hashOf = [&](const fs::path& dir, const std::vector<std::string>& game = {},
                      const std::vector<std::string>& cosmetic = {}) {
        return hpi::gameplayHash(hpi::mountRetailRoot(dir, game, cosmetic));
    };
    try {
        const auto stock = install("stock", base);
        const uint64_t want = hashOf(stock);
        check(hashOf(stock) == want, "the hash is repeatable");

        // Unit-definition sensitivity (always covered).
        check(hashOf(install("unit", with(base, "units/knight.fbi",
                                          "[UNITINFO]\n{\nUnitName=knight;\nObjectName=knight;\nMaxDamage=6;\n}\n"))) != want,
              "a changed unit definition changes the hash");
        // Scripts and models.
        check(hashOf(install("cob", with(base, "scripts/knight.cob", "script-b"))) != want,
              "a changed unit script changes the hash");
        check(hashOf(install("3do", with(base, "objects3d/knight.3do", "model-b"))) != want,
              "a changed unit model changes the hash");
        check(hashOf(install("corpse", with(base, "objects3d/rock1.3do", "corpse-b"))) != want,
              "a changed model no unit names changes the hash");
        check(hashOf(install("addcob", with(base, "scripts/archer.cob", "script-c"))) != want,
              "an added script changes the hash");
        check(hashOf(install("add3do", with(base, "objects3d/archer.3do", "model-c"))) != want,
              "an added model changes the hash");
        check(hashOf(install("nocob", without(base, "scripts/knight.cob"))) != want,
              "a removed script changes the hash");
        check(hashOf(install("no3do", without(base, "objects3d/knight.3do"))) != want,
              "a removed model changes the hash");
        // Moving content between a script and a model is not the same data.
        check(hashOf(install("swap", with(with(base, "scripts/knight.cob", "model-a"), "objects3d/knight.3do", "script-a"))) != want,
              "swapped script/model content changes the hash");

        // Feature definitions: every key the sim's feature loader reads is covered,
        // while art keys stay free for reskins.
        check(hashOf(install("height", with(base, "features/stones.tdf",
                                            stones("height=30;\nsacredsite=1;\nanimatable=1;\nfilename=stones;\n")))) != want,
              "a feature's projectile height changes the hash");
        check(hashOf(install("sacred", with(base, "features/stones.tdf",
                                            stones("height=20;\nsacredsite=2;\nanimatable=1;\nfilename=stones;\n")))) != want,
              "a feature's sacred-site rate changes the hash");
        check(hashOf(install("animatable", with(base, "features/stones.tdf",
                                                stones("height=20;\nsacredsite=1;\nanimatable=0;\nfilename=stones;\n")))) != want,
              "a feature's animatable flag changes the hash");
        check(hashOf(install("reskin", with(base, "features/stones.tdf",
                                            stones("height=20;\nsacredsite=1;\nanimatable=1;\nfilename=other;\n")))) == want,
              "a feature's art keys do not change the hash");

        // Canonical: archive order, names' case and separators, and how the files are
        // split across the install's archives do not matter.
        {
            Entries reordered(base.rbegin(), base.rend());
            for (auto& [name, bytes] : reordered) {
                if (name == "scripts/knight.cob") name = "Scripts\\KNIGHT.COB";
                else if (name == "objects3d/knight.3do") name = "OBJECTS3D/Knight.3DO";
                else if (name == "units/knight.fbi") name = "Units/Knight.FBI";
            }
            check(hashOf(install("reordered", reordered)) == want,
                  "archive order, case and separators do not change the hash");
            const auto split = install("split", {base[0], base[4], base[6]});
            archive(split / "ipdata.hpi", {base[5], base[3], base[2], base[1]});
            check(hashOf(split) == want, "the same files split across two archives hash the same");
        }

        // Cosmetic-only differences are hash-neutral: sounds, cosmetic game data, a
        // script's source and a stray file beside the models.
        check(hashOf(install("cosmetic", with(with(with(with(base, "sounds/knight.wav", "sound-b"),
                                                            "gamedata/sound.tdf", "[KNIGHT]\n{\nselect=b;\n}\n"),
                                                       "scripts/knight.bos", "source"),
                                                  "objects3d/readme.txt", "notes"))) == want,
              "cosmetic files do not change the hash");

        // Mod packs: a cosmetic pack cannot carry scripts or models (and its other files
        // are cosmetic), so it never changes the hash; a game pack's script or model does.
        {
            put(stock / "mods/cosmetic/Skin/scripts/knight.cob", "script-skin");
            put(stock / "mods/cosmetic/Skin/objects3d/knight.3do", "model-skin");
            put(stock / "mods/cosmetic/Skin/sounds/knight.wav", "sound-skin");
            check(hashOf(stock, {}, {"Skin"}) == want, "a cosmetic pack never changes the hash");
            put(stock / "mods/game/Rules/Scripts/Knight.cob", "script-rules");
            check(hashOf(stock, {"Rules"}) != want, "a game pack's script changes the hash");
            put(stock / "mods/game/Shapes/objects3d/knight.3do", "model-shapes");
            check(hashOf(stock, {"Shapes"}) != want, "a game pack's model changes the hash");
            archive(stock / "mods/game/Same/same.hpi", {{"scripts/knight.cob", "script-a"}, {"objects3d/knight.3do", "model-a"}});
            check(hashOf(stock, {"Same"}) == want, "a game pack repeating the stock files keeps the hash");
            // A loose mixed-case pack and an archived lowercase one with equal content agree.
            put(stock / "mods/game/Loose/Scripts/KNIGHT.COB", "script-z");
            archive(stock / "mods/game/Packed/packed.hpi", {{"scripts/knight.cob", "script-z"}});
            check(hashOf(stock, {"Loose"}) == hashOf(stock, {"Packed"}), "loose and archived pack files hash the same");
        }

        // Per-room map resources: a map archive under Maps/ (its scripts/models are
        // dropped at mount; its feature definitions belong to the room) and a room's
        // map files leave the handshake hash alone.
        {
            const auto mapped = install("mapped", base);
            archive(mapped / "Maps/Arena.kmp", {{"kmap/arena.tnt", "tnt"}, {"kmap/arena.ota", "ota"},
                                                {"features/arena/rock.tdf", "[rock]\n{\nfootprintx=3;\n}\n"},
                                                {"scripts/knight.cob", "script-map"},
                                                {"objects3d/knight.3do", "model-map"}});
            auto vfs = hpi::mountRetailRoot(mapped);
            check(vfs.has("kmap/arena.tnt") && vfs.has("features/arena/rock.tdf"), "the map archive is mounted");
            check(hpi::gameplayHash(vfs) == want, "a map archive leaves the hash alone");
            hpi::Vfs room(&vfs);
            room.setMapFiles(std::make_shared<const hpi::Vfs::Files>(hpi::Vfs::Files{
                {"kmap/arena.tnt", {'t'}}, {"features/arena/tree.tdf", {'f'}}}));
            check(hpi::gameplayHash(room) == want, "a room's map files leave the hash alone");
        }
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        ++failures;
    }
    if (failures) { std::cerr << failures << " failure(s)\n"; return 1; }
    std::cout << "gameplay hash: ok\n";
    return 0;
}
