#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ta::hpi {

// TAK ships its data in HPI version-2 archives (header version 0x00020000),
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
    uint32_t date = 0;            // time_t
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
// while a Maps/ layer adds ".kmp" (single-map HPIs) and an overrides/ layer
// keeps loose+archive with the widest extension set.
struct MountConfig {
    bool includeLoose = true;                                   // loose file overrides archives
    std::vector<std::string> archiveExts{".hpi", ".ufo"};       // scanned as ordered groups
    // Optional path filter: an entry (archive or loose) is exposed only if keep()
    // returns true. Empty = keep everything. Used for the cosmetic override tier,
    // which excludes gameplay-affecting files so they can't diverge in MP.
    std::function<bool(const std::string&)> keep;
    // Optional archive-name whitelist (LOWERCASED filenames incl. extension). When
    // non-empty, ONLY archives whose filename is listed are mounted -- so the retail
    // root loads exactly the known HPIs and never a stray *.hpi dropped alongside them.
    std::vector<std::string> archiveNames;
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
//   overrides/   (loose + *.hpi/*.ufo/*.kmp)         <- wins everything
//   Maps/        (*.kmp single-map HPIs + loose)     -> kmap/<name>.*
//   <root>/      (*.hpi only, no loose)              -> the base game + expansions
//   Music/       (loose *.wav)                       -> music/<file>
//
// Build one with mountRetailRoot(). Every asset read in the engine goes through
// Vfs::read / Vfs::list, so this is the ONLY way the engine touches game files.
class Vfs {
public:
    // Push a layer on TOP (highest precedence). `prefix` (e.g. "music/") maps the
    // layer's own namespace under that virtual directory; "" mounts it as-is.
    void addLayer(MountSet ms, const std::string& prefix = "");

    bool has(const std::string& path) const;
    std::vector<uint8_t> read(const std::string& path) const;          // throws if absent
    std::optional<std::vector<uint8_t>> tryRead(const std::string& path) const;
    std::vector<std::string> list(const std::string& prefix) const;    // union, deduped

private:
    struct Layer { MountSet ms; std::string prefix; };   // prefix keyed, "" or trailing '/'
    std::vector<Layer> layers_;                          // back = highest precedence
};

// Which override files a game will mount (Phase 3 multiplayer policy). None: no
// overrides. Cosmetic: only files that cannot affect the deterministic sim
// (art/models/anim/sound/music/fonts/gui). Full: every override, including
// gameplay data (*.fbi, weapon/side/game *.tdf, canbuild, features, maps).
enum class OverridePolicy { None, Cosmetic, Full };

// True if an internal path names a file our deterministic sim consumes -- so it
// must be identical across multiplayer peers (folded into the agreed data hash)
// and is excluded from the "cosmetic" override tier. Everything else is cosmetic.
bool affectsGameplay(const std::string& path);

// A 64-bit fingerprint of the gameplay data the sim consumes (unit stats, weapon/
// side/game data, build lists, features) as resolved from the VFS -- NOT maps
// (per-game) and NOT cosmetic files. Multiplayer peers must agree on this: it
// verifies the shipped gameplay files are unmodified, and (under the Full override
// tier, where gameplay overrides are mounted) that every player has the same ones.
uint64_t gameplayHash(const Vfs& vfs);

// Build the runtime VFS for a retail install root (see the layer diagram above).
Vfs mountRetailRoot(const std::filesystem::path& root,
                    OverridePolicy overrides = OverridePolicy::Full);

// The canonical root HPI archives of a TA:Kingdoms install (base game + Iron Plague +
// the official map packs), lowercased. mountRetailRoot mounts ONLY these from the root;
// any other *.hpi dropped in the root is ignored.
extern const std::vector<std::string> kRootHpiNames;

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
