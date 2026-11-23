#include "client/retailfog.h"
#include "sim/sim.h"
#include <cstdio>
#include <stdexcept>

using namespace tak::sim;
namespace rf = tak::retailfog;
static void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static void advance(World& w) { for (int i=0;i<20;++i) w.tick(1.f/30); }
// The local display plane of `player` (client/retailfog.h), as GameView::captureFog
// builds it from the World's view counts and Mapping bits.
static std::vector<uint8_t> plane(const World& w, int player, bool revealed=false) {
    rf::PlaneSource s;
    s.viewCounts=w.viewCounts();s.explored=w.navigationExploration();
    s.width=w.viewPlaneW();s.height=w.viewPlaneH();
    s.viewers=1u<<player;s.player=player;
    s.mapRevealed=revealed || w.mapRevealedFor(player);s.losOff=w.powerLosOff(player);
    std::vector<uint8_t> out;rf::buildPlane(s,out);
    return out;
}
// Flat terrain at height 0: a ground point's projected cell is (x/32, z/32).
static uint8_t at(const World& w, const std::vector<uint8_t>& p, float x, float z) {
    return p[size_t(int(z)/32)*size_t(w.viewPlaneW())+size_t(int(x)/32)];
}
int main() {
    UnitType eyes; eyes.id=eyes.name="eyes";eyes.maxHp=100;eyes.sight=128;eyes.sightHeight=40;
    eyes.footX=eyes.footZ=1;
    const auto setup=[](World& w) {
        w.setSerialThreads(true);w.setPlayerCount(2);
        w.setTerrain(std::vector<uint8_t>(128*128,0),128,128,0);
    };
    World w;setup(w);
    const int id=w.spawn(&eyes,512,512,{},0);
    advance(w);
    check(at(w,plane(w,0),512,512)==rf::kInView,"unit reveals its position");
    check(at(w,plane(w,0),720,512)!=rf::kInView,"ordinary sight does not reach distant point");
    w.unit(id)->x=Fixed::fromInt(1408);
    advance(w);
    check(at(w,plane(w,0),512,512)==rf::kExplored,"lost line of sight hides units, terrain stays explored");
    check(at(w,plane(w,0),1760,1760)==rf::kUnexplored,"unvisited terrain stays unexplored");
    const auto hash=w.stateHash();
    check(at(w,plane(w,0,true),1760,1760)==rf::kExplored,"mapped terrain starts foggy, not visible");
    check(w.stateHash()==hash,"terrain display mode does not alter simulation");

    for (bool radar:{false,true}) {
        World normal,doubled;setup(normal);setup(doubled);doubled.setDoubleSight(true);
        UnitType type=eyes;
        if (radar) {type.sight=16;type.radar=128;}
        normal.spawn(&type,512,512,{},0);doubled.spawn(&type,512,512,{},0);
        advance(normal);advance(doubled);
        if (!radar) {
            check(at(normal,plane(normal,0),720,512)!=rf::kInView &&
                  at(doubled,plane(doubled,0),720,512)==rf::kInView,"double vision doubles sight");
            check(at(doubled,plane(doubled,0),832,512)!=rf::kInView,"double vision remains bounded");
            const auto& a=normal.navigationExploration();const auto& b=doubled.navigationExploration();
            size_t na=0,nb=0;for(auto v:a)na+=bool(v&1);for(auto v:b)nb+=bool(v&1);
            check(nb>na,"authoritative exploration uses doubled sight too");
        } else {
            // Radar (0x4f9970) only puts a dot on the minimap: the 200 px point is
            // within the doubled radar, not the plain one, and in view of neither.
            rf::RadarCover n,d;
            n.add(Fixed::fromInt(512).v,Fixed::fromInt(512).v,normal.radarDistance(type));n.finish();
            d.add(Fixed::fromInt(512).v,Fixed::fromInt(512).v,doubled.radarDistance(type));d.finish();
            const int32_t px=Fixed::fromInt(712).v,pz=Fixed::fromInt(512).v;
            check(!n.covers(px,pz) && d.covers(px,pz),"double vision doubles radar");
            check(!d.covers(Fixed::fromInt(832).v,pz),"double radar remains bounded");
            check(at(doubled,plane(doubled,0),712,512)!=rf::kInView,"radar never clears fog");
        }
        check(normal.stateHash()!=doubled.stateHash(),"vision rule mismatch is detectable");
    }
    std::puts("PASS: persistent fog memory, mapped terrain, doubled sight/radar, authoritative exploration");
}
