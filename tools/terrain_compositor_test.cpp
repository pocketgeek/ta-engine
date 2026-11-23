// terrain::Compositor section lifetime and CPU budget (rendering workstream J).
// Synthetic sections (JPEGs encoded here, served through VFS mod files), so
// the test needs no game data. Checks: a pinned image survives clear() and
// eviction; eviction is least-recently-used among unpinned images with a
// high/low watermark; pinned images are never evicted; redecodes are counted;
// cachedSection never decodes; absent keys are remembered; prefetch admission;
// a working set inside the budget never evicts; concurrent decode / pin /
// renderBlock / clear / budget changes keep every pinned image intact.
#include "terrain/terrain.h"
#include "hpi/hpi.h"

#include <jpeglib.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using tak::terrain::Compositor;
using tak::terrain::SectionRef;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

constexpr int kW = 96, kH = 64;   // 24 KiB decoded per section

std::vector<uint8_t> encode(uint32_t key) {
    std::vector<uint8_t> rgb(size_t(kW) * kH * 3);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            uint8_t* p = &rgb[(size_t(y) * kW + x) * 3];
            p[0] = uint8_t(key * 37 + x * 2);
            p[1] = uint8_t(key * 91 + y * 3);
            p[2] = uint8_t((x ^ y) + key);
        }
    jpeg_compress_struct c{};
    jpeg_error_mgr err{};
    c.err = jpeg_std_error(&err);
    jpeg_create_compress(&c);
    unsigned char* out = nullptr;
    unsigned long size = 0;
    jpeg_mem_dest(&c, &out, &size);
    c.image_width = kW;
    c.image_height = kH;
    c.input_components = 3;
    c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, 90, TRUE);
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < c.image_height) {
        JSAMPROW row = &rgb[size_t(c.next_scanline) * kW * 3];
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    std::vector<uint8_t> bytes(out, out + size);
    jpeg_destroy_compress(&c);
    std::free(out);
    return bytes;
}

std::string path(uint32_t key) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "terrain/%08x.jpg", key);
    return buf;
}

struct Fixture {
    tak::hpi::Vfs vfs;
    std::map<uint32_t, tak::jpeg::Image> reference;
    size_t bytes = 0;   // decoded bytes of one section (capacity, as the cache counts it)
    explicit Fixture(uint32_t keys) {
        auto files = std::make_shared<tak::hpi::Vfs::Files>();
        for (uint32_t k = 1; k <= keys; ++k) {
            auto data = encode(k);
            reference[k] = tak::jpeg::load(data);
            (*files)[path(k)] = std::move(data);
        }
        vfs.setModFiles(files);
        bytes = reference.begin()->second.rgba.capacity();
    }
    bool same(uint32_t key, const SectionRef& image) const {
        const auto& r = reference.at(key);
        return image && image->width == r.width && image->height == r.height && image->rgba == r.rgba;
    }
};

void basics() {
    Fixture f(4);
    Compositor c(f.vfs);
    check(!c.cachedSection(1), "nothing cached before a decode");
    check(c.stats().decodes == 0, "cachedSection does not decode");
    SectionRef a = c.sectionImage(1);
    check(f.same(1, a), "decoded pixels");
    check(c.cachedSection(1) == a, "cachedSection returns the cached image");
    check(c.sectionImage(1) == a, "a second request is a cache hit");
    check(c.stats().decodes == 1, "one decode");
    bool absent = false;
    bool threw = false;
    try { c.sectionImage(99); } catch (const std::exception&) { threw = true; }
    check(threw, "absent key throws");
    check(!c.cachedSection(99, false, &absent) && absent, "absent key remembered");
    // A pin outlives clear(): the consumer's image stays valid (ASan would catch a free).
    c.clear();
    check(!c.cachedSection(1), "clear drops the cache's reference");
    check(f.same(1, a), "pinned image intact after clear");
    check(c.stats().usedBytes == 0, "clear empties the accounting");
    check(!c.cachedSection(99, false, &absent) && !absent, "clear forgets absent keys");
    // renderBlock crops through a pin.
    tak::tnt::Map map;
    map.blocksX = 2; map.blocksY = 1;
    map.tileKeys = {2, 3}; map.tileCols = {1, 0}; map.tileRows = {1, 0};
    std::vector<uint8_t> dst(64 * 32 * 4, 0);
    c.renderBlock(map, 0, 0, dst, 64, 0, 0);
    c.renderBlock(map, 1, 0, dst, 64, 32, 0);
    const auto& r2 = f.reference.at(2);
    const auto& r3 = f.reference.at(3);
    bool crop = true;
    for (int y = 0; y < 32; ++y) {
        crop &= !std::memcmp(&dst[size_t(y) * 64 * 4], &r2.rgba[(size_t(32 + y) * kW + 32) * 4], 32 * 4);
        crop &= !std::memcmp(&dst[(size_t(y) * 64 + 32) * 4], &r3.rgba[size_t(y) * kW * 4], 32 * 4);
    }
    check(crop, "renderBlock copies the keyed crop");
}

void budget() {
    Fixture f(8);
    Compositor c(f.vfs);
    const size_t s = f.bytes;
    c.setBudget(s * 7 / 2, s * 5 / 2);   // high 3.5 sections, low 2.5
    for (uint32_t k = 1; k <= 3; ++k) c.sectionImage(k);
    check(c.stats().evictions == 0, "under the high watermark: no eviction");
    c.cachedSection(1);                   // 1 becomes the most recently used
    c.sectionImage(4);                    // 4 sections > 3.5: evict to <= 2.5
    auto st = c.stats();
    check(st.evictions == 2 && st.usedBytes == 2 * s, "evicted down to the low watermark");
    check(!c.cachedSection(2) && !c.cachedSection(3), "least recently used went first");
    check(c.cachedSection(1) && c.cachedSection(4), "recently used stays");
    // Hysteresis: back within (low, high] nothing is evicted.
    c.sectionImage(5);
    check(c.stats().evictions == 2 && c.stats().usedBytes == 3 * s, "no eviction inside the band");
    // A redecode of an evicted key is counted.
    c.sectionImage(2);
    st = c.stats();
    check(st.redecodes == 1, "redecode counted");
    check(st.peakBytes == 4 * s, "peak bytes");
    // Pinned images are never evicted, even far past the budget.
    std::vector<SectionRef> pins;
    for (uint32_t k = 1; k <= 8; ++k) pins.push_back(c.sectionImage(k));
    st = c.stats();
    check(st.usedBytes == 8 * s && st.pinnedBytes == 8 * s, "pinned images exceed the budget, none evicted");
    for (uint32_t k = 1; k <= 8; ++k) check(f.same(k, pins[k - 1]), "pinned pixels intact");
    pins.erase(pins.begin(), pins.begin() + 4);   // unpin 1..4
    c.setBudget(s * 7 / 2, s * 5 / 2);            // re-applying the budget evicts the unpinned
    st = c.stats();
    check(st.usedBytes == 4 * s && st.pinnedBytes == 4 * s, "only unpinned images evicted");
    for (uint32_t k = 5; k <= 8; ++k) check(c.cachedSection(k) != nullptr, "pinned kept");
    // Releasing pins does not reach the cache: trim() brings it back inside the budget.
    pins.clear();
    check(c.stats().usedBytes == 4 * s, "unpinned images linger until the next insertion or trim");
    c.trim();
    check(c.stats().usedBytes <= s * 5 / 2, "trim evicts to the low watermark");
    // A held image is skipped by every later eviction burst, however old it gets.
    pins.clear();
    c.clear();
    SectionRef held = c.sectionImage(1);
    for (uint32_t k = 2; k <= 8; ++k) c.sectionImage(k);
    check(c.cachedSection(1) == held && f.same(1, held), "held image never evicted");
    // Prefetch admission: only below the low watermark, never by eviction.
    c.clear();
    c.setBudget(s * 4, s * 3);
    check(c.admitsPrefetch(s), "empty cache admits a prefetch");
    for (uint32_t k = 1; k <= 3; ++k) c.sectionImage(k);
    check(!c.admitsPrefetch(s), "full to the low watermark: prefetch refused");
}

void workingSet() {
    // With an owner clock, images used within minIdleMs are never evicted: the budget
    // bounds cold images only; trim() brings the cache back once they go cold.
    Fixture f(8);
    Compositor c(f.vfs);
    const size_t s = f.bytes;
    c.setBudget(s * 3, s * 2, 1000);
    c.setClockMs(0);
    for (uint32_t k = 1; k <= 6; ++k) c.sectionImage(k);
    auto st = c.stats();
    check(st.evictions == 0 && st.usedBytes == 6 * s, "the working set exceeds the budget, nothing evicted");
    check(st.youngBytes == 6 * s, "young bytes reported");
    c.setClockMs(600);
    c.cachedSection(5); c.cachedSection(6);   // still in use
    c.setClockMs(1200);                        // 1..4 cold now, 5 and 6 used 600 ms ago
    c.trim();
    st = c.stats();
    check(st.evictions == 4 && st.usedBytes == 2 * s, "cold images evicted down to the low mark");
    check(c.cachedSection(5) && c.cachedSection(6), "the working set stays");
    // A decode/evict cycle inside the window does not happen: revisiting keys keeps them.
    for (int round = 0; round < 20; ++round) {
        c.setClockMs(1200 + uint64_t(round) * 100);
        for (uint32_t k = 1; k <= 8; ++k) c.sectionImage(k);
    }
    check(c.stats().redecodes == 4, "revisited keys are decoded again once, then kept");
    // A clock that restarts (new session) does not make everything cold or ancient.
    c.setClockMs(10);
    c.trim();
    check(c.stats().usedBytes == 8 * s, "a restarted clock keeps the working set");
}

void noThrash() {
    // A cyclic working set inside the low watermark decodes each key once.
    Fixture f(6);
    Compositor c(f.vfs);
    c.setBudget(f.bytes * 6, f.bytes * 5);
    for (int round = 0; round < 50; ++round)
        for (uint32_t k = 1; k <= 5; ++k) c.sectionImage(k);
    const auto st = c.stats();
    check(st.decodes == 5 && st.evictions == 0 && st.redecodes == 0, "working set inside the budget: no thrash");
}

void concurrent() {
    Fixture f(24);
    Compositor c(f.vfs);
    c.setBudget(f.bytes * 6, f.bytes * 4);
    tak::tnt::Map map;
    map.blocksX = 24; map.blocksY = 1;
    for (uint32_t k = 1; k <= 24; ++k) { map.tileKeys.push_back(k); map.tileCols.push_back(0); map.tileRows.push_back(0); }
    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&, t] {
            std::mt19937 rng(1234 + t);
            std::vector<std::pair<uint32_t, SectionRef>> ring(5);
            std::vector<uint8_t> block(32 * 32 * 4);
            for (int i = 0; i < 3000; ++i) {
                const uint32_t key = 1 + rng() % 24;
                switch (rng() % 10) {
                case 0: case 1: case 2: case 3: ring[rng() % ring.size()] = {key, c.sectionImage(key)}; break;
                case 4: case 5: if (auto p = c.cachedSection(key)) ring[rng() % ring.size()] = {key, p}; break;
                case 6: case 7: c.renderBlock(map, int(key - 1), 0, block, 32, 0, 0);
                    if (std::memcmp(block.data(), f.reference.at(key).rgba.data(), 32 * 4)) ++bad;
                    break;
                case 8: if (t == 0 && i % 97 == 0) c.clear(); else c.stats(); break;
                default: if (t == 1 && i % 53 == 0) c.setBudget(f.bytes * (3 + rng() % 6), f.bytes * 2); break;
                }
                for (const auto& [k, p] : ring)
                    if (p && !f.same(k, p)) ++bad;
            }
        });
    for (auto& t : threads) t.join();
    check(bad.load() == 0, "concurrent pins and blocks intact");
    const auto st = c.stats();
    check(st.pinnedBytes == 0, "no pins left after the threads ended");
    check(st.usedBytes == st.entries * f.bytes, "accounting matches the entries");
}
} // namespace

int main() {
    basics();
    budget();
    workingSet();
    noThrash();
    concurrent();
    if (failures) { std::fprintf(stderr, "terrain_compositor: %d failure(s)\n", failures); return 1; }
    std::printf("terrain_compositor: ok\n");
    return 0;
}
