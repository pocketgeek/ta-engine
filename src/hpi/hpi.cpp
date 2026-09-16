#include "hpi/hpi.h"
#include "tdf/tdf.h"
#include "tnt/mapgen.h"

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
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ta::hpi {

namespace {

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

// A compressed v1 file is cut into chunks of this many DECOMPRESSED bytes, and
// the chunk count is derived from the file size rather than stored -- so it has
// to match the writer's exactly.
constexpr uint32_t kV1ChunkSize = 65536;

// --- v1 obfuscation ----------------------------------------------------------
//
// Classic TA masks the WHOLE archive past the header, byte by byte, against each
// byte's own absolute file offset. (v2 dropped this; its only obfuscation is the
// per-SQSH-chunk `encrypted` flag, which v1 also has and which is unrelated.)
// `headerKey` == 0 means the archive was written in the clear -- a few community
// tools do that, and retail reads them.
uint8_t v1Key(uint32_t headerKey) {
    if (headerKey == 0) return 0;
    return uint8_t(~((headerKey * 4) | (headerKey >> 6)));
}

// Copy [off, off+len) out of `data`, undoing the v1 mask. A copy rather than a
// view because the mask is offset-dependent: there is nothing to decode in
// place when the archive is a shared read-only mapping.
std::vector<uint8_t> deobfuscate(const uint8_t* data, size_t size, size_t off,
                                 size_t len, uint8_t key) {
    if (off + len > size) throw std::runtime_error("read overruns archive");
    std::vector<uint8_t> out(data + off, data + off + len);
    if (key)
        for (size_t i = 0; i < len; ++i)
            out[i] = uint8_t(uint8_t((off + i) ^ key) ^ uint8_t(~out[i]));
    return out;
}

// LZ77 with a 4096-byte ring window -- SQSH compression method 1, which v2 never
// uses (everything there is method 2, zlib) but classic TA uses throughout.
//
// The stream is a sequence of 8-item groups preceded by a tag byte whose bits
// say, LSB first, whether each item is a literal or a (offset, length) back
// reference. A back reference is a little-endian u16: the high 12 bits are the
// window position, the low 4 the length less 2. A window position of 0
// terminates the stream -- which is why the window is seeded at index 1.
std::vector<uint8_t> lz77Decompress(const uint8_t* in, size_t inSize, uint32_t outSize) {
    std::vector<uint8_t> out;
    out.reserve(outSize);
    uint8_t window[4096] = {};
    size_t inPos = 0;
    uint32_t windowPos = 1;

    auto take = [&](size_t n) {
        if (inPos + n > inSize) throw std::runtime_error("LZ77 stream overruns chunk");
    };

    // The tag is refilled at the TOP of the loop, not the bottom, so a stream
    // whose last item lands exactly on a group boundary is not required to carry
    // a ninth tag byte that nothing would read. Refilling afterwards made that
    // shape -- legal, and produced by any file whose length is a multiple of the
    // group size -- throw "stream overruns chunk" on a perfectly good archive.
    uint8_t tag = 0;
    int tagBit = 0x100;

    while (out.size() < outSize) {
        if (tagBit == 0x100) {
            take(1);
            tag = in[inPos++];
            tagBit = 1;
        }
        if (tag & tagBit) {
            take(2);
            uint32_t ref = uint32_t(in[inPos]) | (uint32_t(in[inPos + 1]) << 8);
            inPos += 2;
            uint32_t from = ref >> 4;
            if (from == 0) break;            // end-of-stream marker
            uint32_t count = (ref & 0x0f) + 2;
            for (uint32_t i = 0; i < count && out.size() < outSize; ++i) {
                uint8_t b = window[from];
                out.push_back(b);
                window[windowPos] = b;
                from = (from + 1) & 0xfff;
                windowPos = (windowPos + 1) & 0xfff;
            }
        } else {
            take(1);
            uint8_t b = in[inPos++];
            out.push_back(b);
            window[windowPos] = b;
            windowPos = (windowPos + 1) & 0xfff;
        }

        tagBit <<= 1;                        // 0x100 => group exhausted, refill above
    }
    if (out.size() != outSize)
        throw std::runtime_error("LZ77 produced " + std::to_string(out.size()) +
                                 " bytes, expected " + std::to_string(outSize));
    return out;
}

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
    if (method == 1) return lz77Decompress(payload.data(), payload.size(), decompSize);
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

// Walk a v1 directory record. Unlike v2 -- where names and entries live in two
// flat blocks and a subdirectory is a slice of the same array -- v1 offsets are
// absolute into the (deobfuscated) directory region and point anywhere, so this
// recurses on offsets rather than indices. `dir` is that region, decoded from
// file offset 0, which makes an absolute file offset also an index into it.
void walkV1(const std::vector<uint8_t>& dir, uint32_t recordOff,
            const std::string& prefix, std::vector<Entry>& out, int depth) {
    // A malformed or hostile archive can point a subdirectory at its own parent.
    // Retail would loop forever; bail instead. Real trees are a handful deep.
    if (depth > 64) throw std::runtime_error("directory nested too deep (cycle?)");
    if (uint64_t(recordOff) + 8 > dir.size())
        throw std::runtime_error("dir record out of range");
    uint32_t count = u32(&dir[recordOff]);
    uint32_t entries = u32(&dir[recordOff + 4]);

    for (uint32_t i = 0; i < count; ++i) {
        uint64_t e = uint64_t(entries) + uint64_t(i) * 9;
        if (e + 9 > dir.size()) throw std::runtime_error("dir entry out of range");
        uint32_t namePtr = u32(&dir[e]);
        uint32_t dataPtr = u32(&dir[e + 4]);
        bool isDir = dir[e + 8] != 0;

        std::string name = nameAt(dir, namePtr);
        std::string path = prefix.empty() ? name : prefix + "/" + name;

        if (isDir) {
            out.push_back({.path = path, .isDirectory = true});
            walkV1(dir, dataPtr, path, out, depth + 1);
            continue;
        }
        if (uint64_t(dataPtr) + 9 > dir.size())
            throw std::runtime_error("file record out of range");
        Entry f;
        f.path = path;
        f.start = u32(&dir[dataPtr]);
        f.decompressedSize = u32(&dir[dataPtr + 4]);
        f.compression = dir[dataPtr + 8];
        // v2 uses compressedSize==0 to mean "stored"; keep that invariant so the
        // Vfs and the tools do not have to know which version they are holding.
        f.compressedSize = f.compression ? 1 : 0;
        // v1 file records carry no timestamp -- the record is offset, size,
        // compression and nothing else. So EVERY v1 entry's date is 0 and every
        // collision between two of them is a tie, which means for TA (v1 only)
        // the date rule never decides anything and mount order decides all of
        // it. MountSet therefore breaks a tie in favour of the LATER mount, so
        // the expansions and the 3.1 patch override the base game rather than
        // the other way round; see the tie comment there for the measurement.
        f.date = 0;
        out.push_back(std::move(f));
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
        key_ = v1Key(header_.headerKey);
        // The v1 directory is not a block to seek to -- names and records are
        // scattered at absolute offsets anywhere below dirSize. So decode that
        // whole region once (tens of KB against archives of tens of MB) and walk
        // it with file offsets used directly as indices.
        uint32_t dirSize = header_.dirSize;
        if (dirSize < 20 || dirSize > header_.fileSize)
            throw std::runtime_error(file.string() + ": bad v1 directory size");
        std::vector<uint8_t> raw(dirSize);
        std::ifstream in(file_, std::ios::binary);
        if (!in) throw std::runtime_error("cannot open " + file_.string());
        in.read(reinterpret_cast<char*>(raw.data()), std::streamsize(dirSize));
        if (!in) throw std::runtime_error(file.string() + ": short read on v1 directory");
        auto dir = deobfuscate(raw.data(), raw.size(), 0, dirSize, key_);
        walkV1(dir, header_.dataStart, "", entries_, 0);
        return;
    }
    if (header_.version != 0x00020000)
        throw std::runtime_error(file.string() + ": unsupported HPI version " +
                                 std::to_string(header_.version));

    std::ifstream in(file_, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + file_.string());
    auto dirBlock = readBlockAt(in, header_.dirBlock, header_.dirSize);
    auto nameBlock = readBlockAt(in, header_.nameBlock, header_.nameSize);
    walk(dirBlock, nameBlock, 0, "", entries_);
}

bool Archive::Mapping::open(const std::filesystem::path& p) {
#ifdef _WIN32
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
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

Archive::View Archive::bytes() const {
    if (!loaded_) {
        // Map the archive read-only; fall back to an eager read if mapping fails
        // (exotic filesystems). Either way the view is stable for our lifetime.
        if (!map_.open(file_)) fileData_ = readFile(file_);
        loaded_ = true;
    }
    if (map_.data()) return {map_.data(), map_.size()};
    return {fileData_.data(), fileData_.size()};
}

std::vector<uint8_t> Archive::read(const Entry& entry) const {
    if (entry.isDirectory) throw std::runtime_error(entry.path + " is a directory");
    View data = bytes();

    if (header_.version == 0x00010000) {
        if (entry.compression == 0)   // stored: masked, but not chunked
            return deobfuscate(data.data, data.size, entry.start,
                               entry.decompressedSize, key_);

        // Compressed: a u32 compressed-size per chunk, then the chunks. The
        // count is derived, not stored -- ceil(size / 64K) -- so a writer that
        // chunked differently would desync the table from the data.
        uint32_t chunks = (entry.decompressedSize + kV1ChunkSize - 1) / kV1ChunkSize;
        auto table = deobfuscate(data.data, data.size, entry.start,
                                 size_t(chunks) * 4, key_);
        std::vector<uint8_t> out;
        out.reserve(entry.decompressedSize);
        size_t pos = size_t(entry.start) + size_t(chunks) * 4;
        for (uint32_t i = 0; i < chunks; ++i) {
            uint32_t packed = u32(&table[i * 4]);
            // `packed` covers the 19-byte SQSH header too. Decode the chunk into
            // a plain buffer and hand it to the shared reader, which then sees
            // the same bytes a v2 archive would have stored unmasked.
            auto plain = deobfuscate(data.data, data.size, pos, packed, key_);
            size_t at = 0;
            auto part = readChunk(plain.data(), plain.size(), at);
            out.insert(out.end(), part.begin(), part.end());
            pos += packed;
        }
        if (out.size() != entry.decompressedSize)
            throw std::runtime_error(entry.path + ": decompressed size mismatch");
        return out;
    }

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
    if (info.version == 0x00010000) {
        // v1's three words are a different shape entirely -- reading them as a
        // v2 header is how a TA archive used to come back as nonsense rather
        // than as an unsupported version.
        info.dirSize = u32(raw + 8);
        info.headerKey = u32(raw + 12);
        info.dataStart = u32(raw + 16);
        return info;
    }
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
        std::string x = a.filename().string(), y = b.filename().string();
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
                std::string ext = e.path().extension().string();
                for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
                if (ext != want) continue;
                if (!cfg_.archiveNames.empty()) {   // name whitelist (retail root)
                    std::string fn = e.path().filename().string();
                    for (char& c : fn) c = char(std::tolower(static_cast<unsigned char>(c)));
                    if (std::find(cfg_.archiveNames.begin(), cfg_.archiveNames.end(), fn)
                        == cfg_.archiveNames.end()) continue;
                }
                group.push_back(e.path());
            }
        std::sort(group.begin(), group.end(), ci);
        archiveFiles_.insert(archiveFiles_.end(), group.begin(), group.end());
    }

    // Mount each; resolve conflicts by newest entry date, and on a TIE let the
    // LATER-mounted archive win.
    //
    // The tie half is what matters for TA, because TA has no dates at all: it
    // ships HPI **v1**, whose 9-byte file record is offset/size/compression and
    // carries no timestamp (see walkV1) -- so every TA entry's date is 0 and
    // every comparison between two of them is a tie. With the strict '>' this
    // used to be, a tie kept the FIRST mount, and the first group is `.hpi`:
    // the base game therefore beat Core Contingency, Battle Tactics AND the 3.1
    // patch on every path all four ship. Measured on a Commander Pack install,
    // that silently shadowed 1082 files whose expansion/patch copy DIFFERS from
    // the base -- 471 unit FBIs, 329 anims, 105 guis, 24 scripts, 15 gamedata,
    // 15 weapons among them.
    //
    // The shipped data settles which way is right on its own: base totala1.hpi
    // declares `features/lava/VENTS.TDF` [Lavavent003] with `seqname=tvent03`,
    // a sequence that does not exist in anims/lavastuff2.gaf (it has tvent001..
    // tvent010). ccdata.ccx, btdata.ccx and rev31.gp3 all carry the same file
    // with `seqname=tvent003`, which does exist. Retail draws that vent, so
    // retail cannot be resolving this in the base's favour -- and a patch that
    // lost every conflict to the thing it patches would do nothing at all.
    //
    // Flipping the tie is safe inside the base group: across all thirteen root
    // .hpi archives exactly ONE path collides with differing content, and it is
    // `installres/install.inf` -- an installer leftover, not game data.
    //
    // For an archive that DOES carry dates (HPI v2), the date still dominates;
    // only the tie changed.
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
            if (it == map_.end() || e.date >= it->second.entry.date)
                map_[k] = Win{idx, e};
        }
    }
}

bool MountSet::has(const std::string& path) const {
    if (cfg_.keep && !cfg_.keep(path)) return false;
    if (cfg_.includeLoose && std::filesystem::is_regular_file(dir_ / path)) return true;
    return map_.count(key(path)) != 0;
}

std::vector<uint8_t> MountSet::read(const std::string& path) const {
    if (cfg_.keep && !cfg_.keep(path)) throw std::runtime_error("filtered: " + path);
    // 1. A loose file on disk overrides archives (the engine fopen()s first).
    if (cfg_.includeLoose) {
        std::filesystem::path loose = dir_ / path;
        if (std::filesystem::is_regular_file(loose)) {
            std::ifstream f(loose, std::ios::binary);
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
        fs::path base = kp.empty() ? dir_ : dir_ / prefix;
        std::error_code ec;
        if (fs::is_directory(base, ec))
            for (auto it = fs::recursive_directory_iterator(base, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                std::string rel = fs::relative(it->path(), dir_, ec).generic_string();
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
        std::filesystem::path loose = dir_ / path;
        if (std::filesystem::is_regular_file(loose)) return loose.string() + " (loose)";
    }
    auto it = map_.find(key(path));
    if (it == map_.end()) return "<absent>";
    return archiveFiles_[size_t(it->second.archive)].filename().string() +
           "!" + it->second.entry.path;
}

// ---- Vfs: layered runtime read-path over a retail install root -------------

void Vfs::addLayer(MountSet ms, const std::string& prefix) {
    std::string p = MountSet::key(prefix);
    if (!p.empty() && p.back() != '/') p += '/';
    layers_.push_back({std::move(ms), std::move(p)});
}

// Try each layer highest-precedence-first; if the requested path is under a
// layer's virtual prefix, strip the prefix and delegate to that MountSet.
std::optional<std::vector<uint8_t>> Vfs::tryRead(const std::string& path) const {
    std::string kp = MountSet::key(path);
    for (auto it = layers_.rbegin(); it != layers_.rend(); ++it) {
        if (!it->prefix.empty() && kp.compare(0, it->prefix.size(), it->prefix) != 0) continue;
        std::string sub = path.substr(it->prefix.size());
        if (it->ms.has(sub)) return it->ms.read(sub);
    }
    return std::nullopt;
}

std::vector<uint8_t> Vfs::read(const std::string& path) const {
    if (auto b = tryRead(path)) return std::move(*b);
    throw std::runtime_error("not in data set: " + path);
}

bool Vfs::has(const std::string& path) const {
    std::string kp = MountSet::key(path);
    for (auto it = layers_.rbegin(); it != layers_.rend(); ++it) {
        if (!it->prefix.empty() && kp.compare(0, it->prefix.size(), it->prefix) != 0) continue;
        if (it->ms.has(path.substr(it->prefix.size()))) return true;
    }
    return false;
}

std::vector<std::string> Vfs::list(const std::string& prefix) const {
    std::string kp = MountSet::key(prefix);
    std::unordered_map<std::string, std::string> out;   // key -> winning original path
    // Lowest precedence first, so higher layers overwrite the reported source.
    for (const auto& L : layers_) {
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
    // build lists, map geometry + scenario data. Everything else (art, models,
    // animation scripts, sound, music, fonts, gui) is cosmetic.
    // Build-tree markers (canbuild/canbuildcb/<builder>/<buildable>.tdf) define who
    // builds what -- gameplay, and .tdf, so check them BEFORE the .tdf rule below.
    if (k.find("canbuild") != std::string::npos) return true;
    if (ext(".fbi") || ext(".tnt") || ext(".ota") || ext(".crt")) return true;
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
        // form (below), so an art-only override hashes identically while a
        // sim-key edit still faults the version gate instead of desyncing.
        if (k.find("weapons/") != std::string::npos) return true;
        // SIDEDATA is gameplay: it names each side's commander and the whole
        // build tree, so an edit changes what every player can make.
        if (k.find("moveinfo") != std::string::npos || k.find("sidedata") != std::string::npos)
            return true;
        return false;
    }
    return false;
}

uint64_t gameplayHash(const Vfs& vfs) {
    // Every gameplay entry the sim consumes (see affectsGameplay), EXCEPT maps --
    // those are per-game, checked separately. Two installs with identical retail
    // gameplay files hash the same; a modified/overridden gameplay file changes it.
    std::unordered_map<std::string, std::string> byKey;   // key -> winning path (deduped)
    for (const char* pre : {"units", "unitscb", "canbuild", "canbuildcb", "features",
                            "gamedata", "weapons"})
        for (const std::string& p : vfs.list(pre)) {
            if (!affectsGameplay(p)) continue;
            std::string k = MountSet::key(p);
            if (k.size() >= 4) {   // exclude any map files that live under these prefixes
                std::string e = k.substr(k.size() - 4);
                if (e == ".tnt" || e == ".ota" || e == ".crt") continue;
            }
            byKey.emplace(k, p);
        }
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
    // Feature TDFs: hash only the keys the deterministic sim consumes (the
    // exact set matchsetup's FeatDef loader reads), in canonical sorted order.
    // Art keys (filename/seqname/object/world/...) are deliberately absent, so
    // a cosmetic reskin that only redirects art hashes the same as retail --
    // while an override touching footprints, reclaim yield, or the burn chain
    // changes the hash and faults the MP version gate cleanly.
    {
        auto mixs = [&](const std::string& v) {
            mix(reinterpret_cast<const uint8_t*>(v.data()), v.size());
            uint8_t z = 0; mix(&z, 1);
        };
        static const char* kSimKeys[] = {
            "animating", "blocking", "category", "damage", "decomposetime",
            "energy", "featureburnt", "featuredead", "flamable", "footprintx",
            "footprintz", "indestructible", "isfrozen", "isstone",
            "reclaimable", "resurrectable", "sparktime", "spreadchance",
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
            }
        }
    }
    return h;
}

// TA spreads its data across four extensions of the SAME v1 container, and the
// extension is how the retail engine ranks them -- later groups override earlier
// ones. Base game in .hpi, Battle Tactics / Core Contingency content in .ccx,
// the 3.1 patch in .gp3, and user mods in .ufo.
//
// Kingdoms needed a name whitelist here because its install root collects stray
// archives; TA's does not, and its expansions ship files whose names we would
// only be guessing at. So this mounts BY EXTENSION, which is also what retail
// does -- no whitelist, and a new official pack drops in and works.
//
// NOTE: the relative rank of .ccx and .gp3 is asserted from the community
// modding record, not yet confirmed against the retail binary. Its reach is
// bounded and small, which is measured rather than hoped: btdata.ccx and
// ccdata.ccx disagree on 112 paths, but rev31.gp3 (the 3.1 patch, mounted last
// either way) ships 87 of them and so decides them whatever the .ccx order is.
// The 25 left over are ALL in anims/ -- cosmetic GAF art, no gameplay data. So
// on a full Commander Pack install this ordering cannot change what the sim
// computes. See docs/ta-port.md.
const std::vector<std::string> kRootArchiveExts = {".hpi", ".ccx", ".gp3", ".ufo"};

// Lowercased filenames present in the install root (regular files only).
static std::set<std::string> rootFileNames(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    std::set<std::string> have;
    std::error_code ec;
    if (fs::is_directory(root, ec))
        for (const auto& e : fs::directory_iterator(root, ec)) {
            if (ec) break;
            if (!e.is_regular_file(ec)) continue;
            std::string n = e.path().filename().string();
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
    // The two base archives. Everything else -- the expansions, the patch, mods --
    // is optional, so a plain TA install validates as readily as a Commander Pack.
    static const char* kEssential[] = {"totala1.hpi", "totala2.hpi"};
    std::set<std::string> have = rootFileNames(root);
    for (const char* need : kEssential)
        if (!have.count(need)) { if (reason) *reason = std::string("missing ") + need; return false; }
    // Confirm the archives actually mount and a core gameplay file resolves.
    try {
        Vfs vfs = mountRetailRoot(root, OverridePolicy::None);
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
            std::string l = e.path().filename().string();
            for (char& c : l) c = char(std::tolower(static_cast<unsigned char>(c)));
            std::string ext = fs::path(l).extension().string();
            if (std::find(kRootArchiveExts.begin(), kRootArchiveExts.end(), ext)
                == kRootArchiveExts.end()) continue;
            present[l] = fs::file_size(e.path(), ec);
        }
    uint64_t h = 1469598103934665603ull;   // FNV-1a over "name:size;" pairs
    auto mix = [&](const std::string& s) { for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; } };
    for (const auto& [n, sz] : present) { mix(n); mix(":"); mix(std::to_string(sz)); mix(";"); }
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

Vfs mountRetailRoot(const std::filesystem::path& root, OverridePolicy overrides) {
    namespace fs = std::filesystem;
    Vfs vfs;
    // Case-fold the retail subdir names (they ship capitalised: Maps/ Music/
    // Movies/), so this resolves on a case-sensitive filesystem too.
    auto findSub = [&](const char* want) -> fs::path {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(root, ec)) {
            if (ec) break;
            if (!e.is_directory(ec)) continue;
            std::string n = e.path().filename().string();
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
    vfs.addLayer(MountSet(root, MountConfig{.includeLoose = false,
                                            .archiveExts = kRootArchiveExts,
                                            .keep = {}, .archiveNames = {}}));
    // Single-map .kmp archives (each an HPI -> kmap/<name>.*) plus any loose maps.
    // A handful of community .kmp bundle MODDED gameplay data (their own canbuild/
    // units/gamedata); retail reads a .kmp only for its map, so we expose just the
    // map (kmap/... and cosmetic tiles the map ships) and drop any bundled gameplay
    // data -- otherwise one modded map would rewrite every game's unit roster.
    if (fs::path maps = findSub("Maps"); !maps.empty()) {
        MountConfig cfg{true, {".kmp", ".hpi", ".ufo"}, {}, {}};
        cfg.keep = [](const std::string& p) {
            return MountSet::key(p).rfind("kmap/", 0) == 0 || !affectsGameplay(p);
        };
        vfs.addLayer(MountSet(maps, std::move(cfg)));
    }
    // Highest precedence: user overrides (loose files OR archives), filtered by
    // the multiplayer policy. Cosmetic drops any gameplay-affecting override so
    // it cannot diverge between peers; Full mounts everything.
    if (overrides != OverridePolicy::None) {
        if (fs::path ov = findSub("overrides"); !ov.empty()) {
            MountConfig cfg{true, {".hpi", ".ufo", ".kmp"}, {}, {}};
            if (overrides == OverridePolicy::Cosmetic)
                cfg.keep = [](const std::string& p) { return !affectsGameplay(p); };
            vfs.addLayer(MountSet(ov, std::move(cfg)), "");
        }
    }
    return vfs;
}

std::vector<std::pair<std::string, std::string>> listMaps(const Vfs& vfs) {
    std::unordered_map<std::string, std::string> byName;   // lower(name) -> path
    for (const char* ns : {"Maps", "kmap"})
        for (const std::string& p : vfs.list(ns)) {
            std::filesystem::path fp(p);
            std::string ext = fp.extension().string();
            for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".tnt") continue;
            std::string name = fp.stem().string();
            std::string lname = name;
            for (char& c : lname) c = char(std::tolower(static_cast<unsigned char>(c)));
            byName.emplace(lname, p);   // first namespace (Maps) wins a name tie
        }
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(byName.size());
    for (const auto& [ln, path] : byName)
        out.push_back({std::filesystem::path(path).stem().string(), path});
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
            std::filesystem::path fp(p);
            std::string ext = fp.extension().string();
            for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".tnt") continue;
            std::string stem = fp.stem().string();
            for (char& c : stem) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (stem == want) return p;
        }
    return {};
}

} // namespace ta::hpi
