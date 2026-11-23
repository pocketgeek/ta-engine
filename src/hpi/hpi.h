#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <memory>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ta::hpi {

// Total Annihilation ships its data in HPI version-1 archives (header version
// 0x00010000; *.hpi, *.ufo, *.ccx, *.gp3). Layout, checked against TotalA.exe
// (key derivation at 0x4bdf3d) and every archive of the GOG install:
//
//   HPIHEADER1 { "HAPI", 0x00010000, dirSize, headerKey, dirStart }
//   key = ~((headerKey << 2) | (headerKey >> 6)) & 0xff, applied only when
//   headerKey != 0; byte at file offset p decodes as (p ^ key) ^ ~b.
//   The directory occupies file bytes [dirStart, dirSize) (absolute offsets):
//     dir   { u32 count, u32 entriesOff }, entry { u32 nameOff, u32 dataOff, u8 isDir }
//     file  { u32 dataOff, u32 size, u8 method (0 stored, 1 LZ77, 2 zlib) }
//   A compressed file is a table of ceil(size/65536) u32 chunk sizes followed
//   by SQSH chunks (method 1 = LZ77, 2 = zlib; same 19-byte header as v2).
//
// TA Engine's own map bundles use the HPI version-2 revision (0x00020000),
// a revision of Total Annihilation's HPI format. Layout, verified against
// the GOG release and Joe D's HPIPack writer source:
//
//   HPIVERSION  { "HAPI", 0x00020000 }
//   HPIHEADER2  { dirBlock, dirSize, nameBlock, nameSize, dataStart, unused }
//   ...file data (per-file sequences of SQSH chunks, or raw)...
//   name block  (SQSH chunk or raw: null-terminated strings)
//   dir block   (SQSH chunk or raw: tree of 20-byte dir / 24-byte file entries)
//
// SQSH chunk: 19-byte packed header { "SQSH", u8 ver, u8 method(2=zlib),
// u8 encrypted, u32 compSize, u32 decompSize, u32 checksum } + payload.
// Encrypted payload bytes are decoded with b[i] = (b[i] - i) ^ i.

struct HeaderInfo {
    std::string magic;   // expected "HAPI"
    uint32_t version = 0;
    uint32_t dirBlock = 0, dirSize = 0;
    uint32_t nameBlock = 0, nameSize = 0;
    uint32_t dataStart = 0;
    uint64_t fileSize = 0;
};

struct Entry {
    std::string path;        // archive-internal path, '/'-separated
    bool isDirectory = false;
    uint32_t start = 0;      // file data offset in archive
    uint32_t decompressedSize = 0;
    uint32_t compressedSize = 0;  // 0 = stored uncompressed
    uint32_t date = 0;            // time_t (v2 only; v1 entries carry no date)
    uint8_t v1Method = 0;         // v1 only: 0 stored, 1 LZ77, 2 zlib (chunked)
};

// One file to place into a packed archive. `path` is the internal,
// '/'-separated location (e.g. "kmap/My Map/My Map.tnt").
struct PackFile {
    std::string path;
    std::vector<uint8_t> data;
};

// Build an HPI-v2 archive (as read by Archive, above) holding `files`, with the
// directory tree derived from their paths. Files and the directory/name blocks
// are stored uncompressed (the format's raw variant). `date` stamps each file
// entry (a time_t; 0 = unset). This is how the map editor writes a .kmp bundle.
std::vector<uint8_t> pack(const std::vector<PackFile>& files, uint32_t date = 0);

class Archive {
public:
    explicit Archive(const std::filesystem::path& file);

    const std::vector<Entry>& entries() const { return entries_; }

    // Extract one file entry's contents.
    std::vector<uint8_t> read(const Entry& entry) const;
    // Find a file entry by internal path (case-insensitive). nullptr if absent.
    const Entry* find(const std::string& path) const;

private:
    // Read-only memory-mapping of one file (POSIX mmap / Win32 file mapping).
    // Move-only RAII; data() is null until open() succeeds.
    class Mapping {
    public:
        Mapping() = default;
        ~Mapping() { close(); }
        Mapping(Mapping&& o) noexcept { *this = std::move(o); }
        Mapping& operator=(Mapping&& o) noexcept {
            if (this != &o) {
                close();
                data_ = o.data_; size_ = o.size_; handle_ = o.handle_;
                o.data_ = nullptr; o.size_ = 0; o.handle_ = nullptr;
            }
            return *this;
        }
        Mapping(const Mapping&) = delete;
        Mapping& operator=(const Mapping&) = delete;
        bool open(const std::filesystem::path& p);   // false on failure (caller falls back)
        void close();
        const uint8_t* data() const { return data_; }
        size_t size() const { return size_; }
    private:
        const uint8_t* data_ = nullptr;
        size_t size_ = 0;
        void* handle_ = nullptr;   // win32 mapping handle; unused on POSIX
    };

    // View of the whole archive, established on the first read(): a demand-paged
    // memory map, so pages are faulted in per accessed chunk, shared with the OS
    // page cache (and with any other process mapping the same file) instead of
    // slurped + pinned per Vfs instance (~300MB of RSS per mount before). Falls
    // back to an eager read into fileData_ if mapping fails. Not touched at
    // mount, so the map-browser's many .kmp archives (dir-parsed only) cost
    // nothing until one is actually read.
    struct View { const uint8_t* data; size_t size; };
    View bytes() const;

    std::filesystem::path file_;
    HeaderInfo header_;
    std::vector<Entry> entries_;
    bool v1_ = false;      // classic TA archive (header version 0x00010000)
    uint8_t v1Key_ = 0;    // derived v1 byte key (0 = unencrypted)
    std::vector<uint8_t> readV1(const Entry& entry) const;
    mutable Mapping map_;
    mutable std::vector<uint8_t> fileData_;   // fallback only (mapping failed)
    mutable bool loaded_ = false;
};

HeaderInfo inspect(const std::filesystem::path& archive);
std::string describe(const HeaderInfo& info);

// A directory's worth of archives, layered exactly as the retail engine does
// (verified by disassembling KINGDOMS.icd's file-open path, 0x53aff0):
//
//   1. A loose file on disk (dir/<path>) overrides everything -- the engine
//      fopen()s the plain path first and only falls back to archives.
//   2. Otherwise all *.hpi then *.ufo in the directory are searched, and when
//      the same internal path exists in several, the one whose directory entry
//      has the NEWEST date (file entry +0x10) wins; a tie keeps the earlier
//      mount (*.hpi before *.ufo, alphabetical). This is why each retail patch
//      shipped a new HPI that simply superseded older copies of a file.
//
// So dropping newer patch archives (or a loose override file) into a game
// directory Just Works, same as the original.
// How a MountSet scans one directory. The defaults reproduce the retail
// behaviour (loose files override; *.hpi then *.ufo). The runtime data-root
// model overrides these: the retail-install ROOT layer sets includeLoose=false
// and archiveExts={".hpi"} (only the shipped archives are read from the root),
// while a Maps/ layer adds ".kmp" (single-map HPIs) and a mods/ pack keeps
// loose+archive with the widest extension set.
struct MountConfig {
    bool includeLoose = true;                                   // loose file overrides archives
    std::vector<std::string> archiveExts{".hpi", ".ufo"};       // scanned as ordered groups
    // Optional path filter: an entry (archive or loose) is exposed only if keep()
    // returns true. Empty = keep everything. Used for cosmetic mod packs, which
    // exclude gameplay-affecting files so they can't diverge in MP.
    std::function<bool(const std::string&)> keep;
    // Optional archive-name whitelist (LOWERCASED filenames incl. extension). When
    // non-empty, ONLY archives whose filename is listed are mounted -- so the retail
    // root loads exactly the known HPIs and never a stray *.hpi dropped alongside them.
    std::vector<std::string> archiveNames;
    // Total Annihilation precedence (TotalA.exe 0x41d4c0 mounts, 0x4bb320 looks
    // up): archives are searched in MOUNT order and the first one holding the
    // path wins -- entry dates play no part. Off = the TA newest-date rule.
    bool firstMountWins = false;
    // Optional archive filter on the LOWERCASED filename (after archiveNames).
    std::function<bool(const std::string&)> archiveFilter;
};

class MountSet {
public:
    explicit MountSet(const std::filesystem::path& dir, MountConfig cfg = {});

    // Is `path` resolvable (loose file or in some archive)?
    bool has(const std::string& path) const;
    // Read a file by internal path (case-insensitive, '/' or '\\'). Throws if
    // absent. A loose file on disk wins; otherwise the newest archive copy.
    std::vector<uint8_t> read(const std::string& path) const;
    // Every archive-provided path after conflict resolution (loose-only files
    // are not listed -- they're already on disk).
    std::vector<std::string> paths() const;
    // Winning internal paths (original case) under `prefix` (a '/'-separated
    // directory prefix; "" = all). Unions archive entries with loose files
    // (when includeLoose), deduped by lowercased key. This is the directory
    // enumerator the runtime loaders use in place of directory_iterator.
    std::vector<std::string> list(const std::string& prefix) const;
    // Human-readable winning source of `path` (for tooling/debug).
    std::string sourceOf(const std::string& path) const;
    // The archive (index into archiveFiles()) `path` resolves to, or -1 when it is
    // absent, filtered out or a loose file.
    int archiveOf(const std::string& path) const;
    // That archive's own copy of `path` (after this set's filter), even where another
    // archive's copy wins the mount; and every file path it holds (keyed).
    std::optional<std::vector<uint8_t>> readFromArchive(int archive, const std::string& path) const;
    std::vector<std::string> archivePaths(int archive) const;

    const std::vector<std::filesystem::path>& archiveFiles() const { return archiveFiles_; }
    static std::string key(std::string p);       // lowercased, '/'-separated

private:
    struct Win { int archive; Entry entry; };   // archive index into archives_

    std::filesystem::path dir_;
    MountConfig cfg_;
    std::vector<Archive> archives_;
    std::vector<std::filesystem::path> archiveFiles_;
    std::unordered_map<std::string, Win> map_;   // winning archive entry per path
};

// The engine's runtime read-path: an ordered stack of MountSets, each optionally
// exposed under a virtual path prefix, resolved highest-precedence-first. This is
// how a retail install directory is presented to the loaders as one namespace:
//
//   mods/cosmetic/<selected pack>/        (loose + archives, cosmetic files only)
//   mods/cosmetic/always_enabled/<pack>/  (every game, cosmetic files only)
//   mods/game/<selected pack>/            (loose + archives, everything)
//   mods/game/always_enabled/<pack>/      (every non-campaign game this player hosts)
//   Maps/        (*.kmp single-map HPIs + loose)     -> kmap/<name>.*
//   <root>/      (*.hpi only, no loose)              -> the base game + expansions
//   Music/       (loose *.wav)                       -> music/<file>
//
// Build one with mountRetailRoot(). Every asset read in the engine goes through
// Vfs::read / Vfs::list, so this is the ONLY way the engine touches game files.
class Vfs {
public:
    using Files = std::map<std::string, std::vector<uint8_t>>;
    Vfs() = default;
    // A room-local view: no downloaded resource can affect another running room.
    explicit Vfs(const Vfs* base, bool skipMapResources = false) : base_(base), skipMaps_(skipMapResources) {}
    void setModFiles(std::shared_ptr<const Files> files) { modFiles_ = std::move(files); }
    void setMapFiles(std::shared_ptr<const Files> files) { mapFiles_ = std::move(files); }
    // Downloaded maps are discoverable but their resources remain archive-local.
    void refreshMapCache(const std::filesystem::path& root);
    std::optional<std::pair<std::string, std::shared_ptr<const Files>>>
        cachedMap(const std::string& path) const;

    // Push a layer on TOP (highest precedence). `prefix` (e.g. "music/") maps the
    // layer's own namespace under that virtual directory; "" mounts it as-is.
    void addLayer(MountSet ms, const std::string& prefix = "", bool mapResources = false);

    bool has(const std::string& path, bool skipMapResources = false) const;
    // skipMapResources keeps generated terrain independent of downloaded map reskins.
    std::vector<uint8_t> read(const std::string& path, bool skipMapResources = false) const; // throws if absent
    std::optional<std::vector<uint8_t>> tryRead(const std::string& path, bool skipMapResources = false) const;
    std::vector<std::string> list(const std::string& prefix, bool skipMapResources = false) const;    // union, deduped
    // The map `mapPath`'s own copy of `path`: the entry in the single-map archive under
    // Maps/ that holds the map (or in this view's map files), even where another
    // installed map's archive wins that path in the mount (they all share Maps/, and
    // the Cartographer stamps every entry with date 0). A game mod's copy does not hide
    // it. nullopt when the map is loose, in no map archive, or the archive lacks it.
    std::optional<std::vector<uint8_t>> readFromMapArchive(const std::string& mapPath, const std::string& path) const;
    // Every file path (keyed) in that archive or view, for a lossless copy of it.
    std::vector<std::string> mapArchivePaths(const std::string& mapPath) const;

private:
    const Vfs* base_ = nullptr;
    bool skipMaps_ = false;
    struct CachedMap { std::shared_ptr<Archive> archive; std::string path; };
    std::map<std::string, CachedMap> cachedMaps_;
    std::shared_ptr<const Files> mapFiles_, modFiles_;
    struct Layer { MountSet ms; std::string prefix; bool mapResources = false; };   // prefix keyed, "" or trailing '/'
    std::vector<Layer> layers_;                          // back = highest precedence
    enum class MapArchive { None, Files, Mounted, Base };
    MapArchive mapArchive(const std::string& mapPath, const Layer*& layer, int& archive) const;
};

// The two kinds of mod pack under <install>/mods/. Cosmetic packs live in
// mods/cosmetic/ and expose only files that cannot affect the deterministic sim
// (textures/sprites/sound/music/fonts/gui), so every player may pick their own.
// Game packs live in mods/game/ and expose everything, including gameplay data
// (*.fbi, weapon/side/game *.tdf, canbuild, features, maps); only the host picks
// them and they are transferred to everyone. Each kind also has an
// always_enabled/ subfolder whose packs (same format) are always applied: the
// cosmetic ones in every game, the game ones in every non-campaign game this
// player hosts. Loose files in mods/<kind>/ and in always_enabled/ never load.
enum class ModKind { Cosmetic, Game };
// Which kinds of mod pack a launch may use (the debug `--mods none|cosmetic|game|all`
// switch; a release build always uses both). Which packs are ticked is a setting.
struct ModUse { bool game = true, cosmetic = true; };

// True if an internal path names a file our deterministic sim consumes -- so it
// must be identical across multiplayer peers (folded into the agreed data hash)
// and is excluded from cosmetic mod packs. Everything else is cosmetic.
bool affectsGameplay(const std::string& path);

// A 64-bit fingerprint of the gameplay data the sim consumes (unit stats, weapon/
// side/game data, build lists, features, scripts/models) as resolved from the VFS -- NOT maps
// (per-game) and NOT cosmetic files. Multiplayer peers must agree on this: it
// verifies the shipped gameplay files are unmodified, and (when game mods are
// mounted) that every player has the same ones.
uint64_t gameplayHash(const Vfs& vfs);

// Build the runtime VFS for a retail install root (see the layer diagram above).
// Game packs are layered over the original data and cosmetic packs over those
// (cosmetic wins), each kind alphabetical by pack name (later names win); names
// resolve like modFiles. The always-enabled cosmetic packs are NOT added here
// (this mount also feeds the handshake hash and map packages); a game's VFS
// adds them through mountPlayVfs().
Vfs mountRetailRoot(const std::filesystem::path& root,
                    const std::vector<std::string>& gamePacks = {},
                    const std::vector<std::string>& cosmeticPacks = {});
// The per-user folder for downloaded and test maps (MapCache/), game-mod downloads
// (ModCache/) and saved generated maps (Maps/Generated-*.kmp): the game's profile
// folder, or taserver's --map-cache-dir. Nothing is ever written to the game data
// folder. Set once at startup, before any mount; empty (the default, and every test
// that does not set it) means none. mountRetailRoot adds its Maps/ below the
// install's own Maps/ and lists its MapCache/.
void setUserMapRoot(const std::filesystem::path& root);
std::filesystem::path userMapRoot();
// The folder (matched ignoring case) under mods/cosmetic/ and mods/game/ that
// holds the always-enabled packs. It is never itself a pack.
inline constexpr const char* kAlwaysEnabledDir = "always_enabled";
// The ordinary packs a player may tick: immediate, real (non-symlink, non-hidden,
// at most 128-byte) subdirectories of mods/cosmetic/ or mods/game/, sorted. Not
// always_enabled/ (any case), and not a folder whose name equals an always-enabled
// pack's ignoring case: that pack wins the name, so within one kind a name means
// one pack in settings, packages and the room lists. Files directly in mods/,
// mods/cosmetic/ or mods/game/ are never mounted.
std::vector<std::string> modPacks(const std::filesystem::path& root, ModKind kind);
// The always-enabled packs: real subdirectories of mods/<kind>/always_enabled/,
// with the same filters as modPacks, sorted. Loose files there are ignored.
std::vector<std::string> alwaysEnabledPacks(const std::filesystem::path& root, ModKind kind);
// The merged files of the named packs. Each name is an always-enabled pack (if
// alwaysEnabledPacks lists it) or else an ordinary one; any other name throws.
// Always-enabled layers go first, then the ordinary ones, each alphabetical, so
// a ticked pack beats an always-enabled one; all share one budget.
Vfs::Files modFiles(const std::filesystem::path& root, ModKind kind,
                    const std::vector<std::string>& packs);
// A game's VFS: the retail mount, `gameFiles` over it (the room's verified
// game-mod package, or nothing), then -- with `withCosmetic` -- the always-enabled
// cosmetic packs (every game, campaigns included; per player, never sent) and
// this player's ticked cosmetic packs (packs no longer on disk are skipped
// silently). Each later layer wins. A cosmetic layer is its own choice
// and must never stop a game, so one that cannot load (a damaged archive, past
// the 256 MiB / 65536-file budget) or that would change gameplayHash (anims/
// frame timing or availability the sim reads: storms, nimbus, feature burns) is
// left out and named, with the reason, in `skipped`.
Vfs mountPlayVfs(const std::filesystem::path& root, Vfs::Files gameFiles,
                 std::vector<std::string> cosmeticPacks, bool withCosmetic = true,
                 std::vector<std::string>* skipped = nullptr);

// The canonical root HPI archives of a TA:Kingdoms install (base game + Iron Plague +
// the official map packs), lowercased. mountRetailRoot mounts ONLY these from the root;
// any other *.hpi dropped in the root is ignored.
extern const std::vector<std::string> kRootArchiveExts;

// Does `root` look like a usable install? True iff the essential base archives are
// present + readable and a core gameplay file resolves through the mount. When it
// returns false and `reason` is non-null, `reason` gets a short human-readable message.
bool validInstall(const std::filesystem::path& root, std::string* reason = nullptr);

// A cheap authenticity manifest of the canonical root HPIs present in `root`: a hex
// digest folding each file's name + byte size. It changes if a root archive is added,
// removed, replaced or resized, so a value saved in config can flag a tampered/moved
// install on the next launch.
std::string rootManifest(const std::filesystem::path& root);

// Resolve a map by display name (the .tnt stem, case-insensitive) to its VFS
// path, searching both map namespaces: Maps/ (maps.hpi & co.) and kmap/ (.kmp
// single-map archives). Returns "" if no such map. Both client and server use
// this so a map id on the wire resolves identically on each.
std::string findMap(const Vfs& vfs, const std::string& name);

// Every map as {display name, .tnt VFS path}, sorted by name, deduped by name.
std::vector<std::pair<std::string, std::string>> listMaps(const Vfs& vfs);

} // namespace ta::hpi
