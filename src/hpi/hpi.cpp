#include "util/virtualpath.h"
#include "hpi/hpi.h"
#include "gaf/nimbus.h"
#include "gaf/animationtiming.h"
#include "gaf/featureburntiming.h"
#include "tdf/tdf.h"
#include "tnt/mapgen.h"
#include "sim/sides.h"

#include <zlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include "util/u8path.h"

namespace ta::hpi {

namespace {

// Native disk names use UTF-8 at the VFS boundary, independent of the Windows
// process locale. Archive keys remain their original bytes.
std::string utf8(const std::filesystem::path& p) {
    const auto u=p.generic_u8string();
    return std::string(u.begin(),u.end());
}
std::optional<std::filesystem::path> looseFile(const std::filesystem::path& root,
                                               const std::string& name) {
    try {
        auto p=root/ta::u8path(name);
        std::error_code ec;
        if (std::filesystem::is_regular_file(p,ec)) return p;
    } catch (const std::runtime_error&) {
        // Legacy archive names need not be UTF-8. An unrepresentable loose-file
        // candidate must not prevent reading the byte-identical archive entry.
        // libstdc++ reports filesystem_error (a runtime_error); libc++'s UTF-8
        // conversion reports runtime_error directly.
    }
    return {};
}

uint32_t u32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
}

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    std::vector<uint8_t> data(std::filesystem::file_size(path));
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!in) throw std::runtime_error("short read on " + path.string());
    return data;
}

constexpr size_t kChunkHeaderSize = 19;

// Decompress one SQSH chunk at `off`; returns decompressed payload and
// advances `off` past the chunk. Operates on a raw view so it works over both a
// memory-mapped archive and an in-memory buffer.
std::vector<uint8_t> readChunk(const uint8_t* data, size_t size, size_t& off) {
    if (off + kChunkHeaderSize > size || std::memcmp(data + off, "SQSH", 4) != 0)
        throw std::runtime_error("expected SQSH chunk at offset " + std::to_string(off));
    uint8_t method = data[off + 5];
    uint8_t encrypted = data[off + 6];
    uint32_t compSize = u32(data + off + 7);
    uint32_t decompSize = u32(data + off + 11);
    off += kChunkHeaderSize;
    if (off + compSize > size)
        throw std::runtime_error("SQSH chunk overruns archive");

    std::vector<uint8_t> payload(data + off, data + off + compSize);
    off += compSize;
    if (encrypted) {
        for (uint32_t i = 0; i < compSize; ++i)
            payload[i] = uint8_t((payload[i] - i) ^ i);
    }

    if (method == 0) return payload;  // stored
    if (method == 1) {                // TA's LZ77 (4 KiB ring window)
        std::vector<uint8_t> out;
        out.reserve(decompSize);
        uint8_t window[4096] = {};
        int wpos = 1;
        size_t in = 0;
        while (in < payload.size()) {
            uint8_t tag = payload[in++];
            for (int bit = 0; bit < 8; ++bit, tag >>= 1) {
                if ((tag & 1) == 0) {
                    if (in >= payload.size()) goto lzDone;
                    uint8_t c = payload[in++];
                    out.push_back(c);
                    window[wpos] = c;
                    wpos = (wpos + 1) & 0xfff;
                } else {
                    if (in + 1 >= payload.size()) goto lzDone;
                    unsigned v = payload[in] | (payload[in + 1] << 8);
                    in += 2;
                    unsigned off = v >> 4;
                    if (off == 0) goto lzDone;    // end marker
                    unsigned n = (v & 0x0f) + 2;
                    for (unsigned k = 0; k < n; ++k) {
                        uint8_t c = window[off];
                        out.push_back(c);
                        window[wpos] = c;
                        wpos = (wpos + 1) & 0xfff;
                        off = (off + 1) & 0xfff;
                    }
                }
            }
        }
    lzDone:
        if (out.size() != decompSize)
            throw std::runtime_error("LZ77 size mismatch");
        return out;
    }
    if (method != 2)
        throw std::runtime_error("unsupported SQSH compression method " + std::to_string(method));

    std::vector<uint8_t> out(decompSize);
    uLongf outLen = decompSize;
    int rc = uncompress(out.data(), &outLen, payload.data(), compSize);
    if (rc != Z_OK || outLen != decompSize)
        throw std::runtime_error("zlib uncompress failed (rc=" + std::to_string(rc) + ")");
    return out;
}

// Read one dir/name block straight from the archive FILE without slurping the
// whole archive: the block is either a single SQSH chunk (19-byte header +
// compressed payload) or stored raw. The directory+name blocks are tens of KB
// while the archives total hundreds of MB, so seeking to just these makes
// mounting a retail root ~25x faster (and cold-start far cheaper still); the
// full file is only ever read lazily by bytes() when something is extracted.
std::vector<uint8_t> readBlockAt(std::ifstream& in, uint32_t off, uint32_t size) {
    uint8_t hdr[kChunkHeaderSize];
    in.clear();
    in.seekg(off);
    in.read(reinterpret_cast<char*>(hdr), 4);
    if (in && std::memcmp(hdr, "SQSH", 4) == 0) {
        in.read(reinterpret_cast<char*>(hdr + 4), kChunkHeaderSize - 4);
        if (!in) throw std::runtime_error("short read on SQSH block header");
        uint32_t compSize = u32(hdr + 7);
        std::vector<uint8_t> data(kChunkHeaderSize + size_t(compSize));
        std::memcpy(data.data(), hdr, kChunkHeaderSize);
        in.read(reinterpret_cast<char*>(data.data() + kChunkHeaderSize),
                std::streamsize(compSize));
        if (!in) throw std::runtime_error("short read on SQSH block payload");
        size_t pos = 0;
        return readChunk(data.data(), data.size(), pos);
    }
    in.clear();               // not SQSH (or too short): stored raw
    in.seekg(off);
    std::vector<uint8_t> data(size);
    in.read(reinterpret_cast<char*>(data.data()), std::streamsize(size));
    if (!in) throw std::runtime_error("block overruns archive");
    return data;
}

std::string nameAt(const std::vector<uint8_t>& names, uint32_t off) {
    if (off >= names.size()) throw std::runtime_error("name offset out of range");
    const char* s = reinterpret_cast<const char*>(&names[off]);
    return std::string(s, strnlen(s, names.size() - off));
}

void walk(const std::vector<uint8_t>& dir, const std::vector<uint8_t>& names,
          uint32_t off, const std::string& prefix, std::vector<Entry>& out) {
    if (uint64_t(off) + 20 > dir.size()) throw std::runtime_error("dir entry out of range");
    uint32_t namePtr = u32(&dir[off]);
    uint32_t firstSub = u32(&dir[off + 4]);
    uint32_t subCount = u32(&dir[off + 8]);
    uint32_t firstFile = u32(&dir[off + 12]);
    uint32_t fileCount = u32(&dir[off + 16]);

    std::string name = nameAt(names, namePtr);
    std::string path = prefix.empty() ? name : (name.empty() ? prefix : prefix + "/" + name);
    if (!path.empty())
        out.push_back({.path = path, .isDirectory = true});

    for (uint32_t i = 0; i < subCount; ++i)
        walk(dir, names, firstSub + i * 20, path, out);

    for (uint32_t i = 0; i < fileCount; ++i) {
        uint32_t f = firstFile + i * 24;
        if (uint64_t(f) + 24 > dir.size()) throw std::runtime_error("file entry out of range");
        Entry e;
        std::string fname = nameAt(names, u32(&dir[f]));
        e.path = path.empty() ? fname : path + "/" + fname;
        e.start = u32(&dir[f + 4]);
        e.decompressedSize = u32(&dir[f + 8]);
        e.compressedSize = u32(&dir[f + 12]);
        e.date = u32(&dir[f + 16]);
        out.push_back(std::move(e));
    }
}

bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y));
           });
}

} // namespace

Archive::Archive(const std::filesystem::path& file) : file_(file) {
    header_ = inspect(file);
    if (header_.magic != "HAPI")
        throw std::runtime_error(file.string() + ": not an HPI archive");
    if (header_.version == 0x00010000) {
        v1_ = true;
        // HPIHEADER1: dirSize at +8, headerKey at +12, dirStart at +16.
        uint32_t dirSize = header_.dirBlock, headerKey = header_.dirSize, dirStart = header_.nameBlock;
        if (headerKey & 0xff) {
            unsigned k = headerKey & 0xff;
            v1Key_ = uint8_t(~((k << 2) | (k >> 6)));
        }
        if (dirSize < dirStart || dirSize > header_.fileSize)
            throw std::runtime_error(file.string() + ": bad v1 directory bounds");
        std::ifstream in(file_, std::ios::binary);
        if (!in) throw std::runtime_error("cannot open " + file_.string());
        // Directory offsets are absolute file offsets: keep [0, dirSize).
        std::vector<uint8_t> dir(dirSize);
        in.read(reinterpret_cast<char*>(dir.data()), std::streamsize(dirSize));
        if (!in) throw std::runtime_error(file.string() + ": short v1 directory");
        if (v1Key_)
            for (uint32_t p = dirStart; p < dirSize; ++p)
                dir[p] = uint8_t((p ^ v1Key_) ^ uint8_t(~dir[p]));
        auto need = [&](uint64_t off, uint64_t n) {
            if (off < dirStart || off + n > dir.size())
                throw std::runtime_error(file_.string() + ": v1 directory offset out of range");
        };
        auto name = [&](uint32_t off) {
            need(off, 1);
            const char* c = reinterpret_cast<const char*>(&dir[off]);
            return std::string(c, strnlen(c, dir.size() - off));
        };
        std::function<void(uint32_t, const std::string&, int)> walkV1 =
            [&](uint32_t at, const std::string& prefix, int depth) {
                if (depth > 64) throw std::runtime_error(file_.string() + ": v1 directory too deep");
                need(at, 8);
                uint32_t count = u32(&dir[at]), list = u32(&dir[at + 4]);
                for (uint32_t i = 0; i < count; ++i) {
                    uint32_t e = list + i * 9;
                    need(e, 9);
                    std::string nm = name(u32(&dir[e]));
                    std::string path = prefix.empty() ? nm : prefix + "/" + nm;
                    uint32_t data = u32(&dir[e + 4]);
                    if (dir[e + 8]) {
                        entries_.push_back({.path = path, .isDirectory = true});
                        walkV1(data, path, depth + 1);
                    } else {
                        need(data, 9);
                        Entry f;
                        f.path = path;
                        f.start = u32(&dir[data]);
                        f.decompressedSize = u32(&dir[data + 4]);
                        f.v1Method = dir[data + 8];
                        entries_.push_back(std::move(f));
                    }
                }
            };
        walkV1(dirStart, "", 0);
        return;
    }
    if (header_.version != 0x00020000)
        throw std::runtime_error(file.string() + ": unsupported HPI version");

    std::ifstream in(file_, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + file_.string());
    auto dirBlock = readBlockAt(in, header_.dirBlock, header_.dirSize);
    auto nameBlock = readBlockAt(in, header_.nameBlock, header_.nameSize);
    walk(dirBlock, nameBlock, 0, "", entries_);
}

bool Archive::Mapping::open(const std::filesystem::path& p) {
#ifdef _WIN32
    // Editors may atomically replace an open archive. Existing mappings keep
    // reading their original file object; new opens see the replacement.
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0) { CloseHandle(f); return false; }
    HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(f);   // the mapping keeps the file alive
    if (!m) return false;
    void* v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    if (!v) { CloseHandle(m); return false; }
    data_ = static_cast<const uint8_t*>(v);
    size_ = size_t(sz.QuadPart);
    handle_ = m;
    return true;
#else
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { ::close(fd); return false; }
    void* v = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);   // the mapping keeps the pages alive
    if (v == MAP_FAILED) return false;
    data_ = static_cast<const uint8_t*>(v);
    size_ = size_t(st.st_size);
    return true;
#endif
}

void Archive::Mapping::close() {
    if (!data_) return;
#ifdef _WIN32
    UnmapViewOfFile(data_);
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
#else
    munmap(const_cast<uint8_t*>(data_), size_);
#endif
    data_ = nullptr;
    size_ = 0;
    handle_ = nullptr;
}

// Archives open on first read, from any thread (the lobby's preview roll, the terrain
// worker, the feature loader): one lock keeps two first reads from both opening it.
static std::mutex archiveOpenMutex;

Archive::View Archive::bytes() const {
    std::lock_guard<std::mutex> lock(archiveOpenMutex);
    if (!loaded_) {
        // Map the archive read-only; fall back to an eager read if mapping fails
        // (exotic filesystems). Either way the view is stable for our lifetime.
        if (!map_.open(file_)) fileData_ = readFile(file_);
        loaded_ = true;
    }
    if (map_.data()) return {map_.data(), map_.size()};
    return {fileData_.data(), fileData_.size()};
}

std::vector<uint8_t> Archive::readV1(const Entry& entry) const {
    View data = bytes();
    auto get = [&](uint64_t off, uint64_t n) {
        if (off + n > data.size) throw std::runtime_error(entry.path + ": data overruns archive");
        std::vector<uint8_t> b(data.data + off, data.data + off + n);
        if (v1Key_)
            for (uint64_t i = 0; i < n; ++i) {
                uint32_t p = uint32_t(off + i);
                b[i] = uint8_t((p ^ v1Key_) ^ uint8_t(~b[i]));
            }
        return b;
    };
    if (entry.v1Method == 0) return get(entry.start, entry.decompressedSize);
    uint32_t chunks = (entry.decompressedSize + 65535) / 65536;
    auto table = get(entry.start, uint64_t(chunks) * 4);
    std::vector<uint8_t> out;
    out.reserve(entry.decompressedSize);
    uint64_t pos = uint64_t(entry.start) + uint64_t(chunks) * 4;
    for (uint32_t c = 0; c < chunks; ++c) {
        uint32_t len = u32(&table[size_t(c) * 4]);
        auto raw = get(pos, len);
        pos += len;
        size_t off = 0;
        auto chunk = readChunk(raw.data(), raw.size(), off);
        out.insert(out.end(), chunk.begin(), chunk.end());
    }
    if (out.size() != entry.decompressedSize)
        throw std::runtime_error(entry.path + ": decompressed size mismatch");
    return out;
}

std::vector<uint8_t> Archive::read(const Entry& entry) const {
    if (entry.isDirectory) throw std::runtime_error(entry.path + " is a directory");
    if (v1_) return readV1(entry);
    View data = bytes();

    if (entry.compressedSize == 0) {
        if (uint64_t(entry.start) + entry.decompressedSize > data.size)
            throw std::runtime_error(entry.path + ": raw data overruns archive");
        return {data.data + entry.start,
                data.data + entry.start + entry.decompressedSize};
    }

    std::vector<uint8_t> out;
    out.reserve(entry.decompressedSize);
    size_t pos = entry.start;
    while (out.size() < entry.decompressedSize) {
        auto chunk = readChunk(data.data, data.size, pos);
        out.insert(out.end(), chunk.begin(), chunk.end());
    }
    if (out.size() != entry.decompressedSize)
        throw std::runtime_error(entry.path + ": decompressed size mismatch");
    return out;
}

const Entry* Archive::find(const std::string& path) const {
    for (const auto& e : entries_)
        if (!e.isDirectory && iequals(e.path, path)) return &e;
    return nullptr;
}

namespace {

// Directory tree assembled from the pack paths (subdirs sorted for a
// deterministic archive; files kept in insertion order).
struct PackDir {
    std::string name;
    std::map<std::string, PackDir> subs;
    std::vector<std::pair<std::string, const std::vector<uint8_t>*>> files;
};

void put32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    b[off] = uint8_t(v); b[off + 1] = uint8_t(v >> 8);
    b[off + 2] = uint8_t(v >> 16); b[off + 3] = uint8_t(v >> 24);
}
void push32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 24));
}

} // namespace

std::vector<uint8_t> pack(const std::vector<PackFile>& files, uint32_t date) {
    constexpr uint32_t kHeader = 28;   // HAPI + version + 5 u32 fields

    // 1. Build the directory tree from the '/'-separated paths.
    PackDir root;
    for (const auto& f : files) {
        PackDir* d = &root;
        size_t i = 0;
        while (true) {
            size_t slash = f.path.find('/', i);
            std::string part = f.path.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
            if (slash == std::string::npos) { d->files.push_back({part, &f.data}); break; }
            PackDir& sub = d->subs[part];
            sub.name = part;
            d = &sub;
            i = slash + 1;
        }
    }

    // 2. Lay out file data (raw, concatenated) and record each file's start.
    std::vector<uint8_t> fileData;
    std::map<const std::vector<uint8_t>*, uint32_t> fileStart;
    for (const auto& f : files) {
        fileStart[&f.data] = kHeader + uint32_t(fileData.size());
        fileData.insert(fileData.end(), f.data.begin(), f.data.end());
    }

    // 3. Name block: NUL-terminated strings, deduped; offset 0 = "" (root name).
    std::vector<uint8_t> nameBlock;
    nameBlock.push_back(0);
    std::map<std::string, uint32_t> nameOff{{"", 0}};
    auto intern = [&](const std::string& s) -> uint32_t {
        auto it = nameOff.find(s);
        if (it != nameOff.end()) return it->second;
        uint32_t off = uint32_t(nameBlock.size());
        nameBlock.insert(nameBlock.end(), s.begin(), s.end());
        nameBlock.push_back(0);
        nameOff[s] = off;
        return off;
    };

    // 4. Directory block: root's 20-byte entry at offset 0, then each dir's
    //    subdir array (20 bytes each) + file array (24 bytes each), referenced
    //    by offset. Serialize depth-first, allocating arrays as we go.
    std::vector<uint8_t> dir(20, 0);
    std::function<void(const PackDir&, uint32_t)> ser = [&](const PackDir& node, uint32_t entryOff) {
        uint32_t subOff = uint32_t(dir.size());
        dir.resize(subOff + node.subs.size() * 20);
        uint32_t fileOff = uint32_t(dir.size());
        dir.resize(fileOff + node.files.size() * 24);
        put32(dir, entryOff + 0, intern(node.name));
        put32(dir, entryOff + 4, subOff);
        put32(dir, entryOff + 8, uint32_t(node.subs.size()));
        put32(dir, entryOff + 12, fileOff);
        put32(dir, entryOff + 16, uint32_t(node.files.size()));
        int i = 0;
        for (const auto& [fn, data] : node.files) {
            uint32_t f = fileOff + uint32_t(i) * 24;
            put32(dir, f + 0, intern(fn));
            put32(dir, f + 4, fileStart[data]);
            put32(dir, f + 8, uint32_t(data->size()));
            put32(dir, f + 12, 0);          // compressedSize 0 = stored raw
            put32(dir, f + 16, date);
            ++i;                            // f+20..24 left zero
        }
        int s = 0;
        for (const auto& [sn, sub] : node.subs) { ser(sub, subOff + uint32_t(s) * 20); ++s; }
    };
    ser(root, 0);

    // 5. Assemble: header | file data | name block | dir block.
    uint32_t nameBlockOff = kHeader + uint32_t(fileData.size());
    uint32_t dirBlockOff = nameBlockOff + uint32_t(nameBlock.size());
    std::vector<uint8_t> out;
    out.reserve(dirBlockOff + dir.size());
    const char* magic = "HAPI";
    out.insert(out.end(), magic, magic + 4);
    push32(out, 0x00020000);
    push32(out, dirBlockOff);
    push32(out, uint32_t(dir.size()));
    push32(out, nameBlockOff);
    push32(out, uint32_t(nameBlock.size()));
    push32(out, kHeader);                   // dataStart
    out.insert(out.end(), fileData.begin(), fileData.end());
    out.insert(out.end(), nameBlock.begin(), nameBlock.end());
    out.insert(out.end(), dir.begin(), dir.end());
    return out;
}

HeaderInfo inspect(const std::filesystem::path& archive) {
    std::ifstream in(archive, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + archive.string());

    HeaderInfo info;
    info.fileSize = std::filesystem::file_size(archive);

    uint8_t raw[32] = {};
    in.read(reinterpret_cast<char*>(raw), sizeof raw);
    if (!in) throw std::runtime_error(archive.string() + ": too short for an HPI header");

    info.magic.assign(reinterpret_cast<char*>(raw), 4);
    info.version = u32(raw + 4);
    info.dirBlock = u32(raw + 8);
    info.dirSize = u32(raw + 12);
    info.nameBlock = u32(raw + 16);
    info.nameSize = u32(raw + 20);
    info.dataStart = u32(raw + 24);
    return info;
}

std::string describe(const HeaderInfo& info) {
    std::ostringstream out;
    out << "magic:      " << (info.magic == "HAPI" ? "HAPI (ok)" : "'" + info.magic + "' (not an HPI archive?)") << "\n";

    char buf[96];
    std::snprintf(buf, sizeof buf, "version:    0x%08x", info.version);
    out << buf;
    switch (info.version) {
        case 0x00010000: out << " (classic Total Annihilation)"; break;
        case 0x00020000: out << " (TA: Kingdoms)"; break;
        case 0x4B4E4142: out << " ('BANK' savegame)"; break;
        default:         out << " (unknown)"; break;
    }
    out << "\n";
    std::snprintf(buf, sizeof buf,
                  "dir block:  0x%08x (%u bytes)\nname block: 0x%08x (%u bytes)\ndata start: 0x%08x\n",
                  info.dirBlock, info.dirSize, info.nameBlock, info.nameSize, info.dataStart);
    out << buf;
    out << "size:       " << info.fileSize << " bytes\n";
    return out.str();
}

// ---- MountSet: retail asset precedence over a directory of archives --------

std::string MountSet::key(std::string p) {
    for (char& c : p) {
        if (c == '\\') c = '/';
        else c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    if (!p.empty() && p.front() == '/') p.erase(p.begin());
    return p;
}

MountSet::MountSet(const std::filesystem::path& dir, MountConfig cfg)
    : dir_(dir), cfg_(std::move(cfg)) {
    namespace fs = std::filesystem;
    // Case-insensitive filename sort, like the retail FindFirstFile scan (so a
    // tie in file date breaks the same way it did originally).
    auto ci = [](const fs::path& a, const fs::path& b) {
        std::string x = utf8(a.filename()), y = utf8(b.filename());
        for (char& c : x) c = char(std::tolower(static_cast<unsigned char>(c)));
        for (char& c : y) c = char(std::tolower(static_cast<unsigned char>(c)));
        return x < y;
    };
    // Collect archives one extension group at a time, each group alphabetical,
    // groups concatenated in cfg order -- reproducing the retail
    // FindFirstFile("*.HPI") then ("*.UFO") mount order for the default config.
    for (const std::string& want : cfg_.archiveExts) {
        std::vector<fs::path> group;
        if (fs::is_directory(dir_))
            for (const auto& e : fs::directory_iterator(dir_)) {
                if (!e.is_regular_file()) continue;
                std::string ext = utf8(e.path().extension());
                for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
                if (ext != want) continue;
                if (!cfg_.archiveNames.empty()) {   // name whitelist (retail root)
                    std::string fn = utf8(e.path().filename());
                    for (char& c : fn) c = char(std::tolower(static_cast<unsigned char>(c)));
                    if (std::find(cfg_.archiveNames.begin(), cfg_.archiveNames.end(), fn)
                        == cfg_.archiveNames.end()) continue;
                }
                if (cfg_.archiveFilter) {
                    std::string fn = utf8(e.path().filename());
                    for (char& c : fn) c = char(std::tolower(static_cast<unsigned char>(c)));
                    if (!cfg_.archiveFilter(fn)) continue;
                }
                group.push_back(e.path());
            }
        std::sort(group.begin(), group.end(), ci);
        archiveFiles_.insert(archiveFiles_.end(), group.begin(), group.end());
    }

    // Mount each; resolve conflicts by newest entry date (a strict '>' keeps the
    // earlier mount on a tie, matching the retail 'jae' comparison).
    for (const auto& file : archiveFiles_) {
        int idx = int(archives_.size());
        try {
            archives_.emplace_back(file);
        } catch (const std::exception&) {
            continue;   // skip an unreadable archive rather than abort the mount
        }
        for (const auto& e : archives_.back().entries()) {
            if (e.isDirectory) continue;
            if (cfg_.keep && !cfg_.keep(e.path)) continue;   // filtered (e.g. cosmetic tier)
            std::string k = key(e.path);
            auto it = map_.find(k);
            if (it == map_.end() || (!cfg_.firstMountWins && e.date > it->second.entry.date))
                map_[k] = Win{idx, e};
        }
    }
}

bool MountSet::has(const std::string& path) const {
    if (cfg_.keep && !cfg_.keep(path)) return false;
    if (cfg_.includeLoose && looseFile(dir_,path).has_value()) return true;
    return map_.count(key(path)) != 0;
}

std::vector<uint8_t> MountSet::read(const std::string& path) const {
    if (cfg_.keep && !cfg_.keep(path)) throw std::runtime_error("filtered: " + path);
    // 1. A loose file on disk overrides archives (the engine fopen()s first).
    if (cfg_.includeLoose) {
        auto loose = looseFile(dir_,path);
        if (loose) {
            std::ifstream f(*loose, std::ios::binary);
            return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
        }
    }
    // 2. Otherwise the winning (newest) archive copy.
    auto it = map_.find(key(path));
    if (it == map_.end()) throw std::runtime_error("not in mount set: " + path);
    return archives_[size_t(it->second.archive)].read(it->second.entry);
}

std::vector<std::string> MountSet::paths() const {
    std::vector<std::string> out;
    out.reserve(map_.size());
    for (const auto& [k, w] : map_) out.push_back(w.entry.path);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> MountSet::list(const std::string& prefix) const {
    namespace fs = std::filesystem;
    std::string kp = key(prefix);
    if (!kp.empty() && kp.back() != '/') kp += '/';   // treat as a directory prefix
    std::unordered_map<std::string, std::string> out;  // key -> winning original path
    auto under = [&](const std::string& k) { return kp.empty() || k.compare(0, kp.size(), kp) == 0; };
    // Archive winners first...
    for (const auto& [k, w] : map_)
        if (under(k)) out[k] = w.entry.path;
    // ...then loose files override (same precedence as read()).
    if (cfg_.includeLoose) {
        fs::path base = kp.empty() ? dir_ : dir_ / ta::u8path(prefix);
        std::error_code ec;
        if (fs::is_directory(base, ec))
            for (auto it = fs::recursive_directory_iterator(base, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                std::string rel = utf8(fs::relative(it->path(), dir_, ec));
                if (rel.empty()) continue;
                if (cfg_.keep && !cfg_.keep(rel)) continue;
                out[key(rel)] = rel;
            }
    }
    std::vector<std::string> paths;
    paths.reserve(out.size());
    for (const auto& [k, p] : out) paths.push_back(p);
    std::sort(paths.begin(), paths.end());
    return paths;
}

std::string MountSet::sourceOf(const std::string& path) const {
    if (cfg_.includeLoose) {
        if (auto loose = looseFile(dir_,path)) return utf8(*loose) + " (loose)";
    }
    auto it = map_.find(key(path));
    if (it == map_.end()) return "<absent>";
    return utf8(archiveFiles_[size_t(it->second.archive)].filename()) +
           "!" + it->second.entry.path;
}

int MountSet::archiveOf(const std::string& path) const {
    if (!has(path) || (cfg_.includeLoose && looseFile(dir_,path))) return -1;
    auto it = map_.find(key(path));
    return it == map_.end() ? -1 : it->second.archive;
}

std::optional<std::vector<uint8_t>> MountSet::readFromArchive(int archive, const std::string& path) const {
    if (archive < 0 || size_t(archive) >= archives_.size()) return std::nullopt;
    const Entry* entry = archives_[size_t(archive)].find(key(path));
    if (!entry || (cfg_.keep && !cfg_.keep(entry->path))) return std::nullopt;
    return archives_[size_t(archive)].read(*entry);
}

std::vector<std::string> MountSet::archivePaths(int archive) const {
    std::vector<std::string> paths;
    if (archive < 0 || size_t(archive) >= archives_.size()) return paths;
    for (const auto& e : archives_[size_t(archive)].entries())
        if (!e.isDirectory && (!cfg_.keep || cfg_.keep(e.path))) paths.push_back(key(e.path));
    return paths;
}

// ---- Vfs: layered runtime read-path over a retail install root -------------

namespace {
std::mutex userMapRootMutex;
std::filesystem::path userMapRootPath;
}
void setUserMapRoot(const std::filesystem::path& root) {
    std::lock_guard lock(userMapRootMutex);
    userMapRootPath = root;
}
std::filesystem::path userMapRoot() {
    std::lock_guard lock(userMapRootMutex);
    return userMapRootPath;
}

void Vfs::refreshMapCache(const std::filesystem::path& root) {
    cachedMaps_.clear();
    if (root.empty()) return;
    std::error_code ec;
    const auto folder = root / "MapCache";
    if (!std::filesystem::is_directory(folder, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (ec) break;
        auto digest = utf8(entry.path().stem());
        if (entry.path().extension() != ".kmp" || digest.size() != 64 ||
            digest.find_first_not_of("0123456789abcdef") != std::string::npos) continue;
        try {
            auto archive = std::make_shared<Archive>(entry.path());
            for (const auto& file : archive->entries()) {
                auto path = MountSet::key(file.path);
                if (file.isDirectory || !path.ends_with(".tnt")) continue;
                const auto alias = "kmap/" + ta::vpath::stem(path) + " [" + digest.substr(0,16) + "].tnt";
                cachedMaps_.emplace(alias, CachedMap{archive,path});
                break;
            }
        } catch (const std::exception&) {} // An incomplete/unreadable cache is not a playable map.
    }
}
std::optional<std::pair<std::string, std::shared_ptr<const Vfs::Files>>>
Vfs::cachedMap(const std::string& path) const {
    auto it = cachedMaps_.find(MountSet::key(path));
    if (it == cachedMaps_.end()) return base_ ? base_->cachedMap(path) : std::nullopt;
    auto files = std::make_shared<Files>();
    for (const auto& file : it->second.archive->entries())
        if (!file.isDirectory) (*files)[MountSet::key(file.path)] = it->second.archive->read(file);
    return std::make_pair(it->second.path, std::move(files));
}

void Vfs::addLayer(MountSet ms, const std::string& prefix, bool mapResources) {
    std::string p = MountSet::key(prefix);
    if (!p.empty() && p.back() != '/') p += '/';
    layers_.push_back({std::move(ms), std::move(p), mapResources});
}

// Try each layer highest-precedence-first; if the requested path is under a
// layer's virtual prefix, strip the prefix and delegate to that MountSet.
std::optional<std::vector<uint8_t>> Vfs::tryRead(const std::string& path, bool skipMapResources) const {
    std::string kp = MountSet::key(path);
    skipMapResources = skipMapResources || skipMaps_;
    if (mapFiles_ && !skipMapResources) {
        auto found = mapFiles_->find(kp);
        if (found != mapFiles_->end()) return found->second;
    }
    if (modFiles_) { auto found=modFiles_->find(kp); if(found!=modFiles_->end()) return found->second; }
    for (auto it = layers_.rbegin(); it != layers_.rend(); ++it) {
        if ((skipMapResources || mapFiles_) && it->mapResources) continue;
        if (!it->prefix.empty() && kp.compare(0, it->prefix.size(), it->prefix) != 0) continue;
        std::string sub = path.substr(it->prefix.size());
        if (it->ms.has(sub)) return it->ms.read(sub);
    }
    if (!skipMapResources) {
        auto it = cachedMaps_.find(ta::vpath::replaceExtension(kp,".tnt"));
        if (it != cachedMaps_.end() && (kp.ends_with(".tnt") || kp.ends_with(".ota"))) {
            const auto source = ta::vpath::replaceExtension(it->second.path,ta::vpath::extension(kp));
            if (auto entry = it->second.archive->find(source)) return it->second.archive->read(*entry);
        }
    }
    return base_ ? base_->tryRead(path, skipMapResources || bool(mapFiles_)) : std::nullopt;
}

std::optional<std::vector<uint8_t>> Vfs::readFromMapArchive(const std::string& mapPath, const std::string& path) const {
    int archive = -1;
    const Layer* layer = nullptr;
    switch (mapArchive(mapPath, layer, archive)) {
    case MapArchive::Files: {
        const auto found = mapFiles_->find(MountSet::key(path));
        if (found == mapFiles_->end()) return std::nullopt;
        return found->second;
    }
    case MapArchive::Mounted:
        if (MountSet::key(path).compare(0, layer->prefix.size(), layer->prefix) != 0) return std::nullopt;
        return layer->ms.readFromArchive(archive, path.substr(layer->prefix.size()));
    case MapArchive::Base: return base_->readFromMapArchive(mapPath, path);
    case MapArchive::None: break;
    }
    return std::nullopt;
}

std::vector<std::string> Vfs::mapArchivePaths(const std::string& mapPath) const {
    int archive = -1;
    const Layer* layer = nullptr;
    std::vector<std::string> paths;
    switch (mapArchive(mapPath, layer, archive)) {
    case MapArchive::Files: for (const auto& [name, bytes] : *mapFiles_) paths.push_back(name); break;
    case MapArchive::Mounted:
        for (auto& sub : layer->ms.archivePaths(archive)) paths.push_back(layer->prefix + sub);
        break;
    case MapArchive::Base: return base_->mapArchivePaths(mapPath);
    case MapArchive::None: break;
    }
    return paths;
}

// Where the map `mapPath` lives: this view's map files, a single-map archive in a
// Maps/ layer (the highest layer that has the map, as tryRead resolves it), the base
// view, or nowhere a map's own files can come from (a loose map, the base install's
// maps/, a view that skips map resources).
Vfs::MapArchive Vfs::mapArchive(const std::string& mapPath, const Layer*& layer, int& archive) const {
    if (skipMaps_) return MapArchive::None;
    const std::string mk = MountSet::key(mapPath);
    if (mapFiles_) return mapFiles_->count(mk) ? MapArchive::Files : MapArchive::None;
    for (auto it = layers_.rbegin(); it != layers_.rend(); ++it) {
        if (!it->prefix.empty() && mk.compare(0, it->prefix.size(), it->prefix) != 0) continue;
        const std::string sub = mapPath.substr(it->prefix.size());
        if (!it->ms.has(sub)) continue;
        if (!it->mapResources) return MapArchive::None;
        archive = it->ms.archiveOf(sub);
        layer = &*it;
        return archive >= 0 ? MapArchive::Mounted : MapArchive::None;
    }
    return base_ ? MapArchive::Base : MapArchive::None;
}

std::vector<uint8_t> Vfs::read(const std::string& path, bool skipMapResources) const {
    if (auto b = tryRead(path, skipMapResources)) return std::move(*b);
    throw std::runtime_error("not in data set: " + path);
}

bool Vfs::has(const std::string& path, bool skipMapResources) const {
    skipMapResources = skipMapResources || skipMaps_;
    const std::string kp = MountSet::key(path);
    if (mapFiles_ && !skipMapResources && mapFiles_->count(kp)) return true;
    if (modFiles_ && modFiles_->count(kp)) return true;
    for (auto it = layers_.rbegin(); it != layers_.rend(); ++it) {
        if ((skipMapResources || mapFiles_) && it->mapResources) continue;
        if (!it->prefix.empty() && kp.compare(0, it->prefix.size(), it->prefix) != 0) continue;
        if (it->ms.has(path.substr(it->prefix.size()))) return true;
    }
    if (!skipMapResources && (kp.ends_with(".tnt") || kp.ends_with(".ota"))) {
        auto it = cachedMaps_.find(ta::vpath::replaceExtension(kp,".tnt"));
        if (it != cachedMaps_.end())
            return it->second.archive->find(ta::vpath::replaceExtension(it->second.path,ta::vpath::extension(kp))) != nullptr;
    }
    return base_ && base_->has(path, skipMapResources || bool(mapFiles_));
}

std::vector<std::string> Vfs::list(const std::string& prefix, bool skipMapResources) const {
    skipMapResources = skipMapResources || skipMaps_;
    std::string kp = MountSet::key(prefix);
    std::unordered_map<std::string, std::string> out;   // key -> winning original path
    if (base_) for (const auto& p : base_->list(prefix, skipMapResources || bool(mapFiles_))) out[MountSet::key(p)] = p;
    // Lowest precedence first, so higher layers overwrite the reported source.
    for (const auto& L : layers_) {
        if ((skipMapResources || mapFiles_) && L.mapResources) continue;
        // The virtual prefix each of this layer's paths carries.
        if (L.prefix.empty()) {
            for (const auto& p : L.ms.list(prefix))
                out[MountSet::key(p)] = p;
        } else if (kp.size() <= L.prefix.size()) {
            // Requested prefix is an ancestor of (or equals) the layer prefix:
            // list the whole layer, re-prefix, keep those under the request.
            if (L.prefix.compare(0, kp.size(), kp) != 0) continue;
            for (const auto& p : L.ms.list("")) {
                std::string full = L.prefix + p;
                out[MountSet::key(full)] = full;
            }
        } else {
            // Requested prefix reaches into the layer: strip the layer prefix.
            if (kp.compare(0, L.prefix.size(), L.prefix) != 0) continue;
            std::string sub = prefix.substr(L.prefix.size());
            for (const auto& p : L.ms.list(sub))
                out[MountSet::key(L.prefix + p)] = L.prefix + p;
        }
    }
    if (!skipMapResources) for (const auto& [p, archive] : cachedMaps_) {
        if (kp.empty() || p.starts_with(kp.back() == '/' ? kp : kp + '/')) {
            out[p] = p;
            const auto ota = ta::vpath::replaceExtension(p,".ota");
            if (archive.archive->find(ta::vpath::replaceExtension(archive.path,".ota"))) out[ota] = ota;
        }
    }
    if (modFiles_) for (const auto& [p, bytes] : *modFiles_)
        if (kp.empty() || p == kp || p.starts_with(kp.back() == '/' ? kp : kp + '/')) out[p] = p;
    if (mapFiles_ && !skipMapResources) for (const auto& [p, bytes] : *mapFiles_)
        if (kp.empty() || p == kp || p.starts_with(kp.back() == '/' ? kp : kp + '/')) out[p] = p;
    std::vector<std::string> paths;
    paths.reserve(out.size());
    for (const auto& [k, p] : out) paths.push_back(p);
    std::sort(paths.begin(), paths.end());
    return paths;
}

// ---- gameplay classification + retail-root mounting ------------------------

bool affectsGameplay(const std::string& path) {
    std::string k = MountSet::key(path);
    auto ext = [&](const char* e) {
        size_t n = std::strlen(e);
        return k.size() >= n && k.compare(k.size() - n, n, e) == 0;
    };
    // Files our deterministic sim consumes: unit stats, weapon/side/game data,
    // build lists, map geometry + scenario data, scripts and model origins.
    // Factory scripts and their animated build points affect production.
    // Build-tree markers (canbuild/<builder>/<buildable>.tdf) define who builds
    // what -- gameplay, and .tdf, so check them BEFORE the .tdf rule below. The
    // shipped canbuildcb/ tree is not: retail never reads it, nor does the sim.
    if (k.find("canbuild") != std::string::npos && k.find("canbuildcb") == std::string::npos)
        return true;
    if (ext(".fbi") || ext(".tnt") || ext(".ota") || ext(".crt")) return true;
    // Factory scripts and model build-piece transforms now drive production.
    if (ext(".cob") || ext(".3do")) return true;
    if (ext(".tdf")) {
        // Only the .tdf files our sim actually consumes are gameplay: weapon
        // stats, movement classes, faction data, god timing. Everything else in
        // gamedata/ (explosions, soundclasses, download, ...) is read by the
        // viewer/audio only -> cosmetic.
        // Feature defs are DUAL-USE: mostly art keys (filename/seqname/object)
        // that cosmetic reskin packs edit freely, plus a handful of sim inputs
        // (footprint/energy/flamable/spreadchance/...). Classifying the whole
        // file as gameplay killed cosmetic doodad reskins, so they stay
        // cosmetic-mountable here; the SIM-relevant content is protected
        // separately -- gameplayHash digests just those keys in canonical
        // form (below), so an art-only mod hashes identically while a
        // sim-key edit still faults the version gate instead of desyncing.
        if (k.find("weapons/") != std::string::npos) return true;
        // Named projectile emitters consume the shared deterministic RNG.
        if (k.starts_with("gamedata/effects/")) return true;
        if (k.find("moveinfo") != std::string::npos || k.find("sidedata") != std::string::npos ||
            k.find("gods") != std::string::npos)
            return true;
        return false;
    }
    return false;
}

uint64_t gameplayHash(const Vfs& input) {
    // Community map features/art are transferred and checked per room. They must
    // not prevent a player without that map from passing the base-game handshake.
    Vfs vfs(&input, true);
    // Every gameplay entry the sim consumes (see affectsGameplay), EXCEPT maps --
    // those are per-game, checked separately. Two installs with identical retail
    // gameplay files hash the same; a modified/overridden gameplay file changes it.
    // TypeRegistry::loadDir reads scripts/<id>.cob (production, corpse rules) and
    // objects3d/<id>.3do (production transforms, sight and projectile geometry);
    // every .cob/.3do there is hashed, as none is ever cosmetic.
    std::unordered_map<std::string, std::string> byKey;   // key -> winning path (deduped)
    for (const char* pre : {"units", "canbuild", "features",
                            "gamedata", "weapons", "scripts", "objects3d"})
        for (const std::string& p : vfs.list(pre)) {
            if (!affectsGameplay(p)) continue;
            std::string k = MountSet::key(p);
            if (k.size() >= 4) {   // exclude any map files that live under these prefixes
                std::string e = k.substr(k.size() - 4);
                if (e == ".tnt" || e == ".ota" || e == ".crt") continue;
            }
            byKey.emplace(k, p);
        }
    // Generated maps are assembled from sections rather than a single map TNT.
    // These height/feature planes must agree even when players own different map packs.
    for (const auto& path : mapgen::assetPaths(vfs))
        byKey.emplace(MountSet::key(path), path);
    std::vector<std::string> keys;
    keys.reserve(byKey.size());
    for (const auto& [k, p] : byKey) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    // FNV-1a over each entry's key then content -- order-independent-safe because
    // the keys are sorted, and content-addressed so it's identical across machines.
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](const uint8_t* d, size_t n) {
        for (size_t i = 0; i < n; ++i) { h ^= d[i]; h *= 1099511628211ULL; }
    };
    for (const std::string& k : keys) {
        mix(reinterpret_cast<const uint8_t*>(k.data()), k.size());
        uint8_t z = 0; mix(&z, 1);
        try { auto b = vfs.read(byKey[k]); mix(b.data(), b.size()); } catch (const std::exception&) {}
    }
    // Only availability affects launch delay; changing valid nimbus pixels is
    // still cosmetic. Missing/invalid art must not silently change peer timing.
    for (const auto& [side, animation] : gaf::factionNimbus(vfs)) {
        const std::string key = "nimbus-available:" + side;
        mix(reinterpret_cast<const uint8_t*>(key.data()), key.size());
        const uint8_t present = !animation.empty();
        mix(&present, 1);
    }
    // Wandering start/end frame clocks control damage admission and lifetime.
    // Protect timing and availability without disallowing cosmetic recolors.
    std::set<std::string> stormArt;
    for(const auto& key:keys) if(key.ends_with(".fbi")) {
        const auto bytes=vfs.read(byKey[key]);
        const auto root=tdf::parseText(std::string(bytes.begin(),bytes.end()),key);
        for(const auto& [section,node]:root.children)
            for(const char* field:{"wanderstartart","wanderloopart","wanderendart"}) {
                const auto name=MountSet::key(node.valueOr(field,""));
                if(!name.empty())stormArt.insert(name);
            }
    }
    for(const auto& name:stormArt) {
        const auto timing=gaf::animationTiming(vfs,name);
        const std::string key="storm-timing:"+name;
        mix(reinterpret_cast<const uint8_t*>(key.data()),key.size());
        const uint16_t count=uint16_t(timing.size());
        const uint8_t sizeBytes[]={uint8_t(count),uint8_t(count>>8)};
        mix(sizeBytes,2);
        for(uint16_t duration:timing) {
            const uint8_t durationBytes[]={uint8_t(duration),uint8_t(duration>>8)};
            mix(durationBytes,2);
        }
    }
    // Feature TDFs: hash only the keys the deterministic sim consumes (the
    // exact set matchsetup's FeatDef loader reads), in canonical sorted order.
    // Art keys (filename/seqname/object/world/...) are deliberately absent, so
    // a cosmetic reskin that only redirects art hashes the same as retail --
    // while a mod touching footprints, reclaim yield, or the burn chain
    // changes the hash and faults the MP version gate cleanly.
    {
        auto mixs = [&](const std::string& v) {
            mix(reinterpret_cast<const uint8_t*>(v.data()), v.size());
            uint8_t z = 0; mix(&z, 1);
        };
        gaf::FeatureBurnTiming burnTiming(vfs);
        static const char* kSimKeys[] = {
            "animating", "blocking", "category", "damage",
            "decomposetime", "energy", "featureburnt", "featuredead", "flamable",
            "footprintx", "footprintz", "height", "indestructible",
            "reclaimable", "resurrectable", "sparktime",
            "spreadchance",
        };
        std::map<std::string, std::string> featFiles;   // sorted mount keys
        for (const std::string& p : vfs.list("features")) {
            std::string k = MountSet::key(p);
            if (k.size() < 4 || k.substr(k.size() - 4) != ".tdf") continue;
            featFiles[k] = p;
        }
        for (const auto& [k, p] : featFiles) {
            ta::tdf::Node root;
            try {
                auto b = vfs.read(p);
                root = ta::tdf::parseText(std::string(b.begin(), b.end()), p);
            } catch (const std::exception&) { continue; }   // loader skips it too
            std::map<std::string, const ta::tdf::Node*> secs;   // sorted sections
            for (const auto& n : root.childOrder) {
                std::string lo = n;
                for (char& c : lo) c = char(std::tolower(static_cast<unsigned char>(c)));
                secs[lo] = &root.children.at(n);
            }
            for (const auto& [nm, node] : secs) {
                mixs(nm);
                for (const char* sk : kSimKeys) {
                    std::string v = node->valueOr(sk, "");
                    for (char& c : v)
                        c = char(std::tolower(static_cast<unsigned char>(c)));
                    mixs(v);
                }
                // seqnameburn's NAME is art, but its PRESENCE gates ignition
                // (retail StartBurning requires a burn anim) -- hash the bit.
                mixs(node->valueOr("seqnameburn", "").empty() ? "0" : "1");
                mixs(std::to_string(burnTiming.duration(*node)));
                // seqnamedie's length holds a destroyed feature in place.
                mixs(std::to_string(burnTiming.dieDuration(*node)));
            }
        }
    }
    return h;
}

// Total Annihilation's root archive scan, read from TotalA.exe 0x41d4c0: the
// patch archive rev31.GP3 ("rev%s.GP3" with "31"), then every *.CCX, every *.UFO
// and every *.HPI, each group in directory order, first mount winning a path.
// (Retail stops the *.HPI group after 10 successful mounts -- esi=10 at 0x41d5c0
// -- and then scans CD drives for %c:\*.hpi. On the GOG layout that cap would hide
// totala3/totala4/worlds.hpi, whose maps and campaign briefings exist nowhere
// else, so every *.HPI is mounted; precedence is unchanged.)
const std::vector<std::string> kRootArchiveExts = {".gp3", ".ccx", ".ufo", ".hpi"};
static bool rootArchiveWanted(const std::string& lowerName) {
    auto dot = lowerName.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = lowerName.substr(dot);
    if (ext == ".gp3") return lowerName == "rev31.gp3";
    return ext == ".ccx" || ext == ".ufo" || ext == ".hpi";
}

// Lowercased filenames present in the install root (regular files only).
static std::set<std::string> rootFileNames(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    std::set<std::string> have;
    std::error_code ec;
    if (fs::is_directory(root, ec))
        for (const auto& e : fs::directory_iterator(root, ec)) {
            if (ec) break;
            if (!e.is_regular_file(ec)) continue;
            std::string n = utf8(e.path().filename());
            for (char& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
            have.insert(n);
        }
    return have;
}

bool validInstall(const std::filesystem::path& root, std::string* reason) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec)) { if (reason) *reason = "not a folder"; return false; }
    // The essential base archives must be present (Iron Plague + map packs are optional).
    static const char* kEssential[] = {"totala1.hpi"};
    std::set<std::string> have = rootFileNames(root);
    for (const char* need : kEssential)
        if (!have.count(need)) { if (reason) *reason = std::string("missing ") + need; return false; }
    // Confirm the archives actually mount and a core gameplay file resolves.
    try {
        Vfs vfs = mountRetailRoot(root);
        if (!vfs.has("gamedata/sidedata.tdf")) { if (reason) *reason = "core game data unreadable"; return false; }
    } catch (const std::exception& e) { if (reason) *reason = std::string("mount failed: ") + e.what(); return false; }
    return true;
}

std::string rootManifest(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    std::error_code ec;
    // name -> byte size, for the canonical root HPIs that are actually present (sorted).
    std::map<std::string, uintmax_t> present;
    if (fs::is_directory(root, ec))
        for (const auto& e : fs::directory_iterator(root, ec)) {
            if (ec) break;
            if (!e.is_regular_file(ec)) continue;
            std::string l = utf8(e.path().filename());
            for (char& c : l) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (!rootArchiveWanted(l)) continue;
            present[l] = fs::file_size(e.path(), ec);
        }
    uint64_t h = 1469598103934665603ull;   // FNV-1a over "name:size;" pairs
    auto mix = [&](const std::string& s) { for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; } };
    for (const auto& [n, sz] : present) { mix(n); mix(":"); mix(std::to_string(sz)); mix(";"); }
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

namespace {
// Case-insensitive real (non-symlink) subdirectory `want` of `dir`, or empty.
std::filesystem::path realSubdir(const std::filesystem::path& dir, const char* want) {
    std::error_code ec;
    if(dir.empty())return {};
    for(const auto& entry:std::filesystem::directory_iterator(dir,ec))
        if(MountSet::key(utf8(entry.path().filename()))==want && entry.is_directory(ec) && !entry.is_symlink(ec))return entry.path();
    return {};
}
std::filesystem::path modDirectory(const std::filesystem::path& root,ModKind kind) {
    return realSubdir(realSubdir(root,"mods"),kind==ModKind::Game?"game":"cosmetic");
}
// mods/<kind>/always_enabled/ (any case), or empty.
std::filesystem::path modAlwaysDirectory(const std::filesystem::path& root,ModKind kind) {
    return realSubdir(modDirectory(root,kind),kAlwaysEnabledDir);
}
// The real (non-symlink, non-hidden) subdirectories of `dir`, sorted; files are ignored.
std::vector<std::string> listPackDirs(const std::filesystem::path& dir) {
    std::vector<std::string> result;if(dir.empty())return result;
    std::error_code ec;
    for(const auto& entry:std::filesystem::directory_iterator(dir,ec)) {
        const auto name=utf8(entry.path().filename());
        if(entry.is_directory(ec) && !entry.is_symlink(ec) && !name.empty() && name[0]!='.' && name.size()<=128)result.push_back(name);
    }
    std::sort(result.begin(),result.end());return result;
}
}
std::vector<std::string> alwaysEnabledPacks(const std::filesystem::path& root,ModKind kind) {
    return listPackDirs(modAlwaysDirectory(root,kind));
}
std::vector<std::string> modPacks(const std::filesystem::path& root,ModKind kind) {
    auto result=listPackDirs(modDirectory(root,kind));if(result.empty())return result;
    // always_enabled/ is not a pack, and an always-enabled pack hides an ordinary
    // pack of the same name (ignoring case): its copy is the one that loads.
    std::set<std::string> always;
    for(const auto& name:alwaysEnabledPacks(root,kind))always.insert(MountSet::key(name));
    std::erase_if(result,[&](const std::string& name){
        const auto k=MountSet::key(name);return k==kAlwaysEnabledDir || always.count(k);});
    return result;
}
namespace {
// Feature definitions contain simulation keys; cosmetic files must never replace
// them, even though their art fields also affect presentation. AI profiles are
// game-mod business too: ai/, which holds retail's profiles and our levels'
// ai/<x>.txt files (ai/builtinprofile.h).
bool cosmeticKeep(const std::string& p) {
    const auto k=MountSet::key(p);
    return !affectsGameplay(p) && !k.starts_with("features/") && !k.starts_with("ai/");
}
// Copy every winning path of `mount` into `files` (later copies win), within the
// per-layer budget every mod layer shares.
void collect(const MountSet& mount,Vfs::Files& files,size_t& total) {
    for(const auto& path:mount.list("")) {
        auto bytes=mount.read(path);total+=bytes.size();
        if(total>(256u<<20) || files.size()>=65536)throw std::runtime_error("mod selection too large");
        files[MountSet::key(path)]=std::move(bytes);
    }
}
}
Vfs::Files modFiles(const std::filesystem::path& root,ModKind kind,const std::vector<std::string>& selected) {
    Vfs::Files files;if(selected.empty())return files;
    if(selected.size()>64)throw std::runtime_error("too many mod packs");
    auto names=selected;std::sort(names.begin(),names.end());
    if(std::adjacent_find(names.begin(),names.end())!=names.end())throw std::runtime_error("duplicate mod pack");
    // Each name is an always-enabled pack or an ordinary one (never both: an
    // always-enabled pack hides its namesake). Always-enabled layers go first,
    // then the ordinary ones, each alphabetical; later layers win.
    const auto always=alwaysEnabledPacks(root,kind),available=modPacks(root,kind);
    std::vector<std::filesystem::path> paths,later;
    const auto alwaysDir=modAlwaysDirectory(root,kind),dir=modDirectory(root,kind);
    for(const auto& name:names) {
        if(std::binary_search(always.begin(),always.end(),name))paths.push_back(alwaysDir/ta::u8path(name));
        else if(std::binary_search(available.begin(),available.end(),name))later.push_back(dir/ta::u8path(name));
        else throw std::runtime_error("mod pack not found: "+name);
    }
    paths.insert(paths.end(),later.begin(),later.end());
    size_t total=0;
    for(const auto& path:paths) {
        MountConfig cfg{true,{".hpi",".ufo",".kmp"},{},{}};
        if(kind==ModKind::Cosmetic)cfg.keep=cosmeticKeep;
        collect(MountSet(path,std::move(cfg)),files,total);
    }
    return files;
}
Vfs mountPlayVfs(const std::filesystem::path& root,Vfs::Files gameFiles,std::vector<std::string> cosmeticPacks,
                 bool withCosmetic,std::vector<std::string>* skipped) {
    Vfs vfs=mountRetailRoot(root);
    auto base=std::make_shared<const Vfs::Files>(std::move(gameFiles));
    vfs.setModFiles(base);
    if(!withCosmetic)return vfs;
    struct Layer {std::string name;Vfs::Files files;bool anims=false;};
    std::vector<Layer> layers;
    auto skip=[&](const std::string& name,const std::string& why){if(skipped)skipped->push_back(name+" ("+why+")");};
    auto add=[&](const std::string& name,auto&& load) {
        try {Layer layer{name,load(),false};
            for(const auto& [path,bytes]:layer.files)if(path.starts_with("anims/")){layer.anims=true;break;}
            layers.push_back(std::move(layer));
        }catch(const std::exception& e){skip(name,e.what());}
    };
    // The always-enabled cosmetic packs first (every game, campaigns included),
    // then the ticked ones; each alphabetical, later layers win.
    auto always=alwaysEnabledPacks(root,ModKind::Cosmetic);
    if(always.size()>64)always.resize(64);
    for(const auto& name:always)add(name,[&]{return modFiles(root,ModKind::Cosmetic,{name});});
    // Remember unavailable selections, but a removed cosmetic pack must not stop a
    // game. A tick an always-enabled pack hides is not in modPacks either.
    const auto available=modPacks(root,ModKind::Cosmetic);
    std::sort(cosmeticPacks.begin(),cosmeticPacks.end());
    cosmeticPacks.erase(std::unique(cosmeticPacks.begin(),cosmeticPacks.end()),cosmeticPacks.end());
    std::erase_if(cosmeticPacks,[&](const auto& name){return !std::binary_search(available.begin(),available.end(),name);});
    if(cosmeticPacks.size()>64)cosmeticPacks.resize(64);
    for(const auto& name:cosmeticPacks)add(name,[&]{return modFiles(root,ModKind::Cosmetic,{name});});
    // The cosmetic layers together share one budget; past it the later ones drop.
    {   size_t total=0,count=0;
        std::erase_if(layers,[&](const Layer& layer){
            size_t bytes=0;for(const auto& [path,data]:layer.files)bytes+=data.size();
            if(total+bytes>(256u<<20) || count+layer.files.size()>65536){skip(layer.name,"mod selection too large");return true;}
            total+=bytes;count+=layer.files.size();return false;});
    }
    auto merged=[&](const std::vector<bool>& use) {
        auto files=std::make_shared<Vfs::Files>(*base);
        for(size_t i=0;i<layers.size();++i)if(use[i])for(const auto& [path,bytes]:layers[i].files)(*files)[path]=bytes;
        return files;
    };
    // The cosmetic filter keeps out every file the sim reads EXCEPT anims/ art,
    // whose frame timing and availability gameplayHash (and the sim) read too.
    // Only a layer carrying anims/ can change the hash: check those against the
    // room's data, and drop each one that changes it.
    std::vector<bool> keep(layers.size(),true);
    if(std::any_of(layers.begin(),layers.end(),[](const Layer& l){return l.anims;})) {
        const uint64_t want=gameplayHash(vfs);
        auto hashWith=[&](const std::vector<bool>& use){vfs.setModFiles(merged(use));return gameplayHash(vfs);};
        auto drop=[&](size_t i){keep[i]=false;skip(layers[i].name,"its anims/ art changes gameplay timing");};
        if(hashWith(keep)!=want) {
            for(size_t i=0;i<layers.size();++i)if(layers[i].anims) {
                std::vector<bool> only(layers.size(),false);only[i]=true;
                if(hashWith(only)!=want)drop(i);
            }
            // Two layers that are each harmless can still combine into a change.
            if(hashWith(keep)!=want)for(size_t i=0;i<layers.size();++i)if(keep[i] && layers[i].anims)drop(i);
        }
    }
    vfs.setModFiles(merged(keep));
    return vfs;
}

Vfs mountRetailRoot(const std::filesystem::path& root, const std::vector<std::string>& gamePacks, const std::vector<std::string>& cosmeticPacks) {
    namespace fs = std::filesystem;
    Vfs vfs;
    // Case-fold the retail subdir names (they ship capitalised: Maps/ Music/
    // Movies/), so this resolves on a case-sensitive filesystem too.
    auto findSub = [&](const char* want) -> fs::path {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(root, ec)) {
            if (ec) break;
            if (!e.is_directory(ec)) continue;
            std::string n = utf8(e.path().filename());
            std::string l = n;
            for (char& c : l) c = char(std::tolower(static_cast<unsigned char>(c)));
            std::string w = want;
            for (char& c : w) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (l == w) return e.path();
        }
        return {};
    };

    // Lowest precedence: loose music tracks, mapped under music/.
    if (fs::path music = findSub("Music"); !music.empty())
        vfs.addLayer(MountSet(music, MountConfig{.includeLoose = true, .archiveExts = {}, .keep = {}, .archiveNames = {}}), "music/");
    // The base game + expansions: ONLY the canonical *.hpi archives in the root (no
    // loose files, and never a stray/unknown *.hpi), layered by the retail
    // newest-entry-date rule. maps.hpi and terrain.hpi ride in here too.
    {
        MountConfig cfg{.includeLoose = false, .archiveExts = kRootArchiveExts, .keep = {}, .archiveNames = {}};
        cfg.firstMountWins = true;
        cfg.archiveFilter = rootArchiveWanted;
        MountSet ms(root, std::move(cfg));
        vfs.addLayer(std::move(ms));
    }
    // Single-map .kmp archives (each an HPI -> kmap/<name>.*) plus any loose maps.
    // A handful of community .kmp bundle MODDED gameplay data (their own canbuild/
    // units/gamedata); retail reads a .kmp only for its map, so we expose just the
    // map (kmap/... and cosmetic tiles the map ships) and drop any bundled gameplay
    // data -- otherwise one modded map would rewrite every game's unit roster.
    auto addMaps = [&](const fs::path& maps) {
        MountConfig cfg{true, {".kmp", ".hpi", ".ufo"}, {}, {}};
        cfg.keep = [](const std::string& p) {
            return MountSet::key(p).rfind("kmap/", 0) == 0 || !affectsGameplay(p);
        };
        vfs.addLayer(MountSet(maps, std::move(cfg)), "", true);
    };
    // Saved generated maps in the per-user folder, under the install's own maps.
    const fs::path user = userMapRoot();
    if (std::error_code ec; !user.empty() && fs::is_directory(user / "Maps", ec)) addMaps(user / "Maps");
    if (fs::path maps = findSub("Maps"); !maps.empty()) addMaps(maps);
    if (!gamePacks.empty() || !cosmeticPacks.empty()) {
        auto files = modFiles(root, ModKind::Game, gamePacks);
        for (auto& [path, bytes] : modFiles(root, ModKind::Cosmetic, cosmeticPacks)) files[path] = std::move(bytes);
        vfs.setModFiles(std::make_shared<const Vfs::Files>(std::move(files)));
    }
    vfs.refreshMapCache(user);
    ta::sides::load(vfs);   // gamedata/sidedata.tdf drives every side table
    return vfs;
}

std::vector<std::pair<std::string, std::string>> listMaps(const Vfs& vfs) {
    std::unordered_map<std::string, std::string> byName;   // lower(name) -> path
    for (const char* ns : {"Maps", "kmap"})
        for (const std::string& p : vfs.list(ns)) {
            std::string ext = ta::vpath::extension(p);
            for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".tnt") continue;
            std::string name = ta::vpath::stem(p);
            std::string lname = name;
            for (char& c : lname) c = char(std::tolower(static_cast<unsigned char>(c)));
            byName.emplace(lname, p);   // first namespace (Maps) wins a name tie
        }
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(byName.size());
    for (const auto& [ln, path] : byName)
        out.push_back({ta::vpath::stem(path), path});
    std::sort(out.begin(), out.end());
    return out;
}

std::string findMap(const Vfs& vfs, const std::string& name) {
    // A "~gen1~" random-map id carries its recipe, not a file -- resolve it to itself
    // so every call site (client, server referee, MP lobby) generates in memory.
    if (ta::mapgen::isGeneratedMapId(name)) return name;
    std::string want = name;
    // Accept either a bare name or a name with a .tnt suffix.
    if (want.size() > 4) {
        std::string tail = want.substr(want.size() - 4);
        for (char& c : tail) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (tail == ".tnt") want = want.substr(0, want.size() - 4);
    }
    for (char& c : want) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (const char* ns : {"Maps", "kmap"})
        for (const std::string& p : vfs.list(ns)) {
            std::string ext = ta::vpath::extension(p);
            for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".tnt") continue;
            std::string stem = ta::vpath::stem(p);
            for (char& c : stem) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (stem == want) return p;
        }
    return {};
}

} // namespace ta::hpi
