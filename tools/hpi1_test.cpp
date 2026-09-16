// hpi1_test -- the HPI version-1 reader: classic Total Annihilation's archive
// format, and the gateway to every other asset in the game.
//
// Retail TA archives cannot live in this repo or on a CI runner, so this builds
// its OWN v1 archives in a temp dir and reads them back. Be clear-eyed about
// what that does and does not prove: a round trip against a writer that shares
// the reader's understanding of the layout locks in BEHAVIOUR and catches
// regressions, but it cannot by itself prove the layout matches Cavedog's. The
// independent half of that check is the LZ77 section below, whose streams are
// hand-computed from the format definition rather than produced by our own
// code, plus the real-archive check that runs locally once an install is
// present (see docs/ta-port.md milestone 2).
//
// The cases that matter, and why each is here:
//   * masked AND unmasked archives -- `headerKey == 0` is legal and a few
//     community packers emit it, so the mask must be conditional, not assumed.
//   * stored, zlib and LZ77 files -- one code path each in readChunk.
//   * a file over 64 KB -- v1 derives its chunk count from the file size
//     instead of storing it, so an off-by-one there only shows up above the
//     chunk boundary.
//   * nested directories -- v1 offsets are absolute and point anywhere, unlike
//     v2's flat blocks, so the walk recurses on offsets.
//   * a directory cycle -- a malformed archive must raise, not hang.

#include "hpi/hpi.h"

#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

static int g_fail = 0;
static void check(bool cond, const char* what) {
    std::printf("  %-66s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ++g_fail;
}

// --- tiny byte helpers -------------------------------------------------------

static void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 24));
}
static void at32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    v[off] = uint8_t(x); v[off + 1] = uint8_t(x >> 8);
    v[off + 2] = uint8_t(x >> 16); v[off + 3] = uint8_t(x >> 24);
}

// =============================================================================
// Part 1: LZ77, checked against hand-built streams.
//
// The decompressor is private to hpi.cpp, so it is driven the only way it is
// reachable from outside: wrapped in a one-file, unmasked v1 archive whose
// single file is stored with compression method 1. That also exercises the
// method dispatch in readChunk, which is where a method-1 stream would
// otherwise be rejected outright.
//
// Stream format: a tag byte, then 8 items, LSB of the tag first. A clear bit is
// a literal byte; a set bit is a little-endian u16 whose high 12 bits are the
// ring-window position and whose low 4 are (length - 2). Window position 0 ends
// the stream. The window starts empty with its write cursor at 1 -- which is
// exactly why position 0 is free to be the terminator.
// =============================================================================

// A literal-only group: `n` bytes, tag bits all clear.
static void lzLiterals(std::vector<uint8_t>& s, const std::string& bytes) {
    s.push_back(0x00);                       // tag: 8 literals
    for (char c : bytes) s.push_back(uint8_t(c));
}
static void lzBackref(std::vector<uint8_t>& s, uint32_t from, uint32_t len) {
    uint16_t ref = uint16_t((from << 4) | ((len - 2) & 0x0f));
    s.push_back(uint8_t(ref)); s.push_back(uint8_t(ref >> 8));
}

// =============================================================================
// Part 2: a v1 archive writer, used to exercise the reader end to end.
// =============================================================================

struct InFile {
    std::string path;                 // '/'-separated, e.g. "units/arm/ARMCOM.fbi"
    std::vector<uint8_t> data;
    uint8_t compression = 0;          // 0 stored, 1 LZ77 (raw stream), 2 zlib
    std::vector<uint8_t> rawStream;   // compression==1: the literal SQSH payload
};

static constexpr uint32_t kChunk = 65536;

// Wrap `payload` in a SQSH chunk header. `decompSize` is what it expands to.
static std::vector<uint8_t> sqsh(const std::vector<uint8_t>& payload, uint8_t method,
                                 uint32_t decompSize) {
    std::vector<uint8_t> c;
    c.push_back('S'); c.push_back('Q'); c.push_back('S'); c.push_back('H');
    c.push_back(1);            // version
    c.push_back(method);
    c.push_back(0);            // not per-chunk encrypted
    put32(c, uint32_t(payload.size()));
    put32(c, decompSize);
    uint32_t sum = 0;
    for (uint8_t b : payload) sum += b;
    put32(c, sum);
    c.insert(c.end(), payload.begin(), payload.end());
    return c;
}

static std::vector<uint8_t> zlibDeflate(const uint8_t* p, size_t n) {
    uLongf cap = compressBound(uLong(n));
    std::vector<uint8_t> out(cap);
    if (compress(out.data(), &cap, p, uLong(n)) != Z_OK)
        throw std::runtime_error("deflate failed");
    out.resize(cap);
    return out;
}

// Build a complete v1 archive. `headerKey` of 0 writes it unmasked.
static std::vector<uint8_t> buildV1(const std::vector<InFile>& files, uint32_t headerKey) {
    // Group files by their directory so the tree can be emitted depth-first.
    struct Node {
        std::map<std::string, Node> subs;
        std::vector<const InFile*> files;
    };
    Node root;
    for (const auto& f : files) {
        Node* n = &root;
        size_t start = 0, slash;
        std::string leaf = f.path;
        while ((slash = f.path.find('/', start)) != std::string::npos) {
            n = &n->subs[f.path.substr(start, slash - start)];
            start = slash + 1;
        }
        leaf = f.path.substr(start);
        n->files.push_back(&f);
    }

    // The directory region is laid out first with placeholder data offsets, then
    // the file payloads are appended and the placeholders patched -- the same
    // two-pass shape the retail packer must have used, since a file's offset
    // cannot be known until the directory above it has been sized.
    std::vector<uint8_t> out;
    out.resize(20);                       // header, filled in at the end

    std::map<const InFile*, size_t> dataPtrFixups;   // file -> offset of its u32 data ptr

    auto emitName = [&](const std::string& s) {
        uint32_t off = uint32_t(out.size());
        out.insert(out.end(), s.begin(), s.end());
        out.push_back(0);
        return off;
    };

    // Emit a directory record ({count, entriesOffset}) and its entry array,
    // recursing depth-first. Returns the record's offset.
    std::function<uint32_t(const Node&, const std::string&)> emitDir =
        [&](const Node& n, const std::string& name) -> uint32_t {
        // Names first: an entry holds an absolute pointer to its name, so the
        // strings have to exist before the array that references them.
        std::vector<uint32_t> namePtrs;
        for (const auto& [sub, _] : n.subs) namePtrs.push_back(emitName(sub));
        for (const auto* f : n.files) {
            std::string leaf = f->path;
            size_t s = leaf.rfind('/');
            namePtrs.push_back(emitName(s == std::string::npos ? leaf : leaf.substr(s + 1)));
        }
        (void)name;

        uint32_t count = uint32_t(n.subs.size() + n.files.size());
        uint32_t recordOff = uint32_t(out.size());
        put32(out, count);
        size_t entriesPtrFixup = out.size();
        put32(out, 0);                     // entries offset, patched below

        // Each subdirectory's record has to be emitted before the entry array
        // that points at it (absolute offsets, no forward patching needed).
        std::vector<uint32_t> subRecords;
        for (const auto& [sub, child] : n.subs) subRecords.push_back(emitDir(child, sub));

        std::vector<uint32_t> fileRecords;
        for (const auto* f : n.files) {
            fileRecords.push_back(uint32_t(out.size()));
            dataPtrFixups[f] = out.size();
            put32(out, 0);                            // data offset, patched later
            put32(out, uint32_t(f->data.size()));
            out.push_back(f->compression);
        }

        uint32_t entriesOff = uint32_t(out.size());
        at32(out, entriesPtrFixup, entriesOff);
        size_t k = 0;
        for (size_t s = 0; s < subRecords.size(); ++s, ++k) {
            put32(out, namePtrs[k]); put32(out, subRecords[s]); out.push_back(1);
        }
        for (size_t fi = 0; fi < fileRecords.size(); ++fi, ++k) {
            put32(out, namePtrs[k]); put32(out, fileRecords[fi]); out.push_back(0);
        }
        return recordOff;
    };

    uint32_t rootRecord = emitDir(root, "");
    uint32_t dirSize = uint32_t(out.size());

    // Payloads.
    for (const auto& f : files) {
        at32(out, dataPtrFixups[&f], uint32_t(out.size()));
        if (f.compression == 0) {
            out.insert(out.end(), f.data.begin(), f.data.end());
            continue;
        }
        // Chunk table first (count derived from the decompressed size), then the
        // chunks. Sizes are only known after building each chunk, so reserve.
        uint32_t chunks = uint32_t((f.data.size() + kChunk - 1) / kChunk);
        size_t tableAt = out.size();
        for (uint32_t c = 0; c < chunks; ++c) put32(out, 0);
        for (uint32_t c = 0; c < chunks; ++c) {
            size_t beg = size_t(c) * kChunk;
            size_t len = std::min<size_t>(kChunk, f.data.size() - beg);
            std::vector<uint8_t> chunk;
            if (f.compression == 1)
                chunk = sqsh(f.rawStream, 1, uint32_t(len));
            else
                chunk = sqsh(zlibDeflate(f.data.data() + beg, len), 2, uint32_t(len));
            at32(out, tableAt + size_t(c) * 4, uint32_t(chunk.size()));
            out.insert(out.end(), chunk.begin(), chunk.end());
        }
    }

    // Header, then the mask over everything past it.
    std::memcpy(out.data(), "HAPI", 4);
    at32(out, 4, 0x00010000);
    at32(out, 8, dirSize);
    at32(out, 12, headerKey);
    at32(out, 16, rootRecord);

    if (headerKey != 0) {
        uint8_t key = uint8_t(~((headerKey * 4) | (headerKey >> 6)));
        // The mask covers the whole file INCLUDING the header bytes; retail
        // reads the header raw first and only then starts decoding, so offsets
        // 0..19 are masked too and simply never read through the mask.
        for (size_t i = 20; i < out.size(); ++i)
            out[i] = uint8_t(uint8_t(i ^ key) ^ uint8_t(~out[i]));
    }
    return out;
}

static std::filesystem::path writeTemp(const std::string& name,
                                       const std::vector<uint8_t>& bytes) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::ofstream o(p, std::ios::binary);
    o.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    o.close();
    return p;
}

static std::string textOf(const std::vector<uint8_t>& v) {
    return std::string(v.begin(), v.end());
}

int main() {
    std::printf("hpi1_test\n");

    // --- LZ77, from hand-built streams ---------------------------------------
    {
        // "ABCDEFGH" as eight literals, then a group whose first item is the
        // terminator. Window positions 1..8 now hold A..H.
        std::vector<uint8_t> s;
        lzLiterals(s, "ABCDEFGH");
        s.push_back(0x01);                    // tag: item 0 is a back reference
        lzBackref(s, 0, 2);                   // position 0 -> end of stream

        InFile f{"plain.txt", std::vector<uint8_t>(8, 0), 1, s};
        f.data.assign({'A','B','C','D','E','F','G','H'});
        auto path = writeTemp("ta_hpi1_lz_lit.hpi", buildV1({f}, 0));
        ta::hpi::Archive a(path);
        const auto* e = a.find("plain.txt");
        check(e != nullptr, "LZ77: literal-only stream -- entry found");
        check(e && textOf(a.read(*e)) == "ABCDEFGH",
              "LZ77: eight literals decode to ABCDEFGH");
        std::filesystem::remove(path);
    }
    {
        // Exactly one full group and NOT a byte more -- no trailing tag byte,
        // because there is no ninth item for it to describe. Refilling the tag
        // at the bottom of the decode loop made this legal stream throw
        // "overruns chunk"; any file whose length is a multiple of the group
        // size takes this shape, so it would have hit real data immediately.
        std::vector<uint8_t> s;
        lzLiterals(s, "12345678");
        InFile f{"exact.txt", {}, 1, s};
        f.data.assign({'1','2','3','4','5','6','7','8'});
        auto path = writeTemp("ta_hpi1_lz_exact.hpi", buildV1({f}, 0));
        ta::hpi::Archive a(path);
        const auto* e = a.find("exact.txt");
        check(e && textOf(a.read(*e)) == "12345678",
              "LZ77: a stream ending on a group boundary needs no trailing tag");
        std::filesystem::remove(path);
    }
    {
        // Literals "abcd", then a back reference to window position 1 for 4
        // bytes -- "abcd" again. Expect "abcdabcd".
        std::vector<uint8_t> s;
        s.push_back(0x10);                    // items 0-3 literal, item 4 a backref
        for (char c : std::string("abcd")) s.push_back(uint8_t(c));
        lzBackref(s, 1, 4);
        s.push_back(0x01);
        lzBackref(s, 0, 2);                   // terminate

        InFile f{"rep.txt", {}, 1, s};
        f.data.assign({'a','b','c','d','a','b','c','d'});
        auto path = writeTemp("ta_hpi1_lz_ref.hpi", buildV1({f}, 0));
        ta::hpi::Archive a(path);
        const auto* e = a.find("rep.txt");
        check(e && textOf(a.read(*e)) == "abcdabcd",
              "LZ77: a back reference replays four earlier bytes");
        std::filesystem::remove(path);
    }
    {
        // Overlapping copy: one literal 'x', then a reference starting at that
        // same byte for 5. The window write cursor advances INTO the region
        // being read, so this must produce "xxxxxx" -- run-length expansion, the
        // property a naive memcpy would get wrong.
        std::vector<uint8_t> s;
        s.push_back(0x02);                    // item 0 literal, item 1 a backref
        s.push_back('x');
        lzBackref(s, 1, 5);
        s.push_back(0x01);
        lzBackref(s, 0, 2);

        InFile f{"run.txt", {}, 1, s};
        f.data.assign(6, 'x');
        auto path = writeTemp("ta_hpi1_lz_run.hpi", buildV1({f}, 0));
        ta::hpi::Archive a(path);
        const auto* e = a.find("run.txt");
        check(e && textOf(a.read(*e)) == "xxxxxx",
              "LZ77: an overlapping reference run-length-expands");
        std::filesystem::remove(path);
    }

    // --- archive shape: masked vs unmasked, stored vs zlib, nesting ----------
    for (uint32_t key : {uint32_t(0), uint32_t(0x12345678)}) {
        const char* label = key ? "masked" : "unmasked";
        std::vector<InFile> files;
        files.push_back({"gamedata/sidedata.tdf",
                         std::vector<uint8_t>(), 0, {}});
        files.back().data = {'[','S','I','D','E','0',']','{','}'};
        files.push_back({"units/armcom.fbi", {}, 2, {}});
        {
            std::string t = "[UNITINFO]{UnitName=ARMCOM;BuildCostMetal=4230;}";
            files.back().data.assign(t.begin(), t.end());
        }
        // Over one chunk: v1 derives the chunk count rather than storing it.
        files.push_back({"anims/big.gaf", {}, 2, {}});
        {
            auto& d = files.back().data;
            d.resize(kChunk + 1234);
            for (size_t i = 0; i < d.size(); ++i) d[i] = uint8_t((i * 31 + 7) & 0xff);
        }

        auto path = writeTemp(std::string("ta_hpi1_") + label + ".hpi", buildV1(files, key));
        ta::hpi::Archive a(path);

        auto info = ta::hpi::inspect(path);
        check(info.version == 0x00010000, (std::string(label) + ": reports version 1").c_str());
        check(info.headerKey == key, (std::string(label) + ": header key round-trips").c_str());

        const auto* side = a.find("gamedata/sidedata.tdf");
        check(side && textOf(a.read(*side)) == "[SIDE0]{}",
              (std::string(label) + ": a STORED file reads back").c_str());

        const auto* fbi = a.find("units/armcom.fbi");
        check(fbi && textOf(a.read(*fbi)).find("BuildCostMetal=4230") != std::string::npos,
              (std::string(label) + ": a ZLIB file reads back").c_str());

        const auto* big = a.find("anims/big.gaf");
        bool bigOk = false;
        if (big) {
            auto got = a.read(*big);
            bigOk = got.size() == kChunk + 1234;
            for (size_t i = 0; bigOk && i < got.size(); ++i)
                if (got[i] != uint8_t((i * 31 + 7) & 0xff)) bigOk = false;
        }
        check(bigOk, (std::string(label) + ": a MULTI-CHUNK file reads back byte-exact").c_str());

        // Case-insensitive lookup, as every caller in the engine relies on.
        check(a.find("UNITS/ARMCOM.FBI") != nullptr,
              (std::string(label) + ": lookup is case-insensitive").c_str());

        // Directory entries are emitted for the tree, not just the leaves.
        int dirs = 0;
        for (const auto& e : a.entries()) if (e.isDirectory) ++dirs;
        check(dirs == 3, (std::string(label) + ": the three directories are listed").c_str());

        std::filesystem::remove(path);
    }

    // --- a malformed archive must raise, not spin ----------------------------
    {
        // Point a directory record's single subdirectory entry back at itself.
        std::vector<InFile> files;
        files.push_back({"a/b.txt", {'h','i'}, 0, {}});
        auto bytes = buildV1(files, 0);
        // The root record is at header word 4; its entry array's first entry is
        // a directory pointing at the "a" record. Redirect that to the root.
        uint32_t rootRec = uint32_t(bytes[16] | (bytes[17] << 8) |
                                    (bytes[18] << 16) | (uint32_t(bytes[19]) << 24));
        uint32_t entries = uint32_t(bytes[rootRec + 4] | (bytes[rootRec + 5] << 8) |
                                    (bytes[rootRec + 6] << 16) |
                                    (uint32_t(bytes[rootRec + 7]) << 24));
        at32(bytes, entries + 4, rootRec);          // subdir -> itself
        auto path = writeTemp("ta_hpi1_cycle.hpi", bytes);
        bool threw = false;
        try { ta::hpi::Archive a(path); } catch (const std::exception&) { threw = true; }
        check(threw, "a directory cycle raises instead of recursing for ever");
        std::filesystem::remove(path);
    }

    // --- a v2 archive must still load ----------------------------------------
    {
        // The fork still has to read HPI v2: the map editor writes .kmp bundles
        // with hpi::pack, and breaking that while adding v1 would be silent.
        std::vector<ta::hpi::PackFile> pf{{"dir/file.txt", {'o','k'}}};
        auto path = writeTemp("ta_hpi2_roundtrip.hpi", ta::hpi::pack(pf, 0));
        ta::hpi::Archive a(path);
        const auto* e = a.find("dir/file.txt");
        check(e && textOf(a.read(*e)) == "ok", "v2 archives still read (pack round trip)");
        std::filesystem::remove(path);
    }

    // --- archive precedence: a later group must override an earlier one -------
    {
        // This guards a bug that was live and completely silent. TA is HPI v1,
        // and a v1 file record carries NO timestamp -- so every entry's date is
        // 0 and every same-path collision between two v1 archives is a tie. The
        // mount resolved collisions with a strict '>' on the date, which handed
        // every tie to the FIRST-mounted archive; the first extension group is
        // `.hpi`, so the base game beat ccdata.ccx, btdata.ccx and rev31.gp3 on
        // every path they share. On a Commander Pack install that shadowed 1082
        // files whose expansion copy differs -- 471 unit FBIs among them, and
        // the Commander's own build menu and D-gun damage with them.
        //
        // Synthetic archives on purpose: this has to run on CI, where no retail
        // data exists. What it pins is the RULE, which is where the bug was.
        auto dir = std::filesystem::temp_directory_path() / "ta_hpi_prec_test";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        auto put = [&](const char* name, const char* body) {
            InFile f{"gamedata/thing.tdf", {}, 0, {}};
            f.data.assign(body, body + std::strlen(body));
            auto bytes = buildV1({f}, 0);
            std::ofstream o(dir / name, std::ios::binary);
            o.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        };
        // Same path in all four groups, each naming itself.
        put("base.hpi", "hpi");
        put("expansion.ccx", "ccx");
        put("patch.gp3", "gp3");
        put("mod.ufo", "ufo");
        {
            ta::hpi::Vfs vfs = ta::hpi::mountRetailRoot(dir, ta::hpi::OverridePolicy::None);
            check(textOf(vfs.read("gamedata/thing.tdf")) == "ufo",
                  "precedence: .ufo (last group) wins over .gp3/.ccx/.hpi");
        }
        // Drop the layers from the top and the next one down must take over --
        // an ordering check, not just a "something wins" check.
        std::filesystem::remove(dir / "mod.ufo");
        {
            ta::hpi::Vfs vfs = ta::hpi::mountRetailRoot(dir, ta::hpi::OverridePolicy::None);
            check(textOf(vfs.read("gamedata/thing.tdf")) == "gp3",
                  "precedence: the .gp3 patch then wins over .ccx and .hpi");
        }
        std::filesystem::remove(dir / "patch.gp3");
        {
            ta::hpi::Vfs vfs = ta::hpi::mountRetailRoot(dir, ta::hpi::OverridePolicy::None);
            check(textOf(vfs.read("gamedata/thing.tdf")) == "ccx",
                  "precedence: the .ccx expansion then wins over the base .hpi");
        }
        std::filesystem::remove(dir / "expansion.ccx");
        {
            ta::hpi::Vfs vfs = ta::hpi::mountRetailRoot(dir, ta::hpi::OverridePolicy::None);
            check(textOf(vfs.read("gamedata/thing.tdf")) == "hpi",
                  "precedence: the base .hpi is used when it is all there is");
        }
        std::filesystem::remove_all(dir);
    }

    std::printf(g_fail ? "hpi1_test: %d FAILURE(S)\n" : "hpi1_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
