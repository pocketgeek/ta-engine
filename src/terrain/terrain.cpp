#include "terrain/terrain.h"

#include "hpi/hpi.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace tak::terrain {

namespace {
constexpr int kBlock = 32;

std::string sectionPath(uint32_t key) {
    // Terrain tiles are content-addressed by the map's u32 tile key: terrain/<8hex>.jpg.
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", key);
    return std::string("terrain/") + buf + ".jpg";
}
}

Compositor::Compositor(const hpi::Vfs& vfs) : vfs_(&vfs) {}

void Compositor::clear() {
    std::vector<SectionRef> dropped;
    {
        std::lock_guard<std::mutex> lk(mu_);
        dropped.reserve(cache_.size());
        for (auto& [id, e] : cache_) dropped.push_back(std::move(e.image));
        cache_.clear();
        absent_.clear();
        evicted_.clear();
        used_ = 0;
        // A decode in flight inserts into the cleared cache when it finishes. It read
        // the VFS the caller is responsible for keeping alive until then (quiesce).
    }
    // Images freed here (or by their last holder), outside the lock.
}

void Compositor::setBudget(size_t high, size_t low, uint64_t minIdleMs) {
    std::vector<SectionRef> dropped;
    std::lock_guard<std::mutex> lk(mu_);
    high_ = high;
    low_ = std::min(low, high);
    minIdleMs_ = minIdleMs;
    if (used_ > high_) evictLocked(low_, dropped);
}

void Compositor::setClockMs(uint64_t ms) {
    std::lock_guard<std::mutex> lk(mu_);
    if (clockSet_ && ms < nowMs_)   // the clock restarted: forget the old use times
        for (auto& [id, e] : cache_) e.lastUseMs = ms;
    nowMs_ = ms;
    clockSet_ = true;
}

void Compositor::touchLocked(Entry& e) {
    e.lastUse = ++clock_;
    e.lastUseMs = nowMs_;
}

bool Compositor::evictableLocked(const Entry& e) const {
    // use_count() == 1 under the lock is exact: the only other way to obtain a
    // reference is through this cache, which needs the lock, and copying a
    // consumer's ref requires one to exist.
    if (e.image.use_count() != 1) return false;   // pinned
    return !clockSet_ || !minIdleMs_ || nowMs_ - e.lastUseMs >= minIdleMs_;
}

void Compositor::trim() {
    std::vector<SectionRef> dropped;
    std::lock_guard<std::mutex> lk(mu_);
    if (used_ > high_) evictLocked(low_, dropped);
}

bool Compositor::admitsPrefetch(size_t bytes) {
    std::lock_guard<std::mutex> lk(mu_);
    return low_ == SIZE_MAX || used_ + bytes <= low_;
}

SectionRef Compositor::cachedSection(uint32_t key, bool stockTerrain, bool* absent) {
    const Id id{key, stockTerrain};
    std::lock_guard<std::mutex> lk(mu_);
    if (absent) *absent = absent_.count(id) != 0;
    auto it = cache_.find(id);
    if (it == cache_.end()) return {};
    touchLocked(it->second);
    return it->second.image;
}

SectionRef Compositor::sectionImage(uint32_t key, bool stockTerrain) {
    const Id id{key, stockTerrain};
    {
        std::unique_lock<std::mutex> lk(mu_);
        for (;;) {
            if (auto it = cache_.find(id); it != cache_.end()) {
                touchLocked(it->second);
                return it->second.image;
            }
            if (absent_.count(id)) break;
            if (!decoding_.count(id)) { decoding_.insert(id); break; }
            decodedCv_.wait(lk);   // another thread decodes this key right now
        }
        if (absent_.count(id))
            throw std::runtime_error("terrain JPG not found: " + sectionPath(key).substr(8, 8));
    }
    // Decode outside the lock: other sections stay available to every thread
    // (the render thread's cachedSection never waits behind a JPEG decode).
    SectionRef image;
    bool missing = false;
    try {
        const std::string path = sectionPath(key);
        if (!vfs_->has(path)) missing = true;
        else image = std::make_shared<const jpeg::Image>(jpeg::load(vfs_->read(path, stockTerrain)));
    } catch (...) {
        std::lock_guard<std::mutex> lk(mu_);
        decoding_.erase(id);
        decodedCv_.notify_all();
        throw;
    }
    std::vector<SectionRef> dropped;
    {
        std::lock_guard<std::mutex> lk(mu_);
        decoding_.erase(id);
        if (missing) absent_.insert(id);
        else {
            ++decodes_;
            if (evicted_.count(id)) ++redecodes_;
            insertLocked(id, image, dropped);
        }
    }
    decodedCv_.notify_all();
    if (missing) throw std::runtime_error("terrain JPG not found: " + sectionPath(key).substr(8, 8));
    return image;
}

void Compositor::insertLocked(const Id& id, SectionRef image, std::vector<SectionRef>& dropped) {
    Entry e;
    e.bytes = image->rgba.capacity();
    e.image = std::move(image);
    touchLocked(e);
    auto [it, added] = cache_.try_emplace(id, std::move(e));
    if (!added) return;   // inserted meanwhile (after a clear() raced a decode): keep the first
    used_ += it->second.bytes;
    peak_ = std::max(peak_, used_);
    if (used_ > high_) evictLocked(low_, dropped);
}

void Compositor::evictLocked(size_t target, std::vector<SectionRef>& dropped) {
    // Least recently used first, skipping pinned images and the working set.
    while (used_ > target) {
        auto victim = cache_.end();
        for (auto it = cache_.begin(); it != cache_.end(); ++it)
            if (evictableLocked(it->second) &&
                (victim == cache_.end() || it->second.lastUse < victim->second.lastUse)) victim = it;
        if (victim == cache_.end()) break;   // everything left is pinned or in use
        used_ -= victim->second.bytes;
        ++evictions_;
        evictedBytes_ += victim->second.bytes;
        evicted_.insert(victim->first);
        dropped.push_back(std::move(victim->second.image));
        cache_.erase(victim);
    }
}

Compositor::Stats Compositor::stats() {
    std::lock_guard<std::mutex> lk(mu_);
    Stats s;
    s.usedBytes = used_;
    s.peakBytes = peak_;
    s.entries = cache_.size();
    s.highBytes = high_;
    s.lowBytes = low_;
    for (const auto& [id, e] : cache_) {
        if (e.image.use_count() > 1) s.pinnedBytes += e.bytes;
        else if (!evictableLocked(e)) s.youngBytes += e.bytes;
    }
    s.decodes = decodes_;
    s.redecodes = redecodes_;
    s.evictions = evictions_;
    s.evictedBytes = evictedBytes_;
    return s;
}

void Compositor::renderBlock(const tnt::Map& map, int bx, int by,
                             std::vector<uint8_t>& dst, int dstW, int dx, int dy) {
    size_t b = size_t(by) * map.blocksX + bx;
    SectionRef pin;
    // A key the install can't resolve must not throw out of the chunk worker
    // thread (std::terminate); the block just stays empty.
    try { pin = sectionImage(map.tileKeys[b], map.stockTerrain); } catch (const std::exception&) { return; }
    // The pin keeps the image alive for the copy; no lock is held.
    const jpeg::Image& img = *pin;
    int sx = (map.tileCols[b] * kBlock) % std::max(img.width, 1);
    int sy = (map.tileRows[b] * kBlock) % std::max(img.height, 1);
    // Water is just a texture, exactly like land: retail draws the sea sections
    // as-is (flat dark teal art; the earlier "near-black" impression was our
    // wallpaper sampling the JPGs' black PADDING -- only part of each sea section
    // is real art, see mapgen's per-world seaK). The only retail water effect is
    // "WackyWater", a debug-console cheat, off in normal play. No tint here.
    for (int y = 0; y < kBlock; ++y) {
        if (sy + y >= img.height) break;
        const uint8_t* srow = &img.rgba[(size_t(sy + y) * img.width + sx) * 4];
        uint8_t* drow = &dst[(size_t(dy + y) * dstW + dx) * 4];
        std::memcpy(drow, srow, size_t(std::min(kBlock, img.width - sx)) * 4);
    }
}

jpeg::Image Compositor::renderMap(const tnt::Map& map) {
    jpeg::Image out;
    out.width = map.blocksX * kBlock;
    out.height = map.blocksY * kBlock;
    out.rgba.assign(size_t(out.width) * out.height * 4, 0);
    for (int by = 0; by < map.blocksY; ++by)
        for (int bx = 0; bx < map.blocksX; ++bx)
            renderBlock(map, bx, by, out.rgba, out.width, bx * kBlock, by * kBlock);
    return out;
}

} // namespace tak::terrain
