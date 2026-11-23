// World::stateHash's plane folds (sim/statefold.h): pinned values, so every
// compiler, optimization level and architecture the CI builds must agree on
// them, and the same hash whether the pool folds the chunks or the caller does.
#include "sim/statefold.h"
#include "sim/sim.h"
#include <cstdio>
#include <vector>
namespace {
int failures=0;
void check(bool ok,const char* what) {
    if (!ok) { std::fprintf(stderr,"FAIL: %s\n",what);++failures; }
}
std::vector<uint16_t> cells(size_t n) {
    std::vector<uint16_t> v(n);
    uint64_t s=0x9e3779b97f4a7c15ull;
    for (auto& c:v) { s^=s<<13;s^=s>>7;s^=s<<17;c=(s&7)<3 ? uint16_t(s>>20) : 0; }
    return v;
}
uint64_t plane(tak::sim::SimPool* pool,const std::vector<uint16_t>& v) {
    return tak::sim::foldPlane(pool,v.size(),[&](size_t begin,size_t end) {
        return tak::sim::foldCells16(end-begin,[&](size_t i){return v[begin+i];});
    });
}
}
int main() {
    using namespace tak::sim;
    // Pinned: a change here changes every lockstep hash (a protocol bump).
    const struct { size_t n; uint64_t fold; } pinned[]={
        {0,0x3fdf455f9dcf1e62ull},{1,0x9fbf3a3707d89113ull},{5,0x9dc2a3147c26def9ull},
        {17,0x196645c29ea0ef9dull},{1000003,0x82aaab98ec8cc357ull}};
    for (const auto& p:pinned) {
        const auto v=cells(p.n);
        const uint64_t fold=foldCells16(v.size(),[&](size_t i){return v[i];});
        if (fold!=p.fold) std::fprintf(stderr,"n=%zu fold=%016llx\n",p.n,(unsigned long long)fold);
        check(fold==p.fold,"foldCells16 matches its pinned value");
    }
    const auto big=cells(5*65536+7);
    const uint64_t inline0=plane(nullptr,big);
    if (inline0!=0x0f3c20b81853a53cull) std::fprintf(stderr,"foldPlane=%016llx\n",(unsigned long long)inline0);
    check(inline0==0x0f3c20b81853a53cull,"foldPlane matches its pinned value");
    SimPool one(1),three(3),wide(23);
    check(plane(&one,big)==inline0 && plane(&three,big)==inline0 && plane(&wide,big)==inline0,
          "foldPlane is the same on any number of workers");
    auto changed=big;changed[3*65536+11]^=1;
    check(plane(&three,changed)!=inline0,"one changed cell changes the plane");

    // A whole world on a map whose planes span several chunks: serial and
    // pooled worlds hash alike, tick after tick.
    UnitType type{};
    type.name=type.id="sight-probe";type.maxHp=100;
    type.maxVel=Fixed::fromInt(1);type.canMove=true;
    type.footX=type.footZ=1;type.sight=160;type.sightHeight=24;
    World serial,parallel,wideWorld;   // no workers, 3 and 23 (a 24-thread machine's most)
    serial.setSerialThreads(true);parallel.setSimWorkers(3);wideWorld.setSimWorkers(23);
    constexpr int kSize=1024;
    std::vector<uint8_t> terrain(size_t(kSize)*kSize);
    for (int z=0;z<kSize;++z) for (int x=0;x<kSize;++x)
        terrain[size_t(z)*kSize+x]=uint8_t((x*7+z*3)%180);
    for (auto* world:{&serial,&parallel,&wideWorld}) {
        world->setTerrain(terrain,kSize,kSize,30);
        for (int i=0;i<4096;++i) world->spawn(&type,float((i%64)*256),float((i/64)*256),0,i%4);
    }
    uint64_t first=0;
    for (int step=0;step<6;++step) {
        for (auto* world:{&serial,&parallel,&wideWorld}) {
            for (size_t i=0;i<world->units().size();++i) {
                auto& u=world->units()[i];
                u.x=Fixed::fromInt(int((i*13+step*7)%1040)*16-32);
                u.z=Fixed::fromInt(int((i*7+step*11)%1040)*16-32);
            }
            world->tick(1.f/30);
        }
        const uint64_t a=serial.stateHash(),b=parallel.stateHash(),c=wideWorld.stateHash();
        if (a!=b || a!=c)
            std::fprintf(stderr,"step %d: serial %016llx pooled %016llx (3 workers) %016llx (23)\n",step,
                         (unsigned long long)a,(unsigned long long)b,(unsigned long long)c);
        check(a==b && a==c,"serial and pooled worlds hash alike");
        if (!step) first=a; else check(a!=first,"the hash follows the state");
    }
    check(serial.viewCounts().size()>2*65536 && serial.navigationExploration().size()>2*65536,
          "the world's planes span several chunks");
    std::printf("state hash: %d failures\n",failures);
    return failures?1:0;
}
