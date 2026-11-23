#pragma once

#include "cartographer/units.h"
#include "hpi/hpi.h"
#include "tnt/ota.h"
#include "tnt/tnt.h"

#include <filesystem>
#include <atomic>
#include <map>
#include <set>

namespace cart {

// The map's AI profiles: every root ai/<name>.txt the document carries, keyed by
// MountSet::key, as raw bytes. Kept losslessly -- the bytes are the truth, files
// outside the map-package whitelist included (they stay in the .kmp but never travel).
using AiFiles = std::map<std::string, std::vector<uint8_t>>;
// A root ai/<name>.txt path: no further '/', a name usable as a file name.
bool aiFileKey(const std::string& path);
AiFiles aiFilesFromArchive(const tak::hpi::Archive&);
// <mapFolder>/ai/*.txt beside a loose .tnt (the folder name in any case).
AiFiles aiFilesFromFolder(const std::filesystem::path& mapFolder);
// An installed map opened by name: the root ai/<name>.txt files of its own archive
// (Vfs::readFromMapArchive), not another installed map's that wins the same path.
// None for a loose map or one in the base install.
AiFiles aiFilesFromMapArchive(const tak::hpi::Vfs&, const std::string& mapPath);
// Moves the root ai/ entries out of a recovery snapshot's files.
AiFiles liftAiFiles(tak::hpi::Vfs::Files&);

struct DocumentFiles {
    std::vector<tak::hpi::PackFile> members;      // kmap/<stem>/ in a .kmp; beside the .tnt loose
    std::vector<tak::hpi::PackFile> rootMembers;  // ai/<x>.txt: the .kmp root; <dir>/ai/ loose
};
// One serialization path for normal saves, recovery, playtests and loose export.
// The input document is never changed by saving it. Empty AI files are dropped.
DocumentFiles documentFiles(
    const tak::tnt::Map&, tak::tnt::Scenario metadata,
    const tak::crt::Scenario&, const std::vector<PlacedUnit>&,
    const std::set<std::string>& useOnly, const AiFiles&, const std::string& name);

// Atomic single-file replacement, with the previous file retained as .bak.
// Loose exports stage every member before replacement and roll back failures;
// use a KMP for an atomic whole-document save.
bool writeDocumentFiles(const std::filesystem::path& directory,
                        const std::vector<tak::hpi::PackFile>&,
                        std::string& error, const std::atomic_bool* cancel = nullptr);
bool writeDocumentBundle(const std::filesystem::path&, const DocumentFiles&,
                         std::string& error, const std::atomic_bool* cancel = nullptr);
// Loose export: members into `directory`, AI files into its ai/ folder. Refused when
// AI files would land in (or under) the install's Maps folder, where a loose
// Maps/ai/<x>.txt is read for every map and never counts as this map's own.
bool writeDocumentLoose(const std::filesystem::path& directory, const DocumentFiles&,
                        const std::filesystem::path& installMaps,
                        std::string& error, const std::atomic_bool* cancel = nullptr);
// ---- AI profiles as a game mod (design 7.2): <dataRoot>/mods/game/<pack>/ai/<x>.txt ----
// The same place on every OS: mods live in the data folder, never in prefPath.
// Why `name` cannot name a pack ("" = it can): 1-128 bytes, a valid file name, no
// leading '.' (hpi::modPacks lists nothing else).
std::string modPackNameError(const std::string& name);
// Why `name` cannot be an editable game pack in `dataRoot` ("" = it can): it is
// always_enabled (the folder of the packs the game always applies), or an
// always-enabled pack (mods/game/always_enabled/<name>, any case) already uses it,
// which would hide a mods/game/<name> folder. Cartographer edits ordinary packs
// only; to edit an always-enabled one, move it out of always_enabled/ first.
std::string modPackReservedError(const std::filesystem::path& dataRoot, const std::string& name);
// TEST IN GAME's note when `dataRoot` has always-enabled game packs (they apply in
// every room the player hosts, the test room too): " Always-enabled game packs
// (mods/game/always_enabled/: A, B) also apply in the test game." or "".
std::string alwaysEnabledGameNote(const std::filesystem::path& dataRoot);
struct ModProfileChange {
    enum class Action { Create, Same, Replace, Delete };
    std::string key;                    // ai/takengine.txt
    Action action = Action::Create;
    std::filesystem::path path;         // the file on disk (an existing file's own spelling)
    std::vector<uint8_t> data;          // the new bytes (Create/Replace/Same)
    std::vector<uint8_t> existing;      // the bytes on disk (Same/Replace/Delete)
    std::string archive;                // a pack archive that also holds the path ("" = none); the loose file wins
};
struct ModProfilePlan {
    std::string pack;                   // as spelled on disk for an existing pack (matched ignoring case)
    std::filesystem::path packFolder, folder;   // <...>/<pack> and its ai/ folder (either may not exist yet)
    std::vector<std::filesystem::path> create;  // folders a write makes, outermost first
    std::vector<ModProfileChange> changes;      // sorted by key
    bool writes() const;                // anything other than Same
};
// What writing `files` (empty bytes are left out) and deleting `remove` would do to
// one pack, without touching the disk. Existing mods/, game/, <pack>/ and ai/ folders
// and files are reused whatever their case; a symlinked or non-folder component is
// refused (hpi::modPacks would never list the pack), as is a symlinked file.
bool planModProfiles(const std::filesystem::path& dataRoot, const std::string& pack, const AiFiles& files,
                     const std::set<std::string>& remove, ModProfilePlan&, std::string& error);
// Carries out a plan: re-checks it against the disk (a file changed since is an
// error), copies every file it replaces or deletes to
// <backupRoot>/mod-backups/<pack>/<UTC stamp>/ai/ first (never into the pack: every pack file
// travels in the mod transfer), creates the missing folders, then installs each file
// atomically (staged beside the packs in mods/game/, which is never mounted) and
// rolls back on failure. Same files and every other pack file are left alone.
bool writeModProfiles(const std::filesystem::path& dataRoot, const ModProfilePlan&, const std::filesystem::path& backupRoot,
                      std::string& error, std::filesystem::path* backupFolder = nullptr);
// A pack's root ai/<x>.txt files as the game reads them (loose over its archives),
// for Open from game mod. Throws when the pack is not listed by hpi::modPacks (an
// always-enabled pack is not: hpi::modFiles alone would read it).
AiFiles readModProfiles(const std::filesystem::path& dataRoot, const std::string& pack);

// The data root's Maps folder (any case), or empty.
std::filesystem::path installMapsFolder(const std::filesystem::path& dataRoot);
bool insideFolder(const std::filesystem::path& path, const std::filesystem::path& folder);
bool validDocumentName(const std::string&);

} // namespace cart
