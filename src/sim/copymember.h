#pragma once
// Members that let an owner keep a defaulted copy (World::clone, the Legion navigator).
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace ta::sim {

// A member a defaulted copy leaves empty, for the owner to fill by hand afterwards:
// an owning pointer whose target needs its own clone, a table of owned arrays, a
// cache that has to be rebuilt round its copies.
template<class T> struct LeftEmptyOnCopy : T {
    using T::T;
    LeftEmptyOnCopy()=default;
    LeftEmptyOnCopy(T&& value):T(std::move(value)) {}
    LeftEmptyOnCopy(const LeftEmptyOnCopy&) noexcept:T() {}
    LeftEmptyOnCopy(LeftEmptyOnCopy&&)=default;
    LeftEmptyOnCopy& operator=(const LeftEmptyOnCopy&) {static_cast<T&>(*this)=T();return *this;}
    LeftEmptyOnCopy& operator=(LeftEmptyOnCopy&&)=default;
};

// An atomic a copy carries by value. Copying is not synchronised: the worlds are
// copied between ticks, when nothing else writes them.
template<class T> struct CopyableAtomic : std::atomic<T> {
    CopyableAtomic(T value=T()) noexcept:std::atomic<T>(value) {}
    CopyableAtomic(const CopyableAtomic& o) noexcept:std::atomic<T>(o.load(std::memory_order_relaxed)) {}
    CopyableAtomic& operator=(const CopyableAtomic& o) noexcept {this->store(o.load(std::memory_order_relaxed));return *this;}
    using std::atomic<T>::operator=;
};

// The copies made on this thread while one of these is alive are made to be kept and
// restored from, not to run (World::keyframe): the members below that pack hold runs.
class KeepingCopies {
public:
    KeepingCopies() {on()=true;}
    ~KeepingCopies() {on()=false;}
    KeepingCopies(const KeepingCopies&)=delete;
    KeepingCopies& operator=(const KeepingCopies&)=delete;
    static bool active() {return on();}
private:
    static bool& on() {static thread_local bool keeping=false;return keeping;}
};

// An array as runs, exactly: a header (count << 1 | literal) per run, and in `values` the
// one value of a run of equals or the `count` values of a literal one. The per-cell maps
// of a world are mostly runs of one value (zeros with a few bodies' cells in them, sight
// counts, labels, clearances): kilobytes this way for megabytes as arrays.
template<class T> struct ArrayRuns {
    static_assert(std::is_trivially_copyable_v<T>);
    std::vector<uint32_t> headers;
    std::vector<T> values;
    size_t count=0;   // elements of the array
    static constexpr size_t kMinRun=8;            // equal elements that make a run; fewer stay literal
    static constexpr size_t kMinElements=4096;    // arrays under this are not worth the scan
    static bool same(const T& a,const T& b) {return std::memcmp(&a,&b,sizeof(T))==0;}
    // False, with nothing held, when the runs would take more than half of what the array does.
    bool pack(const T* p,size_t n) {
        headers.clear();values.clear();count=0;
        if(n<kMinElements||n>=size_t(1)<<31)return false;
        const size_t limit=n*sizeof(T)/2;
        size_t literal=0;   // where the literal stretch not yet written starts
        const auto over=[&](size_t upTo) {
            return (headers.size()+2)*sizeof(uint32_t)+(values.size()+(upTo-literal))*sizeof(T)>limit;
        };
        const auto writeLiteral=[&](size_t end) {
            if(end==literal)return;
            headers.push_back(uint32_t((end-literal)<<1|1));
            values.insert(values.end(),p+literal,p+end);
        };
        const auto giveUp=[&] {headers.clear();values.clear();return false;};
        for(size_t i=0;i<n;) {
            size_t j=i+1;   // the equals of p[i] end at j
            // A short run is found element by element; a long one in blocks (p[j-1..] against
            // p[j..] is "all equal" in one compare), which is what makes a sparse map cheap.
            while(j<n&&j-i<2*kMinRun&&same(p[j],p[i]))++j;
            if(j-i==2*kMinRun)while(j<n) {
                const size_t step=std::min<size_t>(n-j,1024);
                if(std::memcmp(p+j-1,p+j,step*sizeof(T))==0) {j+=step;continue;}
                while(same(p[j],p[i]))++j;
                break;
            }
            if(j-i>=kMinRun) {
                if(over(i))return giveUp();
                writeLiteral(i);
                headers.push_back(uint32_t((j-i)<<1));
                values.push_back(p[i]);
                literal=j;
            } else if(over(j))return giveUp();
            i=j;
        }
        if(over(n))return giveUp();
        writeLiteral(n);
        headers.shrink_to_fit();values.shrink_to_fit();
        count=n;
        return true;
    }
    void unpack(T* out) const {   // `count` elements
        const T* value=values.data();
        for(const uint32_t header:headers) {
            const size_t run=header>>1;
            if(header&1) {std::memcpy(static_cast<void*>(out),value,run*sizeof(T));value+=run;}
            else std::fill_n(out,run,*value++);
            out+=run;
        }
    }
};

// A per-cell array its copies share until one of them writes. The terrain's maps never
// change after setup and a few more change only where a structure or a feature does
// (a grid's clearances, Legion's planes): megabytes each, and a replay keeps a copy of
// the world every 30 s of game. Reads are an array's. A write goes through edit() or
// assign(), which first make the storage this copy's own, so a copy and its source are
// independent from the moment either changes. Worlds are copied between ticks and each
// then runs on its own threads: the holder count is atomic, and a holder that reads
// "alone" is alone, since a new holder only ever comes from a copy of this one.
//
// `packWhenKept`, for the ones that do change: a copy made to keep (KeepingCopies) holds
// runs of its own instead of a share, so the world it was taken from never has to copy
// the array for its sake, and gives the array back to whatever is copied from it. One
// that does not pack to half its size is shared after all. A kept copy reads as empty.
template<class T,bool packWhenKept=false> class SharedArray {
public:
    SharedArray()=default;
    SharedArray(const SharedArray& o) {copyFrom(o);}
    SharedArray& operator=(const SharedArray& o) {if(this!=&o)copyFrom(o);return *this;}
    SharedArray(SharedArray&& o) noexcept:store_(std::move(o.store_)),runs_(std::move(o.runs_)),data_(o.data_),size_(o.size_) {
        o.data_=nullptr;o.size_=0;
    }
    SharedArray& operator=(SharedArray&& o) noexcept {
        if(this!=&o) {
            store_=std::move(o.store_);runs_=std::move(o.runs_);data_=o.data_;size_=o.size_;
            o.data_=nullptr;o.size_=0;
        }
        return *this;
    }
    SharedArray& operator=(std::vector<T>&& values) {
        store_=std::make_shared<std::vector<T>>(std::move(values));runs_.reset();bind();
        return *this;
    }
    const T& operator[](size_t i) const {return data_[i];}
    const T* data() const {return data_;}
    const T* begin() const {return data_;}
    const T* end() const {return data_+size_;}
    size_t size() const {return size_;}
    bool empty() const {return !size_;}
    size_t capacity() const {return store_?store_->capacity():0;}
    const std::vector<T>& values() const {static const std::vector<T> none;return store_?*store_:none;}
    // The elements, to write: this copy's own from here on. The size stays.
    T* edit() {
        if(!store_)return nullptr;
        if(store_.use_count()>1) {store_=std::make_shared<std::vector<T>>(*store_);bind();}
        return store_->data();
    }
    // The same for writers that must not touch this object (workers, each on its own
    // part of the array): edit() made the storage this copy's own before they started.
    T* owned() const {
        assert(!shared());
        return store_?store_->data():nullptr;
    }
    void assign(size_t n,const T& value) {
        if(store_&&store_.use_count()==1)store_->assign(n,value);
        else store_=std::make_shared<std::vector<T>>(n,value);
        runs_.reset();bind();
    }
    void clear() {store_.reset();runs_.reset();data_=nullptr;size_=0;}
    bool shared() const {return store_&&store_.use_count()>1;}
    bool packed() const {return bool(runs_);}   // a kept copy holding runs: empty to read
    // What a write would have to copy first: the bytes shared with another copy.
    uint64_t sharedBytes() const {return shared()?uint64_t(size_)*sizeof(T):0;}
    friend bool operator==(const SharedArray& a,const SharedArray& b) {
        return a.size_==b.size_&&(a.data_==b.data_||std::equal(a.begin(),a.end(),b.begin()));
    }
private:
    void bind() {data_=store_->data();size_=store_->size();}
    void copyFrom(const SharedArray& o) {
        if(o.runs_&&KeepingCopies::active()) {   // a kept copy of a kept copy: the same runs
            store_.reset();runs_=o.runs_;data_=nullptr;size_=0;
        } else if(o.runs_) {                      // out of a kept copy: the array again, this one's own
            if(!store_||store_.use_count()>1)store_=std::make_shared<std::vector<T>>();
            store_->resize(o.runs_->count);
            o.runs_->unpack(store_->data());
            runs_.reset();bind();
        } else if(packWhenKept&&KeepingCopies::active()&&o.size_) {
            auto runs=std::make_shared<ArrayRuns<T>>();
            if(runs->pack(o.data_,o.size_)) {store_.reset();runs_=std::move(runs);data_=nullptr;size_=0;}
            else {store_=o.store_;runs_.reset();data_=o.data_;size_=o.size_;}
        } else {
            store_=o.store_;runs_.reset();data_=o.data_;size_=o.size_;
        }
    }
    std::shared_ptr<std::vector<T>> store_;
    std::shared_ptr<const ArrayRuns<T>> runs_;   // instead of store_, in a copy made to keep
    const T* data_=nullptr;
    size_t size_=0;
};
template<class T> using SharedPackedArray=SharedArray<T,true>;

// A per-cell array of state that changes all the time, so there is nothing to share,
// but is mostly runs of one value. A copy made to run carries it as the array it is. A
// copy made to keep (KeepingCopies) carries its runs instead, and whatever is then
// copied or assigned from that copy gets the array back, byte for byte. An array that
// does not pack to half its size is kept as it is. Only the owner of a kept copy knows
// it must not be read as an array (World::keyframe).
template<class T> class PackedWhenKept : public std::vector<T> {
public:
    using std::vector<T>::vector;
    PackedWhenKept()=default;
    PackedWhenKept(const PackedWhenKept& o):std::vector<T>() {copyFrom(o);}
    PackedWhenKept(PackedWhenKept&& o) noexcept
        :std::vector<T>(std::move(static_cast<std::vector<T>&>(o))),runs_(std::move(o.runs_)) {o.runs_=ArrayRuns<T>();}
    PackedWhenKept& operator=(const PackedWhenKept& o) {if(this!=&o)copyFrom(o);return *this;}
    PackedWhenKept& operator=(PackedWhenKept&& o) noexcept {
        if(this!=&o) {
            std::vector<T>::operator=(std::move(static_cast<std::vector<T>&>(o)));
            runs_=std::move(o.runs_);o.runs_=ArrayRuns<T>();
        }
        return *this;
    }
    // Held as runs: the array itself is empty until this is copied or assigned from.
    bool packed() const {return runs_.count!=0;}
private:
    void copyFrom(const PackedWhenKept& o) {
        if(o.runs_.count&&KeepingCopies::active()) {   // a kept copy of a kept copy
            std::vector<T>::clear();
            runs_=o.runs_;
        } else if(o.runs_.count) {
            std::vector<T>::resize(o.runs_.count);
            o.runs_.unpack(std::vector<T>::data());
            runs_=ArrayRuns<T>();
        } else if(KeepingCopies::active()&&runs_.pack(o.data(),o.size())) {
            std::vector<T>::clear();
        } else {
            std::vector<T>::operator=(o);
            runs_=ArrayRuns<T>();
        }
    }
    ArrayRuns<T> runs_;
};

}  // namespace ta::sim
