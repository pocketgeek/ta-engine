#pragma once
#include <SDL.h>
#include "terrain/terrain.h"
#include "client/terrainwork.h"
#include <atomic>
#include <memory>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace tak {
// Camera-independent terrain images. The worker owns mip generation/composition;
// all SDL operations and cache bookkeeping stay on the render thread.
//
// Work follows the camera (workstream K, policy in client/terrainwork.h): the worker
// takes the most urgent queued job (visible cells without any image, then visible cells
// a stand-in covers, then leading-edge cells while the camera moves, then kept work the
// camera just left), not the oldest. Every frame the render thread re-ranks pending jobs
// and cancels obsolete ones (outside the keep band, or of a level the zoom left past its
// grace); a running job stops cooperatively at its next row check and only drops its
// pins. Completed results upload most urgent first under the frame's UploadBudget, and
// stand-ins of visible cells are evicted last.
class TerrainChunks {
public:
    static constexpr int span=1024;
    struct Stats {
        size_t bytes=0,images=0,uploads=0,visible=0,ready=0;uint64_t revision=0;size_t pending=0;
        // Render telemetry (cumulative since construction; the reader takes deltas).
        // Jobs: queued -> completed by the worker -> used (uploaded) or discarded;
        // cancelled = pending jobs cancelled by clear()/invalidate().
        uint64_t jobsQueued=0,jobsCompleted=0,jobsCancelled=0,jobsDiscarded=0,jobsUsed=0;
        uint64_t uploadBytes=0,uploadNs=0,composeNs=0;
        size_t mipBytes=0;   // worker-owned mip images (CPU), current
        // Workstream J: the mip cache's CPU budget. Pinned = held by the running job.
        size_t mipPinnedBytes=0,mipPeakBytes=0;
        uint64_t mipEvictions=0,mipRegens=0,mipTrims=0;   // cumulative
        // Workstream K. What the last draw() showed of the visible cells: `ready` = at
        // the wanted level, `standin` = another level's image, `holes` = no image (the
        // section fallback or the underlay shows). wastedNs = worker time spent on jobs
        // whose result was never uploaded (cumulative).
        size_t standin=0,holes=0;
        uint64_t wastedNs=0;
        // Cumulative: jobs cancelled as obsolete, running jobs stopped by such a cancel,
        // nearby (leading-edge) jobs queued, results uploaded for cells not visible then,
        // uploads the frame budget postponed (counted once per job and frame).
        uint64_t jobsObsolete=0,jobsAborted=0,jobsNearby=0,jobsOffview=0,uploadsDeferred=0;
        uint64_t framesGated=0;   // frames the calm gate queued no new work
        // The texture slot pool: textures created / reused from the pool (cumulative),
        // and the free slots' bytes now (held for reuse, counted in the budget).
        uint64_t slotCreates=0,slotReuses=0;size_t pooledBytes=0;
    };
    explicit TerrainChunks(terrain::Compositor& compositor);
    ~TerrainChunks();
    void clear();
    // Block coordinates, exclusive upper bounds. Includes filtering gutters.
    void invalidate(int bx0,int by0,int bx1,int by1);
    // Render thread: stop reading the old compositor before its images change.
    void resetSource();
    // nowMs: the owner's clock (wall time, or the render study's presentation clock)
    // for the mip cache's idle trim.
    // budget: the frame's terrain upload budget, shared with the owner's section uploads.
    // Uploads results that fill visible holes, and -- unless holes remain, which the
    // owner's fallback sections fill first -- the rest; those then wait for
    // uploadDeferred().
    void prepare(SDL_Renderer*,const tnt::Map&,float x,float y,float zoom,int w,int h,bool linear,uint64_t nowMs,
                 terrainwork::UploadBudget& budget);
    // After the owner's section uploads: completed results prepare() left (upgrades,
    // nearby, kept work), within what the frame's budget has left. Shown from the next draw.
    void uploadDeferred(SDL_Renderer*,terrainwork::UploadBudget& budget);
    // Scheduling policy (debug: Params::legacy() = the pre-K behaviour).
    void setParams(const terrainwork::Params& p) {params_=p;}
    const terrainwork::Params& params() const {return params_;}
    // Visible cells without an image of any level after prepare()'s uploads.
    size_t holesAfterPrepare() const {return holesAfterPrepare_;}
    // CPU budget of the worker's mip images (workstream J): above `high` the least
    // recently used images no running job holds are evicted down to `low`; after
    // `idleMs` (and 120 frames) without terrain work the worker drops them all.
    static constexpr size_t kMipHigh=size_t(64)<<20,kMipLow=size_t(56)<<20;
    static constexpr uint64_t kMipIdleMs=30000;
    void setMipBudget(size_t high,size_t low,uint64_t idleMs);
    bool draw(SDL_Renderer*,float x,float y,float zoom);
    // Did the last draw() cover this block, at the wanted level or a cached stand-in?
    bool covers(int blockX,int blockY) const;
    // Evict chunks not drawn this frame, oldest first (stand-ins of visible cells last),
    // until gpuvram::wouldFit(need) (need 0 = just get under the cap). Returns the bytes freed.
    size_t shed(size_t need);
    Stats stats() const {
        size_t ready=0;for(const auto& key:visible_)ready+=cache_.count(key);
        Stats s{bytes_,cache_.size(),uploads_,visible_.size(),ready,revision_,pending_.size()};
        s.jobsQueued=jobsQueued_;s.jobsCancelled=jobsCancelled_;s.jobsDiscarded=jobsDiscarded_;
        s.jobsUsed=jobsUsed_;s.uploadBytes=uploadBytes_;s.uploadNs=uploadNs_;
        s.jobsCompleted=jobsCompleted_.load(std::memory_order_relaxed);
        s.composeNs=composeNs_.load(std::memory_order_relaxed);
        s.mipBytes=mipBytes_.load(std::memory_order_relaxed);
        s.mipPinnedBytes=mipPinnedBytes_.load(std::memory_order_relaxed);
        s.mipPeakBytes=mipPeakBytes_.load(std::memory_order_relaxed);
        s.mipEvictions=mipEvictions_.load(std::memory_order_relaxed);
        s.mipRegens=mipRegens_.load(std::memory_order_relaxed);
        s.mipTrims=mipTrims_.load(std::memory_order_relaxed);
        s.standin=drawnStandin_;s.holes=drawnHoles_;
        s.wastedNs=wastedNs_+wastedAbortNs_.load(std::memory_order_relaxed);
        s.jobsObsolete=jobsObsolete_;s.jobsAborted=jobsAborted_.load(std::memory_order_relaxed);
        s.jobsNearby=jobsNearby_;s.jobsOffview=jobsOffview_;s.uploadsDeferred=uploadsDeferred_;s.framesGated=framesGated_;
        s.slotCreates=slotCreates_;s.slotReuses=slotReuses_;s.pooledBytes=pooledBytes_;
        return s;
    }
private:
    using Key=std::tuple<int,int,int>; // level, chunk x, chunk y
    struct Tile {uint32_t key;uint8_t col,row;};
    // Shared by the render thread (writes) and the worker (reads): a job's cancel flag
    // and its current priority (smaller = sooner; see terrainwork::Demand::priority).
    struct Control {std::atomic_bool cancelled{false};std::atomic<uint32_t> priority{0};};
    struct Job {
        Key key;uint64_t epoch;uint64_t seq=0;bool stockTerrain=false;
        std::shared_ptr<Control> control;
        int mapW,mapH,bx,by,bw,bh,w,h;
        std::vector<Tile> tiles;
        std::vector<uint8_t> pixels;
        uint64_t composeNs=0;   // worker time this job took (wasted if never uploaded)
    };
    // used: the frame it was last visible at its level; standin: the frame it last stood
    // in for a visible cell whose own level was missing (both render-thread frames).
    struct Entry {SDL_Texture* texture=nullptr;int w=0,h=0;uint64_t used=0,standin=0;};
    struct Pending {std::shared_ptr<Control> control;bool nearby=false;};
    // The image draw() would show for a visible key that has none at its level: the
    // nearest cached level, finer first. end() = none.
    std::map<Key,Entry>::iterator standIn(const Key&);
    bool anyImage(int cx,int cy) const;
    // Evict (lowest terrainwork::evictionRank, then least recently used) until `size`
    // more bytes fit the cache budget and the GPU cap. `cell` = the uploaded key (rank 2
    // goes to its own stand-ins). coldOnly (off-view uploads): only rank-1 images outside
    // the keep band. Returns whether it fits.
    bool makeRoom(size_t size,const Key& cell,bool coldOnly);
    // Bytes off-view work may use: free cache room plus cold images.
    size_t offviewRoom();
    size_t imageBytes(const Key&,const tnt::Map&) const;
    // Texture slots. Every image of a level gets a texture of that level's full size
    // ((span>>level)+2 square; a map-edge image fills its top-left part), so any slot of
    // a level holds any of its images. An evicted image's texture goes back to the
    // level's free list instead of being destroyed, and the next upload of that level
    // reuses it: after the cache has filled, panning and zooming re-fill textures rather
    // than freeing and allocating GPU memory. Free slots count in the cache's budget.
    static size_t slotBytes(int level) {const size_t side=size_t(span>>level)+2;return side*side*4;}
    void releaseSlot(int level,SDL_Texture*);       // to the free list
    bool dropFreeSlot(int keepLevel);               // destroy one free slot of another level (any if keepLevel<0)
    void dropAllFreeSlots();
    void evict(std::map<Key,Entry>::iterator,bool pool);
    // Re-rank pending jobs for this frame's demand; cancel the obsolete ones.
    void rankPending(const tnt::Map&);
    // Upload completed results: urgent ones (holes) only, or everything left.
    void uploadReady(SDL_Renderer*,terrainwork::UploadBudget&,bool urgentOnly,bool finalPass);
    void discard(Job&);
    void releasePending(const Job&);
    void work();
    void compose(Job&);
    // A pinned image of section `key` at mip `level` (0 = the compositor's decoded
    // section). The compose job holds the pins of every image it reads until it ends,
    // so neither cache can drop an input under it (workstream J).
    terrain::SectionRef mip(uint32_t key,int level,bool stockTerrain);
    // Worker only: evict least recently used, unpinned mips down to `target`.
    void evictMips(size_t target);
    terrain::Compositor& compositor_;
    // Worker only (all fields): the mip cache. A mip is pinned while a SectionRef to it
    // exists outside this map -- only the running job and mip()'s recursion hold one,
    // both on the worker, so use_count() is exact here.
    using MipId=std::tuple<uint32_t,int,bool>;
    struct Mip {terrain::SectionRef image;size_t bytes=0;uint64_t lastUse=0;};
    std::map<MipId,Mip> mips_;
    std::set<MipId> mipsEvicted_;   // evicted/trimmed at least once (regeneration count)
    uint64_t mipClock_=0;
    std::atomic<size_t> mipHigh_{kMipHigh},mipLow_{kMipLow};
    uint64_t mipIdleMs_=kMipIdleMs;   // render thread
    bool trimMips_=false;             // guarded by mutex_: the render thread asks, the worker trims
    uint64_t lastWorkMs_=0,lastWorkFrame_=0;size_t lastJobsSeen_=0;   // render thread
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> jobs_,done_;
    // Render thread: completed results taken from done_ and not uploaded yet (budget,
    // or deferred to uploadDeferred). readyHeld_ mirrors its size for the worker, which
    // stops composing while done_ + ready_ hold 8 results.
    std::vector<Job> ready_;
    std::atomic<size_t> readyHeld_{0};
    bool stop_=false,busy_=false;
    uint64_t epoch_=0,frame_=0,revision_=0;
    size_t bytes_=0,uploads_=0;
    // Telemetry: render-thread counters, plus worker-written atomics.
    uint64_t jobsQueued_=0,jobsCancelled_=0,jobsDiscarded_=0,jobsUsed_=0,uploadBytes_=0,uploadNs_=0;
    std::atomic<uint64_t> jobsCompleted_{0},composeNs_{0};
    std::atomic<size_t> mipBytes_{0},mipPinnedBytes_{0},mipPeakBytes_{0};
    std::atomic<uint64_t> mipEvictions_{0},mipRegens_{0},mipTrims_{0};
    std::map<Key,Pending> pending_;
    terrainwork::Params params_;
    terrainwork::Demand demand_;
    uint64_t seq_=0;
    size_t nearbyPending_=0,holesAfterPrepare_=0;
    bool deferred_=false;   // prepare() left non-urgent results for uploadDeferred()
    size_t coldBytes_=0;    // this frame: images off-view work may evict (see makeRoom)
    bool coldValid_=false;  // coldBytes_ computed this frame
    uint64_t jobsObsolete_=0,jobsNearby_=0,jobsOffview_=0,uploadsDeferred_=0,framesGated_=0;
    std::atomic<uint64_t> jobsAborted_{0};
    std::map<Key,Entry> cache_;
    std::vector<SDL_Texture*> free_[6];   // free texture slots per level
    size_t pooledBytes_=0;
    uint64_t slotCreates_=0,slotReuses_=0;
    std::vector<Key> visible_;
    std::vector<std::pair<int,int>> drawn_;   // chunk cells the last draw() covered (any level), sorted
    size_t drawnStandin_=0,drawnHoles_=0;     // the last draw(): visible cells shown by another level / not at all
    uint64_t wastedNs_=0;                     // render thread: compose time of results never uploaded
    std::atomic<uint64_t> wastedAbortNs_{0};  // worker: compose time of jobs cancelled while they ran
    int level_=0;
    bool linear_=false;
};
}
