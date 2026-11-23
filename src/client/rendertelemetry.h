#pragma once

// Shared renderer vocabulary for the rendering optimization program:
// per-frame telemetry, generation and
// revision numbers, and stable render handles. Everything here is PRESENTATION
// state. None of it may become an input to the deterministic simulation, enter
// World::stateHash(), or change what a frame draws.
//
// Telemetry contract
//   * FrameStats is owned by the render thread. Workers never write it: they
//     leave per-item results (UnitGeom::reused, vertex counts, ...) that the
//     render thread tallies after the parallel pass. No atomics, no locks.
//   * Recording is off unless a debug harness asks for it (TAK_RENDER_STATS,
//     or the render study; TAK_RENDER_STATS=0 turns it off inside the study to
//     measure the telemetry's own cost). A release build (NDEBUG) compiles
//     kTelemetryCompiled to false, so every tally, ScopedPhase and submit counter
//     folds away; a debug build pays one not-taken branch while off.
//   * Never allocate or format strings in a hot loop. format() runs once per
//     frame into a caller-owned buffer.
//   * Phase times ("t_") are CPU WALL time on the render thread, including any
//     worker wait inside that phase. Some phases NEST inside others (marked
//     below); never add the t_ values up. Worker times ("w_") are CPU time
//     SUMMED over every thread that ran the work -- not wall time, and not
//     comparable to t_ values. GPU time is reported by the render study as the
//     GL draw TIMELINE (GL_TIME_ELAPSED) -- it includes idle gaps waiting for CPU
//     submission and is NOT device busy time or utilization. Nothing here
//     measures GPU busy time (no portable per-process counter exists).
//
// Generation / revision vocabulary (use these words, not new ones)
//   * Generation: a monotonically increasing 64-bit number naming a LIFETIME.
//     0 (kNoGeneration) means "never valid". Anything that caches a resource
//     created under a generation stores that generation and treats a mismatch
//     as a miss. Generations defined for the renderer:
//       rendererGeneration -- SDL renderer / render-target lifetime. Bumped by
//                             GameView::invalidateRenderTargets (target or
//                             device reset) and GPU teardown. Every cached
//                             SDL_Texture*, GL buffer or atlas placement is
//                             only meaningful under the generation it was made.
//       assetGeneration    -- model / texture / palette registration lifetime
//                             (mod remount, model reload).
//       snapshotGeneration -- the published sim snapshot the render pinned.
//   * Revision: a per-entry counter that increments when that entry's OUTPUT
//     changes. Stages each own one: pose, material (body appearance), shadow
//     coverage, placement (screen position / clipping). A revision is a cheap
//     equality token for downstream caches; it is never derived from the
//     simulation tick alone (presentation inputs change between ticks), and it
//     never replaces exact validation while a scheme is being proven.
//   * RenderHandle: {index, generation} into a bounded slot table. A handle is
//     a stable identity for a cache entry that survives painter-order changes.
//     A reused index always carries a new generation, so a stale handle (a
//     destroyed unit, a reused unit id, a replaced model) can never alias the
//     new occupant.

#include <algorithm>
#include <functional>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace tak::render {

using Generation = uint64_t;
inline constexpr Generation kNoGeneration = 0;

// A lifetime counter. Starts at 1 so a default/zero stored generation never
// matches a live one.
class GenerationCounter {
public:
    Generation current() const { return value_; }
    Generation bump() { return ++value_; }
private:
    Generation value_ = 1;
};

struct RenderHandle {
    static constexpr uint32_t kNoIndex = UINT32_MAX;
    uint32_t index = kNoIndex;
    uint32_t generation = 0;
    bool valid() const { return index != kNoIndex && generation != 0; }
    friend bool operator==(const RenderHandle&, const RenderHandle&) = default;
};

// Bounded generational slot allocator. Owns only identities; the caller keeps
// the payload in a parallel store indexed by handle.index (which is stable:
// indices are recycled, never compacted while live). Acquire fails (invalid
// handle) at the capacity limit rather than growing without bound.
//
// Generations come from ONE counter for the whole table, so a generation is
// never handed out twice (until 2^32 acquisitions) -- not even for an index
// that trimTrailing() dropped and a later acquire recreated. Free indices are
// reused lowest first, which keeps live entries packed at the front so the
// tail can be trimmed once demand falls.
class HandleSlots {
public:
    explicit HandleSlots(uint32_t capacity) : capacity_(capacity) {}
    RenderHandle acquire() {
        uint32_t index;
        if (!free_.empty()) {
            std::pop_heap(free_.begin(), free_.end(), std::greater<>{});
            index = free_.back(); free_.pop_back();
        } else if (generations_.size() < capacity_) {
            index = uint32_t(generations_.size());
            generations_.push_back(0);
            live_.push_back(false);
        } else return {};
        // Skip generation 0 on wrap so a recycled slot never looks unallocated.
        if (++nextGeneration_ == 0) ++nextGeneration_;
        generations_[index] = nextGeneration_;
        live_[index] = true;
        ++liveCount_;
        return {index, generations_[index]};
    }
    // Drop free indices at the END of the table (render thread, between uses).
    // Returns the new allocated() size; never drops a live index.
    uint32_t trimTrailing(uint32_t keep = 0) {
        size_t size = generations_.size();
        while (size > keep && !live_[size - 1]) --size;
        if (size == generations_.size()) return allocated();
        generations_.resize(size);
        live_.resize(size);
        free_.erase(std::remove_if(free_.begin(), free_.end(),
                                   [&](uint32_t i) { return i >= size; }), free_.end());
        std::make_heap(free_.begin(), free_.end(), std::greater<>{});
        return allocated();
    }
    bool valid(RenderHandle h) const {
        return h.valid() && h.index < generations_.size() && live_[h.index] &&
               generations_[h.index] == h.generation;
    }
    bool release(RenderHandle h) {
        if (!valid(h)) return false;
        live_[h.index] = false;
        free_.push_back(h.index);
        std::push_heap(free_.begin(), free_.end(), std::greater<>{});
        --liveCount_;
        return true;
    }
    uint32_t live() const { return liveCount_; }
    uint32_t allocated() const { return uint32_t(generations_.size()); }
    uint32_t capacity() const { return capacity_; }
    void setCapacity(uint32_t capacity) { capacity_ = capacity; }   // affects later acquires only
private:
    uint32_t capacity_;
    uint32_t liveCount_ = 0;
    uint32_t nextGeneration_ = 0;
    std::vector<uint32_t> generations_;
    std::vector<bool> live_;
    std::vector<uint32_t> free_;
};

// ---- Telemetry ---------------------------------------------------------------

#ifdef NDEBUG
inline constexpr bool kTelemetryCompiled = false;
#else
inline constexpr bool kTelemetryCompiled = true;
#endif

// CPU phases, render thread. Keep names stable: tools/render_benchmark.py
// parses them from the RENDERSTATS line (prefix "t_").
enum class Phase : uint8_t {
    Candidates,      // unit/feature candidate selection + fog/frustum culls
    PainterSort,     // painter-list stable sort
    AtlasMaintain,   // colour atlases, palette pages, animated texture repaint
    GeometryPrep,    // parallel buildUnitGeom (key evaluation + prep), incl. wait
    DistantPrepare,  // distant-image cache lookup / bakes
    ShadowPack,      // shadow atlas allocation, packing, coverage bakes
    ShadowComposite, // ground + air shadow composition draws
    BodyAssembly,    // planning + parallel vertex copy into the body buffer
    BodySubmit,      // body draw-op replay (features, special units, batches)
    Terrain,         // MapView::draw: chunk scheduling, uploads, terrain draws
    TerrainUpload,   // (nested in Terrain) chunk + section texture create/upload
    ShadowAlloc,     // (nested in ShadowPack) tile packing, page allocation, dirty stamps
    ShadowPrep,      // (nested in ShadowPack) coverage vertex transform into tiles, incl. wait
    ShadowRaster,    // (nested in ShadowPack) tile clears + coverage submission
    PoolDispatch,    // (nested) waking the worker pool for a parallel pass
    PoolWait,        // (nested) caller idle after its own chunks, waiting for workers
    Effects,         // particles, effects, unit fx
    Fog,             // fog-of-war overlay
    SchedulePlan,    // (nested) building the cost plans of the pool passes (workstream I)
    HiddenUnits,     // HIDDEN UNITS option: cover search, fade and dot tests
    Cursor,          // the in-game cursor's classification (desiredCursor), after the world pass
    Ghosts,          // the queued-build ghosts of the local player's builders (world pass)
    BuildGhosts,     // the same scope as Ghosts, under workstream E's name (drawGhostAt loop)
    Count
};

// CPU time summed over every thread that ran the work (prefix "w_"). Workers
// leave per-item nanoseconds; the render thread folds them in.
enum class Work : uint8_t {
    GeometryKey,     // building + comparing the exact geometry key (hits and misses)
    GeometryBuild,   // rebuilding geometry after a miss (body + shadow)
    PoolBusy,        // every participant's time inside pool chunks (caller included)
    PoolWake,        // dispatch -> each worker's first chunk (wake latency, summed)
    TerrainCompose,  // terrain chunk worker: mip generation + composition
    TerrainUpload,   // render thread: chunk + section texture create/upload
    SectionDecode,   // section JPEG decode worker
    TerrainWasted,   // terrain chunk worker time on jobs never uploaded (cancelled, discarded)
    UnitGridBuild,   // capture thread: Frame::unitGrid build, tallied on the first frame showing it (workstream L)
    Count
};

enum class Counter : uint8_t {
    UnitsScanned,        // snapshot units examined for the painter list
    UnitCandidates,      // units that passed fog + frustum (painter items)
    FeatureCandidates,   // feature-index candidates examined
    FeaturesPainted,     // features that entered the painter list
    PainterItems,
    VisibleUnits,        // visUnits_ (units with a geometry slot this frame)
    SpecialUnits,        // drawn whole through drawUnit (clip/effects/build)
    DistantImages,       // bodies drawn from a distant image
    DistantBakes,
    GeometryHits,
    GeometryMisses,      // total; by-cause breakdown is MissCause
    VertsGenerated,      // body vertices generated by local rebuilds this frame (since D: not re-placements)
    ShadowVertsGenerated,
    BodyVertsCopied,     // vertices assembled into the shared body buffer
    BodyBytesCopied,
    BodySubmissions,     // logical batched body submissions (API calls)
    ShadowUnits,         // units with shadow coverage packed in the atlas
    ShadowTilesReused,
    ShadowTilesDirty,
    ShadowDirtyPixels,   // area of re-rasterized coverage tiles (incl. padding)
    ShadowPages,
    AtlasRegionsPainted, // animated-texture atlas regions repainted
    AtlasPixelsPainted,
    TargetSwitches,      // render-target changes the renderer made (where counted)
    GeomPoolEntries,     // live per-unit geometry entries (visible + retained; workstream C)
    GeomPoolUsedBytes,   // bytes in use across retained geometry vectors
    GeomPoolCapacityBytes, // bytes retained (capacity) across those vectors
    BodyBufferCapacityBytes,
    TerrainChunkBytes,   // resident terrain chunk textures (GPU, tracked)
    TerrainChunkImages,
    TerrainChunkUploads, // cumulative
    TerrainChunkPending,
    TerrainSectionBytes,
    GpuTrackedBytes,     // gpuvram tracked textures + buffers (excl. driver internals)
    // ---- added by workstream A (counts per frame unless noted) ----
    GeometryOwnerReassign, // an entry rebuilt for a different unit than its last owner (0 since workstream C)
    InvalPose,           // misses whose inputs changed in pose (pieces, heading incl. emote spin, pitch/roll, corpse)
    InvalMaterial,       // ... body appearance (palette, tint, opacity, veterancy, emotes, construction, texture frame)
    InvalPlacement,      // ... screen anchor / clip (interpolated position, lift, sink, altitude, emote bob, occlusion)
    InvalZoom,           // ... zoom
    InvalResource,       // ... type, model, atlas texture, piece table
    InvalSettings,       // ... shadows toggle
    GeomReallocs,        // per-unit geometry vector capacity growths during this frame's builds
    FrameReallocs,       // capacity growths of the per-frame lists, body buffer and shadow scratch
    ShadowDirtyNew,      // dirty tiles by cause (one cause each, first that applies)
    ShadowDirtyOwner,    //   tile now holds a different geometry slot / unit
    ShadowDirtyContent,  //   geometry revision changed
    ShadowDirtyRelocated,//   packer moved the tile to another slot position
    ShadowDirtyResized,  //   tile size or output scale changed
    ShadowDirtyMoved,    //   screen pixel position changed (placement only)
    ShadowDirtyForced,   //   TAK_SHADOW_NOCACHE
    ShadowPageAllocs,    // silhouette pages created this frame (growth or resize)
    ShadowFallback,      // 1 when the silhouette atlas fell back to direct shadows
    ShadowDirectUnits,   // unit shadows drawn through the direct (non-atlas) path
    DistantHits,         // distant image reused without a bake
    DistantHeld,         // inputs changed but reused inside the refresh interval
    DistantMissNew,      // bake: no entry / invalid entry
    DistantMissScale,    // bake: zoom or output scale changed
    DistantMissContent,  // bake: revision / texture tick / source changed and interval elapsed
    DistantRejectBudget, // over the per-frame bake budget: live geometry instead
    DistantRejectShape,  // translucent, too large or degenerate: live geometry instead
    DistantRejectPages,  // no page room / VRAM budget: live geometry instead
    AtlasRepaintPasses,  // frames whose animated-texture update repainted any atlas region
    AtlasCopies,         // texture copies into atlases (body + gutters; creation included)
    AtlasFlushes,        // explicit flushes in atlas painting (creation included)
    AtlasCreated,        // colour atlases created this frame
    LogicalDraws,        // draw calls issued by counted world paths (see SubmitCounters)
    NativeDraws,         // glDrawArrays issued by the native GL batch paths
    SdlGeometryDraws,    // batches that fell back to SDL_RenderGeometry
    RetainedDraws,       // native draws from a retained GL buffer (no upload)
    Flushes,             // explicit SDL_RenderFlush calls on counted paths
    VertsSubmitted,      // vertices handed to native GL or SDL by counted batch paths
    BytesUploaded,       // vertex bytes those paths handed to GL/SDL (sizeof-based)
    TextureUploadBytes,  // terrain chunk + section texture upload bytes
    TerrainJobsQueued,
    TerrainJobsCompleted,// composed by the worker (may still be discarded)
    TerrainJobsCancelled,// cancelled while pending (clear / invalidate)
    TerrainJobsDiscarded,// finished but not uploaded (cancelled, failed, left the view, no room)
    TerrainJobsUsed,     // uploaded into the chunk cache
    TerrainSectionUploads,
    TerrainSectionDecodes,
    TerrainMipCpuBytes,  // gauge: worker-owned mip images (CPU)
    TerrainSectionCpuBytes, // gauge: decoded section images held by the compositor (CPU)
    ShadowScratchUsedBytes,
    ShadowScratchCapacityBytes,
    FrameListsUsedBytes,     // painter list, draw ops, copy tasks, visible lists, distant items
    FrameListsCapacityBytes,
    BodyBufferUsedBytes,
    GpuTextureCreates,   // gpuvram create() calls this frame
    GpuTextureDestroys,
    PoolDispatches,      // parallel passes that woke the worker pool
    PoolSerialRuns,      // passes run serially (below the parallel threshold)
    PoolItems,           // items handed to either kind of pass
    // ---- added by workstream C (stable geometry entries, client/geometryowners.h) ----
    GeomEntryAcquired,   // fresh (cold) entries handed to a unit this frame
    GeomEntryReplaced,   // ... of which the id had an entry under another identity (epoch/type/model)
    GeomEntryReleasedDestroyed, // entries of units no longer in the snapshot (or changed identity)
    GeomEntryReleasedExpired,   // absent longer than the retention window
    GeomEntryReleasedBudget,    // absent entries released by the retained byte/count budget
    GeomEntryTrimmed,    // released entries whose memory was returned
    GeomEntryRefused,    // claims refused (duplicate owner in a frame, index bound)
    GeomEntriesRetained, // gauge: live entries not in view this frame
    GeomEntriesWarmFree, // gauge: released entries still holding capacity
    GeomEntriesAllocated,// gauge: entry index space (live + free)
    GeomRetainedBytes,   // gauge: bytes held by retained (absent) entries
    GeomWarmFreeBytes,   // gauge: bytes held by released entries awaiting reuse/trim
    // ---- added by workstream D (staged unit geometry, client/geometrystages.h) ----
    GeomLocalBuilds,     // units whose local stage was rebuilt (transforms, collection, sort, shadow)
    GeomPlacementOnly,   // units re-placed from retained local geometry (no local rebuild)
    GeomMaterialOnly,    // units re-coloured from retained local geometry (no local rebuild)
    VertsPlaced,         // body + shadow vertices re-placed without a local rebuild
    VertsRecoloured,     // body vertices re-coloured without a local rebuild
    GeomShadowKept,      // local rebuilds whose shadow coverage came out identical (shadowRevision kept)
    // Workstream E (split body and shadow invalidation):
    GeomShadowSkipped,   // local rebuilds whose shadow inputs were unchanged: silhouette not collected
    ShadowReusedBody,    // tiles kept although the body changed (the pre-E key redrew them)
    ShadowReusedTranslated,  // tiles kept after a placement change: every tile point bit-identical
    // Workstream F (persistent shadow tile cells, client/shadowatlas.h):
    ShadowCellAllocs,    // cells allocated this frame (new tile, outgrown/oversized cell, compaction, migration)
    ShadowCellKept,      // tiles drawn in the cell they held last frame (content may still be redrawn)
    ShadowCellFreed,     // cells freed this frame (any reason)
    ShadowCellExpired,   //   ... absent from the atlas longer than the retention window
    ShadowCellEvicted,   //   ... absent cells freed under space pressure or by compaction
    ShadowCompactions,   // full repacks forced by a failed allocation in a fragmented atlas
    ShadowCompactionsFree, // full repacks of a loose atlas on a frame that redraws nearly every tile anyway
    ShadowMigrated,      // tiles moved off a sparse last page (bounded reclamation)
    ShadowPageReleases,  // silhouette pages destroyed after staying empty
    ShadowPageShrinks,   // page-size reductions after sustained low demand
    ShadowCells,         // gauge: live cells (this frame's tiles + retained)
    ShadowCellsRetained, // gauge: cells of units not in this frame's atlas
    ShadowAtlasCapacityPx, // gauge: live pages x page area
    ShadowAtlasCellPx,   // gauge: allocated cell area
    ShadowAtlasTilePx,   // gauge: this frame's tile area (incl. 1 px padding)
    ShadowAtlasShelfFreePx, // gauge: free spans inside shelves (fragmentation)
    ShadowAtlasGpuBytes, // gauge: silhouette page bytes (tracked GPU textures)
    // Workstream G (distant-image lookup before body generation, client/distantmodels.h):
    DistantBodyDeferred, // units whose body was not regenerated: their image is held this frame
    DistantBodyResumed,  // pending bodies regenerated by the parallel pass (image no longer held)
    DistantBodyCatchup,  // deferred bodies regenerated after prepare (image not shown after all)
    DistantBodyLate,     // deferred bodies prepare() had to regenerate for a bake (prediction missed)
    DistantBoundsReused, // repaint candidates whose bounds came from retained metadata (no vertex scan)
    DistantPageReleases, // image pages released after their last image expired (others still live)
    DistantMigrated,     // images moved to a lower page at the same tile position (bounded)
    DistantPages,        // gauge: live image pages
    DistantTiles,        // gauge: live images (tiles in use)
    DistantMetadataBytes,// gauge: CPU bytes of the image cache's maps, page tables and scratch
    // Workstream H (retained body vertex arena, client/vertexarena.h). The
    // frame-mode counters are 0/1 per frame in which body commands were planned.
    ArenaAppend,         // frame drew from the arena, changed sources appended
    ArenaRebuild,        // frame drew from the arena after rewriting it (orphan + contiguous)
    ArenaStream,         // frame streamed (assembled) its native-size commands: unstable / budget / off
    ArenaVertsRetained,  // body vertices drawn from resident arena ranges (nothing copied or uploaded)
    ArenaVertsUploaded,  // body vertices written into the arena (also in body_verts_copied, bytes_uploaded)
    ArenaVertsTwice,     // of those, vertices also copied into a small (assembled) command this frame:
                         // an uploaded source goes up whole, so its runs drawn assembled are copied twice
    ArenaDraws,          // commands drawn from the arena (one native draw each)
    ArenaRanges,         // merged ranges those draws covered (glMultiDrawArrays entries)
    ArenaFallbacks,      // arena commands drawn assembled after all (write failure, texture check)
    ArenaReleases,       // storage released (idle, GPU pressure, failure)
    ArenaCapacityBytes,  // gauge: arena storage (tracked GPU buffer bytes)
    ArenaInflightBytes,  // gauge: orphaned/released storage still counted (in flight)
    ArenaCpuBytes,       // gauge: planner records, per-frame command lists, staging, mirror
    // Workstream I (cost-planned pool passes, client/workschedule.h).
    PoolPosted,          // workers woken by parallel passes (participants minus the caller)
    PoolIdleWakes,       // woken workers that found every chunk already taken
    PoolChunks,          // chunks handed out by parallel passes
    SchedGeometryThreads,// participants of the geometry pass (1 = ran on the render thread)
    SchedShadowThreads,  // participants of the shadow check + prep passes (summed)
    SchedCopyThreads,    // participants of the body copy pass
    SchedGeometryEstimateUs, // the geometry plan's estimated work, microseconds
    // Workstream J (CPU cache budgets). Gauges unless noted; "pinned" = held by an
    // active consumer (not evictable now), "peak" = highest since the view was built.
    TerrainSectionPinnedBytes, // decoded sections a consumer holds (compose job, upload)
    TerrainSectionPeakBytes,
    TerrainSectionEvictions,   // per frame: decoded sections dropped by the CPU budget
    TerrainSectionRedecodes,   // per frame: decodes of sections evicted earlier
    TerrainSectionPrefetchSkipped, // per frame: load-time prefetches refused (budget full)
    TerrainSectionYoungBytes,  // unpinned sections used within the working-set window (not evictable)
    TerrainMipPinnedBytes,     // mip images the running compose job holds
    TerrainMipPeakBytes,
    TerrainMipEvictions,       // per frame: mip images dropped by the CPU budget
    TerrainMipRegens,          // per frame: mip images regenerated after an eviction/trim
    TerrainMipTrims,           // per frame: idle trims of the mip cache (sustained low demand)
    CpuTrims,                  // per frame: retained renderer buffers shrunk after low demand
    CpuTrimBytes,              // per frame: capacity those trims released
    WorkerScratchUsedBytes,    // render workers' per-thread scratch: last use
    WorkerScratchCapacityBytes,// ... retained capacity
    GeomPinnedBytes,           // geometry entries of units in view (not evictable)
    CpuCacheUsedBytes,         // totals over the renderer's CPU caches and buffers
    CpuCacheRetainedBytes,     //   (see the J accounting table in the progress record)
    CpuCachePinnedBytes,
    CpuCacheEvictableBytes,
    CpuCachePeakBytes,         // highest CpuCacheRetainedBytes seen at a frame end
    // Workstream K (camera-prioritized terrain). Gauges of the last terrain draw:
    // visible chunk cells, of which at the wanted level / shown by another level's
    // image (stand-in) / without any image (holes: the section fallback draws them);
    // fallback tiles whose section was not resident (the blurry underlay shows).
    TerrainChunkVisible,
    TerrainChunkReady,
    TerrainChunkStandin,
    TerrainChunkHoles,
    TerrainUnderlayTiles,
    // Per frame: queued/running jobs cancelled as obsolete (outside the keep band,
    // past the level-change grace, or displaced from a full queue by visible work);
    // running jobs the worker stopped on such a cancel; nearby (leading-edge) jobs
    // queued; results uploaded for cells not visible at that moment (nearby or kept
    // speculative work); completed results whose upload the frame budget postponed.
    TerrainJobsObsolete,
    TerrainJobsAborted,
    TerrainJobsNearby,
    TerrainJobsOffview,
    TerrainUploadDeferred,
    TerrainGatedFrames,  // 1 when the calm gate queued no new terrain work this frame (fast motion / jump storm)
    // Unit broad phase (workstream L): live units in the snapshot; grid queries this
    // frame (painter candidates + screen overlays); units they returned (summed);
    // grid rows they visited; queries answered with the whole list (full-scan
    // reference, or a grid that does not match the live list); grid capacity bytes
    // of the shown snapshot.
    UnitsLive,
    UnitIndexQueries,
    UnitIndexReturned,
    UnitIndexRows,
    UnitIndexFullScans,
    UnitGridBytes,
    // Structure palette sheets (workstream M, client/palettesheets.h): the form held
    // (gauge 0 none / 1 baked / 2 indexed), layout pages, tracked GPU bytes of the
    // form (baked pages, or key textures + index pages + identity + LUT), CPU bytes
    // of the sources, banks and sheet records; per frame: texture bytes uploaded for
    // sheets, runs drawn by the indexed program, indexed runs that failed to draw,
    // form switches.
    PaletteMode,
    PalettePages,
    PaletteGpuBytes,
    PaletteCpuBytes,
    PaletteUploadBytes,
    PaletteIndexedDraws,
    PaletteFallbacks,
    PaletteSwitches,
    PaletteSpans,        // native body passes holding indexed palette runs (one priming each)
    PaletteSpanCommands, // body commands drawn inside those passes (palette + plain)
    // Workstream N (client/nativepass.h): native passes opened in the body replay,
    // body commands drawn inside them, passes refused (SDL priming failed, or the
    // primed state was not the expected one: the commands drew one by one), and
    // every SDL priming (degenerate draw + flush) on the counted native paths --
    // the state transition N reduces; a subset of `flushes`.
    NativePasses,
    NativePassCommands,
    NativePassRefused,
    NativePrimes,
    // HIDDEN UNITS (client/hiddenunits.h): eligible ground units looked at, of which
    // a later cover reaches (candidates; DOTS: overlaps), units given a dot (DOTS),
    // and the covers drawn faded (FADE: fading out, faded, or fading back in).
    HiddenEligible,
    HiddenCandidates,
    HiddenMarked,
    HiddenFaded,
    // Interactive render cases (tools/render_benchmark.py --scenes interactive):
    // cursor classifications run (desiredCursor), the precise per-unit pick tests
    // (unitUnderCursor) since the frame began and those that hit; queued-build ghosts
    // the overlay pass asked for and those of them drawGhostAt found a model for and
    // prepared (with workstream E's retained geometry most reuse it: ghost_collects
    // counts the real collections); HIDDEN UNITS FADE coverage tests run against a cover
    // (sprite texels or structure triangles); and the frame's shadow tile area (incl.
    // the 1 px padding) as the atlas is asked for it, recorded whether or not the
    // atlas then takes it (shadow_atlas_tile_px is 0 on a fallback frame).
    CursorEvals,
    CursorPickTests,
    CursorPickHits,
    GhostsQueued,
    GhostPreps,
    HiddenCoverTests,
    ShadowDemandPx,
    // Build ghosts (GameView::drawGhostAt: queued builds, placed sites, the placement
    // preview): ghosts requested; of which rejected before any geometry work because
    // they lie wholly outside the clip; model collections + depth sorts run; ghosts
    // drawn from retained model-local geometry; triangles submitted; gauge: bytes
    // of the retained ghost geometry.
    GhostDraws,
    GhostCulled,
    GhostCollects,
    GhostCacheHits,
    GhostTris,
    GhostCacheBytes,
    // FADE's cover bookkeeping (perf follow-up F): covers the unit queries handed out
    // (each one an exact test before the per-frame bookkeeping) and exact cover tests
    // (rectangle, then sprite/triangles) asked of them; hidden_cover_tests above counts
    // only the sprite/triangle tests those ran.
    HiddenCoverVisits,
    HiddenCoverReaches,
    // Shadow atlas capacity (perf follow-up K): composite submissions (one per run
    // of painter-order neighbours on one silhouette page, plus one per shadow drawn
    // directly) and the page limit that applied (gauge). The tile area the frame
    // asked for is ShadowDemandPx above (measurement B and K defined the same gauge;
    // K's sum is kept, which also covers the units examined before an early failure).
    ShadowCompositeRuns,
    ShadowPageLimit,
    // ... live pages past the base pages (gauge; client/shadowatlas.h), and the
    // atlas resets that gave them back under GPU pressure (per frame).
    ShadowExtendedPages,
    ShadowPressureSheds,
    // Terrain chunk texture slots (client/terrainchunks.h): textures created and slots
    // reused (cumulative), and the free slots' bytes (gauge).
    TerrainSlotCreates,
    TerrainSlotReuses,
    TerrainSlotPooledBytes,
    // Section fallback textures and shadow pages kept for reuse: reuses (cumulative)
    // and kept bytes (gauge).
    TerrainSectionReuses,
    TerrainSectionPooledBytes,
    ShadowPageReuses,
    ShadowSparePageBytes,
    // World effects in the painter list (collectWorldFx): items collected, of
    // them queued by depth, and the draw ops (runs) they made in the replay.
    FxItems,
    FxQueued,
    FxOps,
    Count
};

// Why a retained entry could not be reused. Phase 0 can only attribute
// "Cold" (no prior entry for this unit in the slot) vs "Changed" (exact key
// mismatch); later workstreams split Changed into the specific stages.
// Since workstream A the "Changed" misses are attributed to the first stage
// whose inputs changed (Resource > Settings > Pose > Material > Zoom >
// Placement); "changed" remains only for a difference no stage claims. The
// Inval* counters count every stage that changed, so one miss may add to several.
enum class MissCause : uint8_t {
    Cold, Changed, Owner, Pose, Material, Placement, Resource, Settings, Reference, Zoom, Count
};

inline constexpr const char* kPhaseNames[size_t(Phase::Count)] = {
    "candidates", "painter_sort", "atlas", "geometry", "distant", "shadow_pack",
    "shadow_composite", "body_assembly", "body_submit", "terrain", "terrain_upload",
    "shadow_alloc", "shadow_prep", "shadow_raster", "pool_dispatch", "pool_wait", "effects",
    "fog", "schedule_plan", "hidden_units", "cursor", "ghosts", "build_ghosts"};
inline constexpr const char* kWorkNames[size_t(Work::Count)] = {
    "geometry_key", "geometry_build", "pool_busy", "pool_wake", "terrain_compose",
    "terrain_upload", "section_decode", "terrain_wasted", "unit_grid_build"};
inline constexpr const char* kCounterNames[size_t(Counter::Count)] = {
    "units_scanned", "unit_candidates", "feature_candidates", "features_painted",
    "painter_items", "visible_units", "special_units", "distant_images", "distant_bakes",
    "geometry_hits", "geometry_misses", "verts_generated", "shadow_verts_generated",
    "body_verts_copied", "body_bytes_copied", "body_submissions", "shadow_units",
    "shadow_tiles_reused", "shadow_tiles_dirty", "shadow_dirty_pixels", "shadow_pages",
    "atlas_regions", "atlas_pixels", "target_switches", "geom_entries", "geom_used_bytes",
    "geom_capacity_bytes", "body_capacity_bytes", "terrain_chunk_bytes",
    "terrain_chunk_images", "terrain_chunk_uploads", "terrain_chunk_pending",
    "terrain_section_bytes", "gpu_tracked_bytes",
    "geometry_owner_reassign", "inval_pose", "inval_material", "inval_placement", "inval_zoom",
    "inval_resource", "inval_settings", "geom_reallocs", "frame_reallocs",
    "shadow_dirty_new", "shadow_dirty_owner", "shadow_dirty_content", "shadow_dirty_relocated",
    "shadow_dirty_resized", "shadow_dirty_moved", "shadow_dirty_forced", "shadow_page_allocs",
    "shadow_fallback", "shadow_direct_units",
    "distant_hits", "distant_held", "distant_miss_new", "distant_miss_scale",
    "distant_miss_content", "distant_reject_budget", "distant_reject_shape",
    "distant_reject_pages",
    "atlas_repaint_passes", "atlas_copies", "atlas_flushes", "atlas_created",
    "logical_draws", "native_draws", "sdl_geometry_draws", "retained_draws", "flushes",
    "verts_submitted", "bytes_uploaded", "texture_upload_bytes",
    "terrain_jobs_queued", "terrain_jobs_completed", "terrain_jobs_cancelled",
    "terrain_jobs_discarded", "terrain_jobs_used", "terrain_section_uploads",
    "terrain_section_decodes", "terrain_mip_cpu_bytes", "terrain_section_cpu_bytes",
    "shadow_scratch_used_bytes", "shadow_scratch_capacity_bytes", "frame_lists_used_bytes",
    "frame_lists_capacity_bytes", "body_used_bytes", "gpu_texture_creates",
    "gpu_texture_destroys", "pool_dispatches", "pool_serial_runs", "pool_items",
    "geom_entry_acquired", "geom_entry_replaced", "geom_entry_released_destroyed",
    "geom_entry_released_expired", "geom_entry_released_budget", "geom_entry_trimmed",
    "geom_entry_refused", "geom_entries_retained", "geom_entries_warm_free",
    "geom_entries_allocated", "geom_retained_bytes", "geom_warm_free_bytes",
    "geom_local_builds", "geom_placement_only", "geom_material_only", "verts_placed",
    "verts_recoloured", "geom_shadow_kept", "geom_shadow_skipped", "shadow_reused_body",
    "shadow_reused_translated",
    "shadow_cell_allocs", "shadow_cell_kept", "shadow_cell_freed", "shadow_cell_expired",
    "shadow_cell_evicted", "shadow_compactions", "shadow_compactions_free", "shadow_migrated", "shadow_page_releases",
    "shadow_page_shrinks", "shadow_cells", "shadow_cells_retained", "shadow_atlas_capacity_px",
    "shadow_atlas_cell_px", "shadow_atlas_tile_px", "shadow_atlas_shelf_free_px",
    "shadow_atlas_gpu_bytes",
    "distant_body_deferred", "distant_body_resumed", "distant_body_catchup", "distant_body_late",
    "distant_bounds_reused", "distant_page_releases", "distant_migrated", "distant_pages",
    "distant_tiles", "distant_metadata_bytes",
    "arena_append", "arena_rebuild", "arena_stream", "arena_verts_retained", "arena_verts_uploaded",
    "arena_verts_twice", "arena_draws", "arena_ranges", "arena_fallbacks", "arena_releases", "arena_capacity_bytes",
    "arena_inflight_bytes", "arena_cpu_bytes",
    "pool_posted", "pool_idle_wakes", "pool_chunks", "sched_geometry_threads", "sched_shadow_threads",
    "sched_copy_threads", "sched_geometry_estimate_us",
    "terrain_section_pinned_bytes", "terrain_section_peak_bytes", "terrain_section_evictions",
    "terrain_section_redecodes", "terrain_section_prefetch_skipped", "terrain_section_young_bytes",
    "terrain_mip_pinned_bytes",
    "terrain_mip_peak_bytes", "terrain_mip_evictions", "terrain_mip_regens", "terrain_mip_trims",
    "cpu_trims", "cpu_trim_bytes", "worker_scratch_used_bytes", "worker_scratch_capacity_bytes",
    "geom_pinned_bytes", "cpu_cache_used_bytes", "cpu_cache_retained_bytes",
    "cpu_cache_pinned_bytes", "cpu_cache_evictable_bytes", "cpu_cache_peak_bytes",
    "terrain_chunk_visible", "terrain_chunk_ready", "terrain_chunk_standin", "terrain_chunk_holes",
    "terrain_underlay_tiles", "terrain_jobs_obsolete", "terrain_jobs_aborted", "terrain_jobs_nearby",
    "terrain_jobs_offview", "terrain_upload_deferred", "terrain_gated_frames",
    "units_live", "unit_index_queries", "unit_index_returned", "unit_index_rows",
    "unit_index_full_scans", "unit_grid_bytes",
    "palette_mode", "palette_pages", "palette_gpu_bytes", "palette_cpu_bytes", "palette_upload_bytes",
    "palette_indexed_draws", "palette_fallbacks", "palette_switches", "palette_spans",
    "palette_span_commands", "native_passes", "native_pass_commands", "native_pass_refused",
    "native_primes", "hidden_eligible", "hidden_candidates", "hidden_marked", "hidden_faded",
    "cursor_evals", "cursor_pick_tests", "cursor_pick_hits", "ghosts_queued", "ghost_preps", "hidden_cover_tests",
    "shadow_demand_px", "ghost_draws", "ghost_culled", "ghost_collects", "ghost_cache_hits", "ghost_tris",
    "ghost_cache_bytes", "hidden_cover_visits", "hidden_cover_reaches", "shadow_composite_runs",
    "shadow_page_limit", "shadow_extended_pages", "shadow_pressure_sheds",
    "terrain_slot_creates", "terrain_slot_reuses", "terrain_slot_pooled_bytes",
    "terrain_section_reuses", "terrain_section_pooled_bytes", "shadow_page_reuses",
    "shadow_spare_page_bytes", "fx_items", "fx_queued", "fx_ops"};
inline constexpr const char* kMissNames[size_t(MissCause::Count)] = {
    "cold", "changed", "owner", "pose", "material", "placement", "resource", "settings",
    "reference", "zoom"};

// A missing name would print "(null)" keys; the last entry of each table must exist.
static_assert(kPhaseNames[size_t(Phase::Count) - 1] != nullptr);
static_assert(kWorkNames[size_t(Work::Count) - 1] != nullptr);
static_assert(kCounterNames[size_t(Counter::Count) - 1] != nullptr);
static_assert(kMissNames[size_t(MissCause::Count) - 1] != nullptr);

// Counted draw/flush/upload work on the shared submission paths (GeometrySubmit,
// OpaqueShadowSubmit, terrain and the world pass). Render thread only. These
// tick in every debug build -- a few integer adds per draw CALL, never per
// vertex -- and GameView moves them into FrameStats once per frame. Paths that
// call SDL directly without counting (HUD, most effects) are not included, so
// these are lower bounds on the frame's SDL work.
struct SubmitCounters {
    uint64_t logical = 0, native = 0, sdl = 0, retained = 0, flushes = 0, targetSwitches = 0,
             verts = 0, bytes = 0, textureBytes = 0, primes = 0;
};
inline SubmitCounters gSubmit;
inline void countLogicalDraw(uint64_t n = 1) { if constexpr (kTelemetryCompiled) gSubmit.logical += n; }
inline void countNativeDraw(uint64_t verts, uint64_t bytes, bool retained) {
    if constexpr (kTelemetryCompiled) {
        ++gSubmit.logical; ++gSubmit.native; gSubmit.verts += verts;
        if (retained) ++gSubmit.retained; else gSubmit.bytes += bytes;
    }
}
inline void countSdlGeometry(uint64_t verts, uint64_t bytes) {
    if constexpr (kTelemetryCompiled) { ++gSubmit.logical; ++gSubmit.sdl; gSubmit.verts += verts; gSubmit.bytes += bytes; }
}
inline void countUploadBytes(uint64_t bytes) { if constexpr (kTelemetryCompiled) gSubmit.bytes += bytes; }
inline void countFlush() { if constexpr (kTelemetryCompiled) ++gSubmit.flushes; }
// One SDL priming of native GL state (a degenerate draw + flush; counts the flush too).
inline void countPrime() { if constexpr (kTelemetryCompiled) { ++gSubmit.flushes; ++gSubmit.primes; } }
inline void countTargetSwitch() { if constexpr (kTelemetryCompiled) ++gSubmit.targetSwitches; }
inline void countTextureUpload(uint64_t bytes) { if constexpr (kTelemetryCompiled) gSubmit.textureBytes += bytes; }

struct FrameStats {
    bool on = false;   // sampled once per frame by the owner; guards every tally
    std::array<double, size_t(Phase::Count)> ms{};
    std::array<double, size_t(Work::Count)> work{};   // summed thread CPU ms
    std::array<uint64_t, size_t(Counter::Count)> n{};
    std::array<uint64_t, size_t(MissCause::Count)> miss{};
    void begin(bool enabled) {
        on = kTelemetryCompiled && enabled; ms.fill(0); work.fill(0); n.fill(0); miss.fill(0);
    }
    bool enabled() const { return kTelemetryCompiled && on; }
    void add(Counter c, uint64_t v = 1) { if (enabled()) n[size_t(c)] += v; }
    void set(Counter c, uint64_t v) { if (enabled()) n[size_t(c)] = v; }
    void addMiss(MissCause c, uint64_t v = 1) { if (enabled()) miss[size_t(c)] += v; }
    void addMs(Phase p, double v) { if (enabled()) ms[size_t(p)] += v; }
    void addWorkNs(Work w, uint64_t ns) { if (enabled()) work[size_t(w)] += double(ns) * 1e-6; }
    uint64_t get(Counter c) const { return n[size_t(c)]; }

    // One line: "t_<phase>=ms ... w_<work>=ms ... <counter>=n ... miss_<cause>=n".
    // Returns the length written (truncated output still NUL-terminates).
    int format(char* out, size_t size) const {
        int used = 0;
        if (size) out[0] = 0;
        const auto put = [&](const char* fmt, const char* key, auto value) {
            if (size_t(used) >= size) return;
            const int w = std::snprintf(out + used, size - size_t(used), fmt, key, value);
            if (w > 0) used = std::min(used + w, int(size) - 1);
        };
        for (size_t i = 0; i < ms.size(); ++i) put(" t_%s=%.4f", kPhaseNames[i], ms[i]);
        for (size_t i = 0; i < work.size(); ++i) put(" w_%s=%.4f", kWorkNames[i], work[i]);
        for (size_t i = 0; i < n.size(); ++i)
            put(" %s=%llu", kCounterNames[i], static_cast<unsigned long long>(n[i]));
        for (size_t i = 0; i < miss.size(); ++i)
            put(" miss_%s=%llu", kMissNames[i], static_cast<unsigned long long>(miss[i]));
        return used;
    }
};

// Accumulates elapsed CPU time into one phase while in scope, only when the
// stats are on. steady_clock: monotonic, no SDL dependency (unit tests).
class ScopedPhase {
public:
    ScopedPhase(FrameStats& stats, Phase phase) : stats_(stats.enabled() ? &stats : nullptr), phase_(phase) {
        if (stats_) start_ = std::chrono::steady_clock::now();
    }
    ~ScopedPhase() {
        if (stats_)
            stats_->ms[size_t(phase_)] += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start_).count();
    }
    ScopedPhase(const ScopedPhase&) = delete;
    ScopedPhase& operator=(const ScopedPhase&) = delete;
private:
    FrameStats* stats_;
    Phase phase_;
    std::chrono::steady_clock::time_point start_{};
};

}  // namespace tak::render
