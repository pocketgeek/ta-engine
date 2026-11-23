// The GAME DATA FOLDER setting shared by takclient's Options and Cartographer's
// Preferences (client/datafolder.h): launch precedence, what a pick means, path
// elision, restart arguments for both programs, the Windows command line a restart
// builds, and the settings file -- a dataDir round trip, a folder-only write that
// leaves unsaved edits out, and one program's save not undoing the other's change.

#include "cartographer/restart.h"
#include "client/datafolder.h"
#include "client/settings.h"
#include "util/relaunch.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "util/u8path.h"

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); ++failures; }
}

using Args = std::vector<std::string>;
std::string join(const Args& a) {
    std::string s;
    for (auto& x : a) s += "[" + x + "]";
    return s;
}

// CommandLineToArgvW's rules (the MSVC CRT's are the same for argv[1..]): the
// reference windowsCommandLine has to round-trip through.
Args splitWindows(const std::string& line) {
    Args out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size()) break;
        std::string arg;
        bool quoted = false;
        while (i < line.size() && (quoted || (line[i] != ' ' && line[i] != '\t'))) {
            size_t slashes = 0;
            while (i < line.size() && line[i] == '\\') { ++slashes; ++i; }
            if (i < line.size() && line[i] == '"') {
                arg.append(slashes / 2, '\\');
                if (slashes % 2) { arg += '"'; ++i; continue; }
                if (quoted && i + 1 < line.size() && line[i + 1] == '"') { arg += '"'; i += 2; continue; }
                quoted = !quoted; ++i; continue;
            }
            arg.append(slashes, '\\');
            if (i < line.size() && (quoted || (line[i] != ' ' && line[i] != '\t'))) arg += line[i++];
        }
        out.push_back(arg);
    }
    return out;
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

}  // namespace

int main() {
    using namespace tak;
    namespace fs = std::filesystem;

    // ---- launch precedence: --data > launch folder > saved > picker -----------------
    {
        const std::set<std::string> installs = {"/games/tak", "/elsewhere/tak"};
        const DataFolderValidator valid = [&](const std::string& f, std::string*) { return installs.count(f) > 0; };
        auto pick = [&](const std::string& cli, const std::string& here, const std::string& saved) {
            return chooseDataFolder(cli, here, saved, valid);
        };
        auto c = pick("/not/an/install", "/games/tak", "/elsewhere/tak");
        check(c.origin == DataFolderOrigin::CommandLine && c.folder == "/not/an/install",
              "--data wins over everything, even when it does not validate");
        c = pick("", "/games/tak", "/elsewhere/tak");
        check(c.origin == DataFolderOrigin::LaunchFolder && c.folder == "/games/tak", "a valid launch folder beats the saved one");
        c = pick("", "/home/me", "/elsewhere/tak");
        check(c.origin == DataFolderOrigin::Saved && c.folder == "/elsewhere/tak", "an invalid launch folder falls through to the saved one");
        c = pick("", "/home/me", "/gone/tak");
        check(c.origin == DataFolderOrigin::Picker && c.folder.empty(), "a saved folder that no longer validates asks");
        c = pick("", "", "");
        check(c.origin == DataFolderOrigin::Picker, "nothing at all asks");
        check(!dataFolderOverrideReason(DataFolderOrigin::CommandLine).empty() &&
              dataFolderOverrideReason(DataFolderOrigin::CommandLine).find("--data") != std::string::npos,
              "a --data launch greys the setting and names --data");
        check(!dataFolderOverrideReason(DataFolderOrigin::LaunchFolder).empty(), "a launch-folder launch greys the setting");
        check(dataFolderOverrideReason(DataFolderOrigin::Saved).empty() && dataFolderOverrideReason(DataFolderOrigin::Picker).empty(),
              "a saved or picked folder can be changed");
    }

    // ---- what a pick means --------------------------------------------------------------
    {
        const DataFolderValidator valid = [](const std::string& f, std::string* why) {
            if (f.find("good") != std::string::npos) return true;
            if (why && f.find("quiet") == std::string::npos) *why = "missing data.hpi";
            return false;
        };
        const auto manifest = [](const std::string& f) { return "m:" + f; };
        auto r = evaluateDataFolderPick("", "/good/a", valid, manifest);
        check(r.outcome == DataFolderPick::Cancelled, "an empty pick is a cancel");
        r = evaluateDataFolderPick("/good/a/", "/good/a", valid, manifest);
        check(r.outcome == DataFolderPick::Unchanged, "re-picking the saved folder (another spelling) is no change");
        r = evaluateDataFolderPick("/bad/b", "/good/a", valid, manifest);
        check(r.outcome == DataFolderPick::Invalid && r.reason == "missing data.hpi" && r.manifest.empty(),
              "an invalid pick is refused with the validator's reason");
        r = evaluateDataFolderPick("/bad/quiet", "/good/a", valid, manifest);
        check(r.outcome == DataFolderPick::Invalid && !r.reason.empty(), "an invalid pick always has a reason");
        r = evaluateDataFolderPick("/good/b", "/good/a", valid, manifest);
        check(r.outcome == DataFolderPick::Accepted && r.folder == "/good/b" && r.manifest == "m:/good/b",
              "a valid new folder is accepted with its manifest");
        r = evaluateDataFolderPick("/good/b", "", valid, manifest);
        check(r.outcome == DataFolderPick::Accepted, "with nothing saved, any valid pick is a change");
        check(dataFolderInvalidMessage("missing maps.hpi").find("missing maps.hpi") != std::string::npos,
              "the invalid-folder message carries the reason");

        check(sameFolder("/a/b/", "/a/b") && sameFolder("/a/./b", "/a/b") && sameFolder("/a/c/../b", "/a/b"),
              "spellings of one folder compare equal");
        check(!sameFolder("/a/b", "/a/c") && !sameFolder("/a/b", ""), "different folders do not");
        check(!dataFolderNeedsRestart("", "/a") && !dataFolderNeedsRestart("/a/", "/a") && dataFolderNeedsRestart("/b", "/a"),
              "a restart is needed only when the saved folder is not the one in use");
    }

    // ---- path elision (at the start) ----------------------------------------------------
    {
        check(elidePathStart("/short", 20) == "/short", "a path that fits is untouched");
        const std::string p = "/home/someone/Games/Total Annihilation Kingdoms";
        const std::string e = elidePathStart(p, 30);
        check(e.size() <= 30 && e.rfind("...", 0) == 0, "a long path is cut to fit, with a ... prefix (" + e + ")");
        check(e == "...Total Annihilation Kingdoms", "a cut right after a separator keeps that whole name (" + e + ")");
        const std::string q = elidePathStart("/home/someone/Games/TAK Kingdoms Install", 25);
        check(q == ".../TAK Kingdoms Install", "a cut inside a name moves to the next separator (" + q + ")");
        check(p.substr(p.size() - (e.size() - 3)) == e.substr(3), "the tail kept is the path's real tail");
        const std::string w = "C:\\Program Files (x86)\\Cavedog\\Kingdoms";
        check(elidePathStart(w, 20) == "...\\Cavedog\\Kingdoms", "Windows separators count too (" + elidePathStart(w, 20) + ")");
        const std::string longName = "/x/" + std::string(40, 'a');
        const std::string ln = elidePathStart(longName, 20);
        check(ln.size() == 20 && ln.rfind("...", 0) == 0, "a single long name is cut mid-name when no separator is near");
        const std::string u = "/home/j\xc3\xb6rg/\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9/tak";   // jörg / éééé
        for (size_t n = 4; n < u.size(); ++n) {
            const std::string cut = elidePathStart(u, n);
            check(cut.size() <= n, "elision never exceeds the room (" + std::to_string(n) + ")");
            check(cut.size() <= 3 || (static_cast<unsigned char>(cut[3]) & 0xC0) != 0x80,
                  "elision never starts inside a UTF-8 sequence (" + std::to_string(n) + ")");
        }
        check(elidePathStart(p, 2) == "..", "no room: just dots");
    }

    // ---- restart arguments -------------------------------------------------------------
    {
        check(clientRestartArguments({"--data", "/x", "--play-map", "/t/m.kmp"}) == Args{"--data", "/x"},
              "takclient drops the one-shot --play-map handoff");
        check(clientRestartArguments({"--data", "/x", "--play-map", "/t/m.kmp", "--game-mod", "My AI"}) == Args{"--data", "/x"},
              "takclient drops the --game-mod that came with --play-map");
        check(clientRestartArguments({"game", "Ulasem Arena", "--side", "zon"}) == Args{"game", "Ulasem Arena", "--side", "zon"},
              "takclient keeps everything else, in order");
        check(clientRestartArguments({}).empty(), "a bare launch restarts bare");
        check(withoutOptions({"a", "--x", "1", "2", "b"}, {{"--x", 2}}) == Args{"a", "b"}, "an option drops its values");

        const Args got = cart::restartArguments({"Old Map", "--out", "/maps", "--new", "8x8", "--world", "zhon", "--random"},
                                                "/maps/New.kmp");
        check(got == Args{"--out", "/maps", "/maps/New.kmp"}, "Cartographer keeps --out, drops --new/--world/--random, reopens the document: " + join(got));
        check(cart::restartArguments({"Ulasem Arena"}, "") .empty(), "an unsaved new map restarts on no map");
        check(cart::restartArguments({"--data", "/x", "--shot", "a.png", "--shot-tool", "units", "M"}, "M") == Args{"--data", "/x", "M"},
              "Cartographer drops --shot and --shot-tool");
        check(cart::restartArguments({"--stamp", "s", "1", "2", "--save", "o.tnt", "--bundle", "o.kmp"}, "") .empty(),
              "Cartographer drops the headless one-shots and their values");
        check(cart::restartArguments({"--out"}, "") == Args{"--out"}, "an option missing its value is kept as parseArguments sees it");
    }

    // ---- the Windows command line a restart builds ---------------------------------------
    {
        const Args samples[] = {
            {"C:\\Program Files\\TAK\\takclient.exe", "--data", "D:\\Games\\TAK #2"},
            {"x.exe", "", "a b", "tab\there", "quote\"inside", "trail\\", "trail space\\", "\\\\server\\share\\"},
            {"x.exe", "back\\\\\"quote", "\"", "\\\"", "plain"},
            {"x.exe", "C:\\Users\\J\xc3\xb6rg\\TAK"},
        };
        for (const auto& a : samples) {
            const std::string line = windowsCommandLine(a);
            check(splitWindows(line) == a, "windowsCommandLine round-trips " + join(a) + " via " + line);
        }
        check(windowsCommandLine({"a.exe", "plain"}) == "a.exe plain", "plain arguments are not quoted");
        check(windowsCommandLine({"a.exe", ""}) == "a.exe \"\"", "an empty argument survives as \"\"");
    }

    // ---- this executable ----------------------------------------------------------------
    {
        const std::string self = selfExecutable();
        std::error_code ec;
        check(!self.empty() && fs::is_regular_file(tak::u8path(self), ec) &&
              tak::u8path(self).filename().u8string().find(u8"datafolder_test") != std::u8string::npos,
              "selfExecutable names this program (" + self + ")");
    }

    // ---- the settings file ----------------------------------------------------------------
    {
        const fs::path dir = fs::temp_directory_path() / ("tak-datafolder-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir);
        const fs::path file = dir / "settings.ini";
        setSettingsPath(file.string());
        check(settingsPath() == file.string(), "setSettingsPath redirects the settings file");

        // A folder name with '#', spaces and a trailing space-free tail round-trips.
        Settings s;
        s.dataDir = "D:\\Games\\TAK #2 (copy)";
        s.dataManifest = "0123456789abcdef";
        s.playerName = "alice";
        check(saveSettings(s), "save");
        Settings back = loadSettings();
        check(back.dataDir == s.dataDir && back.dataManifest == s.dataManifest, "dataDir with '#' round-trips (" + back.dataDir + ")");
        check(back.playerName == "alice", "the rest round-trips too");
        {
            std::ofstream f(file, std::ios::app);
            f << "playerName = bob # a hand-written comment\n";
        }
        check(loadSettings().playerName == "bob", "'#' still starts a comment elsewhere");

        // A folder-only write: the screen's unsaved edits stay out of the file.
        Settings mem = loadSettings();
        mem.playerName = "unsaved edit";
        check(saveDataFolderSetting("/new/install", "feed"), "saveDataFolderSetting");
        Settings disk = loadSettings();
        check(disk.dataDir == "/new/install" && disk.dataManifest == "feed", "the folder and its manifest are written");
        check(disk.playerName == "bob", "nothing else in the file changes");

        // The game is open (it read the file); Cartographer then changes the folder. The
        // game's next save of anything must not put its stale folder back.
        Settings game = loadSettings();
        game.playerName = "carol";
        {   // Cartographer, another process: the same read-modify-write, simulated on disk
            std::string t = slurp(file);
            const auto at = t.find("dataDir = /new/install");
            t.replace(at, std::string("dataDir = /new/install").size(), "dataDir = /from/cartographer");
            std::ofstream(file, std::ios::trunc) << t;
        }
        check(saveSettings(game), "the game saves");
        disk = loadSettings();
        check(disk.dataDir == "/from/cartographer", "a save that did not touch the folder keeps the other program's (" + disk.dataDir + ")");
        check(disk.playerName == "carol", "and still saves its own change");
        // ...but a folder the game itself changed is the game's to write.
        game = loadSettings();
        game.dataDir = "/game/choice";
        check(saveSettings(game) && loadSettings().dataDir == "/game/choice", "a folder this process changed is written");
        // Cartographer reads what the game wrote, and DEFAULTS never clears it.
        check(loadSettings().dataDir == "/game/choice", "the other program reads the shared setting");
        check(preferenceDefaults(loadSettings()).dataDir == "/game/choice", "DEFAULTS keeps the data folder");

        setSettingsPath("");
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::puts("PASS: data folder precedence, picks, elision, restart arguments, Windows command line, shared settings file");
    return 0;
}
