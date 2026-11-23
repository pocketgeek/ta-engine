#include "client/terrainchunks.h"
#include "client/gpuvram.h"
#include "client/rendertelemetry.h"
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>

namespace tak {
namespace {
// Resident images plus free texture slots: 40% of the player's GPU MEMORY LIMIT, from
// 256 MiB up to 2 GiB (UNLIMITED: 2 GiB) -- 512 MiB at the 1.25 GiB default. A typical
// game tracks 0.4-0.7 GiB against that default, and at a fixed 256 MiB a camera that
// pans and zooms about a big map evicted and re-baked images it came back to; a higher
// limit now buys fewer re-bakes. It follows the setting, not the cap a failed allocation
// tightens for a while: room() yields to that cap and to everything else resident.
size_t budget() {
    constexpr size_t floor=size_t(256)<<20,ceiling=size_t(2048)<<20;
    const size_t limit=gpuvram::limit();
    if(limit==gpuvram::kUnlimited)return ceiling;
    return std::clamp(limit/5*2,floor,ceiling);
}
uint64_t nowNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
// What the cache may hold right now: its own budget, or less when the global GPU cap
// (Options GPU MEMORY LIMIT, or a post-failure tightening) leaves less room beside
// everything else that is resident.
size_t room(size_t own) {
    const size_t others=gpuvram::bytes()-std::min(own,gpuvram::bytes());
    const size_t cap=gpuvram::cap();
    return cap<=others?0:std::min(budget(),cap-others);
}
}
TerrainChunks::TerrainChunks(terrain::Compositor& c):compositor_(c) {worker_=std::thread([this]{work();});}
TerrainChunks::~TerrainChunks() {
    {std::lock_guard lock(mutex_);stop_=true;}
    cv_.notify_all();worker_.join();clear();
}
void TerrainChunks::clear() {
    for(const auto& [key,p]:pending_)p.control->cancelled.store(true,std::memory_order_relaxed);
    jobsCancelled_+=pending_.size();
    {std::lock_guard lock(mutex_);++epoch_;jobs_.clear();done_.clear();ready_.clear();readyHeld_.store(0,std::memory_order_relaxed);}
    cv_.notify_all();pending_.clear();nearbyPending_=0;visible_.clear();drawn_.clear();
    for(auto& [key,e]:cache_)gpuvram::destroy(e.texture);
    cache_.clear();bytes_=0;++revision_;
    dropAllFreeSlots();
}
void TerrainChunks::releaseSlot(int level,SDL_Texture* texture) {
    free_[level].push_back(texture);pooledBytes_+=slotBytes(level);
}
bool TerrainChunks::dropFreeSlot(int keepLevel) {
    // The coarsest other level first: its slots are the cheapest to make again.
    for(int level=5;level>=0;--level)
        if(level!=keepLevel && !free_[level].empty()) {
            gpuvram::destroy(free_[level].back());free_[level].pop_back();
            pooledBytes_-=slotBytes(level);return true;
        }
    return false;
}
void TerrainChunks::dropAllFreeSlots() {
    for(auto& slots:free_) {for(SDL_Texture* t:slots)gpuvram::destroy(t);slots.clear();}
    pooledBytes_=0;
}
void TerrainChunks::evict(std::map<Key,Entry>::iterator i,bool pool) {
    const int level=std::get<0>(i->first);
    bytes_-=slotBytes(level);
    if(pool)releaseSlot(level,i->second.texture);
    else gpuvram::destroy(i->second.texture);
    cache_.erase(i);++revision_;
}
void TerrainChunks::invalidate(int bx0,int by0,int bx1,int by1) {
    if(bx0>=bx1 || by0>=by1)return;
    const auto affected=[&](const Key& key) {
        const auto [level,cx,cy]=key;
        return bx0<(cx+1)*32+1 && bx1>cx*32-1 && by0<(cy+1)*32+1 && by1>cy*32-1;
    };
    // Jobs own immutable tile snapshots. Cancel only edited chunks (including
    // gutters); continuous painting must not starve unrelated pending images.
    for(auto i=pending_.begin();i!=pending_.end();) {
        if(affected(i->first)) {
            i->second.control->cancelled.store(true,std::memory_order_relaxed);
            nearbyPending_-=i->second.nearby;i=pending_.erase(i);++jobsCancelled_;
        }
        else ++i;
    }
    {
        std::lock_guard lock(mutex_);
        const auto cancelled=[](const Job& job) {return job.control->cancelled.load(std::memory_order_relaxed);};
        std::erase_if(jobs_,cancelled);std::erase_if(done_,cancelled);
        std::erase_if(ready_,cancelled);readyHeld_.store(ready_.size(),std::memory_order_relaxed);
    }
    cv_.notify_all();
    for(auto i=cache_.begin();i!=cache_.end();) {
        if(affected(i->first))evict(i++,true);
        else ++i;
    }
    ++revision_;
}
void TerrainChunks::resetSource() {
    clear();
    std::unique_lock lock(mutex_);
    cv_.wait(lock,[&]{return !busy_;});
    // The worker is idle and holds the lock's handshake: its fields are ours now.
    mips_.clear();mipsEvicted_.clear();mipBytes_.store(0,std::memory_order_relaxed);
}
void TerrainChunks::setMipBudget(size_t high,size_t low,uint64_t idleMs) {
    mipHigh_.store(high,std::memory_order_relaxed);
    mipLow_.store(std::min(low,high),std::memory_order_relaxed);
    mipIdleMs_=idleMs;
}
void TerrainChunks::evictMips(size_t target) {
    while(mipBytes_.load(std::memory_order_relaxed)>target) {
        auto victim=mips_.end();
        for(auto i=mips_.begin();i!=mips_.end();++i)
            if(i->second.image.use_count()==1 && (victim==mips_.end() || i->second.lastUse<victim->second.lastUse))victim=i;
        if(victim==mips_.end())return;   // the rest is pinned by the running job
        mipBytes_.fetch_sub(victim->second.bytes,std::memory_order_relaxed);
        mipEvictions_.fetch_add(1,std::memory_order_relaxed);
        mipsEvicted_.insert(victim->first);
        mips_.erase(victim);
    }
}
terrain::SectionRef TerrainChunks::mip(uint32_t key,int level,bool stockTerrain) {
    if(!level)return compositor_.sectionImage(key,stockTerrain);
    const auto id=std::tuple{key,level,stockTerrain};
    if(auto i=mips_.find(id);i!=mips_.end()) {i->second.lastUse=++mipClock_;return i->second.image;}
    const terrain::SectionRef source=mip(key,level-1,stockTerrain);   // pinned while it is read
    const auto& src=*source;
    auto image=std::make_shared<jpeg::Image>();auto& out=*image;
    out.width=std::max(1,src.width/2);out.height=std::max(1,src.height/2);
    out.rgba.resize(size_t(out.width)*out.height*4);
    for(int y=0;y<out.height;++y)for(int x=0;x<out.width;++x)for(int c=0;c<4;++c) {
        unsigned sum=0;
        for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx)
            sum+=src.rgba[(size_t(std::min(y*2+dy,src.height-1))*src.width+std::min(x*2+dx,src.width-1))*4+c];
        out.rgba[(size_t(y)*out.width+x)*4+c]=uint8_t((sum+2)/4);
    }
    const size_t bytes=out.rgba.capacity();
    const size_t total=mipBytes_.fetch_add(bytes,std::memory_order_relaxed)+bytes;
    if(total>mipPeakBytes_.load(std::memory_order_relaxed))mipPeakBytes_.store(total,std::memory_order_relaxed);
    if(mipsEvicted_.count(id))mipRegens_.fetch_add(1,std::memory_order_relaxed);
    terrain::SectionRef result=std::move(image);   // pinned by the caller
    mips_[id]=Mip{result,bytes,++mipClock_};
    if(total>mipHigh_.load(std::memory_order_relaxed))evictMips(mipLow_.load(std::memory_order_relaxed));
    return result;
}
void TerrainChunks::compose(Job& j) {
    const auto [level,cx,cy]=j.key;const int tile=32>>level;
    std::vector<const jpeg::Image*> images;images.reserve(j.tiles.size());
    // Job-scoped ownership: every image this job reads stays pinned until it returns.
    std::map<uint32_t,terrain::SectionRef> decoded;
    for(const auto& t:j.tiles) {
        if(j.control->cancelled.load(std::memory_order_relaxed))return;
        auto [entry,added]=decoded.try_emplace(t.key);
        if(added)try {entry->second=mip(t.key,level,j.stockTerrain);}catch(...) {}
        images.push_back(entry->second.get());
    }
    if(level) {   // level 0 images are the compositor's (counted there)
        size_t pinned=0;
        for(const auto& [key,image]:decoded)if(image)pinned+=image->rgba.capacity();
        mipPinnedBytes_.store(pinned,std::memory_order_relaxed);
    }
    j.pixels.resize(size_t(j.w+2)*(j.h+2)*4);
    // A texel of real neighbouring terrain on every side prevents filtering
    // seams. At the map boundary repeat its edge, rather than wrapping a tile.
    for(int y=-1;y<=j.h;++y) {
        if(j.control->cancelled.load(std::memory_order_relaxed))return;
        for(int x=-1;x<=j.w;++x) {
            const int gx=std::clamp((cx*span>>level)+x,0,j.mapW-1);
            const int gy=std::clamp((cy*span>>level)+y,0,j.mapH-1);
            const size_t index=size_t(gy/tile-j.by)*j.bw+gx/tile-j.bx;
            const auto* image=images[index];if(!image)continue;
            const auto& t=j.tiles[index];
            const int sx=(int(t.col)*tile)%image->width,sy=(int(t.row)*tile)%image->height;
            const size_t from=(size_t(std::min(sy+gy%tile,image->height-1))*image->width+
                                     std::min(sx+gx%tile,image->width-1))*4;
            const size_t to=(size_t(y+1)*(j.w+2)+x+1)*4;
            std::memcpy(j.pixels.data()+to,image->rgba.data()+from,4);
        }
    }
}
void TerrainChunks::work() {
    std::unique_lock lock(mutex_);
    const auto hasSpace=[&]{return done_.size()+readyHeld_.load(std::memory_order_relaxed)<8;};
    for(;;) {
        cv_.wait(lock,[&]{return stop_ || trimMips_ || (!jobs_.empty() && hasSpace());});
        if(stop_)return;
        if(trimMips_ && jobs_.empty()) {
            // Sustained low demand (the render thread's request): no job runs, so no
            // mip is pinned; the worker drops its own images (only it touches them).
            trimMips_=false;busy_=true;lock.unlock();
            if(!mips_.empty()) {
                for(const auto& [id,m]:mips_)mipsEvicted_.insert(id);
                mips_.clear();mipBytes_.store(0,std::memory_order_relaxed);
                mipTrims_.fetch_add(1,std::memory_order_relaxed);
            }
            lock.lock();busy_=false;cv_.notify_all();continue;
        }
        trimMips_=false;   // work arrived: demand is not low after all
        std::erase_if(jobs_,[](const Job& j){return j.control->cancelled.load(std::memory_order_relaxed);});
        if(jobs_.empty() || !hasSpace())continue;
        // The most urgent queued job (smallest priority; the oldest among equals, so
        // without priorities -- the legacy policy -- this is FIFO).
        auto best=jobs_.begin();
        uint32_t bestPriority=best->control->priority.load(std::memory_order_relaxed);
        for(auto i=std::next(best);i!=jobs_.end();++i) {
            const uint32_t priority=i->control->priority.load(std::memory_order_relaxed);
            if(priority<bestPriority || (priority==bestPriority && i->seq<best->seq)) {best=i;bestPriority=priority;}
        }
        auto job=std::move(*best);jobs_.erase(best);busy_=true;lock.unlock();
        const uint64_t started=nowNs();
        try {compose(job);}catch(...) {job.pixels.clear();}
        mipPinnedBytes_.store(0,std::memory_order_relaxed);
        // The job's pins are released: images it held past the CPU budgets can go now.
        compositor_.trim();
        if(mipBytes_.load(std::memory_order_relaxed)>mipHigh_.load(std::memory_order_relaxed))
            evictMips(mipLow_.load(std::memory_order_relaxed));
        job.composeNs=nowNs()-started;
        composeNs_.fetch_add(job.composeNs,std::memory_order_relaxed);
        jobsCompleted_.fetch_add(1,std::memory_order_relaxed);
        lock.lock();
        if(job.epoch==epoch_ && !job.control->cancelled.load(std::memory_order_relaxed))done_.push_back(std::move(job));
        else {   // cancelled while it ran (obsolete, an edit, a reset): its pins are gone, its work wasted
            wastedAbortNs_.fetch_add(job.composeNs,std::memory_order_relaxed);
            jobsAborted_.fetch_add(1,std::memory_order_relaxed);
        }
        busy_=false;cv_.notify_all();
    }
}
std::map<TerrainChunks::Key,TerrainChunks::Entry>::iterator TerrainChunks::standIn(const Key& key) {
    const auto [want,cx,cy]=key;
    for(int d=1;d<=5;++d)
        for(int level:{want-d,want+d})
            if(level>=0 && level<=5)
                if(auto i=cache_.find({level,cx,cy});i!=cache_.end())return i;
    return cache_.end();
}
bool TerrainChunks::anyImage(int cx,int cy) const {
    for(int level=0;level<=5;++level)if(cache_.count({level,cx,cy}))return true;
    return false;
}
size_t TerrainChunks::imageBytes(const Key& key,const tnt::Map&) const {
    return slotBytes(std::get<0>(key));   // a whole slot, whatever part of it the image fills
}
bool TerrainChunks::makeRoom(size_t size,const Key& cell,bool coldOnly) {
    const auto [cellLevel,cellX,cellY]=cell;
    // A free slot of the level holds the image with no new GPU memory. Otherwise free
    // slots of other levels go first, then images, whose slots join the free lists (one
    // of this level ends the search).
    const auto room=[&]{return !free_[cellLevel].empty() ||
                               (bytes_+pooledBytes_+size<=budget() && gpuvram::wouldFit(size));};
    while(!room()) {
        if(dropFreeSlot(cellLevel))continue;
        auto victim=cache_.end();int victimRank=INT_MAX;
        for(auto i=cache_.begin();i!=cache_.end();++i) {
            const auto [level,cx,cy]=i->first;(void)level;
            const int rank=terrainwork::evictionRank(i->second.used==frame_,i->second.standin==frame_,
                                                     cx==cellX && cy==cellY,params_);
            if(coldOnly && (rank!=1 || demand_.keep().contains(cx,cy)))continue;
            if(rank && (rank<victimRank || (rank==victimRank && i->second.used<victim->second.used))) {victim=i;victimRank=rank;}
        }
        if(victim==cache_.end())break;
        evict(victim,true);
    }
    return room();
}
void TerrainChunks::releasePending(const Job& job) {
    if(auto i=pending_.find(job.key);i!=pending_.end() && i->second.control==job.control) {
        nearbyPending_-=i->second.nearby;pending_.erase(i);
    }
}
void TerrainChunks::discard(Job& job) {
    ++jobsDiscarded_;wastedNs_+=job.composeNs;
    job.pixels=std::vector<uint8_t>();
}
size_t TerrainChunks::offviewRoom() {
    // Cold images: what off-view work (nearby, kept) may replace. Once per frame, and
    // only when off-view work asks.
    if(!coldValid_) {
        coldBytes_=0;coldValid_=true;
        for(const auto& [key,e]:cache_) {
            const auto [level,cx,cy]=key;
            if(terrainwork::evictionRank(e.used==frame_,e.standin==frame_,false,params_)==1 && !demand_.keep().contains(cx,cy))
                coldBytes_+=slotBytes(level);
        }
    }
    // Free slots are room: an off-view image reuses one.
    const size_t limit=room(bytes_+pooledBytes_);
    return (limit>bytes_ ? limit-bytes_ : 0)+coldBytes_;
}
void TerrainChunks::rankPending(const tnt::Map& map) {
    if(!params_.cancel && !params_.prioritize)return;
    using terrainwork::Tier;
    bool purge=false;
    for(auto i=pending_.begin();i!=pending_.end();) {
        const auto [level,cx,cy]=i->first;
        Tier tier=demand_.classify(level,cx,cy,anyImage(cx,cy));
        // Kept work is worth finishing only while its result could still be uploaded,
        // which off-view results do into free or cold room only.
        if(tier==Tier::Speculative && imageBytes(i->first,map)>offviewRoom())tier=Tier::Obsolete;
        if(params_.cancel && tier==Tier::Obsolete) {
            i->second.control->cancelled.store(true,std::memory_order_relaxed);
            nearbyPending_-=i->second.nearby;i=pending_.erase(i);++jobsObsolete_;purge=true;
            continue;
        }
        i->second.control->priority.store(params_.prioritize ? demand_.priority(tier,cx,cy) : 0,std::memory_order_relaxed);
        ++i;
    }
    if(purge) {
        std::lock_guard lock(mutex_);
        const auto cancelled=[](const Job& job) {return job.control->cancelled.load(std::memory_order_relaxed);};
        std::erase_if(jobs_,cancelled);std::erase_if(done_,cancelled);
    }
}
void TerrainChunks::uploadReady(SDL_Renderer* renderer,terrainwork::UploadBudget& ub,bool urgentOnly,bool finalPass) {
    using terrainwork::Tier;using terrainwork::UploadBudget;
    if(ready_.empty())return;
    struct Item {size_t index;Tier tier;uint32_t priority;uint64_t seq;};
    std::vector<Item> order;order.reserve(ready_.size());
    for(size_t i=0;i<ready_.size();++i) {
        const auto [level,cx,cy]=ready_[i].key;
        const Tier tier=demand_.classify(level,cx,cy,anyImage(cx,cy));
        order.push_back({i,tier,params_.prioritize ? demand_.priority(tier,cx,cy) : 0,ready_[i].seq});
    }
    std::sort(order.begin(),order.end(),[](const Item& a,const Item& b) {
        return a.priority!=b.priority ? a.priority<b.priority : a.seq<b.seq;
    });
    std::vector<char> keep(ready_.size(),0);
    int handled=0;bool stop=false;
    for(const auto& item:order) {
        Job& job=ready_[item.index];
        if(job.control->cancelled.load(std::memory_order_relaxed)) {discard(job);continue;}
        const auto [level,cx,cy]=job.key;
        const bool visible=level==level_ && demand_.visible().contains(cx,cy);
        // Legacy: anything not visible now is discarded. K: only obsolete results.
        if(job.pixels.empty() || cache_.count(job.key) || (params_.cancel ? item.tier==Tier::Obsolete : !visible)) {
            releasePending(job);discard(job);continue;
        }
        const bool urgent=!params_.coordinate || item.tier==Tier::Hole;
        const size_t size=job.pixels.size();
        if(stop || (urgentOnly && !urgent) || handled>=64) {keep[item.index]=1;continue;}
        if(!ub.admit(UploadBudget::Chunk,urgent,size)) {
            keep[item.index]=1;
            if(finalPass)++uploadsDeferred_;
            stop=!params_.coordinate;   // legacy: the rest waits behind it (FIFO)
            continue;
        }
        releasePending(job);++handled;
        // A visible image makes room (stand-ins last); an off-view one only free room or
        // cold images (not visible, no stand-in, outside the keep band).
        const bool fits=makeRoom(slotBytes(std::get<0>(job.key)),job.key,!visible);
        if(!fits || gpuvram::blocked()) {discard(job);continue;}
        const uint64_t uploadStart=nowNs();
        const int side=(span>>level)+2;
        SDL_Texture* texture=nullptr;
        if(!free_[level].empty()) {
            texture=free_[level].back();free_[level].pop_back();pooledBytes_-=slotBytes(level);++slotReuses_;
        } else {
            texture=gpuvram::create(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,side,side);
            if(!texture){gpuvram::noteFail();discard(job);continue;}
            ++slotCreates_;
        }
        // The image (with its one-texel border) fills the slot's top-left; draw() reads
        // only that part.
        const SDL_Rect area{0,0,job.w+2,job.h+2};
        if(SDL_UpdateTexture(texture,&area,job.pixels.data(),(job.w+2)*4)!=0){releaseSlot(level,texture);discard(job);continue;}
        SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture,linear_?SDL_ScaleModeLinear:SDL_ScaleModeNearest);
        const uint64_t uploadNs=nowNs()-uploadStart;   // CPU submission only: no GPU synchronization
        uploadNs_+=uploadNs;ub.record(UploadBudget::Chunk,size,uploadNs);
        render::countTextureUpload(size);
        cache_.emplace(job.key,Entry{texture,job.w,job.h,visible ? frame_ : 0});
        bytes_+=slotBytes(level);++uploads_;++revision_;
        ++jobsUsed_;uploadBytes_+=size;jobsOffview_+=!visible;
        job.pixels=std::vector<uint8_t>();
    }
    size_t kept=0;
    for(size_t i=0;i<ready_.size();++i)if(keep[i]) {if(kept!=i)ready_[kept]=std::move(ready_[i]);++kept;}
    ready_.resize(kept);
    // Under the lock: the worker reads readyHeld_ in its wait predicate, so a store
    // between its check and its wait would lose this wakeup (nothing else may notify
    // while the camera rests).
    {std::lock_guard lock(mutex_);readyHeld_.store(kept,std::memory_order_relaxed);}
    cv_.notify_all();   // the worker may compose again
}
void TerrainChunks::uploadDeferred(SDL_Renderer* renderer,terrainwork::UploadBudget& ub) {
    if(!deferred_)return;
    deferred_=false;
    uploadReady(renderer,ub,false,true);
}
void TerrainChunks::prepare(SDL_Renderer* renderer,const tnt::Map& map,float x,float y,float zoom,int w,int h,bool linear,uint64_t nowMs,
                            terrainwork::UploadBudget& ub) {
    using terrainwork::Tier;
    ++frame_;visible_.clear();holesAfterPrepare_=0;deferred_=false;
    // Mip idle trim (workstream J): terrain work = a job queued or completed. After
    // mipIdleMs_ and 120 frames without any, ask the worker to drop its mip images
    // (regenerated on demand; the chunks already composed are unaffected).
    {
        const size_t seen=size_t(jobsQueued_+jobsCompleted_.load(std::memory_order_relaxed));
        if(seen!=lastJobsSeen_ || !lastWorkFrame_) {lastJobsSeen_=seen;lastWorkMs_=nowMs;lastWorkFrame_=frame_;}
        else if(mipBytes_.load(std::memory_order_relaxed) && nowMs-lastWorkMs_>=mipIdleMs_ && frame_-lastWorkFrame_>=120) {
            {std::lock_guard lock(mutex_);trimMips_=true;}
            cv_.notify_all();
            lastWorkMs_=nowMs;lastWorkFrame_=frame_;   // one request per idle period
        }
    }
    if(map.blocksX<=0 || map.blocksY<=0 || zoom<=0)return;
    float sx=1,sy=1;SDL_RenderGetScale(renderer,&sx,&sy);
    level_=0;
    const float density=zoom*std::max(sx,sy);
    while(level_<5 && (1.f/float(2<<(level_)))>=density)++level_;
    const int nx=(map.blocksX*32+span-1)/span,ny=(map.blocksY*32+span-1)/span;
    const int x0=std::clamp(int(std::floor(x/span)),0,nx-1),y0=std::clamp(int(std::floor(y/span)),0,ny-1);
    const int x1=std::clamp(int(std::floor((x+w/zoom)/span)),0,nx-1),y1=std::clamp(int(std::floor((y+h/zoom)/span)),0,ny-1);
    // If supersampling would exceed the cache, preserve logical-pixel detail
    // first. Never cycle visible chunks through an undersized cache each frame.
    const size_t limit=room(bytes_+pooledBytes_);
    while(level_<5 && (1.f/float(2<<level_))>=zoom &&
          size_t(x1-x0+1)*(y1-y0+1)*size_t((span>>level_)+2)*((span>>level_)+2)*4>limit)
        ++level_;
    size_t missing=0;   // visible cells without their wanted level (the common steady state: none)
    for(int cy=y0;cy<=y1;++cy)for(int cx=x0;cx<=x1;++cx) {
        Key key{level_,cx,cy};visible_.push_back(key);
        if(auto i=cache_.find(key);i!=cache_.end())i->second.used=frame_;
        else ++missing;
    }
    demand_.update(level_,terrainwork::Rect{x0,y0,x1,y1},x+float(w)/zoom*.5f,y+float(h)/zoom*.5f,float(w)/zoom,float(h)/zoom,
                   span,nx,ny,nowMs,frame_,params_);
    // The images standing in for visible cells whose own level is missing (the same
    // choice draw() makes) are evicted last while their replacements are prepared.
    if(params_.protectStandins && missing)
        for(const auto& key:visible_)
            if(!cache_.count(key))
                if(auto i=standIn(key);i!=cache_.end())i->second.standin=frame_;
    coldValid_=false;   // computed on first use this frame (offviewRoom)
    // A lowered GPU limit takes effect now: give back off-screen chunks until the
    // total is under the cap again, rather than waiting for the next upload.
    if(gpuvram::overCap())shed(0);
    // A lowered GPU MEMORY LIMIT lowers the budget: free slots go at once; resident
    // images past it are evicted as uploads make room.
    while(bytes_+pooledBytes_>budget() && dropFreeSlot(-1)) {}
    if(linear!=linear_)for(auto& [key,e]:cache_)
        SDL_SetTextureScaleMode(e.texture,linear?SDL_ScaleModeLinear:SDL_ScaleModeNearest);
    linear_=linear;
    // Pending work follows the view: re-rank it, cancel what became obsolete.
    rankPending(map);
    // Completed results, most urgent first. Holes first; unless holes remain (the
    // owner's fallback sections fill them, ahead of upgrades), everything else now too.
    {
        std::lock_guard lock(mutex_);
        for(auto& job:done_)ready_.push_back(std::move(job));
        done_.clear();readyHeld_.store(ready_.size(),std::memory_order_relaxed);
    }
    uploadReady(renderer,ub,params_.coordinate,!params_.coordinate);
    if(missing)for(const auto& key:visible_)holesAfterPrepare_+=!anyImage(std::get<1>(key),std::get<2>(key));
    if(params_.coordinate) {
        if(holesAfterPrepare_)deferred_=true;
        else uploadReady(renderer,ub,false,true);
    }
    // New work for visible cells, most urgent first (legacy: row order) -- not while the
    // camera moves fast at level 0 or jumps repeatedly (the calm gate): the fallback
    // draws them.
    if(demand_.gated()) {++framesGated_;return;}
    const auto queue=[&](const Key& key,uint32_t priority,bool nearby) {
        const auto [level,cx,cy]=key;Job job;job.key=key;job.epoch=epoch_;job.seq=++seq_;job.stockTerrain=map.stockTerrain;
        job.mapW=map.blocksX*(32>>level);job.mapH=map.blocksY*(32>>level);
        job.w=std::min(span>>level,job.mapW-(cx*span>>level));
        job.h=std::min(span>>level,job.mapH-(cy*span>>level));
        job.bx=std::max(0,cx*32-1);job.by=std::max(0,cy*32-1);
        job.bw=std::min(map.blocksX,(cx+1)*32+1)-job.bx;
        job.bh=std::min(map.blocksY,(cy+1)*32+1)-job.by;
        job.tiles.reserve(size_t(job.bw)*job.bh);
        for(int by=job.by;by<job.by+job.bh;++by)for(int bx=job.bx;bx<job.bx+job.bw;++bx) {
            const auto at=size_t(by)*map.blocksX+bx;
            job.tiles.push_back({map.tileKeys[at],map.tileCols[at],map.tileRows[at]});
        }
        job.control=std::make_shared<Control>();
        job.control->priority.store(priority,std::memory_order_relaxed);
        pending_.emplace(key,Pending{job.control,nearby});++jobsQueued_;
        if(nearby) {++nearbyPending_;++jobsNearby_;}
        {std::lock_guard lock(mutex_);jobs_.push_back(std::move(job));}cv_.notify_all();
    };
    std::vector<std::pair<uint32_t,Key>> wanted;
    for(const auto& key:visible_) {
        if(cache_.count(key) || pending_.count(key))continue;
        const auto [level,cx,cy]=key;
        const Tier tier=anyImage(cx,cy) ? Tier::Upgrade : Tier::Hole;
        wanted.push_back({params_.prioritize ? demand_.priority(tier,cx,cy) : 0,key});
    }
    if(params_.prioritize)std::stable_sort(wanted.begin(),wanted.end(),[](const auto& a,const auto& b){return a.first<b.first;});
    bool displaced=false;
    for(const auto& [priority,key]:wanted) {
        if(gpuvram::blocked())break;
        if(pending_.size()>=params_.maxPending) {
            // A full queue gives a slot of nearby or kept work to visible work.
            auto worst=pending_.end();uint32_t worstPriority=0;
            if(params_.cancel)
                for(auto i=pending_.begin();i!=pending_.end();++i) {
                    const uint32_t p=i->second.control->priority.load(std::memory_order_relaxed);
                    if(p>>24>=uint32_t(Tier::Nearby) && p>priority && (worst==pending_.end() || p>worstPriority)) {worst=i;worstPriority=p;}
                }
            if(worst==pending_.end())break;
            worst->second.control->cancelled.store(true,std::memory_order_relaxed);
            nearbyPending_-=worst->second.nearby;pending_.erase(worst);++jobsObsolete_;displaced=true;
        }
        const size_t size=imageBytes(key,map);
        if((bytes_+size>budget() || !gpuvram::wouldFit(size)) &&
           std::none_of(cache_.begin(),cache_.end(),[&](const auto& entry){return entry.second.used!=frame_;}))
            continue; // wait for space instead of repeatedly composing unusable images
        queue(key,priority,false);
    }
    if(displaced) {
        std::lock_guard lock(mutex_);
        const auto cancelled=[](const Job& job) {return job.control->cancelled.load(std::memory_order_relaxed);};
        std::erase_if(jobs_,cancelled);
    }
    // While the camera moves: the cells about to enter, nearest first, into free room only.
    if(params_.nearby && demand_.moving() && !gpuvram::blocked()) {
        std::vector<std::pair<uint32_t,Key>> lead;
        const auto& g=demand_.guard();
        for(int cy=g.y0;cy<=g.y1;++cy)for(int cx=g.x0;cx<=g.x1;++cx) {
            const Key key{level_,cx,cy};
            if(demand_.leading(cx,cy) && !cache_.count(key) && !pending_.count(key))
                lead.push_back({demand_.priority(Tier::Nearby,cx,cy),key});
        }
        std::sort(lead.begin(),lead.end());
        for(const auto& [priority,key]:lead) {
            if(nearbyPending_>=params_.maxNearby || pending_.size()+params_.nearbyReserve>=params_.maxPending)break;
            if(imageBytes(key,map)*(nearbyPending_+1)>offviewRoom())break;
            queue(key,priority,true);
        }
    }
}
size_t TerrainChunks::shed(size_t need) {
    size_t freed=0;
    // GPU pressure: free slots are destroyed first, then evicted images' textures too.
    while(!gpuvram::wouldFit(need) && pooledBytes_) {
        const size_t before=pooledBytes_;dropFreeSlot(-1);freed+=before-pooledBytes_;
    }
    while(!gpuvram::wouldFit(need)) {
        auto oldest=cache_.end();int oldestRank=INT_MAX;
        for(auto i=cache_.begin();i!=cache_.end();++i) {
            const int rank=terrainwork::evictionRank(i->second.used==frame_,i->second.standin==frame_,false,params_);
            if(rank && (rank<oldestRank || (rank==oldestRank && i->second.used<oldest->second.used))) {oldest=i;oldestRank=rank;}
        }
        if(oldest==cache_.end())break;
        freed+=slotBytes(std::get<0>(oldest->first));
        evict(oldest,false);
    }
    return freed;
}
bool TerrainChunks::covers(int bx,int by) const {
    return std::binary_search(drawn_.begin(),drawn_.end(),std::pair{bx/32,by/32});
}
bool TerrainChunks::draw(SDL_Renderer* renderer,float x,float y,float zoom) {
    bool complete=true;drawn_.clear();drawnStandin_=drawnHoles_=0;
    for(const auto& key:visible_) {
        auto i=cache_.find(key);
        const auto [want,cx,cy]=key;
        if(i==cache_.end()) {
            complete=false;
            // Still baking at this level (a zoom just changed it, or a pan brought it
            // in): stand in with the same map area at the nearest level still cached,
            // a little softer or sharper for a few frames, rather than sending the
            // whole view to the section fallback. Not stamped as used, so it never
            // holds room the real level needs (prepare() marks it a stand-in: evicted
            // last while the real level is prepared).
            i=standIn(key);
            if(i==cache_.end()) {++drawnHoles_;continue;}
            ++drawnStandin_;
        }
        const int level=std::get<0>(i->first);const auto& e=i->second;
        // Shared rounded boundaries prevent cracks. The one-texel border lets
        // filtering sample neighbouring terrain at the edges of this crop.
        const float x0=std::round((cx*span-x)*zoom),y0=std::round((cy*span-y)*zoom);
        const float x1=std::round((cx*span+(e.w<<level)-x)*zoom),y1=std::round((cy*span+(e.h<<level)-y)*zoom);
        const SDL_Rect source{1,1,e.w,e.h};const SDL_FRect dest{x0,y0,x1-x0,y1-y0};
        render::countLogicalDraw();
        if(SDL_RenderCopyF(renderer,e.texture,&source,&dest)!=0) {complete=false;continue;}
        drawn_.push_back({cx,cy});
    }
    std::sort(drawn_.begin(),drawn_.end());
    return complete && !visible_.empty();
}
}
