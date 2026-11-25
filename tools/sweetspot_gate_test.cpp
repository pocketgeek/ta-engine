// TotalA.exe's fire gate measures to the SweetSpot.
//
// The weapon update's gate 0x49aa80 (called at 0x49e3c8) takes the slot's
// target point -- a unit target's SweetSpot (0x48a1e0 -> 0x43e3c0) -- and
// tests the horizontal range from the shooter's origin to it, the surfaced
// test and the arc test. Attack_Chase decides "in range" to the target's
// origin (0x49abb0), but walks a path into the range circle (0x438930) and so
// ends inside it; World stops the walker on the origin test, so an ordered
// attacker keeps closing until its target point passes the gate too
// (tickCombat's sweetShort). Checked here: walkers ordered onto targets from
// out of range all fire, and every shot leaves with its target point inside
// the weapon's range of the shooter (the gate held).
//
//   sweetspot_gate_test <retail-install-dir>
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ta::sim;

namespace {

constexpr float kTick=1.f/30;
int failures=0;
void check(bool ok,const std::string& what) {
    std::printf("[%s] %s\n",ok?"PASS":"FAIL",what.c_str());
    if (!ok) ++failures;
}

} // namespace

int main(int argc,char** argv) {
    if (argc!=2) return 2;
    auto vfs=ta::hpi::mountRetailRoot(argv[1]);
    TypeRegistry registry;setupRegistry(registry,vfs);
    struct Pair { const char* shooter; const char* target; float dx,dz; };
    const Pair pairs[]={{"armpw","corak",0,420},{"armpw","corllt",300,300},{"armham","corstorm",-200,500},
                        {"armstump","corraid",450,0},{"corak","armsolar",0,-380},{"armrock","corstorm",350,-350}};
    for (const Pair& k:pairs) {
        const UnitType* s=registry.find(k.shooter);const UnitType* t=registry.find(k.target);
        if (!s || !t) { check(false,std::string("missing ")+k.shooter+" or "+k.target);continue; }
        World w;w.setSerialThreads(true);w.setPlayerCount(2);
        w.setTerrain(std::vector<uint8_t>(256*256,60),256,256,20);
        w.buildNavClasses(registry);w.setPathService(true);
        const int a=w.spawn(s,1500,1500,0.f,0);
        const int b=w.spawn(t,1500+k.dx,1500+k.dz,0.f,1);
        // Both last the whole check (a target may shoot back).
        if (Unit* tu=w.unit(b)) tu->hp=Fixed::fromInt(30000);
        if (Unit* su=w.unit(a)) su->hp=Fixed::fromInt(30000);
        w.tick(kTick);
        w.attack(a,b,false);
        const Weapon& wpn=s->weapons.at(0);
        int shots=0,outside=0;uint32_t first=0;
        for (int i=0;i<900;++i) {
            w.tick(kTick);
            const Unit* su=w.unit(a);
            if (!su || !w.unit(b)) break;
            const Unit& u=*su;
            if (!(u.firedWeapons&1u)) continue;
            ++shots;if (!first) first=w.tickCount();
            const auto& p=u.weaponAimPoints[0];
            const double dx=double(p[0]-u.x.v)/65536.0,dz=double(p[2]-u.z.v)/65536.0;
            if (std::sqrt(dx*dx+dz*dz)>double(wpn.range)+1.0) ++outside;
        }
        std::printf("  %s -> %s: %d shots, first at tick %u\n",k.shooter,k.target,shots,first);
        check(shots>0,std::string(k.shooter)+" ordered onto "+k.target+" from out of range closes in and fires");
        check(outside==0,std::string(k.shooter)+"'s shots all leave with the SweetSpot inside its range (0x49aa80)");
    }
    std::printf("%s (%d failure%s)\n",failures?"FAILED":"OK",failures,failures==1?"":"s");
    return failures?1:0;
}
