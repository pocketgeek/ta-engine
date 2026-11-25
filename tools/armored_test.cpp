// TotalA.exe's ARMORED bit and the script's ACTIVATION.
//
// SET 20 (0x480be3) writes unit +0x10e bit 1 through 0x48b090(2, value),
// which starts no call-in; GET 20 reads it back (0x480aa9). While it is set a
// hit or record under 30000 goes through the type's damagemodifier
// (0x489bc3..0x489bf1; the test is `+0x10e & 2`, not the activation bit).
// The Toaster (cortoast, damagemodifier 0.15) sets ARMORED in Stop and
// clears it in Go: closed it takes 15% of a hit, open all of it.
//
// SET 1 (0x480b49 -> 0x48b090(1, value)) switches the activation: a change
// starts Activate or Deactivate (0x48b101 / 0x48b126). armsolar's
// HitByWeapon closes the collector (SET ACTIVATION 0) and reopens it after
// eight seconds (SET ACTIVATION 1).
//
//   armored_test <retail-install-dir>
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"

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

void setup(World& w,const TypeRegistry& registry) {
    w.setSerialThreads(true);
    w.setPlayerCount(2);
    w.setTerrain(std::vector<uint8_t>(256*256,60),256,256,20);
    w.buildNavClasses(registry);
    w.setStartResources(0,1e6f,1e6f);w.setStartResources(1,1e6f,1e6f);
}

int hpOf(World& w,int id) { const Unit* u=w.unit(id);return u ? u->hp.floorInt() : -1; }

} // namespace

int main(int argc,char** argv) {
    if (argc!=2) return 2;
    auto vfs=ta::hpi::mountRetailRoot(argv[1]);
    TypeRegistry registry;setupRegistry(registry,vfs);
    for (const char* id:{"cortoast","armsolar","armpw"})
        if (!registry.find(id)) { std::printf("FAIL missing type %s\n",id);return 1; }
    const UnitType* toast=registry.find("cortoast");
    check(toast->damageModifier==0x2666,"cortoast's damagemodifier is 0.15 (16.16 0x2666)");

    {
        World w;setup(w,registry);
        const int t=w.spawn(toast,1000,1000,0.f,0);
        for (int i=0;i<90;++i) w.tick(kTick);
        // Created closed by its own statics (no Stop): +0x10e starts clear
        // (0x485b7f), so ARMORED reads 0 and a hit is taken whole.
        check(!w.scriptGetWords(t).armored,"a new Toaster answers GET ARMORED 0 (no SET 20 yet)");
        int before=hpOf(w,t);
        w.scenarioAdjustHealth(t,200,false);
        const int newLoss=before-hpOf(w,t);
        check(newLoss==200,"and takes a hit whole");

        // An enemy in range opens it (AimPrimary -> RequestState -> Go: SET
        // ARMORED 0); with the enemy gone, RestoreAfterDelay closes it (Stop:
        // SET ARMORED 1).
        const int target=w.spawn(registry.find("armsolar"),1000,1500,0.f,1);
        uint32_t armoredAt=0;
        for (int i=0;i<1800 && !armoredAt;++i) {
            w.tick(kTick);
            if (Unit* e=w.unit(target);e && e->alive() && i==600) e->hp=Fixed();
            if (w.scriptGetWords(t).armored) armoredAt=w.tickCount();
        }
        std::printf("  ARMORED set at tick %u\n",armoredAt);
        check(armoredAt!=0,"after firing it closes: GET ARMORED reads 1 (Stop's SET 20)");
        before=hpOf(w,t);
        w.scenarioAdjustHealth(t,1000,false);
        const int closedLoss=before-hpOf(w,t);
        std::printf("  closed: 1000 damage takes %d HP\n",closedLoss);
        check(closedLoss==int((1000ll*0x2666)>>16),"ARMORED, it takes a hit through its damagemodifier (0x489bcd)");
    }

    {
        World w;setup(w,registry);
        const int s=w.spawn(registry.find("armsolar"),1000,1000,0.f,0);
        for (int i=0;i<60;++i) w.tick(kTick);
        check(w.unitScriptActivated(s),"a built armsolar is active (activatewhenbuilt)");
        check(!w.scriptGetWords(s).armored,"and open: GET ARMORED 0 (Go's SET 20)");
        // A shot from an enemy runs its HitByWeapon: SET ACTIVATION 0, eight
        // seconds, SET ACTIVATION 1.
        const int pw=w.spawn(registry.find("armpw"),1000,1150,0.f,1);
        w.attack(pw,s,false);
        uint32_t closedAt=0,reopenedAt=0;
        bool armoredWhileClosed=false;
        for (int i=0;i<600 && !closedAt;++i) {
            w.tick(kTick);
            if (!w.unitScriptActivated(s)) closedAt=w.tickCount();
        }
        check(closedAt!=0,"a hit closes it: HitByWeapon's SET ACTIVATION 0 deactivates the script");
        if (Unit* p=w.unit(pw)) p->hp=Fixed();   // stop the shooting
        for (int i=0;i<60;++i) { w.tick(kTick);armoredWhileClosed=armoredWhileClosed || w.scriptGetWords(s).armored; }
        check(armoredWhileClosed,"closing runs Deactivate -> Stop: GET ARMORED 1");
        for (int i=0;i<600 && !reopenedAt;++i) {
            w.tick(kTick);
            if (w.unitScriptActivated(s)) reopenedAt=w.tickCount();
        }
        std::printf("  closed at %u, reopened at %u\n",closedAt,reopenedAt);
        check(reopenedAt!=0,"and SET ACTIVATION 1 reopens it after its sleep");
    }

    std::printf("%s (%d failure%s)\n",failures?"FAILED":"OK",failures,failures==1?"":"s");
    return failures?1:0;
}
