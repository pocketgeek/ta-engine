// Buildings retain their placement heading while their weapon scripts aim.
//   structure_combat_test <data>         heading cases
//   structure_combat_test <data> reach   a unit that may not leave its post
//     (a non-ballistic tower: L.L.T., Sentinel, Gaat Gun; a Peewee on Hold
//     Position) never auto-acquires an enemy outside tickCombat's reach: it
//     would drop it at once, and every acquisition wipes the fire-at-will
//     sweep.
//   structure_combat_test <data> site    an unfinished Punisher under a
//     Guardian's fire takes no target (retail's hit response skips a site);
//     finished, it shoots the Guardian.
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/retailaim.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
namespace {
// One unit `id` at the centre of a flat field and an enemy structure 20 px
// inside its weapon range (a structure is measured centre to centre, as
// retail's 530580, for every TA weapon: no footprint pad). With
// `decoy`, a mobile enemy also stands nearer, but past the reach: before the
// fix, findTarget took the nearer decoy on every acquisition turn,
// tickCombat dropped it at once, and each acquisition wiped the sweep's slot
// targets, so the structure in reach was barely shot. Returns the damage the
// structure takes.
long reachCase(const ta::sim::TypeRegistry& registry,const char* id,bool hold,int slot,
               float reach,bool decoy,int ticks) {
    const auto& type=*registry.find(id);
    ta::sim::World w;
    w.setTerrain(std::vector<uint8_t>(512*512,70),512,512,64);
    w.buildNavClasses(registry);
    auto mobile=*registry.find("armpw");
    mobile.maxHp=30000;mobile.healTime=0;mobile.weapons.clear();mobile.weapon.damage=0;
    auto building=*registry.find("corrad");   // ShootMe=1: the sweep takes an unarmed building only with it
    building.maxHp=30000;building.healTime=0;building.weapons.clear();building.weapon.damage=0;
    (void)slot;
    const float pad=0.0f;   // every TA weapon class reaches a structure centre to centre
    const int a=w.spawn(&type,4096,4096,0,0);
    // Past the range, inside range+pad: a centre distance of reach+pad/2;
    // with no pad, 20 px inside the range.
    const int inner=w.spawn(&building,4096,4096-(pad>0.0f ? reach+pad*0.5f : reach-20.0f),0,1);
    const int outer=decoy ? w.spawn(&mobile,4096+reach+std::max(pad*0.25f,10.0f),4096,0,1) : 0;
    // An unarmed spotter of the shooter's beside the structure: the sweep and
    // the mission scan take only what the player sees (list +5), and the
    // cases measure the reach, not the shooter's sight.
    const float innerZ=4096-(pad>0.0f ? reach+pad*0.5f : reach-20.0f);
    w.spawn(&mobile,4096-8.0f*float(building.footX)-64.0f,innerZ,0,0);
    w.setStartResources(0,1e6f,1e6f);   // energypershot (L.L.T. 10, Sentinel 75)
    if(hold)w.unit(a)->moveState=0;
    long damage=0;
    for(int tick=0;tick<ticks;++tick) {
        if(outer)w.unit(outer)->hp=ta::sim::Fixed::fromInt(30000);
        w.unit(inner)->hp=ta::sim::Fixed::fromInt(30000);
        w.unit(a)->hp=ta::sim::Fixed::fromInt(type.maxHp);
        w.tick(1.f/30.f);
        damage+=30000-w.unit(inner)->hp.floorInt();
    }
    return damage;
}
// Compares the structure's damage with and without the decoy.
int reachPair(const ta::sim::TypeRegistry& registry,const char* id,bool hold,int slot,float reach) {
    const long alone=reachCase(registry,id,hold,slot,reach,false,1800);
    const long decoyed=reachCase(registry,id,hold,slot,reach,true,1800);
    // Shots are few (a Sentinel reloads in ~1 s, 180 a hit) and splash varies, so
    // half is the bar; the churn left it at nothing.
    const bool ok=alone>0 && decoyed*2>=alone;
    std::printf("%s %s%s slot=%d reach=%.0f: structure in reach takes %ld alone, %ld beside an out-of-reach decoy\n",
                ok?"PASS":"FAIL",id,hold?" (hold)":"",slot,reach,alone,decoyed);
    return ok?0:1;
}
// tickCombat's reach without a structure pad: the longest weapon's range
// when every weapon fires.
float reachOf(const ta::sim::UnitType& t,int slot) {
    if(t.weapons.size()>1)return t.maxRange();
    return float(t.weapons[size_t(std::max(slot,0))].range);
}
int reachCases(const char* data) {
    auto vfs=ta::hpi::mountRetailRoot(data);
    ta::sim::TypeRegistry registry;ta::sim::setupRegistry(registry,vfs);
    int failures=0;
    for(const char* id:{"armllt","armhlt","corhlt","armpw","corrad"})
        if(!registry.find(id)){std::printf("FAIL missing type %s\n",id);return 1;}
    // Non-ballistic towers: the Light Laser and the two High Energy Lasers.
    for(const char* id:{"armllt","armhlt","corhlt"}) {
        const auto& t=*registry.find(id);
        if(t.weapons.empty() || t.weapons[0].ballisticClass()) {
            std::printf("FAIL %s: expected a non-ballistic ranged weapon\n",id);++failures;continue;
        }
        failures+=reachPair(registry,id,false,-1,reachOf(t,-1));
    }
    // A mobile unit on Hold Position: its E.M.G.'s reach, and no further.
    failures+=reachPair(registry,"armpw",true,-1,reachOf(*registry.find("armpw"),-1));
    std::printf("structure_combat reach: %d failures\n",failures);
    return failures?1:0;
}
// A site under long-range fire (first seen in a TAK duel: a site took its
// shooter as an auto target from the hit response and held it without a shot
// until it was finished). Retail's hit response skips a victim with
// construction left (KINGDOMS.icd 409b56..409b67: unit +0x108 != 0.0 jumps to
// the exit; unverified for TotalA.exe), so a site takes no target. Here: an
// unfinished Core Punisher under an Arm Guardian's plasma at 1000 px on a flat
// field; the site must hold no target, order or slot, through several hits,
// and once finished the Punisher picks the Guardian itself and shoots it.
int siteCase(const char* data) {
    auto vfs=ta::hpi::mountRetailRoot(data);
    ta::sim::TypeRegistry registry;ta::sim::setupRegistry(registry,vfs);
    const auto* punisher=registry.find("corpun");const auto* guardian=registry.find("armguard");
    if(!punisher || !guardian){std::printf("FAIL missing corpun or armguard\n");return 1;}
    const float distance=1000.0f;   // inside both 350 px/s shells' flat-ground arc (~1094 px)
    if(punisher->weapons.empty() || guardian->weapons.empty() || distance>punisher->weapons[0].range ||
       distance>guardian->weapons[0].range || distance<guardian->weapons[0].minRange)
        {std::printf("FAIL the duel's distance is not in both reaches\n");return 1;}
    ta::sim::World w;
    w.setTerrain(std::vector<uint8_t>(512*512,70),512,512,64);
    w.buildNavClasses(registry);
    const int site=w.spawn(punisher,4096,4096,0,0);
    const int shooter=w.spawn(guardian,4096+distance,4096,0,1);
    w.unit(site)->underConstruction=true;
    // No GetBuilt on it (as a site that changed sides): an unworked frame's first
    // decay step (TotalA.exe 0x41bcd0) would unbuild it at tick 330.
    w.unit(site)->siteGetBuilt=false;
    // A spotter per side, passive and without shootme (a human-owned sweep
    // skips it): neither structure's sight reaches 1000 px, and a structure
    // fires only through the sweep (Guard_NoMove picks nothing, 401a90) at
    // what its player sees (40f330, list +5).
    auto eye=*registry.find("armpw");eye.id="eye";eye.shootMe=false;
    w.setShootAll(false);   // retail's gate, which the passive spotters rely on
    for(const auto& [x,player]:{std::pair{4096.0f+distance-150,0},{4096.0f+150,1}}) {
        const int id=w.spawn(&eye,x,4096+120,0,player);
        w.unit(id)->fireState=0;w.unit(id)->moveState=0;
    }
    // Kept just under full each tick: one plasma shell (360) never kills it, and
    // an unattended site neither decays nor counts as finished.
    const auto siteHp=ta::sim::Fixed::fromInt(punisher->maxHp-1);
    const auto keep=ta::sim::Fixed::fromInt(guardian->maxHp);
    int hits=0,heldTicks=0;
    for(int tick=0;tick<1800;++tick) {
        w.unit(site)->hp=siteHp;   // no decay, no death
        w.unit(shooter)->hp=keep;
        w.tick(1.f/30.f);
        if(!w.unit(site) || !w.unit(site)->alive()){std::printf("FAIL the site died at tick %d\n",tick);return 1;}
        const auto& u=*w.unit(site);
        if(u.lastHitBy==shooter && u.hp<siteHp-ta::sim::Fixed::fromInt(100))++hits;   // a shell, not decay
        if(!u.orders.empty() || u.slotTarget[0] || u.scriptAimTarget)++heldTicks;
    }
    const bool quiet=hits>=2 && heldTicks==0;
    std::printf("%s unfinished corpun under armguard fire at %.0f px: %d hits, %d ticks with a target\n",
                quiet?"PASS":"FAIL",distance,hits,heldTicks);
    // Finished, the Punisher chooses for itself (the sweep, its spotter seeing).
    w.unit(site)->underConstruction=false;
    long damage=0;
    for(int tick=0;tick<900;++tick) {
        w.unit(shooter)->hp=keep;w.unit(site)->hp=ta::sim::Fixed::fromInt(punisher->maxHp);
        w.tick(1.f/30.f);
        damage+=guardian->maxHp-w.unit(shooter)->hp.floorInt();
    }
    const bool fires=damage>0;
    std::printf("%s finished corpun deals %ld to the armguard in 30 s\n",fires?"PASS":"FAIL",damage);
    return quiet && fires?0:1;
}
} // namespace
int main(int argc,char** argv) {
    if(argc==3 && std::strcmp(argv[2],"reach")==0)return reachCases(argv[1]);
    if(argc==3 && std::strcmp(argv[2],"site")==0)return siteCase(argv[1]);
    if(argc!=2)return 2;
    auto vfs=ta::hpi::mountRetailRoot(argv[1]);
    int failures=0,cases=0;
    {
        ta::sim::TypeRegistry registry;ta::sim::setupRegistry(registry,vfs);
        for(const auto& [id,type]:registry.types()) {
            if(!type.isStructure() || type.weapon.damage<=0)continue;
            // Weapons that never take a ground unit on an Attack order: an
            // anti-air gun (toairweapon, bit 17), a stockpiled missile with no
            // stock (bit 28: Retaliator, Silencer, Annihilator...) and an
            // anti-missile interceptor (bit 30) (docs/retail-weapon-scripts.md
            // section 1, weapon +0x111).
            if(type.weapon.hasTaFlag(17) || type.weapon.hasTaFlag(28) || type.weapon.hasTaFlag(30))continue;
            for(float fraction:{0.6f,0.98f,1.1f}) for(int direction=0;direction<4;++direction) {
                ta::sim::World w;
                const bool water=type.domain==ta::sim::UnitType::Domain::Water;
                // A field wide enough for the long-range guns (Intimidator 5120).
                const int cells=type.weapon.range>3500 ? 1024 : 512;
                const float centre=float(cells*8);
                w.setTerrain(std::vector<uint8_t>(size_t(cells)*cells,water?10:70),cells,cells,64);
                w.buildNavClasses(registry);
                auto victim=*registry.find(water?"armpt":"armpw");
                victim.maxHp=30000;victim.healTime=0;victim.weapons.clear();victim.weapon.damage=0;
                const float distance=std::max(float(type.weapon.minRange)+50,float(type.weapon.range)*fraction);
                const float dx=direction==0?distance:direction==1?-distance:0;
                const float dz=direction==2?distance:direction==3?-distance:0;
                int a=w.spawn(&type,centre,centre,0,0),b=w.spawn(&victim,centre+dx,centre+dz,0,1);
                w.setStartResources(0,1e6f,1e6f);   // energypershot
                const auto initial=w.unit(a)->heading;
                w.attack(a,b,false);
                bool fired=false,turned=false;
                // 42 s: the long-range guns' scripts turn the turret at <5>
                // degrees a second (armbrtha.bos, corint.bos AimPrimary), so
                // a target straight behind takes 36 s of turning before AimX
                // returns and the first shell leaves.
                for(int tick=0;tick<1260 && !fired;++tick) {
                    w.unit(b)->hp=ta::sim::Fixed::fromInt(30000);
                    w.tick(1.f/30.f);
                    fired|=w.unit(a)->justFired;
                    turned|=w.unit(a)->heading.v!=initial.v;
                }
                // A ballistic weapon fires only where it has an arc from the
                // unit origin (docs/retail-weapon-scripts.md section 3, range
                // gate 0x49aa80): a Guardian's 350 px/s shell carries ~1094 px
                // on flat ground, short of its 1250 range.
                const auto& wp=type.weapon;
                const bool arc=!wp.ballisticClass() ||
                    ta::retailBallisticPitch(distance,0,0,wp.projVel/30.f,wp.gravityAdj,wp.lobPreferred,
                                             w.ballisticGravityRaw())!=0x8000;
                ++cases;
                std::printf("%s direction=%d range=%.2f fired=%d turned=%d%s\n",id.c_str(),direction,fraction,fired,turned,
                            arc?"":" (no arc)");
                if(turned || (fraction<1 && arc && !fired))++failures;
            }
        }
    }
    std::printf("structure_combat: %d cases, %d failures\n",cases,failures);
    return failures?1:0;
}
