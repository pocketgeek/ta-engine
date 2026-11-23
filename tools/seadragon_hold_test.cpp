// A Sea Dragon on Hold Position, fire at will, with an enemy 472 px off:
// inside Fire Breath's 525 and Water Ball's 800, outside the Tsunami's 450.
//   seadragon_hold_test <data> [trace]
//
// Owned by a computer player it never fired in the original
// (tools/re/probe_seadragon_switch.py runs the sweep 40f330 and the switch
// 51a8b0 natively). The AI weapon switch (40f411..40fb92) tests no range:
// with Fire Breath holding a lone, tough enemy it takes the Tsunami
// (aoe 180, no ally near the target), whose own sweep finds nothing in 450;
// with no target the costly Tsunami goes back to the primary on
// rand(10) == 0, and each return restarts Fire Breath's switchreloadtime,
// so it never fired. Ours (engine deviation, World::aiSlotServes): a target
// the sweep found must be in the chosen weapon's range from where the unit
// stands, so it fights with a weapon that reaches and does not flip.
//
// Owned by a player (no AI switching) the same Sea Dragon keeps Fire Breath
// and fires it.
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/powercodes.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
using namespace tak;
namespace {
int failures=0;
void check(bool ok,const std::string& label) {
    std::printf("%s %s\n",ok?"PASS":"FAIL",label.c_str());failures+=!ok;
}
struct Result { int shots[3]{}; int switches=0; int backToBreath=0; int longestBreath=0; bool reloadRestarted=true; };
Result runCase(const sim::TypeRegistry& registry,const char* targetId,float dist,bool ai,bool trace,int ticks) {
    const auto& dragon=*registry.find("verdrag");
    sim::World w;
    w.setSerialThreads(true);
    w.setPlayerCount(2);
    w.setTerrain(std::vector<uint8_t>(512*512,70),512,512,64);
    w.buildNavClasses(registry);
    w.setPathService(true);
    w.player(0).automaticGates=ai;   // computer-owned: AI weapon switching
    w.player(0).mana=1e6;
    w.setPowerCodes(true);
    w.applyPowerCode(0,sim::PowerCode::NowISee,sim::PowerArg::None,0);
    auto target=*registry.find(targetId);
    target.maxHp=30000;target.healTime=0;target.weapons.clear();target.weapon.damage=0;
    const int t=w.spawn(&target,4096,4096,0,1);
    w.setStance(t,2);w.unit(t)->moveState=0;w.unit(t)->fireState=0;
    for(int i=0;i<15;++i)w.tick(1.f/30);
    const int a=w.spawn(&dragon,4096-dist,4096,1.5707963f,0);
    w.unit(a)->mana=dragon.maxMana;
    w.unit(a)->moveState=0;w.unit(a)->fireState=2;
    Result r;
    int lastSlot=w.unit(a)->weaponSlot,run=0;
    for(int tick=0;tick<ticks;++tick) {
        w.unit(t)->hp=sim::Fixed::fromInt(30000);
        w.tick(1.f/30);
        const sim::Unit& u=*w.unit(a);
        for(int s=0;s<3;++s)if(u.firedWeapons>>s&1)++r.shots[s];
        if(u.weaponSlot!=lastSlot) {
            ++r.switches;
            if(u.weaponSlot==0) {
                ++r.backToBreath;
                // The reload may already have counted down once this tick.
                const int full=int(dragon.weapons[0].switchReloadTicks);
                r.reloadRestarted=r.reloadRestarted && u.reloads[0]>=full-1;
            }
            lastSlot=u.weaponSlot;
        }
        run=u.weaponSlot==0 ? run+1 : 0;
        r.longestBreath=std::max(r.longestBreath,run);
        if(trace && (tick%5==0 || u.firedWeapons))
            std::printf("t=%4d slot=%d tgt=%d,%d,%d fired=%x reload=%d/%d/%d mana=%.0f pos=%.1f,%.1f orders=%zu\n",
                        tick,u.weaponSlot,u.slotTarget[0],u.slotTarget[1],u.slotTarget[2],u.firedWeapons,
                        u.reloads[0],u.reloads[1],u.reloads[2],u.mana,u.x.toFloat(),u.z.toFloat(),u.orders.size());
    }
    return r;
}
} // namespace
int main(int argc,char** argv) {
    if(argc<2)return 2;
    const bool trace=argc>2 && std::strcmp(argv[2],"trace")==0;
    auto vfs=hpi::mountRetailRoot(argv[1]);
    sim::TypeRegistry registry;sim::setupRegistry(registry,vfs,false);
    for(const char* id:{"verdrag","araarch","aralode"})
        if(!registry.find(id)){std::printf("FAIL missing type %s\n",id);return 1;}
    const auto& dragon=*registry.find("verdrag");
    if(!dragon.weaponSwitching || dragon.weapons.size()<3 || dragon.weapons[0].range<=472 ||
       dragon.weapons[2].range>=472 || dragon.weapons[0].switchReloadTicks<=30) {
        std::printf("FAIL verdrag: expected Fire Breath past 472, the Tsunami short of it\n");return 1;
    }
    // A unit, and a building (measured centre to centre, as retail's 530580).
    for(const char* tgt:{"araarch","aralode"}) {
        const Result ai=runCase(registry,tgt,472.5f,true,trace,900);
        std::printf("  %s, computer-owned: shots %d/%d/%d, %d switches, back to Fire Breath %d times, longest %d ticks\n",
                    tgt,ai.shots[0],ai.shots[1],ai.shots[2],ai.switches,ai.backToBreath,ai.longestBreath);
        check(ai.shots[0]+ai.shots[1]>0 && ai.shots[2]==0,
              std::string(tgt)+": the computer's Sea Dragon fires with weapons that reach (the original never fired)");
        check(ai.switches<20,std::string(tgt)+": it no longer flips between Fire Breath and the Tsunami");
        const Result player=runCase(registry,tgt,472.5f,false,trace,900);
        std::printf("  %s, player-owned: shots %d/%d/%d, %d switches\n",tgt,player.shots[0],player.shots[1],player.shots[2],player.switches);
        check(player.shots[0]>0 && player.switches==0,std::string(tgt)+": a player's Sea Dragon keeps Fire Breath and fires it");
    }
    std::printf("seadragon_hold: %d failures\n",failures);
    return failures?1:0;
}
