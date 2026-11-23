#pragma once

// The GAME DATA FOLDER setting, shared by takclient (Options) and Cartographer
// (View > Preferences): which retail install a launch uses, what changing it does,
// and the strings both programs show for it. Pure logic here (unit-tested by
// tools/datafolder_test.cpp); the picker (client/dirpicker.h), the persistence
// (tak::saveDataFolderSetting in client/settings.h) and the relaunch
// (util/relaunch.h) are the only side effects, and live in their own files.
//
// ONE setting for both programs: Settings::dataDir in the game's settings file.
// Cartographer has no copy of its own -- it reads and writes that key -- so a folder
// chosen in either program is the one both start with next time.
//
// A new folder is applied by RESTARTING, never by remounting in place. The game
// mounts the install once at startup and a great deal hangs off that mount: the
// retail gameplay-data fingerprint sent in every multiplayer Hello (computed once per
// process), the menu's GUI art, music and door videos, the campaign list, the map
// catalogue and its verified-map cache, the hotkey and mod-pack state. Cartographer
// likewise keeps its unit registry, unit-type list, section catalogue and the open
// document's VFS. Swapping all of that live is a long list of places to get exactly
// right; a fresh process gets every one of them right by construction.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "util/u8path.h"

namespace tak {

// Where the folder a launch is using came from, in precedence order.
enum class DataFolderOrigin {
    CommandLine,    // --data <dir>: always wins, even when it does not validate
    LaunchFolder,   // started from inside an install (the working directory is one)
    Saved,          // Settings::dataDir, re-validated at every launch
    Picker,         // none of the above: ask (first run, or the saved one went away)
};

// The launch-time choice. `folder` is empty when the caller must ask (Picker).
struct DataFolderChoice {
    std::string folder;
    DataFolderOrigin origin = DataFolderOrigin::Picker;
};

using DataFolderValidator = std::function<bool(const std::string& folder, std::string* why)>;

// --data > the folder the program was started from > the saved setting > the picker.
// An explicit --data is honoured even if it is not a valid install (a developer
// override); the other two must validate, or they are skipped.
inline DataFolderChoice chooseDataFolder(const std::string& commandLine, const std::string& launchFolder,
                                         const std::string& saved, const DataFolderValidator& valid) {
    if (!commandLine.empty()) return {commandLine, DataFolderOrigin::CommandLine};
    if (!launchFolder.empty() && valid(launchFolder, nullptr)) return {launchFolder, DataFolderOrigin::LaunchFolder};
    if (!saved.empty() && valid(saved, nullptr)) return {saved, DataFolderOrigin::Saved};
    return {{}, DataFolderOrigin::Picker};
}

// Why the setting cannot be changed from inside this launch ("" = it can). Both
// overrides would win again after a restart, so the row is greyed rather than
// offering a change that would silently not take.
inline std::string dataFolderOverrideReason(DataFolderOrigin origin) {
    switch (origin) {
        case DataFolderOrigin::CommandLine:
            return "Set by --data on the command line, which overrides this setting.";
        case DataFolderOrigin::LaunchFolder:
            return "Started from inside a game folder, which is always used first.";
        default: return {};
    }
}

// Two spellings of one folder ("C:\TAK\" and "c:/tak", "/x/y/" and "/x/y") compare
// equal, so re-picking the folder in use is not mistaken for a change. Case is only
// folded on Windows, where the file system ignores it.
inline std::string comparableFolder(const std::string& folder) {
    if (folder.empty()) return {};
    const auto text = tak::u8path(folder).lexically_normal().generic_u8string();
    std::string s(text.begin(), text.end());
    while (s.size() > 1 && s.back() == '/') s.pop_back();
#ifdef _WIN32
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
#endif
    return s;
}
inline bool sameFolder(const std::string& a, const std::string& b) { return comparableFolder(a) == comparableFolder(b); }

// What a folder picked in Options / Preferences means. Invalid keeps the current
// setting untouched; Accepted is the only outcome that writes anything.
struct DataFolderPick {
    enum Outcome { Cancelled, Unchanged, Invalid, Accepted } outcome = Cancelled;
    std::string folder;     // the pick (Invalid / Accepted)
    std::string reason;     // Invalid: what is wrong with it ("missing data.hpi")
    std::string manifest;   // Accepted: hpi::rootManifest of the new folder
};

// `saved` is the current setting. Picking the saved folder again is Unchanged (nothing
// to write); anything else must validate before it is Accepted.
inline DataFolderPick evaluateDataFolderPick(const std::string& picked, const std::string& saved,
                                             const DataFolderValidator& valid,
                                             const std::function<std::string(const std::string&)>& manifest) {
    DataFolderPick r;
    if (picked.empty()) return r;
    r.folder = picked;
    if (sameFolder(picked, saved)) { r.outcome = DataFolderPick::Unchanged; return r; }
    std::string why;
    if (!valid(picked, &why)) {
        r.outcome = DataFolderPick::Invalid;
        r.reason = why.empty() ? std::string("not a game folder") : why;
        return r;
    }
    r.outcome = DataFolderPick::Accepted;
    r.manifest = manifest ? manifest(picked) : std::string();
    return r;
}

// The picker's title and the words for a folder that is not an install, shared by the
// first-run prompt, Options and Cartographer's Preferences.
inline constexpr const char* kDataFolderPickerTitle = "Select your Total Annihilation: Kingdoms install folder";
inline std::string dataFolderInvalidMessage(const std::string& why) {
    return "That folder isn't a Total Annihilation: Kingdoms install (" + why +
           "). Choose the folder that contains data.hpi, terrain.hpi and Maps/.";
}

// A restart is needed when the saved setting is not the folder this launch mounted.
inline bool dataFolderNeedsRestart(const std::string& saved, const std::string& inUse) {
    return !saved.empty() && !sameFolder(saved, inUse);
}

// A long path cut at its START, which is the part that matters least
// ("/home/me/Games/TAK" -> ".../Games/TAK"). The cut prefers to land on a separator so
// the shown tail starts with a whole folder name, as long as that does not throw away
// more than a third of the room. Never splits a UTF-8 sequence. maxChars counts bytes,
// which is what the block fonts that draw this count (one glyph per byte).
inline std::string elidePathStart(std::string_view path, size_t maxChars) {
    if (path.size() <= maxChars) return std::string(path);
    static constexpr std::string_view kDots = "...";
    if (maxChars <= kDots.size()) return std::string(kDots.substr(0, maxChars));
    const size_t room = maxChars - kDots.size();
    size_t start = path.size() - room;
    while (start < path.size() && (static_cast<unsigned char>(path[start]) & 0xC0) == 0x80) ++start;
    const size_t sep = path.find_first_of("/\\", start);
    if (sep != std::string_view::npos && sep > start && sep - start <= room / 3) start = sep;
    return std::string(kDots) + std::string(path.substr(start));
}

// The same arguments minus the named options (each with the number of values it
// takes), e.g. the one-shot --play-map handoff, which must not run again on restart.
inline std::vector<std::string> withoutOptions(const std::vector<std::string>& args,
                                               const std::vector<std::pair<std::string, int>>& drop) {
    std::vector<std::string> out;
    for (size_t i = 0; i < args.size(); ++i) {
        auto it = std::find_if(drop.begin(), drop.end(), [&](const auto& d) { return d.first == args[i]; });
        if (it == drop.end()) { out.push_back(args[i]); continue; }
        i += size_t(std::max(0, it->second));
    }
    return out;
}

// takclient's restart arguments: the launch's own arguments (so a debug launch keeps
// its mode and flags, and the working directory and --data rules apply as before),
// minus --play-map, a one-shot Cartographer handoff that imported a snapshot into the
// OLD install's map cache, and the --game-mod that went with it (a pack of that install).
inline std::vector<std::string> clientRestartArguments(const std::vector<std::string>& args) {
    return withoutOptions(args, {{"--play-map", 1}, {"--game-mod", 1}});
}

}  // namespace tak
