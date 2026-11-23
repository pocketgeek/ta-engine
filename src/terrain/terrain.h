#pragma once

#include "tnt/tnt.h"
#include "util/jpeg.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace tak::hpi { class Vfs; }

namespace tak::terrain {

// Composites TNT map terrain from the content-addressed section JPGs the VFS
// resolves as terrain/<hexkey>.jpg (from terrain.hpi). Each 32px map block
// crops (col*32, row*32, 32, 32) out of its keyed JPG.
//
// Section lifetime (rendering workstream J). A decoded section is handed out as a
// SectionRef -- shared ownership of an immutable image. Holding the ref PINS the
// image: the cache never evicts an entry while anyone else holds it (the cache's
// own reference is then not the only one), and an image the cache dropped (budget
// eviction, clear()) stays alive until its last holder lets go. So a consumer keeps
// its inputs exactly as long as it uses them -- the chunk worker for one compose
// job, the render thread for one texture upload -- and everything else is
// evictable. Before J the API returned a reference "valid until clear()", which
// made clear() require every consumer to stop first and made any eviction unsafe.
using SectionRef = std::shared_ptr<const jpeg::Image>;

class Compositor {
public:
    explicit Compositor(const hpi::Vfs& vfs);

    // Drop the cache's references (decoded images and the record of absent keys).
    // Images still pinned by a consumer stay valid until released. Callers that are
    // about to swap or remount the VFS must still stop its readers first (the decode
    // reads the VFS outside the cache lock).
    void clear();

    // Render the whole map at full resolution (width*16 x height*16 px).
    jpeg::Image renderMap(const tnt::Map& map);

    // Render one 32px block into `dst` (RGBA, dstW px wide) at (dx, dy).
    // Thread-safe: background chunk/minimap builders and the main thread may
    // composite concurrently. The block's section is pinned for the copy only.
    void renderBlock(const tnt::Map& map, int bx, int by,
                     std::vector<uint8_t>& dst, int dstW, int dx, int dy);

    // Decode (or fetch the cached) section JPG for `key` and pin it. Thread-safe;
    // the JPEG decode runs outside the cache lock (a second caller wanting the same
    // key waits for that decode instead of repeating it). Throws if absent.
    SectionRef sectionImage(uint32_t key, bool stockTerrain = false);

    // The cached section, never decoding: null when it is not decoded (yet, or any
    // more after an eviction). `absent` (optional) reports a key known to have no JPG.
    SectionRef cachedSection(uint32_t key, bool stockTerrain = false, bool* absent = nullptr);

    // CPU budget for decoded sections (bytes). Default: unbounded -- the editor's
    // validators, thumbnails and tools decode what they ask for. When an insertion
    // takes the cache above `high`, the least recently used EVICTABLE images are
    // evicted until it is at or below `low` (hysteresis: one eviction burst per
    // excursion, not one per insertion). Evictable = unpinned and, when the owner
    // supplies a clock (setClockMs) and `minIdleMs` > 0, unused for minIdleMs: the
    // budget bounds the COLD images, never the working set of the last minIdleMs, so
    // a camera that keeps revisiting more than the budget does not decode the same
    // sections over and over; once they go cold, trim() brings the cache back.
    // Pinned or young images make the cache exceed `high` for as long as that lasts.
    void setBudget(size_t high, size_t low, uint64_t minIdleMs = 0);
    // The owner's clock (ms) for the working-set rule: wall time in play, the render
    // study's presentation clock in benchmarks. Any thread may read it; the owner
    // updates it (MapView: once per frame). Without it no image counts as young.
    void setClockMs(uint64_t ms);

    // Evict to the low mark if the cache is above the high mark. Insertions do this
    // themselves; consumers call it after releasing pins (a compose job that held
    // more than the budget), since releasing a pin does not reach the cache.
    void trim();

    // Speculative decode admission (load-time prefetch): true while the cache has
    // room below `low`. A prefetch never evicts anything; demand decodes do.
    bool admitsPrefetch(size_t bytes);

    struct Stats {
        size_t usedBytes = 0;      // decoded images the cache references (pinned included)
        size_t pinnedBytes = 0;    // ... of which a consumer also holds (not evictable now)
        size_t peakBytes = 0;      // highest usedBytes since construction
        size_t entries = 0;
        size_t highBytes = SIZE_MAX, lowBytes = SIZE_MAX;
        // Cumulative since construction (readers take deltas).
        uint64_t decodes = 0;      // JPEG decodes performed
        uint64_t redecodes = 0;    // ... of keys this cache had decoded and evicted before
        uint64_t evictions = 0;    // images dropped by the budget
        size_t youngBytes = 0;     // unpinned images inside the working-set window (not evictable)
        uint64_t evictedBytes = 0;
    };
    Stats stats();

private:
    using Id = std::pair<uint32_t, bool>;
    struct Entry { SectionRef image; size_t bytes = 0; uint64_t lastUse = 0, lastUseMs = 0; };
    bool evictableLocked(const Entry& e) const;
    void touchLocked(Entry& e);
    // Under mu_: record a use / insert a decoded image, evicting past the budget.
    // Evicted refs are moved into `dropped` so their memory is freed after unlocking.
    void insertLocked(const Id& id, SectionRef image, std::vector<SectionRef>& dropped);
    void evictLocked(size_t target, std::vector<SectionRef>& dropped);

    const hpi::Vfs* vfs_ = nullptr;
    std::mutex mu_;                           // guards everything below
    std::condition_variable decodedCv_;       // a decode in flight finished
    std::map<Id, Entry> cache_;
    std::set<Id> absent_;                     // keys whose JPG the VFS does not have
    std::set<Id> decoding_;                   // decodes in flight (outside the lock)
    std::set<Id> evicted_;                    // keys evicted at least once (redecode count)
    size_t used_ = 0, peak_ = 0;
    size_t high_ = SIZE_MAX, low_ = SIZE_MAX;
    uint64_t minIdleMs_ = 0, nowMs_ = 0;
    bool clockSet_ = false;
    uint64_t clock_ = 0;                      // use order for LRU
    uint64_t decodes_ = 0, redecodes_ = 0, evictions_ = 0, evictedBytes_ = 0;
};

} // namespace tak::terrain
