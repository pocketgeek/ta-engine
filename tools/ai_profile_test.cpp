// Our AI levels' built-in build profiles and the one file that adjusts them all,
// ai/takengine.txt (ai/builtinprofile.h).
//
//   ai_profile_test           the tables, the line format, and which mod layers may
//                             supply ai/takengine.txt (scratch mod folders, no data)
//   ai_profile_test <data>    also: the built-in Normal table still equals the
//                             retail default profile (the base install's winning
//                             ai/default.txt) the levels were tuned on; and the
//                             AI files (ai/default.txt, ai/takengine.txt) travel
//                             in a map's package when the map's own archive
//                             carries them, and a game mod's copy wins over the
//                             map's (the map's over the base install's); two
//                             installed maps carrying the same AI file names each
//                             send their own
#include "ai/ai.h"
#include "ai/builtinprofile.h"
#include "ai/classicprofile.h"
#include "hpi/hpi.h"
#include "net/crypto.h"
#include "net/mappackage.h"
#include "net/modpackage.h"
#include "tnt/ota.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

using namespace tak;

static int failures = 0;
static void check(bool ok, const std::string& label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok) ++failures;
}

static bool same(const ai::Profile& a, const ai::Profile& b) { return a.weight == b.weight && a.limit == b.limit; }

// The first difference between two profiles, for a failing comparison's log.
static std::string firstDifference(const ai::Profile& want, const ai::Profile& got) {
    for (const auto* kind : {"weight", "limit"}) {
        const auto& w = std::string(kind) == "weight" ? want.weight : want.limit;
        const auto& g = std::string(kind) == "weight" ? got.weight : got.limit;
        for (const auto& [unit, v] : w) {
            const auto it = g.find(unit);
            if (it == g.end()) return std::string(kind) + " " + unit + ": missing from the built-in table";
            if (it->second != v) return std::string(kind) + " " + unit + ": file " + std::to_string(v) +
                                        ", built-in " + std::to_string(it->second);
        }
        for (const auto& [unit, v] : g)
            if (!w.count(unit)) return std::string(kind) + " " + unit + ": built-in only";
    }
    return "none";
}

static void tables() {
    const auto& normal = ai::builtinProfile(ai::Difficulty::Normal);
    check(normal.weight.size() == 146 && normal.limit.size() == 146, "the built-in table holds 146 units, weight + limit each");
    check(normal.weight.at("aradrag") == 25 && normal.limit.at("aradrag") == 1, "an entry reads back (aradrag 25 / 1)");
    // Behaviour must not change yet: every level still plays the profile it was tuned on.
    for (uint8_t level = 0; level < ai::kAiLevels; ++level) {
        const auto d = ai::Difficulty(level);
        if (d == ai::Difficulty::Classic) continue;
        check(same(ai::builtinProfile(d), normal), "level " + std::to_string(level) + "'s table equals Normal's (no level diverges yet)");
    }
    const auto& classic = ai::builtinProfile(ai::Difficulty::Classic);
    check(classic.weight.empty() && classic.limit.empty() && !ai::levelProfileFile(ai::Difficulty::Classic),
          "Classic has no built-in table and no mod file");
    bool ok = true;
    for (uint8_t level = 0; level < ai::kAiLevels; ++level) {
        const auto d = ai::Difficulty(level);
        if (d == ai::Difficulty::Classic) continue;
        ok = ok && ai::levelProfileFile(d) && std::string(ai::levelProfileFile(d)) == "ai/takengine.txt";
    }
    check(ok && std::string(ai::kEngineProfileFile) == "ai/takengine.txt",
          "every one of our levels reads the one file ai/takengine.txt");

    ai::Profile p;
    ai::applyProfileText("// comment\r\nweight AraBow 3\r\nlimit arabow 0\nlimit arabow 2 trailing\nplan x 1\n/weight arafly 9\nweight arafly\n", p);
    check(p.weight.size() == 1 && p.weight.at("arabow") == 3 && p.limit.size() == 1 && p.limit.at("arabow") == 2,
          "line format: lowercased units, CRLF, later line wins, comments/other keywords/short lines skipped");
    ai::Profile tower;
    ai::applyProfileText("weight AraSeige 3\nlimit arasiege 5\n", tower);
    check(tower.weight.size() == 1 && tower.weight.at("arasiege") == 3 && tower.limit.at("arasiege") == 5 &&
          ai::builtinProfile(ai::Difficulty::Normal).limit.at("arasiege") == 10,
          "the Rolling Tower reads under either spelling (the shipped files say araseige); the built-in limit is 10");
}

static void put(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}
static void archive(const std::filesystem::path& file, std::vector<std::pair<std::string, std::string>> entries) {
    std::vector<hpi::PackFile> files;
    for (auto& [name, text] : entries) files.push_back({name, std::vector<uint8_t>(text.begin(), text.end())});
    const auto bytes = hpi::pack(files);
    put(file, std::string(bytes.begin(), bytes.end()));
}

// The effective profile a level must have when `changes` is ai/takengine.txt: the
// level's OWN built-in table with exactly these entries replaced.
static ai::Profile over(ai::Difficulty d, const std::vector<std::tuple<const char*, int, int>>& changes) {
    ai::Profile p = ai::builtinProfile(d);
    for (const auto& [unit, weight, limit] : changes) {
        if (weight >= 0) p.weight[unit] = weight;
        if (limit >= 0) p.limit[unit] = limit;
    }
    return p;
}

// Each of our levels (Classic excluded) passes `test`.
template <class F> static bool everyLevel(F test) {
    bool ok = true;
    for (uint8_t level = 0; level < ai::kAiLevels; ++level)
        if (ai::Difficulty(level) != ai::Difficulty::Classic) ok = ok && test(ai::Difficulty(level));
    return ok;
}

static void modLayers() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("tak-ai-profiles-" + crypto::toHex(crypto::randomVec(8)));
    const std::string engineFile = "// a game mod's profile for our levels\nlimit arabow 0\nweight AraBow 2\nweight modunit 7\n";
    // A loose file in a game pack's ai/ folder, and ai/ inside an archive in another.
    put(root / "mods/game/my_ai/AI/TAKENGINE.TXT", engineFile);
    archive(root / "mods/game/archived/ai.hpi", {{"ai/takengine.txt", "limit arafly 0\n"}});
    // Other file names in ai/ are not read, in a game pack's ai/ or an archive's.
    put(root / "mods/game/old/ai/n.txt", "limit arabow 0\n");
    archive(root / "mods/game/old/ai.hpi", {{"ai/h.txt", "limit arabow 0\n"}, {"ai/ad.txt", "limit arabow 0\n"}});
    // Not the file: at a pack's root (loose or an archive's), or in another folder.
    put(root / "mods/game/rooted/takengine.txt", "limit arabow 0\n");
    archive(root / "mods/game/rooted/root.hpi", {{"takengine.txt", "limit arabow 0\n"}});
    put(root / "mods/game/nested/sub/takengine.txt", "limit arabow 0\n");
    put(root / "mods/game/nested/sub/ai/takengine.txt", "limit arabow 0\n");
    // Cosmetic packs, ticked or always-enabled, can never supply one.
    put(root / "mods/cosmetic/skin/ai/takengine.txt", "limit arabow 0\n");
    put(root / "mods/cosmetic/skin/sounds/x.wav", "x");
    archive(root / "mods/cosmetic/always_enabled/always/always.hpi", {{"ai/takengine.txt", "limit araking 0\n"}, {"sounds/y.wav", "y"}});
    // An always-enabled GAME pack can: it travels in the host's package like a ticked one.
    put(root / "mods/game/always_enabled/house_ai/ai/takengine.txt", "limit arafly 0\n");
    // The base install's own ai/ folder (in a root archive) is not a game mod.
    archive(root / "data.hpi", {{"ai/takengine.txt", "limit arabow 0\n"}, {"gamedata/x.tdf", "x"}});
    const auto builtinAll = [](const hpi::Vfs::Files* files) {
        return everyLevel([&](ai::Difficulty d) {
            std::string s;
            return same(ai::levelProfile(d, files, nullptr, &s), ai::builtinProfile(d)) && s.empty();
        });
    };
    try {
        const auto loose = hpi::modFiles(root, hpi::ModKind::Game, {"my_ai"});
        check(loose.count("ai/takengine.txt") == 1, "a game pack's loose ai/takengine.txt is in its files (case-insensitive name)");
        const std::vector<std::tuple<const char*, int, int>> changes{{"arabow", 2, 0}, {"modunit", 7, -1}};
        check(everyLevel([&](ai::Difficulty d) {
                  std::string source;
                  const auto p = ai::levelProfile(d, &loose, nullptr, &source);
                  return source == "ai/takengine.txt" && p.limit.at("arabow") == 0 && p.weight.at("arabow") == 2 &&
                         p.weight.at("modunit") == 7;
              }),
              "ai/takengine.txt adjusts every one of our levels (limit arabow 0: no level builds it)");
        check(everyLevel([&](ai::Difficulty d) { return same(ai::levelProfile(d, &loose), over(d, changes)); }),
              "each level applies it over its OWN built-in table; units it does not name keep that level's values");
        check(same(ai::levelProfile(ai::Difficulty::Classic, &loose), ai::builtinProfile(ai::Difficulty::Classic)),
              "Classic (RETAIL) never reads ai/takengine.txt");

        const auto archived = hpi::modFiles(root, hpi::ModKind::Game, {"archived"});
        check(everyLevel([&](ai::Difficulty d) { return same(ai::levelProfile(d, &archived), over(d, {{"arafly", -1, 0}})); }),
              "an ai/takengine.txt inside an archive in a game pack adjusts every level");

        const auto old = hpi::modFiles(root, hpi::ModKind::Game, {"old"});
        check(old.count("ai/n.txt") && old.count("ai/h.txt") && old.count("ai/ad.txt") && builtinAll(&old),
              "other files in ai/ (ai/n.txt, ai/h.txt, ai/ad.txt) are not read");
        const auto rooted = hpi::modFiles(root, hpi::ModKind::Game, {"rooted"});
        check(rooted.count("takengine.txt") && builtinAll(&rooted), "a root takengine.txt (loose or an archive's) is not read");
        const auto nested = hpi::modFiles(root, hpi::ModKind::Game, {"nested"});
        check(builtinAll(&nested), "takengine.txt in another folder (sub/, sub/ai/) is not read");

        const auto cosmetic = hpi::modFiles(root, hpi::ModKind::Cosmetic, {"skin"});
        const auto always = hpi::modFiles(root, hpi::ModKind::Cosmetic, {"always"});
        check(cosmetic.count("sounds/x.wav") && !cosmetic.count("ai/takengine.txt") && always.count("sounds/y.wav") &&
              !always.count("ai/takengine.txt"), "cosmetic packs, always-enabled ones too, cannot carry ai/takengine.txt");
        {
            const auto played = hpi::mountPlayVfs(root, {}, {});
            const auto file = played.read("ai/takengine.txt");
            check(played.has("sounds/y.wav") && std::string(file.begin(), file.end()) == "limit arabow 0\n",
                  "an always-enabled cosmetic pack loads in play, its ai/ filtered out (the base install's file stays)");
            const auto house = hpi::modFiles(root, hpi::ModKind::Game, {"house_ai"});
            check(everyLevel([&](ai::Difficulty d) { return same(ai::levelProfile(d, &house), over(d, {{"arafly", -1, 0}})); }),
                  "an always-enabled game pack's ai/takengine.txt adjusts every level");
        }

        // The real transfer codec: the host's package, decoded as the server receives it.
        const auto package = net::mods::build(root, {"my_ai", "archived"});
        const auto received = net::mods::decode(package->bytes, package->digest);
        // Packs layer alphabetically (later wins): my_ai's file replaces archived's.
        check(received->files->count("ai/takengine.txt") &&
                  everyLevel([&](ai::Difficulty d) { return same(ai::levelProfile(d, received->files.get()), over(d, changes)); }),
              "the game-mod package carries ai/takengine.txt to the server (later pack wins)");
        // The base install's ai/ folder: in the played data, never in the package.
        const auto play = hpi::mountPlayVfs(root, {}, {"skin"});
        check(play.has("ai/takengine.txt") && play.has("sounds/x.wav"),
              "the played data holds the base install's ai/takengine.txt");
        const auto none = net::mods::build(root, {"rooted"});
        const auto noneReceived = net::mods::decode(none->bytes, none->digest);
        check(!received->files->count("gamedata/x.tdf") && !noneReceived->files->count("ai/takengine.txt") &&
              builtinAll(noneReceived->files.get()),
              "the base install's ai/takengine.txt never reaches the package, so no level reads it");
        // No game mods and no map package: only the built-in tables.
        check(builtinAll(nullptr), "no game-mod or map files: the built-in tables");
    } catch (const std::exception& e) {
        check(false, std::string("mod layers threw: ") + e.what());
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}

// MinGW's create_symlink always fails with ENOSYS: fall back to a hard link, then a copy.
static void linkOrCopy(const std::filesystem::path& target, const std::filesystem::path& link) {
    std::error_code ec;
    std::filesystem::create_symlink(target, link, ec);
    if (ec) std::filesystem::create_hard_link(target, link, ec);
    if (ec) std::filesystem::copy_file(target, link, std::filesystem::copy_options::overwrite_existing, ec);
}

static bool isHpi(const std::filesystem::path& p) { return hpi::MountSet::key(p.extension().string()) == ".hpi"; }

static void retailDefault(const char* data) {
    const auto vfs = hpi::mountRetailRoot(data);
    check(vfs.has("ai/default.txt"), "the install has ai/default.txt");
    const auto file = ai::loadProfile(vfs);
    const auto& builtin = ai::builtinProfile(ai::Difficulty::Normal);
    check(same(file, builtin), "the built-in Normal table equals the base install's ai/default.txt (first difference: " +
                               firstDifference(file, builtin) + ")");
}

// A single-map archive under Maps/ that carries its own ai/default.txt: the map's
// package carries the file (and only that map's package does), the server's view of
// the room (base + package) gives it to the Classic AI, and a package naming an AI
// file the map's .ota does not choose is refused.
static void mapPackages(const char* data) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("tak-ai-map-" + crypto::toHex(crypto::randomVec(8)));
    try {
        fs::create_directories(root);
        for (const auto& e : fs::directory_iterator(data))
            if (e.is_regular_file() && isHpi(e.path())) linkOrCopy(fs::absolute(e.path()), root / e.path().filename());
        const auto base = hpi::mountRetailRoot(data);
        const std::string own = "// the map's own profile\nlimit tarcage 0\n";
        // The map archive also carries ai/takengine.txt, and other files in ai/.
        std::vector<std::pair<std::string, std::string>> entries{{"ai/default.txt", own},
                                                                 {"ai/takengine.txt", "limit tarcage 0\n"}};
        for (const char* f : {"ai/d.txt", "ai/e.txt", "ai/n.txt", "ai/h.txt", "ai/a.txt", "ai/ad.txt"})
            entries.push_back({f, "limit tarcage 1\n"});
        for (const char* ext : {".tnt", ".ota", ".crt", ".txt"})
            if (auto b = base.tryRead(std::string("maps/Inner Circle") + ext))
                entries.push_back({std::string("kmap/AI Circle") + ext, std::string(b->begin(), b->end())});
        archive(root / "Maps/AI Circle.kmp", entries);
        const auto vfs = hpi::mountRetailRoot(root);
        const auto pkg = net::maps::build(vfs, "AI Circle");
        check(pkg->files->count("ai/default.txt") &&
              std::string(pkg->files->at("ai/default.txt").begin(), pkg->files->at("ai/default.txt").end()) == own,
              "a map archive's own ai/default.txt travels in its package");
        check(pkg->files->count(ai::kEngineProfileFile) == 1, "so does its ai/takengine.txt (the name the package accepts)");
        bool noOld = true;
        for (const char* f : {"ai/d.txt", "ai/e.txt", "ai/n.txt", "ai/h.txt", "ai/a.txt", "ai/ad.txt"})
            noOld = noOld && !pkg->files->count(f);
        check(noOld, "other files in ai/ (ai/d.txt ... ai/ad.txt) stay out of the package");
        {
            const auto received = net::maps::decode(pkg->bytes, pkg->digest);
            const auto* mapFiles = received->files.get();
            std::string source;
            bool fromMap = false;
            check(everyLevel([&](ai::Difficulty d) {
                      const auto p = ai::levelProfile(d, nullptr, mapFiles, &source, &fromMap);
                      return same(p, over(d, {{"tarcage", -1, 0}})) && source == "ai/takengine.txt" && fromMap;
                  }),
                  "the map package's ai/takengine.txt adjusts every level (none builds tarcage)");
            const hpi::Vfs::Files mods{{"ai/takengine.txt", {'l', 'i', 'm', 'i', 't', ' ', 't', 'a', 'r', 'c', 'a', 'g', 'e', ' ', '5'}},
                                       {"ai/default.txt", {'/', '/', 'm', 'o', 'd'}}};
            check(everyLevel([&](ai::Difficulty d) {
                      const auto p = ai::levelProfile(d, &mods, mapFiles, &source, &fromMap);
                      return same(p, over(d, {{"tarcage", -1, 5}})) && !fromMap;
                  }),
                  "a game mod's ai/takengine.txt wins over the map's, for every level");
            hpi::Vfs roomView(&base);
            roomView.setMapFiles(received->files);
            ai::ClassicProfileLayer layer;
            auto script = ai::classicProfileScript(roomView, "", &mods, mapFiles, &layer);
            check(script.text == "//mod" && layer == ai::ClassicProfileLayer::GameMods,
                  "DEFAULT: a game mod's ai/default.txt wins over the map's");
            script = ai::classicProfileScript(roomView, "", nullptr, mapFiles, &layer);
            check(script.text == own && layer == ai::ClassicProfileLayer::Map, "DEFAULT: the map's wins over the base's");
            const hpi::Vfs plainView(&vfs, true);
            script = ai::classicProfileScript(plainView, "", nullptr, nullptr, &layer);
            check(!script.text.empty() && script.text != own && layer == ai::ClassicProfileLayer::Base,
                  "DEFAULT: else the base install's");
        }
        const auto stock = net::maps::build(vfs, "Inner Circle");
        bool noAi = true;
        for (const auto& [name, bytes] : *stock->files) noAi = noAi && !name.starts_with("ai/");
        check(noAi, "another map's package carries no AI file (nor the base's)");
        const auto received = net::maps::decode(pkg->bytes, pkg->digest);
        hpi::Vfs room(&base);
        room.setMapFiles(received->files);
        const auto script = ai::classicProfileScript(room, ai::classicMapAiProfile(room, received->mapPath));
        check(hpi::MountSet::key(script.path) == "ai/default.txt" && script.text == own, "the room's view gives Classic the map's file");
        const hpi::Vfs noMaps(&vfs, true);
        const auto plain = ai::classicProfileScript(noMaps, "");
        check(!plain.text.empty() && plain.text != own, "a view without the map package skips Maps/ archives' AI files");
        // The same package with an AI file the .ota does not choose (ai/n.txt too).
        for (const char* extra : {"ai/other.txt", "ai/n.txt"}) {
            auto files = *received->files;
            files[extra] = {'x'};
            net::Writer w; w.u32(2); w.str(received->mapPath); w.u32(uint32_t(files.size()));
            for (const auto& [name, bytes] : files) { w.str(name); w.u32(uint32_t(bytes.size())); w.b.insert(w.b.end(), bytes.begin(), bytes.end()); }
            bool refused = false;
            try { net::maps::decode(w.b, ""); } catch (const std::exception&) { refused = true; }
            check(refused, std::string("a package carrying an AI file its map does not name (") + extra + ") is refused");
        }
    } catch (const std::exception& e) {
        check(false, std::string("map packages threw: ") + e.what());
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}

// Two installed single-map archives that carry the same AI file names (ai/takengine.txt and
// the same ai/<aiprofile>.txt), stamped with date 0 as the Cartographer writes them:
// the Maps/ mount gives only one archive each path, yet each map's package carries
// the file from ITS OWN archive. A ticked game mod's copy does not change what a map's
// package carries (the server's lookup lets the mod's file win); a stock map's carries
// none.
static void mapClash(const char* data) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("tak-ai-clash-" + crypto::toHex(crypto::randomVec(8)));
    try {
        fs::create_directories(root);
        for (const auto& e : fs::directory_iterator(data))
            if (e.is_regular_file() && isHpi(e.path())) linkOrCopy(fs::absolute(e.path()), root / e.path().filename());
        const auto base = hpi::mountRetailRoot(data);
        const auto ota = base.read("maps/Inner Circle.ota");
        auto scenario = tnt::Scenario::parse(std::string(ota.begin(), ota.end()));
        scenario.aiProfile = "islands";
        const auto otaText = scenario.write();
        auto text = [](const std::vector<uint8_t>& b) { return std::string(b.begin(), b.end()); };
        for (const char* name : {"Clash A", "Clash B"}) {
            std::vector<std::pair<std::string, std::string>> entries{
                {"ai/takengine.txt", std::string("// ") + name + "'s levels\nlimit tarcage 0\n"},
                {"ai/islands.txt", std::string("// ") + name + "'s DEFAULT\nlimit tarcage 0\n"},
                {std::string("kmap/") + name + ".ota", otaText}};
            for (const char* ext : {".tnt", ".crt", ".txt"})
                if (auto b = base.tryRead(std::string("maps/Inner Circle") + ext))
                    entries.push_back({std::string("kmap/") + name + ext, text(*b)});
            archive(root / "Maps" / (std::string(name) + ".kmp"), entries);
        }
        put(root / "mods/game/aimod/ai/takengine.txt", "// the mod's levels\nlimit tarcage 5\n");
        for (const bool modded : {false, true}) {
            const auto vfs = modded ? hpi::mountRetailRoot(root, {"aimod"}) : hpi::mountRetailRoot(root);
            const std::string how = modded ? " (with a game mod's ai/takengine.txt ticked)" : "";
            for (const char* name : {"Clash A", "Clash B"}) {
                const auto pkg = net::maps::build(vfs, name);
                const auto& files = *pkg->files;
                const auto own = [&](const char* path, const char* what) {
                    return files.count(path) && text(files.at(path)) == std::string("// ") + name + "'s " + what + "\nlimit tarcage 0\n";
                };
                check(own("ai/takengine.txt", "levels"), std::string(name) + "'s package carries its own ai/takengine.txt" + how);
                check(own("ai/islands.txt", "DEFAULT"), std::string(name) + "'s package carries its own ai/islands.txt" + how);
                if (modded) {
                    const auto received = net::maps::decode(pkg->bytes, pkg->digest);
                    const hpi::Vfs::Files mods{{"ai/takengine.txt", {'l', 'i', 'm', 'i', 't', ' ', 't', 'a', 'r', 'c', 'a', 'g', 'e', ' ', '5'}}};
                    check(everyLevel([&](ai::Difficulty d) {
                              return ai::levelProfile(d, &mods, received->files.get()).limit.at("tarcage") == 5;
                          }),
                          std::string(name) + ": the game mod's ai/takengine.txt still wins in the room");
                }
            }
            const auto stock = net::maps::build(vfs, "Inner Circle");
            bool noAi = true;
            for (const auto& [path, bytes] : *stock->files) noAi = noAi && !path.starts_with("ai/");
            check(noAi, "a stock map's package carries no AI file" + how);
        }
    } catch (const std::exception& e) {
        check(false, std::string("map clash threw: ") + e.what());
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}

int main(int argc, char** argv) {
    tables();
    modLayers();
    if (argc > 1) { retailDefault(argv[1]); mapPackages(argv[1]); mapClash(argv[1]); }
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
