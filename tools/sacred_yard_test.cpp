// sacred_yard_test -- a lodestone's S yard ignores fixed map obstacles.
//
// Retail decodes yardmap S as mask 0x8f (4c0fe3; the one-character "S" every
// lodestone carries fills the footprint, 4c0f59..4c101f). 507400 then tests
// sacred-site coverage, height range, water depth and occupancy, but no feature
// bit, so a Divine Lodestone may cover the Standing Stones beside a sacred site
// (emulated: every covering origin passes, a blocker on a corner included). On
// Inner Circle the ZonHenge05 tails beside two of the four sites refused three
// covering origins here. Checks, on the real map:
//   - every faction's Divine Lodestone is admitted at those origins;
//   - partial coverage, bodies and a removable blocker still refuse;
//   - the stones stay blocked after a lodestone over them is destroyed.
// Needs a retail install, so it registers only with -DTAK_TEST_DATA.
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"
#include <cstdio>
#include <memory>
#include <string>
using namespace tak;
namespace {
int failures=0;
void check(bool ok,const std::string& label) {
    std::printf("%s %s\n",ok?"PASS":"FAIL",label.c_str());failures+=!ok;
}
struct Origin { int x,z; };
// Covering origins of the 3x3 yard whose corner cells hold a Standing Stone.
constexpr Origin kOverStones[]={{127,36},{23,76},{24,76}};
float centre(int origin,int foot) { return float(origin*16+foot*8); }
} // namespace

int main(int argc,char** argv) {
    if (argc!=2) return 2;
    auto vfs=hpi::mountRetailRoot(argv[1]);
    sim::TypeRegistry reg;sim::setupRegistry(reg,vfs,false);
    const std::string mapPath=hpi::findMap(vfs,"Inner Circle");
    auto setup=[&](sim::World& w) {
        sim::MatchConfig cfg;cfg.vfs=&vfs;cfg.mapPath=mapPath;
        cfg.slots={sim::MatchSlot{},sim::MatchSlot{}};
        sim::setupMatch(w,reg,cfg);
    };
    auto world=std::make_unique<sim::World>();auto& w=*world;
    setup(w);
    const int W=w.mapW();
    const auto& obstacles=w.obstacles();
    for (const char* id:{"aramana","tarmana","vermana","zonmana","cremana"}) {
        const auto* t=reg.find(id);
        check(t && t->sacredIncome && t->yardMap.empty() && t->footX==3 && t->footZ==3,
              std::string(id)+": a one-character S yard over a 3x3 footprint");
        if (!t) continue;
        for (const auto& o:kOverStones) {
            // The fixture must exercise the rule: some footprint cell is blocked.
            int blocked=0;
            for (int j=0;j<3;++j) for (int i=0;i<3;++i)
                blocked+=obstacles[size_t(o.z+j)*W+o.x+i]!=0 && !w.navFor(t).walkable(o.x+i,o.z+j);
            const std::string at=std::to_string(o.x)+","+std::to_string(o.z);
            check(blocked>0,std::string(id)+": origin "+at+" covers a Standing Stone");
            check(w.canPlace(t,centre(o.x,3),centre(o.z,3),0),
                  std::string(id)+": origin "+at+" over the stones is admitted (retail 0x8f)");
        }
        // Two of the four sacred cells: coverage still refuses.
        check(!w.canPlace(t,centre(128,3),centre(36,3),0),std::string(id)+": partial coverage refused");
    }
    const auto* mana=reg.find("aramana");
    const float sx=centre(23,3),sz=centre(76,3);
    {   // Occupancy: a unit inside the footprint refuses it.
        const int body=w.spawn(reg.find("arasword"),centre(23,1),centre(78,1),0,1);
        check(!w.canPlace(mana,sx,sz,0),"a unit inside the footprint refuses it");
        w.unit(body)->deadFor=sim::World::kRetiredTicks;
        w.tick(1.f/30);
        check(w.canPlace(mana,sx,sz,0),"admitted again once the unit is gone");
    }
    {   // A removable blocker (a registered feature) keeps the reclaim-first rule:
        // its later removal would clear the lodestone's own cells.
        sim::World f;setup(f);
        f.addFeature(77*W+23,centre(23,1),centre(77,1),10,1,1,true,-1);
        check(!f.canPlace(mana,sx,sz,0),"a removable blocking feature under the yard refuses it");
    }
    {   // Built over the stones and destroyed (scenario setup stamps twice):
        // the stone cells stay blocked, the open cells open again.
        std::vector<size_t> stones,open;
        for (int j=0;j<3;++j) for (int i=0;i<3;++i) {
            const size_t c=size_t(76+j)*W+23+i;
            (obstacles[c] ? stones : open).push_back(c);
        }
        check(!stones.empty() && !open.empty(),"the footprint holds stone cells and open cells");
        const int id=w.spawn(mana,sx,sz,0,0);
        w.blockFoot(*mana,sx,sz,true);w.blockFoot(*mana,sx,sz,true);
        bool all=true;
        for (int j=0;j<3;++j) for (int i=0;i<3;++i) all=all && obstacles[size_t(76+j)*W+23+i];
        check(all,"the lodestone blocks its whole footprint");
        w.scenarioDestroy(id);
        bool stonesKept=true,opened=true;
        for (size_t c:stones) stonesKept=stonesKept && obstacles[c];
        for (size_t c:open) opened=opened && !obstacles[c];
        check(stonesKept,"the Standing Stone cells stay blocked after the lodestone is destroyed");
        check(opened,"the open cells are free again");
    }
    {   // A map without sacred sites (a lodestone may stand on open ground
        // there, an engine extension): nothing ties the footprint to a site, so
        // a fixed blocker (a 0xFFFC wall cell) under it still refuses.
        constexpr int kMap=64;
        sim::World open;open.setPlayerCount(2);
        std::vector<uint16_t> cells(size_t(kMap)*kMap,0xffff);
        cells[size_t(20)*kMap+21]=0xfffc;
        open.setTerrain(std::vector<uint8_t>(size_t(kMap)*kMap,100),kMap,kMap,20,&cells);
        check(open.sacredSites().empty(),"no-site map: the fixture has no sacred site");
        check(open.canPlace(mana,centre(40,3),centre(40,3),0),"no-site map: open ground is admitted");
        check(!open.canPlace(mana,centre(20,3),centre(20,3),0),"no-site map: a wall cell under the yard refuses it");
    }
    std::printf("%s\n",failures ? "FAILED" : "PASS: sacred yard");
    return failures ? 1 : 0;
}
