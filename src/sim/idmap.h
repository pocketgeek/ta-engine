#pragma once
// IdMap<T>: the part of std::map<int,T> Legion uses (WE E4.4, D5), flat.
//
// Keys are small non-negative ids (unit ids, group ids). Each value lives in a
// chunked slab (its address never changes while it is in the map, as a
// std::map node's does not), found by id through a dense pointer table, so a
// lookup is one array read. Ordered iteration walks a sorted vector of the live
// keys instead of red-black tree links.
//
// Same visit order as std::map, including under mutation during a walk:
//   - an iterator remembers its key; after any reshuffle of the key vector it
//     re-finds its place by that key, so incrementing always reaches the
//     smallest live key above it (std::map's rule);
//   - a key inserted mid-walk above the iterator is visited, one below is not;
//   - an erased key becomes a tombstone the walk skips (the key vector is
//     compacted later), and erasing the element an iterator points at leaves
//     that iterator incrementable (a superset of std::map, where it dangles).
// Inserting a key above the current maximum appends; a key below it goes to an
// unsorted tail (or revives its own tombstone) and is merged in by the next
// ordered operation. Not thread-safe for concurrent readers of an ordered
// walk (begin/lower_bound/++ may merge the tail); lookups never mutate.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace tak::sim {

template<class T> class IdMap {
public:
    using key_type=int;
    using mapped_type=T;
    using value_type=std::pair<const int,T>;

private:
    struct Node {std::optional<value_type> v;uint32_t slot=0;};
    static constexpr size_t npos=~size_t(0);
    static constexpr size_t unknown=npos-1;   // a find() result: position looked up on ++

public:
    template<bool Const> class Iter {
    public:
        using iterator_category=std::forward_iterator_tag;
        using value_type=IdMap::value_type;
        using difference_type=std::ptrdiff_t;
        using reference=std::conditional_t<Const,const value_type&,value_type&>;
        using pointer=std::conditional_t<Const,const value_type*,value_type*>;
        Iter()=default;
        template<bool C=Const,class=std::enable_if_t<C>> Iter(const Iter<false>& o):m(o.m),pos(o.pos),key(o.key),ver(o.ver) {}
        reference operator*() const {return *m->byId_[size_t(key)]->v;}
        pointer operator->() const {return &*m->byId_[size_t(key)]->v;}
        Iter& operator++() {m->advance(pos,key,ver);return *this;}
        Iter operator++(int) {Iter t=*this;++*this;return t;}
        friend bool operator==(const Iter& a,const Iter& b) {
            return (a.pos==npos)==(b.pos==npos)&&(a.pos==npos||a.key==b.key);
        }
        friend bool operator!=(const Iter& a,const Iter& b) {return !(a==b);}
    private:
        friend class IdMap;
        template<bool> friend class Iter;
        Iter(const IdMap* map,size_t p,int k,uint32_t v):m(map),pos(p),key(k),ver(v) {}
        const IdMap* m=nullptr;
        size_t pos=npos;int key=0;uint32_t ver=0;
    };
    using iterator=Iter<false>;
    using const_iterator=Iter<true>;

    IdMap()=default;
    // A copy holds equal values at new addresses; pointers to them are the copier's.
    IdMap(const IdMap& o) {for(const auto& [id,value]:o)try_emplace(id,value);}
    IdMap& operator=(const IdMap&)=delete;
    IdMap(IdMap&&)=default;
    IdMap& operator=(IdMap&&)=default;

    size_t size() const {return size_;}
    bool empty() const {return size_==0;}
    // The value stored under id, or nullptr: the dense lookup without an iterator.
    T* get(int id) const {
        const Node* n=node(id);
        return n?const_cast<T*>(&n->v->second):nullptr;
    }
    size_t count(int id) const {return node(id)?1:0;}

    iterator find(int id) {return node(id)?iterator(this,unknown,id,version_):end();}
    const_iterator find(int id) const {return node(id)?const_iterator(this,unknown,id,version_):end();}
    iterator begin() {return first<false>(0);}
    const_iterator begin() const {return first<true>(0);}
    iterator end() {return iterator(this,npos,0,0);}
    const_iterator end() const {return const_iterator(this,npos,0,0);}
    iterator lower_bound(int id) {return first<false>(id);}
    const_iterator lower_bound(int id) const {return first<true>(id);}

    template<class... A> std::pair<iterator,bool> try_emplace(int id,A&&... args) {
        if(node(id))return {iterator(this,unknown,id,version_),false};
        Node* n;
        if(!free_.empty()) {n=&nodeAt(free_.back());free_.pop_back();}
        else {
            if(used_%kChunk==0)chunks_.push_back(std::make_unique<Node[]>(kChunk));
            n=&nodeAt(used_);n->slot=used_++;
        }
        n->v.emplace(std::piecewise_construct,std::forward_as_tuple(id),std::forward_as_tuple(std::forward<A>(args)...));
        if(size_t(id)>=byId_.size())byId_.resize(size_t(id)+1,nullptr);
        byId_[size_t(id)]=n;++size_;
        listKey(id);
        return {iterator(this,unknown,id,version_),true};
    }
    template<class V> std::pair<iterator,bool> emplace(int id,V&& value) {return try_emplace(id,std::forward<V>(value));}
    T& operator[](int id) {return try_emplace(id).first->second;}

    size_t erase(int id) {
        Node* n=const_cast<Node*>(node(id));
        if(!n)return 0;
        n->v.reset();free_.push_back(n->slot);
        byId_[size_t(id)]=nullptr;--size_;
        // The key stays in keys_ as a tombstone until the next compaction.
        if(keys_.size()>2*size_+64)normalize();
        return 1;
    }
    iterator erase(iterator it) {
        iterator next=it;++next;   // the next live key (above it.key) before it goes
        erase(it.key);
        return next;
    }

private:
    const Node* node(int id) const {
        return id>=0&&size_t(id)<byId_.size()?byId_[size_t(id)]:nullptr;
    }
    bool live(int id) const {return byId_[size_t(id)]!=nullptr;}   // keys_ only holds ids < byId_.size()
    void listKey(int id) {
        if(sorted_==keys_.size()&&(keys_.empty()||id>keys_.back())) {keys_.push_back(id);sorted_=keys_.size();}
        else {
            // Its own tombstone in the sorted part: revive in place (an
            // erase+insert of one id does not reshuffle anything).
            const auto at=std::lower_bound(keys_.begin(),keys_.begin()+std::ptrdiff_t(sorted_),id);
            if(at!=keys_.begin()+std::ptrdiff_t(sorted_)&&*at==id)return;
            keys_.push_back(id);
        }
        if(keys_.size()>2*size_+64)normalize();
    }
    // Sort the tail into the sorted part, drop tombstones and duplicates.
    // Every iterator re-finds its position by key afterwards (version_).
    void normalize() const {
        if(sorted_<keys_.size()) {
            std::sort(keys_.begin()+std::ptrdiff_t(sorted_),keys_.end());
            std::inplace_merge(keys_.begin(),keys_.begin()+std::ptrdiff_t(sorted_),keys_.end());
        }
        size_t out=0;
        for(size_t i=0;i<keys_.size();++i) {
            const int k=keys_[i];
            if(live(k)&&(out==0||keys_[out-1]!=k))keys_[out++]=k;
        }
        keys_.resize(out);sorted_=out;++version_;
    }
    void order() const {if(sorted_<keys_.size())normalize();}
    void skipDead(size_t& pos) const {while(pos<keys_.size()&&!live(keys_[pos]))++pos;}
    template<bool Const> Iter<Const> first(int id) const {
        order();
        size_t pos=size_t(std::lower_bound(keys_.begin(),keys_.end(),id)-keys_.begin());
        skipDead(pos);
        if(pos==keys_.size())return Iter<Const>(this,npos,0,0);
        return Iter<Const>(this,pos,keys_[pos],version_);
    }
    void advance(size_t& pos,int& key,uint32_t& ver) const {
        if(pos==npos)return;
        order();
        if(ver!=version_||pos>=keys_.size()||keys_[pos]!=key)
            pos=size_t(std::upper_bound(keys_.begin(),keys_.end(),key)-keys_.begin());
        else ++pos;
        skipDead(pos);
        if(pos==keys_.size()) {pos=npos;key=0;ver=0;return;}
        key=keys_[pos];ver=version_;
    }

    // Values in chunks of kChunk nodes (stable addresses; neighbours in id
    // order are usually neighbours in memory, as ids are mostly handed out
    // ascending). An erased node's slot is reused.
    static constexpr uint32_t kChunk=64;
    Node& nodeAt(uint32_t slot) {return chunks_[slot/kChunk][slot%kChunk];}
    std::vector<std::unique_ptr<Node[]>> chunks_;
    uint32_t used_=0;
    std::vector<uint32_t> free_;
    std::vector<Node*> byId_;
    size_t size_=0;
    // Every live key once (ascending in [0, sorted_), unsorted after), plus
    // tombstones of erased keys and, in the tail, possibly repeats.
    mutable std::vector<int> keys_;
    mutable size_t sorted_=0;
    mutable uint32_t version_=1;
};

}  // namespace tak::sim
