#pragma once
// World::stateHash's folds: its words, and the per-cell planes (exploration,
// current view, map features) of millions of 16-bit cells on a big map, read
// once per lockstep check. Integer arithmetic only, cells packed with shifts:
// the same value from every compiler, optimization level and byte order
// (tools/state_hash_test.cpp pins it).
#include "sim/simpool.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tak::sim {

// stateHash's fold of its words: one xor-multiply-xorshift round per 64-bit
// word, the words dealt to four lanes in turn. A lane's rounds depend on each
// other, the lanes' do not, so four multiplies are in flight at once (the
// byte-wise FNV-1a this replaced spent eight dependent multiplies a word).
// Each round is a bijection of its lane, so one changed word always changes
// the digest.
struct WordMix {
    uint64_t lane0,lane1,lane2,lane3,words=0;
    explicit WordMix(uint64_t seed):lane0(seed),lane1(seed^0x9e3779b185ebca87ull),
        lane2(seed^0xc2b2ae3d27d4eb4full),lane3(seed^0x165667b19e3779f9ull) {}
    void operator()(uint64_t v) {
        uint64_t x=(lane0^v)*0x9e3779b97f4a7c15ull;
        x^=x>>32;
        lane0=lane1;lane1=lane2;lane2=lane3;lane3=x;
        ++words;
    }
    uint64_t digest() const {
        uint64_t x=std::rotl(lane0,1)+std::rotl(lane1,7)+std::rotl(lane2,12)+std::rotl(lane3,18)+words;
        x^=x>>33;x*=0xc2b2ae3d27d4eb4full;x^=x>>29;x*=0x165667b19e3779f9ull;
        return x^(x>>32);
    }
};

// Four cells make a word, packed little-endian, and the words go round-robin
// to four independent xxHash64 lanes, so the multiplies overlap instead of
// queueing one behind another. The last word is zero-padded; the cell count
// is folded, so the padding is unambiguous.
template<class CellAt>
uint64_t foldCells16(size_t n,CellAt cellAt) {
    constexpr uint64_t p1=0x9e3779b185ebca87ull,p2=0xc2b2ae3d27d4eb4full;
    constexpr uint64_t p3=0x165667b19e3779f9ull,p4=0x85ebca77c2b2ae63ull;
    auto round=[](uint64_t lane,uint64_t word) {return std::rotl(lane+word*p2,31)*p1;};
    auto word=[&](size_t i) {
        return uint64_t(cellAt(i))|uint64_t(cellAt(i+1))<<16|uint64_t(cellAt(i+2))<<32|uint64_t(cellAt(i+3))<<48;
    };
    uint64_t lane[4]={p1+p2,p2,0,0-p1};
    size_t i=0;
    for (;i+16<=n;i+=16) {
        lane[0]=round(lane[0],word(i));lane[1]=round(lane[1],word(i+4));
        lane[2]=round(lane[2],word(i+8));lane[3]=round(lane[3],word(i+12));
    }
    for (int k=0;i<n;i+=4,++k) {
        uint64_t w=0;
        for (size_t j=0;j<4 && i+j<n;++j) w|=uint64_t(cellAt(i+j))<<(16*j);
        lane[k]=round(lane[k],w);
    }
    uint64_t h=std::rotl(lane[0],1)+std::rotl(lane[1],7)+std::rotl(lane[2],12)+std::rotl(lane[3],18);
    for (uint64_t l:lane) h=(h^round(0,l))*p1+p4;
    h+=uint64_t(n);
    h^=h>>33;h*=p2;h^=h>>29;h*=p3;h^=h>>32;
    return h;
}

// A plane folds in fixed chunks of 64Ki cells, chunk(begin,end) digesting one
// (foldCells16 over it, say), and the digests are combined in chunk order: the
// same value on any number of workers (pool = nullptr runs them inline), while
// every thread of the pool reads the plane.
template<class Chunk>
uint64_t foldPlane(SimPool* pool,size_t n,Chunk chunk) {
    constexpr size_t kCells=size_t(1)<<16;
    std::vector<uint64_t> digests((n+kCells-1)/kCells);
    auto run=[&](size_t k,size_t begin,size_t end,unsigned) {digests[k]=chunk(begin,end);};
    if (pool) pool->forChunks(n,kCells,2*kCells,[&](size_t k,size_t begin,size_t end,unsigned slot) {run(k,begin,end,slot);});
    else for (size_t k=0;k<digests.size();++k) run(k,k*kCells,std::min(n,(k+1)*kCells),0u);
    uint64_t h=uint64_t(n)*0x9e3779b185ebca87ull;
    for (uint64_t d:digests) h=std::rotl(h^d,27)*0x9e3779b185ebca87ull+0x85ebca77c2b2ae63ull;
    h^=h>>33;h*=0xc2b2ae3d27d4eb4full;h^=h>>29;
    return h;
}

} // namespace tak::sim
