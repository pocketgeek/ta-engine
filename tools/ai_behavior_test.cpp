// Command-level AI policy tests; movement continues through the normal World API.
#include "ai/ai.h"
#include "ai/builtinprofile.h"
#include "ai/classicbrain.h"
#include "ai/classicsquads.h"
#include "net/matchconfig.h"
#include "net/replayhdr.h"
#include "sim/matchsetup.h"
#include "hpi/hpi.h"
#include <cmath>
#include <cstdio>
#include <algorithm>
namespace ta::ai {
struct PlacementProbe {
    static bool place(const Controller& c,const sim::World& w,const sim::UnitType* t,
                      const sim::Unit& builder,float& x,float& z) {
        return c.placeSite(w,t,builder,800,800,x,z);
    }
    // placeSite's first probe is ring 70 at angle 0: (nx+70,nz) snapped to cells,
    // so this asks whether (x,z) itself is usable -- it comes back unchanged.
    static bool usable(const Controller& c,const sim::World& w,const sim::UnitType* t,
                       const sim::Unit& builder,float x,float z) {
        float ox=0,oz=0;
        return c.placeSite(w,t,builder,x-70,z,ox,oz) && ox==x && oz==z;
    }
    static bool placeFrom(const Controller& c,const sim::World& w,const sim::UnitType* t,
                          const sim::Unit& builder,float nx,float nz,float& x,float& z) {
        return c.placeSite(w,t,builder,nx,nz,x,z);
    }
    static int doorSteps(const Controller& c,const sim::World& w,const sim::UnitType* yard,
                         const sim::UnitType* ship,float x,float z) {
        const auto& launches=c.navalLaunchOffsets(yard);
        if (launches.empty()) return -2;
        int worst=0;
        for (const auto& [dx,dz]:launches) {
            const int steps=c.navalDoorSteps(w,yard,ship,x,z,dx,dz);
            if (steps<0) return -1;
            worst=std::max(worst,steps);
        }
        return worst;
    }
    static std::vector<Controller::Rect> doors(const Controller& c,const sim::UnitType* t,float x,float z) {
        std::vector<Controller::Rect> out;c.addDoorStrips(t,x,z,out);return out;
    }
    static void revive(const Controller& c,const sim::World& w,std::vector<int>& assigned,const CommandSink& sink) {
        c.reviveIdle(w,assigned,sink);
    }
    static bool idle(const sim::Unit& u) {return Controller::idle(u);}
};
}
namespace ta::sim {
struct RetailReplayProbe {
    static bool corpse(World& w,Unit& u,int type) {return w.placeCorpse(u,type);}
};
}
using namespace ta;
static int fails=0;
static void check(bool ok,const char* label) {
    std::printf("[%s] %s\n",ok ? "PASS" : "FAIL",label);if (!ok) ++fails;
}
static sim::UnitType fighter() {
    sim::UnitType t;t.id="fighter";t.maxHp=100;t.maxVel=sim::Fixed::fromInt(2);
    t.canMove=true;t.footX=t.footZ=2;t.turnRate=t.turnInPlaceRate=1000;
    t.weapon.damage=10;t.weapon.range=120;t.weapons={t.weapon};
    t.defaultMove=1;t.defaultFire=2;t.buildTime=1;return t;
}
// The room's default FOG OF WAR, EXPLORED: the whole map's ground (its deposits
// too) is known from the start, units only by sight.
static void terrain(sim::World& w,bool islands=false) {
    std::vector<uint8_t> h(256*256,100);
    if (islands) for (int z=0;z<256;++z) for (int x=96;x<160;++x) h[z*256+x]=0;
    w.setTerrain(std::move(h),256,256,20);
    // The room's default FOG OF WAR, EXPLORED: the whole map's ground is known
    // (mana deposits included), enemy units only in sight (fogKnowledge).
    w.setRoomVision(true,false);
}
// The sim's per-player sight (World::viewCount) for the units as they stand: our
// levels target only what their side has seen, and explore what it has not.
static void sight(sim::World& w) {w.updateNavigationExploration();}
// A world past its first tick with no view plane has every unit in sight
// (World::playerSees). Before the first tick our levels see nothing (there is no
// view pass yet: Controller::updateKnowledge), so a test that wants all in sight without
// placing lookouts moves the clock on.
static void pastStart(sim::World& w) {w.resumeClocks(1,w.gameRngState());}
static std::vector<net::Command> think(ai::Controller& ai,sim::World& w,uint32_t tick=0) {
    std::vector<net::Command> out;ai.tick(w,tick,[&](const auto& c){out.push_back(c);});return out;
}
// A think with the sim's sight brought up to date first (World::viewCount):
// what the units placed see, the way a game's sight is after every tick.
static std::vector<net::Command> seen(ai::Controller& ai,sim::World& w,uint32_t tick=0) {sight(w);return think(ai,w,tick);}
static int attacks(const std::vector<net::Command>& cs) {
    return int(std::count_if(cs.begin(),cs.end(),[](const auto& c){return c.kind==net::Cmd::AttackMove || c.kind==net::Cmd::Attack;}));
}
static int moves(const std::vector<net::Command>& cs) {
    return int(std::count_if(cs.begin(),cs.end(),[](const auto& c){return c.kind==net::Cmd::Move;}));
}
// Archers that outrange both our sight and the defended radius must still be
// answered: whoever is hitting a unit inside the territory is a threat.
static void defensiveShooters(const sim::TypeRegistry& registry,const ai::Profile& profile,
                              const sim::UnitType& soldier) {
    auto base=soldier;base.id="base";base.maxVel={};base.sight=300;
    base.weapon.damage=0;base.weapons.clear();
    auto archer=soldier;archer.id="archer";archer.weapon.range=450;archer.weapons={archer.weapon};
    auto blind=soldier;blind.sight=100;
    auto melee=blind;melee.id="melee";melee.weapon.range=30;melee.weapons={melee.weapon};
    for (const auto* defenderType:{&blind,&melee}) {
        const bool isMelee=defenderType==&melee;
        sim::World w;terrain(w);w.player(0).defensiveAi=true;
        w.spawn(&base,400,800,0,0);
        const int defender=w.spawn(defenderType,1000,800,0,0);   // inside the 900 home radius
        const int shooter=w.spawn(&archer,1420,800,0,1);           // 1020 from home, unseen
        ai::Controller defensive(0,registry,profile,1,ai::Difficulty::Defensive,{{3400,800}});
        check(attacks(seen(defensive,w))==0,"an unseen archer outside the territory is no threat until it hits something");
        w.unit(defender)->lastHitBy=shooter;
        auto cs=seen(defensive,w,30);
        // Heard, not seen: an attack-move to where it stands, melee or not. An
        // attack on it by id would follow it wherever it went out of sight.
        check(attacks(cs)==1 && std::any_of(cs.begin(),cs.end(),[&](const auto& c) {
            return c.unitId==defender && c.kind==net::Cmd::AttackMove && std::abs(c.x-1420)<64 && std::abs(c.z-800)<64;
        }),isMelee ? "a melee defender attack-moves on the unseen archer hitting it, never an attack by id"
                   : "a ranged defender attack-moves on the unseen archer hitting it");
        for (const auto& c:cs) sim::applyCommand(w,registry,c);
        check(attacks(seen(defensive,w,60))==0,"the response to an attacker is not re-issued every think");
        w.unit(shooter)->x=sim::Fixed::fromInt(2100);w.unit(defender)->x=sim::Fixed::fromInt(1800);
        cs=seen(defensive,w,90);
        check(attacks(cs)==0 && std::any_of(cs.begin(),cs.end(),[&](const auto& c) {
            return c.unitId==defender && c.kind==net::Cmd::Move && c.x==1000 && c.z==800;
        }),"defenders give up an attacker beyond the pursuit radius and return to their posts");
    }
    {
        sim::World w;terrain(w);w.spawn(&base,400,800,0,0);
        const int defender=w.spawn(&blind,1000,800,0,0);
        const int shooter=w.spawn(&archer,1700,800,0,1);   // out of its own reach of the victim
        w.unit(defender)->lastHitBy=shooter;
        ai::Controller defensive(0,registry,profile,1,ai::Difficulty::Defensive,{{3400,800}});
        check(attacks(seen(defensive,w))==0,"a stale attacker no longer within reach of its victim is ignored");
    }
}

// Each melee defender answers the unit hitting it, keeps after it once it is
// drawn past the territory, and otherwise takes the threat nearest itself rather
// than the whole line walking at the one nearest home.
static void defensiveMeleeTargets(const sim::TypeRegistry& registry,const ai::Profile& profile,
                                  const sim::UnitType& soldier) {
    auto base=soldier;base.id="base";base.maxVel={};base.sight=750;
    base.weapon.damage=0;base.weapons.clear();
    auto archer=soldier;archer.id="archer";archer.weapon.range=450;archer.weapons={archer.weapon};
    auto melee=soldier;melee.id="melee";melee.sight=100;melee.weapon.range=30;melee.weapons={melee.weapon};
    auto attackOf=[](const std::vector<net::Command>& cs,int unit)->int {
        for (const auto& c:cs) if (c.unitId==unit && c.kind==net::Cmd::Attack) return c.targetId;
        return 0;
    };
    {
        sim::World w;terrain(w);w.spawn(&base,400,800,0,0);
        const int intruder=w.spawn(&soldier,600,800,0,1);        // seen, nearest home
        const int defender=w.spawn(&melee,1000,800,0,0);
        const int shooter=w.spawn(&archer,1400,800,0,1);         // outside the radius, hitting it
        w.unit(defender)->lastHitBy=shooter;
        ai::Controller defensive(0,registry,profile,1,ai::Difficulty::Defensive,{{3400,800}});
        auto cs=seen(defensive,w);
        check(attackOf(cs,defender)==shooter,"a melee defender attacks the archer hitting it, not the threat nearest home");
        for (const auto& c:cs) sim::applyCommand(w,registry,c);
        // Drawn past the 900 home radius after its attacker (no other victim near it).
        w.unit(intruder)->x=sim::Fixed::fromInt(450);
        w.unit(defender)->x=sim::Fixed::fromInt(1340);
        cs=seen(defensive,w,30);
        check(std::none_of(cs.begin(),cs.end(),[&](const auto& c){return c.unitId==defender;}) &&
              !w.unit(defender)->orders.empty() && w.unit(defender)->orders.front().targetId==shooter,
              "a melee defender drawn past the territory keeps after the archer still hitting it");
        // A second archer hitting it does not pull it off the one it is closing on.
        const int other=w.spawn(&archer,1340,1200,0,1);
        w.unit(defender)->lastHitBy=other;
        check(attackOf(seen(defensive,w,60),defender)==0,"a melee defender chasing its attacker is not switched to another");
        // Archers are listed through the last unit each one hit, which changes shot
        // by shot; the chase holds while the target stays within the pursuit radius.
        w.unit(defender)->lastHitBy=0;
        cs=seen(defensive,w,90);
        check(std::none_of(cs.begin(),cs.end(),[&](const auto& c){return c.unitId==defender;}),
              "a melee defender keeps its chase when its target drops off the threat list inside the pursuit radius");
    }
    {
        sim::World w;terrain(w);w.spawn(&base,400,800,0,0);
        const int intruder=w.spawn(&soldier,600,800,0,1);
        const int defender=w.spawn(&melee,1000,800,0,0);
        ai::Controller defensive(0,registry,profile,1,ai::Difficulty::Defensive,{{3400,800}});
        auto cs=seen(defensive,w);
        check(attackOf(cs,defender)==intruder,"a melee defender with no attacker of its own answers a spotted intruder");
        for (const auto& c:cs) sim::applyCommand(w,registry,c);
        const int shooter=w.spawn(&archer,1400,800,0,1);
        w.unit(defender)->lastHitBy=shooter;
        check(attackOf(seen(defensive,w,30),defender)==shooter,"a melee defender sent at an intruder turns on the archer that starts hitting it");
    }
    {
        sim::World w;terrain(w);w.spawn(&base,400,800,0,0);
        const int nearHome=w.spawn(&soldier,700,600,0,1);       // ~360 from home
        const int farther=w.spawn(&soldier,900,1100,0,1);       // ~580 from home
        const int first=w.spawn(&melee,700,500,0,0);
        const int second=w.spawn(&melee,950,1150,0,0);
        ai::Controller defensive(0,registry,profile,1,ai::Difficulty::Defensive,{{3400,800}});
        auto cs=seen(defensive,w);
        check(attackOf(cs,first)==nearHome && attackOf(cs,second)==farther,
              "melee defenders spread over the threats nearest each of them");
    }
}

// The Monarch fights back instead of standing on a build/move order under fire,
// and the Defensive AI does not site buildings inside a spotted fight.
// Our levels' strikes weigh every enemy unit they know of, so a winning AI goes
// for the last lodestone it has found in a corner rather than marching on a razed
// start, keeps going once a site is cleared, and finishes the player off. The
// user's 0.9.10 replay: an Absurd AI with 400 units spent the last 22 minutes
// ordering its army back and forth to a dead player's start while the last
// player's two lodestones and four units stood untouched in a far corner. These
// worlds have no view plane and stand past their first tick (pastStart), so
// every unit is in sight (World::playerSees); visionTargets covers what sight hides.
static void strikeTargets(const sim::TypeRegistry& registry,const ai::Profile& profile,
                          const sim::UnitType& soldier) {
    auto lode=soldier;lode.id="lode";lode.maxVel={};lode.canMove=false;lode.onMana=true;
    lode.weapon.damage=0;lode.weapons.clear();lode.buildCost=300;
    auto heavy=soldier;heavy.id="heavy";heavy.buildCost=100;
    // Commands aimed at `target`: an Attack on it or an attack-move ending near it.
    auto aimsAt=[](const std::vector<net::Command>& cs,const sim::World& w,int target) {
        const auto* t=w.unit(target);
        return int(std::count_if(cs.begin(),cs.end(),[&](const auto& c) {
            if (c.kind==net::Cmd::Attack) return c.targetId==target;
            const float dx=c.x-t->x.toFloat(),dz=c.z-t->z.toFloat();
            return c.kind==net::Cmd::AttackMove && dx*dx+dz*dz<=200*200;
        }));
    };
    auto army=[&](sim::World& w,int n) {
        std::vector<int> ids;
        for (int i=0;i<n;++i) ids.push_back(w.spawn(&soldier,400+float(i%7)*40,800+float(i/7)*40,0,0));
        return ids;
    };
    {
        // The enemy's start lies razed; its last lodestone stands in a far corner.
        sim::World w;terrain(w);w.player(0).income=100;army(w,35);
        const int last=w.spawn(&lode,3400,3400,0,1);
        pastStart(w);
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800}});
        const auto cs=think(controller,w);
        check(attacks(cs)==35 && aimsAt(cs,w,last)==35,
              "a winning AI goes for an enemy's last lodestone in sight, not for its razed start");
    }
    {
        // A strike that clears one site goes straight on to the next, even when what
        // is left of it is far short of a new push (and not clearly ahead).
        sim::World w;terrain(w);w.player(0).income=100;const auto ids=army(w,35);
        const int near=w.spawn(&lode,1600,800,0,1),far=w.spawn(&lode,3400,3400,0,1);
        for (int i=0;i<3;++i) w.spawn(&heavy,3800,200+float(i)*40,0,1);   // enough that we are not clearly ahead
        pastStart(w);
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800}});
        auto cs=think(controller,w);
        check(attacks(cs)==35 && aimsAt(cs,w,near)==35,"the push goes for the nearer lodestone first");
        for (const auto& c:cs) sim::applyCommand(w,registry,c);
        w.unit(near)->deadFor=0;
        for (size_t i=0;i<ids.size();++i) {w.stop(ids[i]);if (i>=10) w.unit(ids[i])->deadFor=0;}
        cs=think(controller,w,30);
        check(attacks(cs)==10 && aimsAt(cs,w,far)==10,"the strike's survivors go on to the next lodestone without waiting for a push");
    }
    {
        // Two bare lodestones: Normal sends the whole push at one; Hard splits it.
        for (auto difficulty:{ai::Difficulty::Normal,ai::Difficulty::Hard}) {
            sim::World w;terrain(w);w.player(0).income=100;army(w,35);
            const int a=w.spawn(&lode,3400,800,0,1),b=w.spawn(&lode,3400,3400,0,1);
            pastStart(w);
            ai::Controller controller(0,registry,profile,1,difficulty,{{3400,800}});
            const auto cs=think(controller,w);
            const int toA=aimsAt(cs,w,a),toB=aimsAt(cs,w,b);
            check(attacks(cs)==35 && toA+toB==35 && (difficulty==ai::Difficulty::Hard ? toA>0 && toB>0 : toA==35 || toB==35),
                  difficulty==ai::Difficulty::Hard ? "Hard splits a force that outweighs the sites over several"
                                                   : "Normal sends its push at one site");
        }
    }
    {
        // Melee strikers go for a building's edge: its centre is solid ground to them.
        sim::World w;terrain(w);w.player(0).income=100;
        auto sword=soldier;sword.weapon.range=20;sword.weapons={sword.weapon};
        for (int i=0;i<35;++i) w.spawn(&sword,400+float(i%7)*40,800+float(i/7)*40,0,0);
        auto tower=soldier;tower.id="tower";tower.maxVel={};tower.canMove=false;tower.footX=tower.footZ=3;tower.buildCost=500;
        const int last=w.spawn(&tower,3400,3400,0,1);w.blockFoot(tower,3400,3400,true);
        pastStart(w);
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800}});
        const auto cs=think(controller,w);
        check(attacks(cs)==35 && aimsAt(cs,w,last)==35,"melee strikers find their way to the edge of an enemy's last building");
    }
    {
        // With the Monarch not expendable, killing it wins: it outweighs a nearer lodestone.
        sim::World w;terrain(w);w.player(0).income=100;w.setMonarchExpendable(false);army(w,35);
        auto king=soldier;king.id="king";king.commander=true;king.isBuilder=true;king.buildCost=1000;
        w.spawn(&lode,1600,800,0,1);const int monarch=w.spawn(&king,3400,3400,0,1);
        pastStart(w);
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800}});
        const auto cs=think(controller,w);
        check(attacks(cs)==35 && aimsAt(cs,w,monarch)==35,"a Monarch whose loss defeats its owner draws the strike");
    }
    {
        // Defensive levels mostly defend, but clearly ahead they send half their idle
        // fighters out to clean up.
        for (auto difficulty:{ai::Difficulty::Defensive,ai::Difficulty::AbsurdlyDefensive}) {
            sim::World w;terrain(w);w.player(0).income=100;army(w,30);
            const int last=w.spawn(&lode,3400,3400,0,1);
            pastStart(w);
            ai::Controller defensive(0,registry,profile,1,difficulty,{{3400,800}});
            // The first think walks the army out of the base (stationArmy).
            for (const auto& c:think(defensive,w)) sim::applyCommand(w,registry,c);
            const auto cs=think(defensive,w,30);
            check(attacks(cs)==15 && aimsAt(cs,w,last)==15,"a defensive level clearly ahead cleans up with half its army");
        }
    }
}

// What our levels see is what their side's units see (World::playerSees over the
// sim's view counters): an enemy never seen is not a target, a building once seen
// is remembered where it stood until that spot is in sight again without it, a
// mobile unit for a short while, and with nothing known the army explores -- an
// enemy start not yet in sight first. (User, 2026-10-29: "it really should only
// see what it can actually see... in the absence of things to attack that it can
// see it should start exploring the map just like a human player would.")
static void visionTargets(const sim::TypeRegistry& registry,const ai::Profile& profile,
                          const sim::UnitType& soldier) {
    auto lode=soldier;lode.id="lode";lode.maxVel={};lode.canMove=false;lode.onMana=true;
    lode.weapon.damage=0;lode.weapons.clear();lode.buildCost=300;
    // An unarmed lookout of ours: it sees, but is no fighter for the waves.
    auto post=lode;post.id="post";post.onMana=false;post.sight=300;
    // Commands aimed within 200 px of (x,z): an Attack on a unit there or an attack-move.
    auto aimsAt=[](const std::vector<net::Command>& cs,const sim::World& w,float x,float z) {
        return int(std::count_if(cs.begin(),cs.end(),[&](const auto& c) {
            float tx=c.x,tz=c.z;
            if (c.kind==net::Cmd::Attack) {const auto* t=w.unit(c.targetId);tx=t->x.toFloat();tz=t->z.toFloat();}
            else if (c.kind!=net::Cmd::AttackMove) return false;
            return (tx-x)*(tx-x)+(tz-z)*(tz-z)<=200*200;
        }));
    };
    auto army=[&](sim::World& w,int n) {
        std::vector<int> ids;
        for (int i=0;i<n;++i) ids.push_back(w.spawn(&soldier,400+float(i%7)*40,800+float(i/7)*40,0,0));
        return ids;
    };
    auto moveTo=[](sim::World& w,int id,float x,float z) {
        w.unit(id)->x=sim::Fixed::fromInt(int(x));w.unit(id)->z=sim::Fixed::fromInt(int(z));
    };
    {
        sim::World w;terrain(w);w.player(0).income=100;army(w,35);
        w.spawn(&lode,3400,3400,0,1);
        sight(w);
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800}});
        const auto cs=think(controller,w);
        check(aimsAt(cs,w,3400,3400)==0,"an enemy lodestone never seen is not a target");
        check(attacks(cs)==35 && aimsAt(cs,w,3400,800)==35,"with nothing seen, the push and a scout explore the enemy start");
    }
    {
        // Seen by a lookout, which then goes: the lodestone is remembered, and
        // the push goes there rather than to a nearer one it has never seen (one
        // fighter scouts the enemy start).
        sim::World w;terrain(w);w.player(0).income=100;
        const int eye=w.spawn(&post,3300,3300,0,0);
        w.spawn(&lode,3400,3400,0,1);w.spawn(&lode,1600,1600,0,1);
        sight(w);
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800}});
        think(controller,w);
        moveTo(w,eye,400,400);army(w,35);sight(w);
        const auto cs=think(controller,w,30);
        check(attacks(cs)==35 && aimsAt(cs,w,3400,3400)==34 && aimsAt(cs,w,3400,800)==1 && aimsAt(cs,w,1600,1600)==0,
              "a lodestone seen and then out of sight is remembered; one never seen is not");
    }
    {
        // An enemy start already in sight, and empty, is not explored again: the
        // push goes for the start nobody has looked at, though it is farther.
        sim::World w;terrain(w);w.player(0).income=100;army(w,35);
        w.spawn(&post,1600,800,0,0);
        sight(w);
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Normal,{{1600,800},{3400,3400}});
        const auto cs=think(controller,w);
        check(attacks(cs)==35 && aimsAt(cs,w,3400,3400)==35 && aimsAt(cs,w,1600,800)==0,
              "with nothing known, the army explores toward the enemy start not yet in sight");
    }
    {
        // A remembered lodestone destroyed out of sight is still believed there
        // until a lookout sees the spot again; then the strikers explore on.
        sim::World w;terrain(w);w.player(0).income=100;
        const int eye=w.spawn(&post,3300,3300,0,0);
        const int gone=w.spawn(&lode,3400,3400,0,1);w.spawn(&lode,200,3800,0,1);   // the other is never seen
        sight(w);
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800}});
        think(controller,w);
        moveTo(w,eye,400,400);w.unit(gone)->deadFor=0;army(w,35);sight(w);
        auto cs=think(controller,w,30);
        check(aimsAt(cs,w,3400,3400)==34,"a remembered lodestone destroyed out of sight is still gone for");
        moveTo(w,eye,3300,3300);sight(w);
        cs=think(controller,w,60);
        check(attacks(cs)>0 && aimsAt(cs,w,3400,3400)==0 && aimsAt(cs,w,200,3800)==0,
              "once its spot is in sight without it, the lodestone is forgotten and the strikers explore on");
    }
    {
        // A mobile unit seen is remembered where it was for a short while only.
        sim::World w;terrain(w);w.player(0).income=100;
        const int eye=w.spawn(&post,3300,3300,0,0);
        auto heavy=soldier;heavy.id="heavy";heavy.buildCost=100;
        w.spawn(&heavy,3400,3400,0,1);
        sight(w);
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800}});
        think(controller,w);
        moveTo(w,eye,400,400);army(w,35);sight(w);
        check(aimsAt(think(controller,w,10*30),w,3400,3400)>0,"a mobile enemy out of sight is remembered for a while");
        ai::Controller later(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800}});
        moveTo(w,eye,3300,3300);sight(w);think(later,w);
        moveTo(w,eye,400,400);sight(w);
        check(aimsAt(think(later,w,40*30),w,3400,3400)==0,"... and forgotten once that while is up");
    }
    {
        // Units hitting ours are known where they stand, seen or not.
        sim::World w;terrain(w);w.player(0).income=100;const auto ids=army(w,35);
        auto archer=soldier;archer.id="archer";archer.weapon.range=450;archer.weapons={archer.weapon};
        const int shooter=w.spawn(&archer,1000,800,0,1);
        sight(w);
        w.unit(ids[6])->lastHitBy=shooter;w.unit(ids[6])->attackedTick=1;
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800}});
        const auto cs=think(controller,w);
        check(aimsAt(cs,w,1000,800)>0,"an unseen archer hitting our units is a target");
        // Only heard, not seen: the strikers attack-move to where it stands. An
        // order on it by id would follow it wherever it went out of sight.
        check(std::none_of(cs.begin(),cs.end(),[&](const auto& c){return c.kind==net::Cmd::Attack && c.targetId==shooter;}),
              "... gone for where it stands, not ordered on by id");
    }
    {
        // Player 0 thinks at tick 0, before the first view pass: no plane yet,
        // and World::playerSees would have every unit in sight. It sees nothing
        // then -- no lodestone across the map, no enemy start checked -- so its
        // push and scout explore the start (an AI given the map at tick 0 knew
        // every Monarch and never scouted a start).
        sim::World w;terrain(w);w.player(0).income=100;
        w.spawn(&lode,3400,3400,0,1);
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800}});
        think(controller,w);
        army(w,35);sight(w);
        const auto cs=think(controller,w,30);
        check(attacks(cs)==35 && aimsAt(cs,w,3400,800)==35 && aimsAt(cs,w,3400,3400)==0,
              "the think at tick 0, before any view pass, learns nothing: the army explores the enemy start");
    }
}

// What our levels know is what a player of their side is shown under the room's
// FOG OF WAR (Controller::knowsGround / sees): NOT EXPLORED, only ground their
// units have explored; EXPLORED, the whole map's ground (its mana deposits) but
// enemy units only in view; FULL VISION, every unit; and in view counts radar out
// to radardistance, as our client shows it. A deposit is taken by what the side
// knows stands there -- an enemy lodestone seen, remembered, or learned from the
// real placement test refusing it -- never by one it cannot know of. (User,
// 2026-10-29: "If map is explored but not full vision the AI should be able to
// make decisions on stuff it can see but hasn't explored yet just like a human
// would.")
static void fogKnowledge(const sim::TypeRegistry& registry,const ai::Profile& profile,
                         const sim::UnitType& soldier) {
    auto lode=soldier;lode.id="lode";lode.maxVel={};lode.canMove=false;lode.onMana=true;
    lode.weapon.damage=0;lode.weapons.clear();lode.buildCost=300;lode.sight=50;
    auto post=lode;post.id="post";post.onMana=false;post.sight=50;   // an unarmed lookout of ours
    auto mason=soldier;mason.id="mason";mason.isBuilder=true;mason.sight=50;mason.weapon.damage=0;mason.weapons.clear();
    auto aimsAt=[](const std::vector<net::Command>& cs,const sim::World& w,float x,float z) {
        return int(std::count_if(cs.begin(),cs.end(),[&](const auto& c) {
            float tx=c.x,tz=c.z;
            if (c.kind==net::Cmd::Attack) {const auto* t=w.unit(c.targetId);tx=t->x.toFloat();tz=t->z.toFloat();}
            else if (c.kind!=net::Cmd::AttackMove) return false;
            return (tx-x)*(tx-x)+(tz-z)*(tz-z)<=200*200;
        }));
    };
    auto army=[&](sim::World& w,int n) {
        for (int i=0;i<n;++i) w.spawn(&soldier,400+float(i%7)*40,800+float(i/7)*40,0,0);
    };
    auto moveTo=[](sim::World& w,int id,float x,float z) {
        w.unit(id)->x=sim::Fixed::fromInt(int(x));w.unit(id)->z=sim::Fixed::fromInt(int(z));
    };
    // Where the lodestone search sends a builder at (800,1000), searching from (1400,800).
    auto site=[&](const ai::Controller& c,const sim::World& w,int builder) {
        float x=0,z=0;
        if (!ai::PlacementProbe::placeFrom(c,w,&lode,*w.unit(builder),1400,800,x,z)) return std::pair{-1.0f,-1.0f};
        return std::pair{x,z};
    };
    {
        // NOT EXPLORED: the nearer deposit lies on ground nobody has explored.
        sim::World w;terrain(w);w.setRoomVision(false,false);
        w.setManaSpots({{1400,800},{800,1100}});
        w.spawn(&post,800,800,0,0);
        const int builder=w.spawn(&mason,800,1000,0,0);
        const int scout=w.spawn(&post,800,1100,0,0);   // has explored the farther one, and gone
        sight(w);moveTo(w,scout,400,400);sight(w);
        ai::Controller c(0,registry,profile,1,ai::Difficulty::Normal,{{3400,3400}});
        think(c,w);
        check(site(c,w,builder)==std::pair{800.0f,1100.0f},
              "NOT EXPLORED: the lodestone search passes over a deposit on ground never explored");
        const int eye=w.spawn(&post,1380,800,0,0);
        sight(w);moveTo(w,eye,400,400);sight(w);think(c,w,30);
        check(site(c,w,builder)==std::pair{1400.0f,800.0f},"... and takes it once a unit has explored it");
        // EXPLORED: the whole map's ground is known, nothing scouted.
        sim::World e;terrain(e);e.setManaSpots({{1400,800},{800,1100}});
        e.spawn(&post,800,800,0,0);
        const int b2=e.spawn(&mason,800,1000,0,0);
        sight(e);
        ai::Controller c2(0,registry,profile,1,ai::Difficulty::Normal,{{3400,3400}});
        think(c2,e);
        check(site(c2,e,b2)==std::pair{1400.0f,800.0f},"EXPLORED: a deposit nobody has looked at is known");
    }
    {
        // EXPLORED, an enemy lodestone out of view on the nearer deposit: the real
        // placement test refuses it (a player's order there is refused too), the
        // search goes on to the next, and the lodestone is learned -- the push goes
        // for it, by an attack-move to where it stands.
        sim::World w;terrain(w);w.player(0).income=100;
        w.setManaSpots({{1400,800},{800,1100}});
        w.spawn(&post,800,800,0,0);
        const int builder=w.spawn(&mason,800,1000,0,0);
        const int enemyLode=w.spawn(&lode,1400,800,0,1);
        sight(w);
        ai::Controller c(0,registry,profile,1,ai::Difficulty::Normal,{{3400,3400}});
        think(c,w);
        check(site(c,w,builder)==std::pair{800.0f,1100.0f},"a deposit the real test refuses is passed over");
        army(w,35);sight(w);
        const auto cs=think(c,w,30);
        check(aimsAt(cs,w,1400,800)==34 && std::none_of(cs.begin(),cs.end(),[&](const auto& cmd) {
                  return cmd.kind==net::Cmd::Attack && cmd.targetId==enemyLode; }),
              "the enemy lodestone that refused it is learned: the push attack-moves on it");
    }
    {
        // A lodestone seen on a deposit, then destroyed out of view: the deposit is
        // still held taken -- the search goes past it -- until a unit sees it empty.
        sim::World w;terrain(w);
        w.setManaSpots({{1400,800},{800,1100}});
        w.spawn(&post,800,800,0,0);
        const int builder=w.spawn(&mason,800,1000,0,0);
        const int eye=w.spawn(&post,1380,800,0,0);
        const int gone=w.spawn(&lode,1400,800,0,1);
        sight(w);
        ai::Controller c(0,registry,profile,1,ai::Difficulty::Normal,{{3400,3400}});
        think(c,w);
        moveTo(w,eye,400,400);w.unit(gone)->deadFor=0;sight(w);think(c,w,30);
        check(site(c,w,builder)==std::pair{800.0f,1100.0f},
              "a deposit remembered under an enemy lodestone stays taken while out of view");
        moveTo(w,eye,1380,800);sight(w);think(c,w,60);
        check(site(c,w,builder)==std::pair{1400.0f,800.0f},"... and is free again once seen empty");
    }
    {
        // A builder walking to a deposit that it sees an enemy lodestone go up on
        // is stopped, to take another site.
        sim::World w;terrain(w);
        w.setManaSpots({{1400,800},{800,1100}});
        w.spawn(&post,800,800,0,0);
        const int builder=w.spawn(&mason,800,1000,0,0);
        w.queueBuild(builder,&lode,1400,800,false);
        const bool bound=!w.unit(builder)->orders.empty() && w.unit(builder)->orders.back().buildType;
        ai::Controller c(0,registry,profile,1,ai::Difficulty::Normal,{{3400,3400}});
        sight(w);
        auto stops=[&](const std::vector<net::Command>& cs) {
            return std::count_if(cs.begin(),cs.end(),[&](const auto& cmd){return cmd.kind==net::Cmd::Stop && cmd.unitId==builder;});
        };
        const bool walking=stops(think(c,w))==0;
        w.spawn(&lode,1400,800,0,1);w.spawn(&post,1380,800,0,0);sight(w);
        check(bound && walking && stops(think(c,w,30))==1,
              "a builder bound for a deposit an enemy lodestone is seen on is stopped");
    }
    {
        // FULL VISION: every enemy unit is in view, sight or none.
        for (const bool full : {false,true}) {
            sim::World w;terrain(w);w.setRoomVision(true,full);w.player(0).income=100;army(w,35);
            w.spawn(&lode,3400,3400,0,1);
            sight(w);
            ai::Controller c(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800}});
            const auto cs=think(c,w,30);
            if (full) check(aimsAt(cs,w,3400,3400)>=34,"FULL VISION: an enemy lodestone no unit sees is a target");
            else check(aimsAt(cs,w,3400,3400)==0,"EXPLORED: an enemy lodestone no unit sees is not");
        }
    }
    {
        // Radar shows a player what stands inside radardistance, through terrain.
        sim::World w;terrain(w);w.player(0).income=100;army(w,35);
        auto tower=post;tower.id="tower";tower.radar=1500;
        w.spawn(&tower,2400,2400,0,0);
        w.spawn(&lode,3400,3400,0,1);   // 1414 px off: in radar, far out of sight
        sight(w);
        ai::Controller c(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800}});
        const auto cs=think(c,w,30);
        check(aimsAt(cs,w,3400,3400)>=34,"an enemy lodestone inside a tower's radar is a target");
    }
    {
        // The Monarch hit by an attacker nobody sees attack-moves on it, never an
        // attack by id; one in view it attacks.
        auto monarch=soldier;monarch.id="monarch";monarch.commander=true;monarch.isBuilder=true;monarch.sight=20;
        for (const bool inView : {false,true}) {
            sim::World w;terrain(w);
            const int m=w.spawn(&monarch,600,800,0,0);
            const int shooter=w.spawn(&soldier,780,800,0,1);
            if (inView) w.spawn(&post,780,760,0,0);
            w.unit(m)->lastHitBy=shooter;
            sight(w);
            ai::Controller c(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800}});
            const auto cs=think(c,w,30);
            const bool byId=std::any_of(cs.begin(),cs.end(),[&](const auto& cmd){
                return cmd.kind==net::Cmd::Attack && cmd.unitId==m && cmd.targetId==shooter; });
            const bool moved=std::any_of(cs.begin(),cs.end(),[&](const auto& cmd){
                return cmd.kind==net::Cmd::AttackMove && cmd.unitId==m && std::abs(cmd.x-780)<32 && std::abs(cmd.z-800)<32; });
            if (inView) check(byId && !moved,"a Monarch attacks the attacker it can see by id");
            else check(moved && !byId,"a Monarch hit from out of view attack-moves on its attacker, never by id");
        }
    }
    {
        // The defensive levels' threats are what their side sees (the sim's view),
        // not a disc of sightdistance around each unit: the base's view reaches an
        // intruder 450 px off though its sightdistance is 300.
        auto base=post;base.id="base";base.sight=300;
        sim::World w;terrain(w);w.player(0).defensiveAi=true;
        w.spawn(&base,400,800,0,0);w.spawn(&soldier,400,1000,0,0);
        w.spawn(&soldier,850,800,0,1);
        sight(w);
        ai::Controller c(0,registry,profile,1,ai::Difficulty::Defensive,{{3400,800}});
        check(attacks(think(c,w,30))==1,"Defensive answers an intruder its side's view shows");
    }
}

// Scouting with memory: a scout killed on its way backs that target off (the next
// goes elsewhere), the level's scout losses in five minutes stop new parties, and
// where scouts died a party one larger goes the next time. A start whose owner is
// beaten is not explored at once, nor, once every enemy has been found at a start,
// are the others (a RANDOM start spot may hold nobody). A beaten player's base --
// an enemy's or an ally's -- is a start again once out of sight five minutes,
// unless an enemy building is known there (a target instead).
static void scoutingMemory(const sim::TypeRegistry& registry,const ai::Profile& profile,
                           const sim::UnitType& soldier) {
    auto lode=soldier;lode.id="lode";lode.maxVel={};lode.canMove=false;lode.onMana=true;
    lode.weapon.damage=0;lode.weapons.clear();lode.buildCost=300;
    auto post=lode;post.id="post";post.onMana=false;post.sight=300;
    auto aimsAt=[](const std::vector<net::Command>& cs,const sim::World& w,float x,float z) {
        return int(std::count_if(cs.begin(),cs.end(),[&](const auto& c) {
            float tx=c.x,tz=c.z;
            if (c.kind==net::Cmd::Attack) {const auto* t=w.unit(c.targetId);tx=t->x.toFloat();tz=t->z.toFloat();}
            else if (c.kind!=net::Cmd::AttackMove) return false;
            return (tx-x)*(tx-x)+(tz-z)*(tz-z)<=200*200;
        }));
    };
    auto moveTo=[](sim::World& w,int id,float x,float z) {
        w.unit(id)->x=sim::Fixed::fromInt(int(x));w.unit(id)->z=sim::Fixed::fromInt(int(z));
    };
    auto apply=[&](sim::World& w,const std::vector<net::Command>& cs) {for (const auto& c:cs) sim::applyCommand(w,registry,c);};
    {
        // Hard scouts every 40 s, two at most out, and stops after two losses in
        // five minutes. Six fighters are too few for a raid: every attack-move is a scout.
        sim::World w;terrain(w);w.player(0).income=100;
        for (int i=0;i<6;++i) w.spawn(&soldier,400+float(i)*40,800,0,0);
        w.spawn(&soldier,3800,3800,0,1);   // the enemy is in the game, out of sight
        sight(w);
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Hard,{{3400,800}});
        auto cs=think(controller,w,0);
        check(attacks(cs)==1 && aimsAt(cs,w,3400,800)==1,"the first scout goes for the enemy start");
        const int first=cs.empty() ? 0 : cs.front().unitId;
        apply(w,cs);
        moveTo(w,first,2000,800);sight(w);think(controller,w,600);
        w.unit(first)->deadFor=0;sight(w);
        cs=think(controller,w,1200);
        check(attacks(cs)==1 && aimsAt(cs,w,3400,800)==0,
              "a scout killed on its way backs the start off: the next scout looks elsewhere");
        apply(w,cs);
        const int second=cs.empty() ? 0 : cs.front().unitId;
        if (second) {moveTo(w,second,2000,2400);sight(w);think(controller,w,1240);w.unit(second)->deadFor=0;sight(w);}
        think(controller,w,1260);
        cs=think(controller,w,2400);
        check(attacks(cs)==0,"two scouts lost in five minutes: Hard sends no more for now");
        apply(w,cs);
        std::vector<int> out;
        for (const auto& c:cs) out.push_back(c.unitId);
        cs=think(controller,w,10800);
        check(attacks(cs)==2 && aimsAt(cs,w,3400,800)==2 && std::none_of(cs.begin(),cs.end(),[&](const auto& c) {
                  return std::find(out.begin(),out.end(),c.unitId)!=out.end();
              }),"later a party of two goes where a scout died, not a lone scout");
    }
    {
        // An enemy building seen near a start is that enemy's base; once the enemy
        // is beaten, its start is not explored, though it is the nearest unseen one.
        sim::World w;terrain(w);w.player(0).income=100;
        const int eye=w.spawn(&post,1200,2300,0,0);
        const int base=w.spawn(&lode,1200,2550,0,2);
        w.spawn(&soldier,3800,3800,0,1);
        sight(w);
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800},{1200,3000}});
        think(controller,w,0);
        w.unit(base)->deadFor=0;w.unit(eye)->deadFor=0;w.player(2).defeated=true;
        for (int i=0;i<35;++i) w.spawn(&soldier,400+float(i%7)*40,800+float(i/7)*40,0,0);
        sight(w);
        const auto cs=think(controller,w,30);
        check(attacks(cs)==35 && aimsAt(cs,w,3400,800)==35 && aimsAt(cs,w,1200,3000)==0,
              "a beaten player's start is not explored");
    }
    {
        // A beaten enemy's base is not checked for good: its free deposits may draw
        // someone in, so once out of sight five minutes it is a start again and a
        // scout looks in. Scouts killed on the way back it off.
        sim::World w;terrain(w);w.player(0).income=100;
        const int eye=w.spawn(&post,1200,2300,0,0);
        const int base=w.spawn(&lode,1200,2550,0,2);
        w.spawn(&soldier,3800,3800,0,1);
        sight(w);
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Hard,{{1200,3000}});
        think(controller,w,0);
        w.unit(base)->deadFor=0;w.unit(eye)->deadFor=0;w.player(2).defeated=true;
        sight(w);think(controller,w,20);
        for (int i=0;i<6;++i) w.spawn(&soldier,400+float(i)*40,800,0,0);
        sight(w);
        auto cs=think(controller,w,36000);
        check(attacks(cs)==1 && aimsAt(cs,w,1200,3000)==1,"a beaten enemy's base out of sight a while is scouted again");
        // Scouts die away from the route, so only the back-off (and the dead
        // scout's claim, three minutes) keeps the next one from going there.
        auto killOne=[&](uint32_t sent) {   // of the party sent
            apply(w,cs);
            for (const auto& c:cs) moveTo(w,c.unitId,2600,800);
            sight(w);think(controller,w,sent+600);
            if (!cs.empty()) w.unit(cs.front().unitId)->deadFor=0;
            sight(w);think(controller,w,sent+620);   // the loss noted
        };
        killOne(36000);
        cs=think(controller,w,41600);
        check(attacks(cs)==2 && aimsAt(cs,w,1200,3000)==2,
              "past the back-off a party of two looks at the beaten base again");
        killOne(41600);
        cs=think(controller,w,47200);
        check(attacks(cs)>0 && aimsAt(cs,w,1200,3000)==0,"a beaten base that keeps killing scouts backs off longer");
    }
    {
        // Scouts the beaten player killed going to its start no longer back it off.
        sim::World w;terrain(w);w.player(0).income=100;
        for (int i=0;i<6;++i) w.spawn(&soldier,400+float(i)*40,800,0,0);
        const int last=w.spawn(&soldier,3800,3800,0,2);
        w.spawn(&soldier,3800,200,0,1);
        sight(w);
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Hard);
        controller.setStartSpots({{1200,3000,2}});
        auto cs=think(controller,w,0);
        check(attacks(cs)==1 && aimsAt(cs,w,1200,3000)==1,"the first scout goes for the enemy start the room showed");
        apply(w,cs);
        const int first=cs.empty() ? 0 : cs.front().unitId;
        if (first) {moveTo(w,first,2600,800);sight(w);think(controller,w,600);w.unit(first)->deadFor=0;sight(w);}
        w.unit(last)->deadFor=0;w.player(2).defeated=true;sight(w);
        apply(w,think(controller,w,6000));   // its owner found beaten
        cs=think(controller,w,6000+5*60*30);  // its base out of sight five minutes since
        check(aimsAt(cs,w,1200,3000)==1,"once its owner is beaten, scouts it killed no longer back its start off");
    }
    {
        // On a large map (256 sectors) a beaten enemy's far base out of sight five
        // minutes comes before the many nearer sectors never seen: it is a start
        // again. Not before the five minutes are up.
        auto big=[](sim::World& w) {
            std::vector<uint8_t> h(1024*1024,100);
            w.setTerrain(std::move(h),1024,1024,20);w.setRoomVision(true,false);
        };
        for (const uint32_t at:{uint32_t(8000),uint32_t(20+5*60*30+20)}) {
            sim::World w;big(w);w.player(0).income=100;
            const int last=w.spawn(&lode,12000,12000,0,2);
            w.spawn(&soldier,16000,400,0,1);
            sight(w);
            ai::Controller controller(0,registry,profile,1,ai::Difficulty::Hard);
            controller.setStartSpots({{12000,12000,2}});
            think(controller,w,0);
            w.unit(last)->deadFor=0;w.player(2).defeated=true;sight(w);
            think(controller,w,20);
            for (int i=0;i<6;++i) w.spawn(&soldier,400+float(i)*40,800,0,0);
            sight(w);
            const auto cs=think(controller,w,at);
            if (at==8000) check(attacks(cs)==1 && aimsAt(cs,w,12000,12000)==0,
                                "a beaten enemy's base out of sight under five minutes waits: a sector is scouted");
            else check(attacks(cs)==1 && aimsAt(cs,w,12000,12000)==1,
                       "on a large map a beaten enemy's far base out of sight five minutes is scouted before the sectors");
        }
    }
    {
        // A beaten enemy's base where an enemy building is known (someone moved
        // in) is not scouted: the strikes take it. Once that is gone and the base
        // out of sight five minutes again, it is scouted.
        auto retaken=[&](sim::World& w,int& eye) {
            terrain(w);w.player(0).income=100;
            w.spawn(&lode,1200,3000,0,2);
            w.spawn(&soldier,3800,3800,0,1);
            eye=w.spawn(&post,1650,2700,0,0);
            sight(w);
        };
        sim::World w;int eye=0;retaken(w,eye);
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Hard);
        controller.setStartSpots({{1200,3000,2}});
        think(controller,w,0);
        for (auto& u:w.units()) if (u.player==2) u.deadFor=0;
        w.player(2).defeated=true;
        int moved=w.spawn(&lode,1650,3000,0,1);   // player 1 builds at the empty base, in our sight
        sight(w);think(controller,w,20);
        moveTo(w,eye,400,400);
        for (int i=0;i<6;++i) w.spawn(&soldier,400+float(i)*40,800,0,0);
        sight(w);
        auto cs=think(controller,w,36000);
        check(attacks(cs)==1 && aimsAt(cs,w,1200,3000)==0 && aimsAt(cs,w,1650,3000)==0,
              "a beaten enemy's base where an enemy building is known is not scouted");
        w.unit(moved)->deadFor=0;moveTo(w,eye,1650,2700);sight(w);think(controller,w,36020);   // seen gone
        moveTo(w,eye,400,400);sight(w);
        cs=think(controller,w,36020+5*60*30);
        check(aimsAt(cs,w,1200,3000)==1,"... and once that is gone and the base out of sight five minutes, it is");
        sim::World v;retaken(v,eye);
        ai::Controller army(0,registry,profile,1,ai::Difficulty::Normal);
        army.setStartSpots({{1200,3000,2}});
        think(army,v,0);
        for (auto& u:v.units()) if (u.player==2) u.deadFor=0;
        v.player(2).defeated=true;
        moved=v.spawn(&lode,1650,3000,0,1);
        sight(v);think(army,v,30);
        moveTo(v,eye,400,400);
        for (int i=0;i<35;++i) v.spawn(&soldier,400+float(i%7)*40,800+float(i/7)*40,0,0);
        sight(v);
        cs=think(army,v,36000);
        check(aimsAt(cs,v,1650,3000)>0 && aimsAt(cs,v,1200,3000)==0,"... the army strikes the building known there");
    }
    {
        // A fallen ally's base may be moved into too: out of sight five minutes,
        // a scout looks in.
        sim::World w;terrain(w);w.player(0).income=100;
        const int ally=w.spawn(&lode,1200,3000,0,2);
        w.spawn(&soldier,3800,3800,0,1);
        w.setTeam(2,0);
        sight(w);
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Hard);
        think(controller,w,0);
        // The clock past the 60 ticks a fallen unit's sight lingers (its
        // death revealer) at the ally's base.
        w.unit(ally)->deadFor=0;w.player(2).defeated=true;sight(w);
        w.resumeClocks(100,w.gameRngState());sight(w);
        think(controller,w,20);
        for (int i=0;i<6;++i) w.spawn(&soldier,400+float(i)*40,800,0,0);
        sight(w);
        const auto cs=think(controller,w,20+5*60*30+20);
        check(attacks(cs)==1 && aimsAt(cs,w,1200,3000)==1,"a fallen ally's base out of sight five minutes is scouted");
    }
    {
        // Start spots of unknown owner (RANDOM start locations): with the only enemy
        // found at one, the others hold nobody, and the army goes to look at the one
        // it was found at rather than the nearer empty ones.
        sim::World w;terrain(w);w.player(0).income=100;
        const int eye=w.spawn(&post,3400,2400,0,0);
        const int base=w.spawn(&lode,3400,2850,0,1);
        w.spawn(&soldier,100,3900,0,1);
        sight(w);
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Normal,{{1600,800},{3400,800},{3400,3400}});
        think(controller,w,0);
        w.unit(base)->deadFor=0;sight(w);think(controller,w,30);   // seen gone: forgotten
        moveTo(w,eye,400,400);
        for (int i=0;i<35;++i) w.spawn(&soldier,400+float(i%7)*40,800+float(i/7)*40,0,0);
        sight(w);
        const auto cs=think(controller,w,60);
        check(attacks(cs)==35 && aimsAt(cs,w,3400,3400)==35,
              "once every enemy is found at a start, the other start spots are not explored");
    }
    {
        // The room says whose start each is (FIXED, BY SLOT): a beaten player's
        // start is not explored though nothing of it was ever seen.
        sim::World w;terrain(w);w.player(0).income=100;
        w.spawn(&soldier,3800,3800,0,1);w.player(2).defeated=true;
        for (int i=0;i<35;++i) w.spawn(&soldier,400+float(i%7)*40,800+float(i/7)*40,0,0);
        sight(w);
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Normal);
        controller.setStartSpots({{3400,800,1},{1200,3000,2}});
        const auto cs=think(controller,w,30);
        check(attacks(cs)==35 && aimsAt(cs,w,3400,800)==35,"a beaten player's start the room showed is not explored");
    }
    {
        // The start spots a seat is given: FIXED or BY SLOT, each seated enemy's own,
        // with its owner; RANDOM, every spot of the map but its own and its allies'.
        const std::vector<std::pair<float,float>> slotPos{{100,100},{200,200},{0,0},{400,400},{600,600}};
        const std::vector<uint8_t> seated{1,1,0,1,1};
        auto allied=[](int a,int b){return (a==0 && b==3) || (a==3 && b==0);};
        const auto fixed=ai::enemyStartSpots(0,slotPos,seated,allied,{});
        check(fixed.size()==2 && fixed[0].x==200 && fixed[0].owner==1 && fixed[1].x==600 && fixed[1].owner==4,
              "FIXED starts: each seated enemy's own start, with its owner");
        const auto random=ai::enemyStartSpots(0,slotPos,seated,allied,
                                              {{100,100},{200,200},{300,300},{400,400},{500,500},{600,600}});
        check(random.size()==4 && random[0].x==200 && random[1].x==300 && random[2].x==500 && random[3].x==600 &&
              std::all_of(random.begin(),random.end(),[](const auto& s){return s.owner<0;}),
              "RANDOM starts: every map spot but our own and our allies', owners unknown");
    }
}

// A level that has seen little of the enemy does not take itself for far ahead
// (StrikeBoard: an enemy home not looked at counts as at least our own army).
static void fogScouting(const sim::TypeRegistry& registry,const ai::Profile& profile,
                        const sim::UnitType& soldier) {
    {
        // Defensive with a large army and a lone enemy soldier it has never seen:
        // it cannot know it is far ahead, so it does not go out to clean up (as it
        // does clearly ahead); from its fifth minute a lone scout goes to look.
        sim::World w;terrain(w);w.player(0).income=100;
        for (int i=0;i<30;++i) w.spawn(&soldier,400+float(i%6)*40,800+float(i/6)*40,0,0);
        w.spawn(&soldier,3600,3600,0,1);
        sight(w);
        ai::Controller controller(0,registry,profile,1,ai::Difficulty::Defensive,{{3400,3400}});
        // The first think walks the army out of the base (stationArmy).
        for (const auto& c:think(controller,w,5*60*30)) sim::applyCommand(w,registry,c);
        sight(w);
        const auto cs=think(controller,w,5*60*30+30);
        check(attacks(cs)==1,"Defensive that has seen nothing of the enemy does not think itself far ahead, and scouts");
    }
}

// Lodestone sites by what a player in the seat knows: NOT EXPLORED, a deposit on
// ground its side has not explored is not picked; a deposit held by an enemy
// lodestone it has never seen is refused by the real placement test (as a
// player's order there is) and passed over; one it has seen taken, and still
// remembers, is not picked.
static void lodestoneKnowledge() {
    auto files=std::make_shared<hpi::Vfs::Files>();
    auto file=[&](const std::string& path,const std::string& text) {
        std::string lines;
        for (char ch : text) { lines += ch; if (ch == ';' || ch == '{' || ch == '}') lines += '\n'; }
        (*files)[path]={lines.begin(),lines.end()};
    };
    auto type=[&](const char* id,const char* fields) {
        file(std::string("units/")+id+".fbi",std::string("[UNITINFO]{unitname=")+id+
            ";objectname="+id+";maxdamage=100;side=TAR;sightdistance=600;"+fields+"}");
    };
    type("tarhell","footprintx=4;footprintz=4;builder=1;workertime=30;buildcost=1000;buildtime=100;");
    type("tarpries","footprintx=2;footprintz=2;builder=1;workertime=10;canmove=1;maxvelocity=1;"
                    "buildcost=3000;buildtime=300;");
    type("tarlode","footprintx=2;footprintz=2;yardmap=SSSS;mogriumincome=10;mogriumstorage=1000;"
                   "buildcost=300;buildtime=85;");
    file("canbuild/tarhell/tarpries.tdf","[CANBUILD]{sortbias=0;}");
    file("canbuild/tarpries/tarlode.tdf","[CANBUILD]{sortbias=0;}");
    hpi::Vfs vfs;vfs.setMapFiles(files);
    sim::TypeRegistry reg;reg.loadDir(vfs,"units");reg.loadBuildTree(vfs,"canbuild");
    ai::Profile profile;
    // Home (800,800), the builder at (800,1000): the deposit at (1000,800) lies in
    // their sight, the one at (2400,800) far beyond it.
    auto build=[&](sim::World& w,int& builder) {
        terrain(w);w.buildNavClasses(reg);w.setManaSpots({{1008,808},{2408,808}});
        w.spawn(reg.find("tarhell"),800,800,0,0);
        builder=w.spawn(reg.find("tarpries"),800,1000,0,0);
    };
    auto pick=[&](const ai::Controller& c,const sim::World& w,int builder) {
        float x=0,z=0;
        const bool ok=ai::PlacementProbe::placeFrom(c,w,reg.find("tarlode"),*w.unit(builder),2408,808,x,z);
        return ok ? x : 0.0f;
    };
    {
        sim::World w;int builder=0;build(w,builder);w.setRoomVision(false,false);sight(w);
        ai::Controller controller(0,reg,profile,1,ai::Difficulty::Absurd);
        think(controller,w,40);
        check(pick(controller,w,builder)==1008,"NOT EXPLORED: a deposit on ground never explored is not picked");
        sim::World v;build(v,builder);sight(v);
        ai::Controller explored(0,reg,profile,1,ai::Difficulty::Absurd);
        think(explored,v,40);
        check(pick(explored,v,builder)==2408,"EXPLORED: every deposit on the map is known");
    }
    {
        sim::World w;int builder=0;build(w,builder);
        const int enemy=w.spawn(reg.find("tarlode"),2408,808,0,1);
        sight(w);
        ai::Controller controller(0,reg,profile,1,ai::Difficulty::Absurd);
        think(controller,w,40);
        check(pick(controller,w,builder)==1008,"a deposit an unseen enemy lodestone holds is refused, and passed over");
        // Something of ours gets there and sees it; gone again, it is remembered.
        const int eye=w.spawn(reg.find("tarhell"),2300,1100,0,0);
        sight(w);think(controller,w,80);
        check(pick(controller,w,builder)==1008,"a deposit seen taken is not picked");
        w.unit(eye)->x=sim::Fixed::fromInt(400);w.unit(eye)->z=sim::Fixed::fromInt(3600);sight(w);think(controller,w,120);
        check(w.unit(enemy)->alive() && pick(controller,w,builder)==1008,"... nor while it is remembered taken");
    }
    {
        // NOT EXPLORED, player 0: its think at tick 0 comes before the sim's first
        // view and exploration pass, when it would know no ground at all, while a
        // player sees the ground round their start from the first frame. It waits
        // for tick 1 and orders the lodestone on the deposit in sight there.
        sim::World w;int builder=0;build(w,builder);w.setRoomVision(false,false);
        w.player(0).storage=5000;w.player(0).mana=1000;
        ai::Profile p;p.weight["tarlode"]=10;
        ai::Controller controller(0,reg,p,1,ai::Difficulty::Absurd);
        auto cs=think(controller,w,0);
        const bool waited=cs.empty();
        sight(w);
        const auto next=think(controller,w,1);
        cs.insert(cs.end(),next.begin(),next.end());
        check(waited && std::any_of(cs.begin(),cs.end(),[](const auto& c) {
                  return c.kind==net::Cmd::Build && std::string(c.type)=="tarlode" && c.x==1008; }),
              "NOT EXPLORED: player 0's first think waits a tick for its sight, then takes the deposit in view");
    }
}

static void monarchFightsBack(const sim::TypeRegistry& registry,const ai::Profile& profile,
                              const sim::UnitType& soldier) {
    auto base=soldier;base.id="base";base.maxVel={};base.sight=750;
    base.weapon.damage=0;base.weapons.clear();
    auto monarch=soldier;monarch.id="monarch";monarch.commander=true;monarch.isBuilder=true;monarch.sight=300;
    {
        sim::World w;terrain(w);w.spawn(&base,400,800,0,0);
        const int m=w.spawn(&monarch,600,800,0,0);
        const int enemy=w.spawn(&soldier,680,800,0,1);
        net::Command mv;mv.kind=net::Cmd::Move;mv.player=0;mv.unitId=m;mv.x=900;mv.z=800;
        sim::applyCommand(w,registry,mv);
        ai::Controller ai(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800}});
        auto cs=seen(ai,w);
        check(std::any_of(cs.begin(),cs.end(),[&](const auto& c){
            return c.kind==net::Cmd::Attack && c.unitId==m && c.targetId==enemy;
        }),"a moving Monarch attacks an enemy within its reach");
        w.unit(enemy)->x=sim::Fixed::fromInt(2000);
        cs=seen(ai,w,30);
        check(std::none_of(cs.begin(),cs.end(),[&](const auto& c){return c.kind==net::Cmd::Attack && c.unitId==m;}),
              "a Monarch with no enemy near is left to its work");
        auto blindMonarch=monarch;blindMonarch.sight=60;
        sim::World w2;terrain(w2);w2.spawn(&base,400,800,0,0);
        const int m2=w2.spawn(&blindMonarch,600,800,0,0);
        const int shooter=w2.spawn(&soldier,780,800,0,1);
        w2.unit(m2)->lastHitBy=shooter;
        ai::Controller ai2(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800}});
        cs=seen(ai2,w2);
        check(std::any_of(cs.begin(),cs.end(),[&](const auto& c){
            return c.kind==net::Cmd::Attack && c.unitId==m2 && c.targetId==shooter;
        }),"an idle Monarch hits back at an attacker it cannot see");
    }
    {
        sim::World w;terrain(w);w.spawn(&base,800,800,0,0);
        const int builderId=w.spawn(&monarch,800,800,0,0);
        sim::UnitType building;building.id="site-test";building.maxHp=100;building.maxVel=sim::Fixed();
        building.footX=building.footZ=2;
        ai::Controller defensive(0,registry,profile,1,ai::Difficulty::Defensive,{{3400,800}});
        float x=0,z=0;
        check(ai::PlacementProbe::place(defensive,w,&building,*w.unit(builderId),x,z),
              "Defensive placement baseline finds a site");
        const float fx=x+40,fz=z;
        w.spawn(&soldier,fx,fz,0,1);
        sight(w);think(defensive,w);   // what its side sees (World::viewCount), noted at a think
        float x2=0,z2=0;
        const bool found=ai::PlacementProbe::place(defensive,w,&building,*w.unit(builderId),x2,z2);
        check(!found || (x2-fx)*(x2-fx)+(z2-fz)*(z2-fz)>=400*400,
              "the Defensive AI does not site a building inside a spotted fight");
    }
}

// Retail limits each builder type, not the total (ai/default.txt: arabuild,
// arafly and arapries 4 each), and has no difficulty levels. Normal grows from the
// Monarch plus one to the Monarch plus four as income allows; Easy stays at the
// Monarch plus one. Defensive adds one per 50 income with no ceiling, the
// profile's per-type limit included.
static void builderCaps() {
    auto files=std::make_shared<hpi::Vfs::Files>();
    auto file=[&](const std::string& path,const std::string& text) {
        std::string lines;
        for (char ch : text) { lines += ch; if (ch == ';' || ch == '{' || ch == '}') lines += '\n'; }
        (*files)[path]={lines.begin(),lines.end()};
    };
    auto type=[&](const char* id,const char* fields) {
        file(std::string("units/")+id+".fbi",std::string("[UNITINFO]{unitname=")+id+
            ";objectname="+id+";maxdamage=100;footprintx=2;footprintz=2;buildtime=30;buildcost=10;"+fields+"}");
    };
    type("keep","builder=1;workertime=30;");
    type("king","builder=1;commander=1;workertime=30;canmove=1;maxvelocity=1;");
    type("mason","builder=1;workertime=30;canmove=1;maxvelocity=1;");
    file("canbuild/keep/mason.tdf","[CANBUILD]{sortbias=0;}");
    hpi::Vfs vfs;vfs.setMapFiles(files);
    sim::TypeRegistry reg;reg.loadDir(vfs,"units");reg.loadBuildTree(vfs,"canbuild");
    auto trainsMason=[&](ai::Difficulty difficulty,float income,int masons) {
        sim::World w;terrain(w);w.buildNavClasses(reg);
        w.player(0).mana=10000;w.player(0).income=income;
        w.spawn(reg.find("keep"),400,800,0,0);
        w.spawn(reg.find("king"),800,800,0,0);
        for (int i=0;i<masons;++i) w.spawn(reg.find("mason"),800+float(i)*40,1200,0,0);
        ai::Profile p;p.weight["mason"]=100;p.limit["mason"]=4;
        ai::Controller controller(0,reg,p,1,difficulty);
        const auto cs=think(controller,w,difficulty==ai::Difficulty::Easy ? 90 : 0);
        return std::any_of(cs.begin(),cs.end(),[](const auto& c) {
            return c.kind==net::Cmd::Train && std::string(c.type)=="mason";
        });
    };
    check(!trainsMason(ai::Difficulty::Normal,20,1),"Normal opens with the Monarch and one builder");
    check(trainsMason(ai::Difficulty::Normal,100,1),"Normal adds builders as its income grows");
    check(trainsMason(ai::Difficulty::Normal,150,3),"Normal grows to the Monarch and four builders");
    check(!trainsMason(ai::Difficulty::Normal,1000,4),"Normal stops at the Monarch and four builders");
    check(!trainsMason(ai::Difficulty::Easy,150,1),"Easy keeps the Monarch and one builder");
    for (auto difficulty : {ai::Difficulty::Defensive,ai::Difficulty::AbsurdlyDefensive}) {
        check(!trainsMason(difficulty,20,1),"Defensive opens with the Monarch and one builder");
        check(trainsMason(difficulty,150,1),"Defensive adds builders as its income grows");
        check(trainsMason(difficulty,1000,4),"Defensive passes the profile's builder limit while income allows");
        check(!trainsMason(difficulty,1000,21),"Defensive's builders still follow income: the Monarch and one per 50");
    }
}

static void defensiveResponses(const sim::TypeRegistry& registry,const ai::Profile& profile,
                               const sim::UnitType& soldier) {
    auto base=soldier;base.id="base";base.maxVel={};base.sight=750;
    base.weapon.damage=0;base.weapons.clear();
    auto builder=soldier;builder.isBuilder=true;builder.commander=true;
    for (auto mode:{sim::PathfindingMode::Retail,sim::PathfindingMode::Legion}) {
        sim::World w;terrain(w);w.setPathfindingMode(mode);w.player(0).defensiveAi=true;
        w.spawn(&base,400,800,0,0);
        std::vector<int> army;
        for (int i=0;i<6;++i) army.push_back(w.spawn(&soldier,400,960+float(i)*40,0,0));
        const int monarch=w.spawn(&builder,450,800,0,0);
        const int unfinished=w.spawn(&soldier,500,1200,0,0);w.unit(unfinished)->underConstruction=true;
        const int cargo=w.spawn(&soldier,500,1240,0,0);w.unit(cargo)->inTransport=monarch;
        const int intruder=w.spawn(&soldier,1100,800,0,1);
        ai::Controller defensive(0,registry,profile,1,ai::Difficulty::Defensive,{{3400,800}});
        auto cs=seen(defensive,w);
        check(attacks(cs)==6 && std::all_of(cs.begin(),cs.end(),[&](const auto& c) {
            return c.kind!=net::Cmd::AttackMove || (c.x==1100 && c.z==800 &&
                std::find(army.begin(),army.end(),c.unitId)!=army.end());
        }),"Defensive mobilizes its whole ready army to meet a spotted intruder in both path modes");
        for (const auto& c:cs) sim::applyCommand(w,registry,c);
        check(attacks(seen(defensive,w,30))==0,"Defensive does not restart an unchanged interception every think");
        for (int tick=0;tick<90;++tick) w.tick(1.f/30.f);
        check(std::all_of(army.begin(),army.end(),[&](int id){return w.unit(id)->x.toFloat()>430;}),
              "Defensive interception commands actually move the army through normal navigation");
        // A converted enemy must cease being a threat, including while a
        // defender has an automatic combat order ahead of its movement goal.
        w.unit(intruder)->player=0; // observe the ownership change after conversion
        cs=seen(defensive,w,120);
        check(attacks(cs)==0 && std::count_if(cs.begin(),cs.end(),[&](const auto& c) {
            return c.kind==net::Cmd::Move && c.x==400 &&
                std::find(army.begin(),army.end(),c.unitId)!=army.end();
        })==6,"Defensive recalls responders to their posts when a threat is converted");
        for (const auto& c:cs) sim::applyCommand(w,registry,c);
        check(seen(defensive,w,150).empty(),"Defensive does not repeatedly reset the return march");
    }
    {
        sim::World w;terrain(w);w.spawn(&base,400,800,0,0);
        const int defender=w.spawn(&soldier,400,1000,0,0);
        const int enemy=w.spawn(&soldier,1100,800,0,1);
        ai::Controller defensive(0,registry,profile,1,ai::Difficulty::Defensive,{{3400,800}});
        auto cs=seen(defensive,w);for(const auto& c:cs)sim::applyCommand(w,registry,c);
        w.unit(enemy)->x=sim::Fixed::fromInt(3000);
        cs=seen(defensive,w,30);
        check(attacks(cs)==0 && std::any_of(cs.begin(),cs.end(),[&](const auto& c) {
            return c.unitId==defender && c.kind==net::Cmd::Move && c.x==400 && c.z==1000;
        }),"Defensive recalls its army instead of following a retreating enemy to its base");
        for(const auto& c:cs)sim::applyCommand(w,registry,c);
        w.unit(defender)->orders.clear();w.unit(defender)->x=sim::Fixed::fromInt(500);
        check(seen(defensive,w,60).empty(),"Defensive respects return-order completion, including relaxed crowd arrival");
    }
    {
        sim::World w;terrain(w);auto blindBase=base;blindBase.sight=80;
        w.spawn(&blindBase,400,800,0,0);w.spawn(&soldier,400,1000,0,0);
        const int enemy=w.spawn(&soldier,1000,800,0,1);
        ai::Controller defensive(0,registry,profile,1,ai::Difficulty::Defensive,{{3400,800}});
        check(attacks(seen(defensive,w))==0,"Defensive ignores nearby threats hidden by fog");
        blindBase.radar=700;
        auto cs=seen(defensive,w,30);
        check(attacks(cs)==1,"Defensive responds to threats spotted by radar");
        w.unit(enemy)->cloaked=true;
        check(attacks(seen(defensive,w,60))==0,"Defensive does not target a cloaked enemy");
    }
    {
        sim::World w;terrain(w);w.spawn(&base,400,800,0,0);
        w.spawn(&soldier,400,1000,0,0);
        auto scout=soldier;scout.weapon.damage=0;scout.weapons.clear();scout.sight=500;
        w.spawn(&scout,2500,800,0,0);w.spawn(&soldier,2700,800,0,1);
        ai::Controller defensive(0,registry,profile,1,ai::Difficulty::Defensive,{{2700,800}});
        check(attacks(seen(defensive,w))==0,"Defensive ignores distant visible enemies and known enemy starts");
        w.player(2).team=w.player(0).team;w.spawn(&soldier,1050,800,0,2);
        auto unarmed=scout;w.spawn(&unarmed,1000,800,0,1);
        check(attacks(seen(defensive,w,30))==0,"Allies and harmless scouts do not mobilize the defensive army");
        w.spawn(&base,2500,1200,0,0);w.spawn(&soldier,2700,1200,0,1);
        check(attacks(seen(defensive,w,60))==1,"Defensive also protects owned outlying buildings");
    }
    {
        sim::World w;terrain(w,true);w.spawn(&base,1300,800,0,0);
        const int ground=w.spawn(&soldier,1300,1000,0,0);
        auto flyer=soldier;flyer.canFly=true;
        const int air=w.spawn(&flyer,1300,1200,0,0);
        w.spawn(&soldier,2624,800,0,1);
        ai::Controller defensive(0,registry,profile,1,ai::Difficulty::Defensive);
        // An outlying building reveals the threat across disconnected land.
        w.spawn(&base,2624,1200,0,0);
        const auto cs=seen(defensive,w);
        check(attacks(cs)==1 && std::any_of(cs.begin(),cs.end(),[&](const auto& c) {
            return c.kind==net::Cmd::AttackMove && c.unitId==air;
        }) && std::none_of(cs.begin(),cs.end(),[&](const auto& c) {
            return c.kind==net::Cmd::AttackMove && c.unitId==ground;
        }),"Defensive sends reachable flyers while keeping stranded ground units home");
    }
    {
        sim::World w;terrain(w,true);w.spawn(&base,1480,800,0,0);
        auto boat=soldier;boat.domain=sim::UnitType::Domain::Water;boat.weapon.range=240;
        const int ship=w.spawn(&boat,1800,800,0,0);w.spawn(&soldier,1480,1000,0,1);
        ai::Controller defensive(0,registry,profile,1,ai::Difficulty::Defensive);
        const auto cs=seen(defensive,w);
        check(attacks(cs)==1 && std::any_of(cs.begin(),cs.end(),[&](const auto& c) {
            return c.kind==net::Cmd::AttackMove && c.unitId==ship && c.x>=1536;
        }),"Defensive ships use a reachable water approach to a threat on shore");
    }
    {
        sim::World w;terrain(w);w.spawn(&base,400,800,0,0);
        for(int i=0;i<300;++i)w.spawn(&soldier,400+float(i%10)*32,1000+float(i/10)*32,0,0);
        w.spawn(&soldier,1100,800,0,1);
        // A strong enemy army far off: not clearly winning, so this is interception alone.
        for(int i=0;i<70;++i)w.spawn(&soldier,3200+float(i%10)*32,3200+float(i/10)*32,0,1);
        ai::Controller defensive(0,registry,profile,1,ai::Difficulty::Defensive);
        auto cs=seen(defensive,w);
        check(attacks(cs)==256,"Defensive limits interception commands per think for large armies");
        for(const auto& c:cs)sim::applyCommand(w,registry,c);
        check(attacks(seen(defensive,w,30))==44,"Defensive sends remaining troops without restarting the first wave");
    }
    {
        sim::World w;terrain(w);w.spawn(&base,400,800,0,0);
        const int defender=w.spawn(&soldier,400,1000,0,0);
        const int enemy=w.spawn(&soldier,1100,800,0,1);
        w.attack(defender,enemy,false);
        ai::Controller defensive(0,registry,profile,1,ai::Difficulty::Defensive);
        check(attacks(seen(defensive,w))==0,"Defensive preserves an existing explicit attack order");
    }
}
// Zhon has no factory buildings: its mobile builders train the army and build
// the lodestones. Their allowance is another side's factories plus those away
// at lodestones (Normal, Hard, Absurd) or its builders past the Monarch (Easy),
// not the factories alone.
static void mobileProducers() {
    auto files=std::make_shared<hpi::Vfs::Files>();
    auto file=[&](const std::string& path,const std::string& text) {
        std::string lines;
        for (char ch : text) { lines += ch; if (ch == ';' || ch == '{' || ch == '}') lines += '\n'; }
        (*files)[path]={lines.begin(),lines.end()};
    };
    auto type=[&](const char* id,const char* fields) {
        file(std::string("units/")+id+".fbi",std::string("[UNITINFO]{unitname=")+id+
            ";objectname="+id+";maxdamage=100;footprintx=2;footprintz=2;buildtime=30;buildcost=10;"+fields+"}");
    };
    // ZON: the Monarch conjures handlers, which conjure creatures. TAR: the same
    // menus, plus a factory building.
    for (const char* side : {"zon","tar"}) {
        const std::string s=side,upper=s=="zon" ? "ZON" : "TAR";
        type((s+"king").c_str(),("side="+upper+";builder=1;commander=1;workertime=30;canmove=1;maxvelocity=1;").c_str());
        type((s+"hand").c_str(),("side="+upper+";builder=1;workertime=30;canmove=1;maxvelocity=1;").c_str());
        type((s+"gob").c_str(),("side="+upper+";canmove=1;maxvelocity=1;").c_str());
        type((s+"troll").c_str(),("side="+upper+";canmove=1;maxvelocity=1;").c_str());
        file("canbuild/"+s+"king/"+s+"hand.tdf","[CANBUILD]{sortbias=0;}");
        file("canbuild/"+s+"hand/"+s+"gob.tdf","[CANBUILD]{sortbias=0;}");
        file("canbuild/"+s+"hand/"+s+"troll.tdf","[CANBUILD]{sortbias=0;}");
    }
    type("tarhell","side=TAR;builder=1;workertime=30;");
    file("canbuild/tarhell/targob.tdf","[CANBUILD]{sortbias=0;}");
    hpi::Vfs vfs;vfs.setMapFiles(files);
    sim::TypeRegistry reg;reg.loadDir(vfs,"units");reg.loadBuildTree(vfs,"canbuild");
    auto conjuresHandler=[&](const std::string& side,ai::Difficulty difficulty,float income,int handlers) {
        sim::World w;terrain(w);w.buildNavClasses(reg);
        w.player(0).mana=100000;w.player(0).income=income;
        w.spawn(reg.find(side+"king"),800,800,0,0);
        // Busy handlers: each is already conjuring, so none of them acts this think.
        for (int i=0;i<handlers;++i) {
            const int id=w.spawn(reg.find(side+"hand"),1400+float(i%6)*60,1400+float(i/6)*60,0,0);
            sim::Order o;o.buildType=reg.find(side+"gob");
            w.unit(id)->orders.push_back(o);
        }
        for (int i=0;i<12;++i) w.spawn(reg.find(side+"troll"),2000+float(i)*40,800,0,0);
        ai::Profile p;
        for (const char* id : {"hand","gob","troll"}) {p.weight[side+id]=10;p.limit[side+id]=16;}
        ai::Controller controller(0,reg,p,1,difficulty);
        const auto cs=think(controller,w,difficulty==ai::Difficulty::Easy ? 90 : 0);
        return std::any_of(cs.begin(),cs.end(),[&](const auto& c) {
            return c.kind==net::Cmd::Build && std::string(c.type)==side+"hand";
        });
    };
    // Income 100: two factories (100/60 + 1) and two producers away at lodestones
    // (tools/ai_economy_test.cpp), the Monarch on top.
    check(conjuresHandler("zon",ai::Difficulty::Normal,100,3),
          "Zhon's producers grow to its factories plus those away at lodestones");
    check(!conjuresHandler("zon",ai::Difficulty::Normal,100,4),
          "Zhon's producers stop at its factories plus those away at lodestones");
    check(!conjuresHandler("tar",ai::Difficulty::Normal,100,3),
          "a side with factory buildings keeps its mobile producers to the factory count");
    check(conjuresHandler("zon",ai::Difficulty::Easy,100,2),
          "Easy Zhon keeps the Monarch's one builder on top of its two factories");
    check(!conjuresHandler("zon",ai::Difficulty::Easy,100,3),
          "Easy Zhon stops at two factories and one builder");
}

// Every builder=1 mobile unit counts against the difficulty's cap, the Monarch,
// the unfinished and the queued included; no builder type passes its own limit.
static void builderTotals() {
    auto files=std::make_shared<hpi::Vfs::Files>();
    auto file=[&](const std::string& path,const std::string& text) {
        std::string lines;
        for (char ch : text) { lines += ch; if (ch == ';' || ch == '{' || ch == '}') lines += '\n'; }
        (*files)[path]={lines.begin(),lines.end()};
    };
    auto type=[&](const char* id,const char* fields) {
        file(std::string("units/")+id+".fbi",std::string("[UNITINFO]{unitname=")+id+
            ";objectname="+id+";maxdamage=100;footprintx=2;footprintz=2;buildtime=30;buildcost=10;"+fields+"}");
    };
    // ARA: a keep trains masons and priests; a priest's menu is troops, so it is
    // a factory by category but a builder all the same. ZON: no factory building.
    type("arakeep","side=ARA;builder=1;workertime=30;");
    type("araking","side=ARA;builder=1;commander=1;workertime=30;canmove=1;maxvelocity=1;");
    type("aramason","side=ARA;builder=1;workertime=30;canmove=1;maxvelocity=1;");
    type("arapries","side=ARA;builder=1;workertime=30;canmove=1;maxvelocity=1;");
    type("aramonk","side=ARA;canmove=1;maxvelocity=1;");
    file("canbuild/arakeep/aramason.tdf","[CANBUILD]{sortbias=0;}");
    file("canbuild/arakeep/arapries.tdf","[CANBUILD]{sortbias=0;}");
    file("canbuild/arapries/aramonk.tdf","[CANBUILD]{sortbias=0;}");
    type("zonking","side=ZON;builder=1;commander=1;workertime=30;canmove=1;maxvelocity=1;");
    type("zonhand","side=ZON;builder=1;workertime=30;canmove=1;maxvelocity=1;");
    type("zongob","side=ZON;canmove=1;maxvelocity=1;");
    file("canbuild/zonking/zonhand.tdf","[CANBUILD]{sortbias=0;}");
    file("canbuild/zonhand/zonhand.tdf","[CANBUILD]{sortbias=0;}");
    file("canbuild/zonhand/zongob.tdf","[CANBUILD]{sortbias=0;}");
    hpi::Vfs vfs;vfs.setMapFiles(files);
    sim::TypeRegistry reg;reg.loadDir(vfs,"units");reg.loadBuildTree(vfs,"canbuild");
    auto builders=[](const std::vector<net::Command>& cs,std::initializer_list<const char*> ids) {
        return int(std::count_if(cs.begin(),cs.end(),[&](const auto& c) {
            return (c.kind==net::Cmd::Train || c.kind==net::Cmd::Build) &&
                std::any_of(ids.begin(),ids.end(),[&](const char* id){return std::string(c.type)==id;});
        }));
    };
    // keeps idle keeps; masons/priests already standing; queued masons in one
    // more, busy keep that has not begun them.
    auto aramon=[&](ai::Difficulty difficulty,float income,int keeps,int masons,int priests,int queued,
                    int masonLimit=4) {
        sim::World w;terrain(w);w.buildNavClasses(reg);
        w.player(0).mana=100000;w.player(0).income=income;
        w.spawn(reg.find("araking"),800,800,0,0);
        for (int i=0;i<keeps;++i) w.spawn(reg.find("arakeep"),300+float(i)*100,300,0,0);
        if (queued) {
            const int id=w.spawn(reg.find("arakeep"),300,1600,0,0);
            for (int i=0;i<queued;++i) w.unit(id)->buildQueue.push_back(reg.find("aramason"));
        }
        for (int i=0;i<masons;++i) w.spawn(reg.find("aramason"),1400+float(i)*60,1400,0,0);
        for (int i=0;i<priests;++i) {
            // Busy priests: each is already training, so none of them acts this think.
            const int id=w.spawn(reg.find("arapries"),1400+float(i)*60,1600,0,0);
            sim::Order o;o.buildType=reg.find("aramonk");w.unit(id)->orders.push_back(o);
        }
        for (int i=0;i<12;++i) w.spawn(reg.find("aramonk"),2000+float(i)*40,800,0,0);
        ai::Profile p;
        p.weight["aramason"]=10;p.limit["aramason"]=masonLimit;
        p.weight["arapries"]=10;p.limit["arapries"]=4;p.weight["aramonk"]=10;p.limit["aramonk"]=40;
        ai::Controller controller(0,reg,p,1,difficulty);
        return builders(think(controller,w,difficulty==ai::Difficulty::Easy ? 90 : 0),{"aramason","arapries"});
    };
    // Income 1000: Normal's cap is five, the Monarch included, and eight factories.
    check(aramon(ai::Difficulty::Normal,1000,1,0,4,0)==0,
          "priests count against the builder cap though their menus make them factories");
    check(aramon(ai::Difficulty::Normal,1000,1,2,1,0)==1,
          "below the cap a factory race still adds a builder");
    check(aramon(ai::Difficulty::Normal,1000,1,3,0,1)==0,
          "a builder queued in a factory counts against the cap before it begins");
    check(aramon(ai::Difficulty::Normal,1000,3,3,0,0)==1,
          "several producers in one think stop at the cap");
    check(aramon(ai::Difficulty::Hard,100000,1,4,4,0)==0,
          "Hard holds a builder type to its own ai/default.txt limit");
    auto zhon=[&](ai::Difficulty difficulty,float income,int handlers,int conjuring) {
        sim::World w;terrain(w);w.buildNavClasses(reg);
        w.player(0).mana=100000;w.player(0).income=income;
        w.spawn(reg.find("zonking"),800,800,0,0);
        for (int i=0;i<handlers;++i) {
            // Busy handlers: the first `conjuring` walk to conjure a handler.
            const int id=w.spawn(reg.find("zonhand"),1400+float(i%6)*60,1400+float(i/6)*60,0,0);
            sim::Order o;o.buildType=reg.find(i<conjuring ? "zonhand" : "zongob");
            w.unit(id)->orders.push_back(o);
        }
        for (int i=0;i<12;++i) w.spawn(reg.find("zongob"),2000+float(i)*40,800,0,0);
        ai::Profile p;p.weight["zonhand"]=10;p.limit["zonhand"]=3;p.weight["zongob"]=10;p.limit["zongob"]=40;
        if (difficulty==ai::Difficulty::Normal) p.limit["zonhand"]=16;
        ai::Controller controller(0,reg,p,1,difficulty);
        return builders(think(controller,w,difficulty==ai::Difficulty::Easy ? 90 : 0),{"zonhand"});
    };
    // Income 100: two factories and two away at lodestones, so the Monarch and four handlers.
    check(zhon(ai::Difficulty::Normal,100,3,0)==1,"Zhon's producers grow to its own allowance");
    check(zhon(ai::Difficulty::Normal,100,3,1)==0,
          "a handler ordered but not yet conjured counts against Zhon's allowance");
    // Hard at income 100000: 32 factories and a quarter more for the producers away
    // at lodestones, so 40, and 41 with the Monarch (tools/ai_economy_test.cpp).
    check(zhon(ai::Difficulty::Hard,100000,3,0)==1,
          "Hard Zhon's producers take the factory raise past their ai/default.txt limit");
    check(zhon(ai::Difficulty::Hard,100000,39,0)==1 && zhon(ai::Difficulty::Hard,100000,40,0)==0,
          "Hard Zhon's producers stop at its factories plus those away at lodestones");
}

// The income target met, an idle builder still builds income while the store
// is full (retail's builders draw from the whole menu, the lodestone among it).
static void fullStore() {
    auto files=std::make_shared<hpi::Vfs::Files>();
    auto file=[&](const std::string& path,const std::string& text) {
        std::string lines;
        for (char ch : text) { lines += ch; if (ch == ';' || ch == '{' || ch == '}') lines += '\n'; }
        (*files)[path]={lines.begin(),lines.end()};
    };
    auto type=[&](const char* id,const char* fields) {
        file(std::string("units/")+id+".fbi",std::string("[UNITINFO]{unitname=")+id+
            ";objectname="+id+";maxdamage=100;footprintx=2;footprintz=2;buildtime=30;buildcost=10;"+fields+"}");
    };
    type("keep","builder=1;workertime=30;");
    type("mason","builder=1;workertime=30;canmove=1;maxvelocity=1;");
    type("mine","mogriumincome=10;");
    file("canbuild/keep/mason.tdf","[CANBUILD]{sortbias=0;}");
    file("canbuild/mason/mine.tdf","[CANBUILD]{sortbias=0;}");
    hpi::Vfs vfs;vfs.setMapFiles(files);
    sim::TypeRegistry reg;reg.loadDir(vfs,"units");reg.loadBuildTree(vfs,"canbuild");
    auto buildsMine=[&](ai::Difficulty difficulty,float mana) {
        sim::World w;terrain(w);w.buildNavClasses(reg);
        w.player(0).storage=5000;w.player(0).mana=mana;w.player(0).income=100;
        w.spawn(reg.find("keep"),400,800,0,0);
        for (int i=0;i<3;++i) w.spawn(reg.find("mason"),800+float(i)*60,1200,0,0);
        ai::Profile p;p.weight["mason"]=10;p.limit["mason"]=3;p.weight["mine"]=10;
        ai::Controller controller(0,reg,p,1,difficulty);
        const auto cs=think(controller,w,difficulty==ai::Difficulty::Easy ? 90 : 0);
        return std::any_of(cs.begin(),cs.end(),[](const auto& c) {
            return c.kind==net::Cmd::Build && std::string(c.type)=="mine";
        });
    };
    check(!buildsMine(ai::Difficulty::Normal,3000),"Normal's builders rest once income meets its target");
    check(buildsMine(ai::Difficulty::Normal,4800),"Normal's idle builders build income while the store is full");
    check(buildsMine(ai::Difficulty::Defensive,4800),"Defensive's idle builders build income while the store is full");
    check(!buildsMine(ai::Difficulty::Easy,4800),"Easy's builders rest beside a full store");
}

// The AI's lodestone search tests deposits nearest first and takes the first that passes.
// It must pick what a scan of every deposit in index order, keeping the nearest usable
// (the lower index on a tie), picked: so each deposit is judged alone, in a world with
// only that deposit, and the nearest of the passing ones is the answer. The deposits sit
// on a grid centred on the builder's anchor, so every distance ties four or eight ways,
// listed in shuffled order; a factory shuts some in, and built-on ones fail canPlace.
static void lodestoneChoice() {
    auto files=std::make_shared<hpi::Vfs::Files>();
    auto file=[&](const std::string& path,const std::string& text) {
        std::string lines;
        for (char ch : text) { lines += ch; if (ch == ';' || ch == '{' || ch == '}') lines += '\n'; }
        (*files)[path]={lines.begin(),lines.end()};
    };
    auto type=[&](const char* id,const char* fields) {
        file(std::string("units/")+id+".fbi",std::string("[UNITINFO]{unitname=")+id+
            ";objectname="+id+";maxdamage=100;side=TAR;sightdistance=600;"+fields+"}");
    };
    type("tarhell","footprintx=4;footprintz=4;builder=1;workertime=30;buildcost=1000;buildtime=100;");
    type("tarpries","footprintx=2;footprintz=2;builder=1;workertime=10;canmove=1;maxvelocity=1;"
                    "buildcost=3000;buildtime=300;");
    type("tarlode","footprintx=2;footprintz=2;yardmap=SSSS;mogriumincome=10;mogriumstorage=1000;"
                   "buildcost=300;buildtime=85;");
    file("canbuild/tarhell/tarpries.tdf","[CANBUILD]{sortbias=0;}");
    file("canbuild/tarpries/tarlode.tdf","[CANBUILD]{sortbias=0;}");
    hpi::Vfs vfs;vfs.setMapFiles(files);
    sim::TypeRegistry reg;reg.loadDir(vfs,"units");reg.loadBuildTree(vfs,"canbuild");
    std::vector<std::pair<float,float>> all;
    for (int gz=0;gz<12;++gz) for (int gx=0;gx<12;++gx) all.emplace_back(300+float(gx)*100,300+float(gz)*100);
    uint32_t seed=12345;
    for (size_t i=all.size();i>1;--i) {seed=seed*1664525u+1013904223u;std::swap(all[i-1],all[(seed>>8)%i]);}
    // The factory (800,800) is the home; a builder walks in from (800,1000).
    auto build=[&](const std::vector<std::pair<float,float>>& deposits,const std::vector<std::pair<float,float>>& taken,
                   sim::World& w,int& builder) {
        terrain(w);w.buildNavClasses(reg);w.setManaSpots(deposits);
        w.spawn(reg.find("tarhell"),800,800,0,0);
        for (const auto& [x,z]:taken) w.spawn(reg.find("tarlode"),x,z,0,0);
        builder=w.spawn(reg.find("tarpries"),800,1000,0,0);
    };
    ai::Profile profile;
    std::vector<std::pair<float,float>> taken;
    for (const auto& [ax,az]:{std::pair{850.f,850.f},std::pair{800.f,800.f},std::pair{1250.f,950.f}}) {
        taken.clear();
        int rounds=0,agreed=0;
        for (int round=0;round<8;++round) {
            sim::World w;int builder=0;build(all,taken,w,builder);
            ai::Controller controller(0,reg,profile,1,ai::Difficulty::Absurd);
            float x=0,z=0;
            const bool found=ai::PlacementProbe::placeFrom(controller,w,reg.find("tarlode"),*w.unit(builder),ax,az,x,z);
            // The scan in index order: the nearest deposit that passes alone, the first of equals.
            bool expect=false;float ex=0,ez=0,best=1e18f;
            for (const auto& d:all) {
                sim::World alone;int b=0;build({d},taken,alone,b);
                float ox=0,oz=0;
                ai::Controller solo(0,reg,profile,1,ai::Difficulty::Absurd);
                if (!ai::PlacementProbe::placeFrom(solo,alone,reg.find("tarlode"),*alone.unit(b),ax,az,ox,oz)) continue;
                const float dist=(d.first-ax)*(d.first-ax)+(d.second-az)*(d.second-az);
                if (dist<best) {best=dist;ex=d.first;ez=d.second;expect=true;}
            }
            ++rounds;
            if (found==expect && (!found || (x==ex && z==ez))) ++agreed;
            if (!found) break;
            taken.emplace_back(x,z);
        }
        check(rounds>=6 && agreed==rounds,"the lodestone search picks the nearest usable deposit, the first of a tie, as a full scan did");
    }
}

// Divine Lodestones: an upgrade of an own lodestone in a saturated base that pays
// back within the level's horizon, never one on a free deposit; its priest
// trained when there is none; and no Dragon started at an empty store. The ids
// must be the retail ones: World::lodestoneUpgradeBase pairs tarmana with tarlode.
static void divineLodestones() {
    auto files=std::make_shared<hpi::Vfs::Files>();
    auto file=[&](const std::string& path,const std::string& text) {
        std::string lines;
        for (char ch : text) { lines += ch; if (ch == ';' || ch == '{' || ch == '}') lines += '\n'; }
        (*files)[path]={lines.begin(),lines.end()};
    };
    auto type=[&](const char* id,const char* fields) {
        file(std::string("units/")+id+".fbi",std::string("[UNITINFO]{unitname=")+id+
            ";objectname="+id+";maxdamage=100;side=TAR;sightdistance=600;"+fields+"}");
    };
    type("tarhell","footprintx=4;footprintz=4;builder=1;workertime=30;buildcost=1000;buildtime=100;");
    type("tarpries","footprintx=2;footprintz=2;builder=1;workertime=10;canmove=1;maxvelocity=1;"
                    "buildcost=3000;buildtime=300;");
    type("tarlode","footprintx=2;footprintz=2;yardmap=SSSS;mogriumincome=10;mogriumstorage=1000;"
                   "buildcost=300;buildtime=85;");
    type("tarmana","footprintx=3;footprintz=3;yardmap=SSSSSSSSS;mogriumincome=20;mogriumstorage=2000;"
                   "buildcost=6000;buildtime=1000;");
    type("tardrag","footprintx=2;footprintz=2;canmove=1;maxvelocity=1;buildcost=20000;buildtime=4000;");
    type("tarang","footprintx=2;footprintz=2;canmove=1;maxvelocity=1;buildcost=500;buildtime=100;");
    type("targob","footprintx=2;footprintz=2;canmove=1;maxvelocity=1;buildcost=10;buildtime=30;");
    const auto archer=fighter();
    file("canbuild/tarhell/tarpries.tdf","[CANBUILD]{sortbias=0;}");
    for (const char* id : {"tarmana","tardrag","tarang"})   // the priest's menu: more troops than buildings
        file(std::string("canbuild/tarpries/")+id+".tdf","[CANBUILD]{sortbias=0;}");
    hpi::Vfs vfs;vfs.setMapFiles(files);
    sim::TypeRegistry reg;reg.loadDir(vfs,"units");reg.loadBuildTree(vfs,"canbuild");
    struct Setup {
        ai::Difficulty difficulty=ai::Difficulty::Normal;
        uint32_t tick=11*60*30;
        float mana=5000,income=100;
        bool priest=true;
        int divines=0;            // finished Divine Lodestones already standing
        bool building=false;      // an unfinished Divine Lodestone standing
        bool enemy=false;         // an armed enemy 550 px from the lodestone, 1050 from home
        bool freeDeposit=false;   // a free deposit 1800 px from home
        int army=12;
        bool multOne=false;       // the lodestone's deposit pays x1, not x2
        float storage=10000;
        int divineLimit=9;        // the profile's tarmana limit
        bool troops=false;        // the priest's tarang weighted too
    };
    // Home is the buildings' centre: the hell (400,800) and lodestone A (1400,800)
    // -- each finished Divine Lodestone stands on its own deposit in a row at
    // z 1700. Every deposit within 1600 px of home is taken unless `freeDeposit`.
    auto run=[&](const Setup& s) {
        sim::World w;terrain(w);w.buildNavClasses(reg);
        std::vector<std::pair<float,float>> deposits{{1400,800}};
        for (int i=0;i<s.divines+(s.building ? 1 : 0);++i) deposits.emplace_back(200+float(i)*250,1700);
        if (s.freeDeposit) deposits.emplace_back(2700,800);
        w.setManaSpots(deposits);
        std::vector<sim::SacredSite> sites;
        for (size_t i=0;i<deposits.size();++i)
            sites.push_back({sim::footprintOrigin(deposits[i].first,2),sim::footprintOrigin(deposits[i].second,2),
                             2,2,i==0 && s.multOne ? 1.0f : 2.0f});
        w.setSacredSites(sites);
        w.spawn(reg.find("tarhell"),400,800,0,0);
        w.spawn(reg.find("tarlode"),1400,800,0,0);
        for (int i=0;i<s.divines;++i) w.spawn(reg.find("tarmana"),200+float(i)*250,1700,0,0);
        if (s.building) w.unit(w.spawn(reg.find("tarmana"),200+float(s.divines)*250,1700,0,0))->underConstruction=true;
        const int priest=s.priest ? w.spawn(reg.find("tarpries"),900,1100,0,0) : 0;
        for (int i=0;i<s.army;++i) w.spawn(reg.find("targob"),300+float(i%6)*40,1200+float(i/6)*40,0,0);
        if (s.enemy) w.spawn(&archer,1950,800,0,1);
        auto& me=w.player(0);
        me.storage=s.storage;me.mana=s.mana;me.income=s.income;
        ai::Profile p;
        p.weight["tarpries"]=10;p.limit["tarpries"]=3;p.weight["tarmana"]=5;p.limit["tarmana"]=s.divineLimit;
        p.weight["tardrag"]=25;p.limit["tardrag"]=1;p.weight["targob"]=10;p.limit["targob"]=40;
        if (s.troops) { p.weight["tarang"]=10;p.limit["tarang"]=40; }
        ai::Controller controller(0,reg,p,1,s.difficulty);
        const auto cs=seen(controller,w,s.tick);
        struct Out { bool upgrade=false,freeDivine=false,dragon=false,trains=false,troop=false; } out;
        for (const auto& c:cs) {
            const std::string t=c.type;
            if (c.kind==net::Cmd::Build && t=="tarmana") {
                if (c.unitId==priest && c.x==1400 && c.z==800) out.upgrade=true;
                if (c.x==2700) out.freeDivine=true;
            }
            if (c.kind==net::Cmd::Build && t=="tardrag") out.dragon=true;
            if (c.kind==net::Cmd::Build && t=="tarang" && c.unitId==priest) out.troop=true;
            if (c.kind==net::Cmd::Train && t=="tarpries") out.trains=true;
        }
        return out;
    };
    {
        Setup s;s.difficulty=ai::Difficulty::Hard;s.tick=0;s.mana=0;s.freeDeposit=true;
        check(!run(s).freeDivine,"Hard never starts a Divine Lodestone on a free deposit");
    }
    check(run(Setup{}).upgrade,"Normal upgrades its own lodestone in a saturated base after 10 minutes, with the reserve saved");
    {
        // The rest would each have upgraded under the old ladder: an income under
        // 20 made economy the planner's first want.
        Setup s;s.income=19;s.tick=9*60*30;
        check(!run(s).upgrade,"Normal does not upgrade before 10 minutes");
        s=Setup{};s.income=19;s.enemy=true;
        check(!run(s).upgrade,"no upgrade of a lodestone with an armed enemy within 600 px");
        s=Setup{};s.income=19;s.building=true;
        check(!run(s).upgrade,"one upgrade at a time: none while a Divine Lodestone is unfinished");
        s=Setup{};s.income=19;s.difficulty=ai::Difficulty::Easy;
        check(!run(s).upgrade,"Easy never upgrades");
        s=Setup{};s.mana=3000;
        check(!run(s).upgrade,"no upgrade until the store holds 60% of its cost");
        // No reserve held for an upgrade weightedPick could never buy: the
        // priest, with no plan, conjures a troop from that store instead.
        s=Setup{};s.troops=true;s.mana=3000;s.divines=1;s.divineLimit=1;
        check(run(s).troop,"no reserve for a Divine Lodestone past the profile's limit");
        s=Setup{};s.troops=true;s.mana=3600;s.storage=4000;s.income=19;
        check(run(s).troop,"no reserve a full store plus income over the build cannot turn into the upgrade");
    }
    {
        Setup s;s.priest=false;s.income=30;s.mana=3500;
        check(run(s).trains,"with no priest, a factory trains one for an upgrade that pays");
        s.multOne=true;
        check(!run(s).trains,"no priest for a x1 deposit: priest and upgrade do not pay back in 15 minutes");
    }
    {
        Setup s;s.difficulty=ai::Difficulty::Hard;s.tick=0;s.mana=0;s.army=2;
        check(!run(s).dragon,"a priest starts no Dragon at an empty store, its early army boost gone");
        s.mana=9000;s.army=12;
        check(run(s).dragon,"a priest still starts a Dragon on a store floating half its cost");
    }
    // The levels' limits on Divine Lodestones standing: Defensive 5, Normal 2,
    // Hard 6, Absurd and Absurdly Defensive none; Easy builds none.
    struct Limit { ai::Difficulty difficulty; int limit; const char* label; };
    for (const auto& [difficulty,limit,label] : {
            Limit{ai::Difficulty::Defensive,5,"Defensive stops at five Divine Lodestones"},
            Limit{ai::Difficulty::Normal,2,"Normal stops at two Divine Lodestones"},
            Limit{ai::Difficulty::Hard,6,"Hard stops at six Divine Lodestones"},
            Limit{ai::Difficulty::Absurd,-1,"Absurd keeps upgrading past eight Divine Lodestones"},
            Limit{ai::Difficulty::AbsurdlyDefensive,-1,"Absurdly Defensive keeps upgrading past eight Divine Lodestones"},
            Limit{ai::Difficulty::Easy,0,"Easy builds no Divine Lodestone"}}) {
        Setup s;s.difficulty=difficulty;s.tick=20*60*30;
        bool ok=true;
        for (int have : {0,1,2,4,5,6,8}) {
            s.divines=have;
            ok=ok && run(s).upgrade==(limit<0 || have<limit);
        }
        check(ok,label);
    }
}

// Defensive and Absurdly Defensive keep building in step with their income with no
// AI-imposed ceiling: no cap on factories, builders or towers, no profile limits,
// every idle producer acting each think, the orders of one think held against the
// store. Only game rules -- the unit cap, FBI limits, placement -- bound them.
static void unboundedLevels() {
    auto files=std::make_shared<hpi::Vfs::Files>();
    auto file=[&](const std::string& path,const std::string& text) {
        std::string lines;
        for (char ch : text) { lines += ch; if (ch == ';' || ch == '{' || ch == '}') lines += '\n'; }
        (*files)[path]={lines.begin(),lines.end()};
    };
    auto type=[&](const char* id,const char* fields) {
        file(std::string("units/")+id+".fbi",std::string("[UNITINFO]{unitname=")+id+
            ";objectname="+id+";maxdamage=100;footprintx=2;footprintz=2;buildtime=30;buildcost=10;"+fields+"}");
    };
    type("soldier","canmove=1;maxvelocity=1;");
    type("factory","builder=1;workertime=30;");
    type("lode","mogriumincome=20;");
    type("tower","");
    type("worker","builder=1;workertime=30;canmove=1;maxvelocity=1;");
    file("canbuild/factory/soldier.tdf","[CANBUILD]{sortbias=0;}");
    for (const char* child : {"factory","lode","tower"})
        file(std::string("canbuild/worker/")+child+".tdf","[CANBUILD]{sortbias=0;}");
    hpi::Vfs vfs;vfs.setMapFiles(files);
    sim::TypeRegistry reg;reg.loadDir(vfs,"units");reg.loadBuildTree(vfs,"canbuild");
    const auto levels={ai::Difficulty::Defensive,ai::Difficulty::AbsurdlyDefensive,ai::Difficulty::Hard};
    auto count=[](const std::vector<net::Command>& cs,net::Cmd kind,const char* type) {
        return int(std::count_if(cs.begin(),cs.end(),[&](const auto& c){return c.kind==kind && std::string(c.type)==type;}));
    };
    for (auto difficulty : levels) {
        const bool unbounded=ai::defensiveLevel(difficulty);
        // Factories: Defensive used to stop at 32 (as Hard still does) whatever its income.
        {
            sim::World w;terrain(w);w.buildNavClasses(reg);
            w.player(0).mana=100000;w.player(0).income=1000;
            for (int i=0;i<32;++i) w.spawn(reg.find("factory"),2400+float(i%8)*120,2400+float(i/8)*120,0,0);
            w.spawn(reg.find("worker"),2820,3100,0,0);
            ai::Profile p;p.weight["factory"]=10;p.weight["soldier"]=10;
            ai::Controller controller(0,reg,p,1,difficulty);
            check((count(think(controller,w),net::Cmd::Build,"factory")>0)==unbounded,
                  "Defensive adds factories past 32 while its income asks for more; Hard keeps its ceiling");
        }
        // Producers: every idle factory acts in one think (Defensive used to move three)...
        {
            sim::World w;terrain(w);w.buildNavClasses(reg);
            w.player(0).mana=10000;w.player(0).income=100;
            for (int i=0;i<12;++i) w.spawn(reg.find("factory"),400+float(i%4)*200,400+float(i/4)*200,0,0);
            ai::Profile p;p.weight["soldier"]=10;
            ai::Controller controller(0,reg,p,1,difficulty);
            const int trains=count(think(controller,w),net::Cmd::Train,"soldier");
            check(unbounded ? trains==12 : trains==8,"Defensive moves every idle producer each think; Hard eight");
            // ...but one think's orders are held against the store: 25 mana and no
            // income start two 10-mana soldiers, not twelve.
            w.player(0).mana=25;w.player(0).income=0;
            ai::Controller broke(0,reg,p,1,difficulty);
            const int poor=count(think(broke,w),net::Cmd::Train,"soldier");
            check(!unbounded || poor==2,"Defensive's orders in one think stay within its mana");
        }
        // Profile limits: a second lodestone past `limit lode 1`, a tower past `limit tower 1`.
        {
            sim::World w;terrain(w);w.buildNavClasses(reg);
            w.setManaSpots({{1200,800},{1200,1100}});
            w.spawn(reg.find("factory"),800,800,0,0);
            w.spawn(reg.find("lode"),1200,800,0,0);
            const int worker=w.spawn(reg.find("worker"),1000,1000,0,0);
            w.player(0).mana=5000;w.player(0).income=10;
            ai::Profile p;p.weight["lode"]=10;p.limit["lode"]=1;
            ai::Controller controller(0,reg,p,1,difficulty);
            const auto cs=think(controller,w);
            check(std::any_of(cs.begin(),cs.end(),[&](const auto& c) {
                return c.unitId==worker && c.kind==net::Cmd::Build && std::string(c.type)=="lode";
            })==unbounded || difficulty==ai::Difficulty::Hard,"Defensive builds income past the profile's lodestone limit");
        }
        // Towers: Defensive used to stop at 48, Hard stops at six.
        {
            sim::World w;terrain(w);w.buildNavClasses(reg);
            for (int i=0;i<48;++i) w.spawn(reg.find("tower"),2400+float(i%8)*64,2400+float(i/8)*64,0,0);
            w.spawn(reg.find("worker"),2624,3000,0,0);
            w.player(0).mana=100000;w.player(0).income=2000;
            ai::Profile q;q.weight["tower"]=10;q.limit["tower"]=1;
            ai::Controller towers(0,reg,q,1,difficulty);
            check((count(think(towers,w),net::Cmd::Build,"tower")>0)==unbounded,
                  "Defensive builds towers past 48 and the profile's limit as income grows");
        }
    }
    // Idle fighters walk out of the base to a ring around it and stay home there;
    // a fighter already outside the base is left alone.
    for (auto difficulty : {ai::Difficulty::Defensive,ai::Difficulty::AbsurdlyDefensive}) {
        sim::World w;terrain(w);w.buildNavClasses(reg);
        w.spawn(reg.find("factory"),1600,1600,0,0);
        const int inside=w.spawn(reg.find("soldier"),1640,1680,0,0);
        const int outside=w.spawn(reg.find("soldier"),1600,2400,0,0);
        ai::Profile none;
        ai::Controller controller(0,reg,none,1,difficulty,{{3400,3400}});
        const auto cs=think(controller,w);
        check(attacks(cs)==0 && std::any_of(cs.begin(),cs.end(),[&](const auto& c) {
            const float dx=c.x-1600,dz=c.z-1600;
            return c.unitId==inside && c.kind==net::Cmd::Move && dx*dx+dz*dz>420.f*420 && dx*dx+dz*dz<1200.f*1200;
        }) && std::none_of(cs.begin(),cs.end(),[&](const auto& c){return c.unitId==outside && c.kind==net::Cmd::Move;}),
              "Defensive walks idle troops out of its base to the ring around it, and no further");
    }
    // A fighter is not sent onto a ring spot another stands on, nor onto the enemy's
    // side of the map.
    for (auto difficulty : {ai::Difficulty::Defensive,ai::Difficulty::AbsurdlyDefensive}) {
        sim::World w;terrain(w);w.buildNavClasses(reg);
        w.spawn(reg.find("factory"),1600,1600,0,0);
        const int inside=w.spawn(reg.find("soldier"),1640,1680,0,0);
        // The first spot the inside soldier tries -- the inner ring along its own
        // bearing -- has a fighter standing 34 px off it: clear for placement (28 px),
        // but inside the 40 px a stationed fighter keeps.
        const float a=std::atan2(80.0f,40.0f),sx=1600+468*std::cos(a)+34,sz=1600+468*std::sin(a);
        w.spawn(reg.find("soldier"),sx,sz,0,0);
        ai::Profile none;
        ai::Controller controller(0,reg,none,1,difficulty,{{3400,3400}});
        const auto cs=think(controller,w);
        check(std::any_of(cs.begin(),cs.end(),[&](const auto& c) {
            return c.unitId==inside && c.kind==net::Cmd::Move && (c.x-sx)*(c.x-sx)+(c.z-sz)*(c.z-sz)>=40.f*40;
        }),"Defensive does not station a fighter on a spot another fighter holds");
        sim::World v;terrain(v);v.buildNavClasses(reg);
        v.spawn(reg.find("factory"),1600,1600,0,0);
        const int toward=v.spawn(reg.find("soldier"),1640,1680,0,0);
        ai::Controller near(0,reg,none,1,difficulty,{{2000,2200}});
        const auto vs=think(near,v);
        check(std::any_of(vs.begin(),vs.end(),[&](const auto& c) {
            const float hx=c.x-1600,hz=c.z-1600,ex=c.x-2000,ez=c.z-2200;
            return c.unitId==toward && c.kind==net::Cmd::Move && hx*hx+hz*hz<=ex*ex+ez*ez;
        }),"Defensive stations its troops on its own side of the map");
    }
    // The base grows with its buildings: a filled 600 px disc does not stop the
    // builders (they used to idle beside it with a full store). A site past it
    // lies within retail's base radius, on the AI's own side of the map, with a
    // lane to every building.
    for (auto difficulty : {ai::Difficulty::Defensive,ai::Difficulty::AbsurdlyDefensive}) {
        sim::World w;terrain(w);w.buildNavClasses(reg);
        // 81 towers 140 apart around (800,800): no 2x2 site with a 48 px lane fits
        // within 640 px of home; their footprint (81 * 4) makes a 1440 px base.
        std::vector<std::pair<float,float>> towers;
        for (int i=-4;i<=4;++i) for (int j=-4;j<=4;++j) {
            towers.emplace_back(800+float(i)*140,800+float(j)*140);
            w.spawn(reg.find("tower"),towers.back().first,towers.back().second,0,0);
        }
        const int worker=w.spawn(reg.find("worker"),800,1500,0,0);
        for (const auto& enemy : std::vector<std::pair<float,float>>{{3800,3800},{2400,800}}) {
            ai::Controller controller(0,reg,{},1,difficulty,{enemy});
            float x=0,z=0;
            const bool found=ai::PlacementProbe::place(controller,w,reg.find("tower"),*w.unit(worker),x,z);
            const float d2=(x-800)*(x-800)+(z-800)*(z-800);
            const float e2=(x-enemy.first)*(x-enemy.first)+(z-enemy.second)*(z-enemy.second);
            check(found && d2>600.f*600 && d2<=1440.f*1440 && d2<=e2 &&
                  std::none_of(towers.begin(),towers.end(),[&](const auto& t) {
                      return std::abs(t.first-x)<32+48 && std::abs(t.second-z)<32+48;
                  }),"Defensive builds past a full 600 px disc, within its grown base, on its own side, with lanes");
        }
    }
    // Two builders moved in one think take two sites, a lane apart; and a site whose
    // order came to nothing (the builder idle again, nothing started there) is not
    // picked again at once.
    for (auto difficulty : {ai::Difficulty::Defensive,ai::Difficulty::AbsurdlyDefensive}) {
        sim::World w;terrain(w);w.buildNavClasses(reg);
        w.spawn(reg.find("factory"),1600,1600,0,0);
        w.spawn(reg.find("worker"),1500,1800,0,0);
        w.spawn(reg.find("worker"),1500,1800,0,0);
        w.player(0).mana=100000;w.player(0).income=2000;
        ai::Profile p;p.weight["tower"]=10;
        ai::Controller controller(0,reg,p,1,difficulty,{{3400,3400}});
        auto sites=[&](uint32_t tick) {
            std::vector<std::pair<float,float>> out;
            for (const auto& c:think(controller,w,tick))
                if (c.kind==net::Cmd::Build && std::string(c.type)=="tower") out.emplace_back(c.x,c.z);
            return out;
        };
        const auto first=sites(0);
        check(first.size()==2 && (std::abs(first[0].first-first[1].first)>=80 ||
                                  std::abs(first[0].second-first[1].second)>=80),
              "Defensive's builders in one think take separate sites");
        const auto again=sites(30);   // neither order reached the world
        check(!first.empty() && !again.empty() && std::none_of(again.begin(),again.end(),[&](const auto& s) {
            return std::any_of(first.begin(),first.end(),[&](const auto& f) {
                return std::abs(f.first-s.first)<32 && std::abs(f.second-s.second)<32; });
        }),"Defensive does not send a builder straight back to a site that came to nothing");
    }
    // A factory waiting half a minute on a queue head it cannot place (its pad
    // blocked) gives that order up and trains something else for a while.
    {
        sim::World w;terrain(w);w.buildNavClasses(reg);
        w.player(0).mana=0;w.player(0).income=0;
        const int factory=w.spawn(reg.find("factory"),800,800,0,0);
        w.unit(factory)->buildQueue.push_back(reg.find("soldier"));
        ai::Profile p;p.weight["soldier"]=10;
        ai::Controller controller(0,reg,p,1,ai::Difficulty::Defensive);
        auto unqueues=[&](uint32_t tick) {
            const auto cs=think(controller,w,tick);
            return std::any_of(cs.begin(),cs.end(),[&](const auto& c) {
                return c.kind==net::Cmd::Unqueue && c.unitId==factory && std::string(c.type)=="soldier";
            });
        };
        check(!unqueues(0) && !unqueues(29*30),"a queue head waits half a minute for its pad");
        check(unqueues(30*30),"a queue head that never found a site is given up after half a minute");
        w.unit(factory)->buildQueue.clear();w.player(0).mana=1000;
        const auto retry=think(controller,w,31*30);
        check(std::none_of(retry.begin(),retry.end(),[](const auto& c){return c.kind==net::Cmd::Train;}),
              "the factory does not retry the refused type at once");
    }
}

// Absurdly Defensive (wire level 5) is Defensive with Absurd's double income, end
// to end: the level, the multiplier and stance every peer derives from it, and
// the room and replay records that carry it.
static void levelWiring() {
    check(ai::difficultyFromLevel(5)==ai::Difficulty::AbsurdlyDefensive && ai::difficultyFromLevel(8)==ai::Difficulty::Normal &&
          ai::difficultyFromLevel(255)==ai::Difficulty::Normal,
          "aiLevel 5 is Absurdly Defensive; an unknown level (8 and up) is Normal");
    check(ai::incomeMultFor(ai::Difficulty::AbsurdlyDefensive)==2.0f && ai::incomeMultFor(ai::Difficulty::Absurd)==2.0f &&
          ai::incomeMultFor(ai::Difficulty::Defensive)==1.0f,"Absurdly Defensive earns Absurd's double income");
    const auto a=ai::paramsFor(ai::Difficulty::AbsurdlyDefensive),d=ai::paramsFor(ai::Difficulty::Defensive);
    check(!a.attack && a.unbounded && a.thinkPeriod==d.thinkPeriod && a.producersPerThink==d.producersPerThink &&
          a.limitScale==d.limitScale && a.raidSize==d.raidSize && a.waveSize==d.waveSize,
          "Absurdly Defensive plays exactly as Defensive");
    net::SlotInfo slots[net::kMaxSlots]{};
    slots[0].type=1;slots[0].aiLevel=2;
    slots[1].type=2;slots[1].aiLevel=5;slots[1].faction=3;slots[1].team=1;
    slots[2].type=2;slots[2].aiLevel=0;slots[2].team=2;
    slots[3].type=2;slots[3].aiLevel=4;slots[3].team=3;
    {
        net::Writer wr;net::put(wr,slots[1]);net::Reader rd(wr.b.data(),wr.b.size());net::SlotInfo back;
        net::get(rd,back);
        check(rd.ok && back.aiLevel==5,"the room's slot record carries level 5");
    }
    auto expect=[](const sim::MatchConfig& cfg) {
        return cfg.slots.size()==4 && cfg.slots[0].manaMult==1 && !cfg.slots[0].defensiveAi &&
            cfg.slots[1].manaMult==2 && cfg.slots[1].defensiveAi &&
            cfg.slots[2].manaMult==1 && cfg.slots[2].defensiveAi &&
            cfg.slots[3].manaMult==2 && !cfg.slots[3].defensiveAi;
    };
    net::GameOptions opts;
    check(expect(net::matchConfigFor(opts,slots,7)),"the match config gives level 5 double income and the defensive stance");
    // Through a replay header and back, as the replay loader rebuilds the config.
    net::Writer wr;net::writeReplayHeader(wr,net::replayHeaderFor(opts,"Inner Circle","",slots,7));
    net::Reader rd(wr.b.data()+4,wr.b.size()-4);   // past the TAKR magic, as the loader reads
    net::ReplayHeader h;uint32_t fmt=0,proto=0;
    const bool read=net::readReplayHeader(rd,h,fmt,proto);
    net::GameOptions o2;net::SlotInfo back[net::kMaxSlots];net::matchInputsFrom(h,o2,back);
    check(read && proto==net::kNetVersion && back[1].aiLevel==5 && expect(net::matchConfigFor(o2,back,7)),
          "a replay header keeps level 5 and replays it with double income");
    check(!net::supportedReplayProtocol(fmt,272),"a replay of another protocol is refused");
}

// Classic (wire level 6): the retail AI's port. Its seat's sim config
// is exactly a Normal seat's, the level survives the room and replay records, and
// the P0 brain takes every tick from the Controller and does nothing with it.
static void classicLevel() {
    check(ai::kAiLevels==8 && ai::difficultyFromLevel(6)==ai::Difficulty::Classic,"aiLevel 6 is Classic");
    check(ai::difficultyFromLevel(7)==ai::Difficulty::Passive && ai::incomeMultFor(ai::Difficulty::Passive)==1.0f &&
          !ai::defensiveLevel(ai::Difficulty::Passive),"aiLevel 7 is Passive, with an Easy seat's sim config");
    {
        const auto easy=ai::paramsFor(ai::Difficulty::Easy),passive=ai::paramsFor(ai::Difficulty::Passive);
        check(easy.thinkPeriod==passive.thinkPeriod && easy.waveSize==passive.waveSize && easy.limitScale==passive.limitScale &&
              easy.attack==passive.attack && easy.raidSize==passive.raidSize && easy.unbounded==passive.unbounded &&
              ai::exploreLevelFor(ai::Difficulty::Passive).maxScouts==0,"Passive's knobs are Easy's, with no scouts");
    }
    check(ai::incomeMultFor(ai::Difficulty::Classic)==1.0f && !ai::defensiveLevel(ai::Difficulty::Classic),
          "Classic has no income cheat and no defensive stance");
    net::SlotInfo slots[net::kMaxSlots]{};
    slots[0].type=1;slots[0].aiLevel=2;
    slots[1].type=2;slots[1].aiLevel=6;slots[1].faction=2;slots[1].team=1;
    slots[2].type=2;slots[2].aiLevel=2;slots[2].faction=2;slots[2].team=2;
    {
        net::Writer wr;net::put(wr,slots[1]);net::Reader rd(wr.b.data(),wr.b.size());net::SlotInfo back;
        net::get(rd,back);
        check(rd.ok && back.aiLevel==6,"the room's slot record carries level 6");
    }
    auto sameAsNormal=[](const sim::MatchConfig& cfg) {
        const auto& c=cfg.slots[1];const auto& n=cfg.slots[2];
        return cfg.slots.size()==3 && c.used && c.manaMult==n.manaMult && c.manaMult==1 &&
            c.automaticGates==n.automaticGates && c.automaticGates && c.defensiveAi==n.defensiveAi && !c.defensiveAi;
    };
    net::GameOptions opts;
    check(sameAsNormal(net::matchConfigFor(opts,slots,7)),"a Classic seat gets exactly a Normal seat's match config");
    net::Writer wr;net::writeReplayHeader(wr,net::replayHeaderFor(opts,"Inner Circle","",slots,7));
    net::Reader rd(wr.b.data()+4,wr.b.size()-4);
    net::ReplayHeader h;uint32_t fmt=0,proto=0;
    const bool read=net::readReplayHeader(rd,h,fmt,proto);
    net::GameOptions o2;net::SlotInfo back[net::kMaxSlots];net::matchInputsFrom(h,o2,back);
    check(read && proto==net::kNetVersion && back[1].aiLevel==6 && sameAsNormal(net::matchConfigFor(o2,back,7)),
          "a replay header keeps level 6");
    check(net::kNetVersion>=290 && !net::supportedReplayProtocol(fmt,289),"a replay of another protocol is refused");
    {
        // The brain's constructor already draws (the AI manager's squad rolls,
        // then the build manager's block grids).
        uint32_t mirror=ai::ClassicBrain::seedFor(1,7);
        ai::ClassicSquads squads;
        ai::classicConstructSquads(squads,0,[&](int n){return sim::retailRandom(mirror,n);});
        ai::classicDrawBlockGrids([&](int n){return sim::retailRandom(mirror,n);});
        check(ai::ClassicBrain::seedFor(1,7)!=ai::ClassicBrain::seedFor(2,7) &&
              mirror==ai::ClassicBrain(1,7).seed() && (ai::ClassicBrain::seedFor(1,7)&1),
              "each Classic seat of a game gets its own Park-Miller stream");
    }
    {
        ai::ClassicBrain a(1,7),b(1,7);bool same=true;
        for (int i=0;i<64;++i) same=same && a.rand(1000)==b.rand(1000);
        const auto before=a.seed();
        check(same && a.rand(1)==0 && a.rand(0)==0 && a.seed()==before,"the stream repeats per seed, and n < 2 draws nothing");
    }
    // The Normal Controller attacks with this army at its first think (main's loop);
    // the Classic one is handed every tick and gives each unit retail's standing
    // orders (Stance 0) as it joins a base squad, then (P5) sends the idle ones
    // on patrol around the base (Move, then a queued Patrol), and (P6) its waves
    // only Move, Patrol and Stop (retail's mission clear): never an attack. It
    // also tells the World its strike and VTOL squads (AiGroup, no order).
    auto soldier=fighter();sim::TypeRegistry empty;ai::Profile profile;
    sim::World w;terrain(w);w.player(0).income=100;
    for (int i=0;i<35;++i) w.spawn(&soldier,400+float(i%7)*40,800+float(i/7)*40,0,0);
    ai::Controller classic(0,empty,profile,1,ai::Difficulty::Classic,{{3400,800}});
    size_t stances=0,patrols=0,other=0;
    for (uint32_t tick=0;tick<300;++tick)
        for (const auto& c:think(classic,w,tick)) {
            if (c.kind==net::Cmd::Stance && c.targetId==0) ++stances;
            else if (c.kind==net::Cmd::Patrol && c.queue) ++patrols;
            else if (c.kind!=net::Cmd::Move && c.kind!=net::Cmd::Patrol && c.kind!=net::Cmd::Stop &&
                     c.kind!=net::Cmd::AiGroup) ++other;
        }
    sight(w);
    ai::Controller normal(0,empty,profile,1,ai::Difficulty::Normal,{{3400,800}});
    check(stances==35 && patrols>=1 && other==0 && attacks(think(normal,w))==35,
          "the Classic brain gives each unit its standing orders, idle patrols and wave moves where Normal attacks");
}

// OUR LEVELS RAISE THE DEAD BY ORDER. Retail raises only by order (0x4de530), so
// the sim never raises a body by itself, and these levels give the order
// (Controller::reviveIdle): an idle caster beside a body it can raise -- any
// side's -- is sent to raise the nearest. One that is busy, a body past the 56 px reach, or an allocation
// under 0.7 (a computer player's revive would end unraised) gets none.
static void aiRaisesTheDead(const sim::TypeRegistry& registry,const ai::Profile& profile,
                            const sim::UnitType& soldier) {
    sim::FeatType body;body.name="victim_dead";body.resurrectable=true;body.decomposeTicks=100000;
    auto priest=soldier;priest.id="priest";priest.weapon.damage=0;priest.weapons.clear();
    priest.canResurrect=true;priest.workerTime=10;
    auto victim=soldier;victim.id="victim";
    struct Scene { std::unique_ptr<sim::World> w; int caster,dead; };
    auto scene=[&](float bodyX,int owner) {
        Scene s{std::make_unique<sim::World>(),0,0};
        sim::World& w=*s.w;terrain(w);w.setFeatureTypes({body});w.mapCorpse(&victim,0);
        s.caster=w.spawn(&priest,400,800,0,0);
        s.dead=w.spawn(&victim,bodyX,800,0,owner);
        auto& d=*w.unit(s.dead);d.hp={};d.deadFor=200;d.corpseUntil=100000;   // a settled body
        pastStart(w);
        return s;
    };
    auto raises=[](const std::vector<net::Command>& cs,int caster,int dead) {
        return std::count_if(cs.begin(),cs.end(),[&](const auto& c) {
            return c.kind==net::Cmd::Resurrect && c.unitId==caster && c.targetId==dead;
        })==1;
    };
    for (auto difficulty:{ai::Difficulty::Defensive,ai::Difficulty::Normal}) {
        for (int owner:{0,1}) {
            auto s=scene(440,owner);
            ai::Controller c(0,registry,profile,1,difficulty,{{3400,800}});
            const auto cs=seen(c,*s.w);
            check(raises(cs,s.caster,s.dead),owner ? "an idle caster of our levels is ordered to raise an enemy body beside it"
                                                   : "an idle caster of our levels is ordered to raise its own body beside it");
            for (const auto& cmd:cs) sim::applyCommand(*s.w,registry,cmd);
            const auto& o=s.w->unit(s.caster)->orders;
            check(!o.empty() && o.back().reviveKind==1 && o.back().reclaimFeat==-s.dead,
                  "...and the order is a RESURRECT leg on that body");
            check(!raises(seen(c,*s.w),s.caster,s.dead),
                  "...given once, not again while it runs");
        }
    }
    {
        auto s=scene(400+60,1);   // past the old idle search's 56 px
        ai::Controller c(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800}});
        check(!raises(seen(c,*s.w),s.caster,s.dead),"a body beyond 56 px is left alone");
    }
    {
        auto s=scene(440,1);
        s.w->player(0).constructionAllocation=0.5;
        ai::Controller c(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800}});
        check(!raises(seen(c,*s.w),s.caster,s.dead),"no revive is ordered under 0.7 allocation (it would end unraised)");
    }
}

// The straightforward reviveIdle search, kept as the reference: every eligible
// caster scans every unit record for the nearest raisable body within 56 px.
// reviveIdle must give the same commands, in the same order, and leave the same
// assigned list.
static int referenceTies=0,referenceCapped=0,referenceClaimed=0;
static void referenceReviveIdle(int player,const sim::World& world,std::vector<int>& assigned,
                                const ai::CommandSink& sink) {
    if (double(world.player(player).allocation()) < 0.7 || world.atUnitCap(player)) return;
    constexpr float kReach = 56.0f;
    std::vector<int> claimed;
    for (const auto& u : world.units()) {
        if (!u.alive() || u.player != player || !u.type || u.underConstruction || u.embarked() ||
            u.incapacitated() || u.buildSiteId || !ai::PlacementProbe::idle(u) ||
            !(u.type->canResurrect || (u.type->canAnimate && u.type->animateType)) ||
            std::find(assigned.begin(), assigned.end(), u.id) != assigned.end()) continue;
        int best = 0;
        float bestD = kReach * kReach;
        for (const auto& c : world.units()) {
            if (c.alive() || !c.type || c.corpseFading || c.deadFor < c.corpseAnimationTicks() ||
                c.deadFor >= c.corpseUntil) continue;
            const float dx = c.x.toFloat() - u.x.toFloat(), dz = c.z.toFloat() - u.z.toFloat();
            const float d = dx * dx + dz * dz;
            if (d == bestD && best && world.reviveKind(u.id, c.id)) ++referenceTies;
            if (d > bestD || (d == bestD && best)) continue;
            const uint8_t kind = world.reviveKind(u.id, c.id);
            if (kind && world.atTypeCap(player, kind == 1 ? c.type : u.type->animateType)) ++referenceCapped;
            else if (kind && std::find(claimed.begin(), claimed.end(), c.id) != claimed.end()) ++referenceClaimed;
            if (!kind || world.atTypeCap(player, kind == 1 ? c.type : u.type->animateType) ||
                std::find(claimed.begin(), claimed.end(), c.id) != claimed.end()) continue;
            best = c.id; bestD = d;
        }
        if (!best) continue;
        net::Command c; c.kind = net::Cmd::Resurrect; c.player = uint8_t(player);
        c.unitId = u.id; c.targetId = best;
        sink(c);
        claimed.push_back(best);
        assigned.push_back(u.id);
    }
}

// reviveIdle against the reference over many crowded random scenes: competing
// casters (one caster per body), bodies at equal distances (the earlier record
// wins), bodies still dying, fading or past their time, unraisable bodies,
// type limits (totalallowed on the body's type and on an animatetype), busy,
// frozen, enemy and already-assigned casters, low allocation and the unit cap.
// Positions on an 8 px lattice in a 200 px square make ties common.
static void aiRaisesTheDeadLikeTheReference(const sim::TypeRegistry& registry,const ai::Profile& profile,
                                            const sim::UnitType& soldier) {
    sim::FeatType both;both.name="both_dead";both.resurrectable=true;both.animatable=true;
    sim::FeatType resOnly=both;resOnly.name="res_dead";resOnly.animatable=false;
    sim::FeatType none=both;none.name="none_dead";none.resurrectable=none.animatable=false;
    auto unarmed=soldier;unarmed.weapon.damage=0;unarmed.weapons.clear();
    auto victimA=unarmed;victimA.id="victimA";
    auto victimB=unarmed;victimB.id="victimB";
    auto victimC=unarmed;victimC.id="victimC";
    auto capped=unarmed;capped.id="capped";capped.totalAllowed=2;
    auto skel=unarmed;skel.id="skel";skel.totalAllowed=3;
    auto priest=unarmed;priest.id="priest";priest.canResurrect=true;priest.workerTime=10;
    auto necro=unarmed;necro.id="necro";necro.canAnimate=true;necro.animateType=&skel;
    auto hybrid=priest;hybrid.id="hybrid";hybrid.canAnimate=true;hybrid.animateType=&skel;
    const sim::UnitType* bodies[]={&victimA,&victimA,&victimB,&victimC,&capped,&skel,&priest};
    const sim::UnitType* casters[]={&priest,&priest,&necro,&hybrid};
    uint32_t seed=12345;
    auto next=[&](uint32_t n) {seed=seed*1103515245u+12345u;return (seed>>16)%n;};
    int scenes=0,mismatches=0,commands=0,emptyScenes=0,applied=0,appliedMismatches=0;
    referenceTies=referenceCapped=referenceClaimed=0;
    // One scene, built from the generator's current state; built again from a
    // saved state it is the same scene.
    auto build=[&](int scene,sim::World& w,std::vector<int>& assigned) {
        w.setFeatureTypes({both,resOnly,none});
        w.mapCorpse(&victimA,0);w.mapCorpse(&victimB,1);w.mapCorpse(&victimC,2);
        w.mapCorpse(&capped,0);w.mapCorpse(&skel,0);w.mapCorpse(&priest,1);
        const int population=int(next(90))+1;
        const bool noBodies=scene%10==0;   // many casters, nothing to raise
        std::vector<int> casterIds;
        for (int i=0;i<population;++i) {
            const float x=400+float(next(26))*8+(next(8)==0 ? 0.5f : 0.0f);
            const float z=400+float(next(26))*8+(next(8)==0 ? 0.25f : 0.0f);
            const int role=int(next(10));
            if (role<4) {
                const int id=w.spawn(casters[next(4)],x,z,0,next(5)==0 ? 1 : 0);
                auto& u=*w.unit(id);
                switch (next(12)) {
                    case 0: {sim::Order o;o.goal=true;o.x=u.x;o.z=u.z;u.orders.push_back(o);break;}   // busy
                    case 1: u.frozenFor=30;break;
                    default: break;
                }
                casterIds.push_back(id);
            } else if (role<8 && !noBodies) {
                const int id=w.spawn(bodies[next(7)],x,z,0,int(next(2)));
                auto& d=*w.unit(id);d.hp={};d.deadFor=200;d.corpseUntil=100000;
                switch (next(10)) {
                    case 0: d.deathAnimationTicks=400;break;      // still dying
                    case 1: d.corpseFading=true;break;
                    case 2: d.corpseUntil=200;break;              // its time is up
                    default: break;
                }
            } else {
                // Live units: some of the capped types count toward their limits.
                const sim::UnitType* t=next(3)==0 ? &capped : next(2) ? &skel : &victimA;
                w.spawn(t,x,z,0,0);
            }
        }
        if (scene%37==5) w.player(0).constructionAllocation=0.5;
        if (scene%41==7) w.setUnitCap(1);
        pastStart(w);
        for (int id:casterIds) if (next(6)==0) assigned.push_back(id);
    };
    auto sameStream=[](const std::vector<net::Command>& a,const std::vector<net::Command>& b) {
        bool same=a.size()==b.size();
        for (size_t i=0;same && i<a.size();++i)
            same=a[i].kind==b[i].kind && a[i].player==b[i].player && a[i].unitId==b[i].unitId &&
                 a[i].targetId==b[i].targetId && a[i].queue==b[i].queue;
        return same;
    };
    for (int scene=0;scene<400;++scene) {
        const uint32_t sceneSeed=seed;
        sim::World w;terrain(w);
        std::vector<int> assigned;
        build(scene,w,assigned);
        std::vector<int> refAssigned=assigned;
        std::vector<net::Command> want,got;
        referenceReviveIdle(0,w,refAssigned,[&](const net::Command& cmd){want.push_back(cmd);});
        ai::Controller c(0,registry,profile,1,ai::Difficulty::Normal,{{3400,800}});
        ai::PlacementProbe::revive(c,w,assigned,[&](const net::Command& cmd){got.push_back(cmd);});
        mismatches+=!(sameStream(want,got) && assigned==refAssigned);
        ++scenes;commands+=int(want.size());emptyScenes+=want.empty();
        // A sink that applies each command at once (simperf's): the bodies and
        // the casters still to come must see the same world as the reference
        // did, so the commands and the resulting state agree too.
        if (scene%4==1) {
            sim::World a,b;terrain(a);terrain(b);
            std::vector<int> assignedA,assignedB;
            std::vector<net::Command> wantA,gotB;
            seed=sceneSeed;build(scene,a,assignedA);
            seed=sceneSeed;build(scene,b,assignedB);
            sim::TypeRegistry none;
            const int ties=referenceTies,capped=referenceCapped,claimed=referenceClaimed;
            referenceReviveIdle(0,a,assignedA,[&](const net::Command& cmd) {
                wantA.push_back(cmd);sim::applyCommand(a,none,cmd);
            });
            referenceTies=ties;referenceCapped=capped;referenceClaimed=claimed;   // counted once per scene
            ai::PlacementProbe::revive(c,b,assignedB,[&](const net::Command& cmd) {
                gotB.push_back(cmd);sim::applyCommand(b,none,cmd);
            });
            appliedMismatches+=!(sameStream(wantA,gotB) && assignedA==assignedB && a.stateHash()==b.stateHash());
            applied+=int(gotB.size());
        }
    }
    std::printf("revive reference: %d scenes, %d commands, %d scenes with none, %d equal-distance ties, "
                "%d bodies passed over for a type limit, %d for another caster's claim; %d applied at once\n",
                scenes,commands,emptyScenes,referenceTies,referenceCapped,referenceClaimed,applied);
    check(mismatches==0,"reviveIdle gives the reference search's commands, in its order, and the same assigned list");
    check(appliedMismatches==0 && applied>50,
          "...also when each command is applied at once, with the same world state after");
    check(commands>300 && referenceTies>50 && emptyScenes>40 && referenceCapped>50 && referenceClaimed>50,
          "...over scenes with many raises, equal-distance ties, type limits, competing casters and nothing to raise");
}

int main(int argc,char** argv) {
    sim::TypeRegistry empty;ai::Profile profile;auto soldier=fighter();
    for (auto difficulty : {ai::Difficulty::Defensive,ai::Difficulty::Normal,ai::Difficulty::Hard,ai::Difficulty::Absurd,
                            ai::Difficulty::AbsurdlyDefensive}) {
        sim::World w;terrain(w);w.player(0).income=100;
        for (int i=0;i<35;++i) w.spawn(&soldier,400+float(i%7)*40,800+float(i/7)*40,0,0);
        sight(w);
        ai::Controller controller(0,empty,profile,1,difficulty,{{3400,800}});
        auto cs=think(controller,w);
        // Nothing seen: an attacking level's push explores (the enemy start first)
        // and one fighter goes ahead as a scout.
        check(ai::defensiveLevel(difficulty) ? attacks(cs)==0 : attacks(cs)==35 && moves(cs)==0,
              "difficulty permits heavy attacks only for attacking AIs");
        if (ai::defensiveLevel(difficulty)) {
            check(std::count_if(cs.begin(),cs.end(),[](const auto& c){return c.kind==net::Cmd::Stance && c.targetId==1;})==35,
                  "Passive AI orders defensive rather than hold-fire stances");
        }
    }
    builderCaps();
    unboundedLevels();
    mobileProducers();
    builderTotals();
    fullStore();
    divineLodestones();
    lodestoneChoice();
    defensiveResponses(empty,profile,soldier);
    defensiveShooters(empty,profile,soldier);
    defensiveMeleeTargets(empty,profile,soldier);
    monarchFightsBack(empty,profile,soldier);
    strikeTargets(empty,profile,soldier);
    visionTargets(empty,profile,soldier);
    fogKnowledge(empty,profile,soldier);
    scoutingMemory(empty,profile,soldier);
    fogScouting(empty,profile,soldier);
    lodestoneKnowledge();
    aiRaisesTheDead(empty,profile,soldier);
    aiRaisesTheDeadLikeTheReference(empty,profile,soldier);
    {
        sim::World w;terrain(w);w.player(0).income=100;
        for(int i=0;i<35;++i)w.spawn(&soldier,400+float(i%7)*40,800+float(i/7)*40,0,0);
        sight(w);
        ai::Controller controller(0,empty,profile,1,ai::Difficulty::Easy,{{3400,800}});
        auto cs=think(controller,w,3*60*30);
        check(attacks(cs)==0,"Easy leaves four minutes before its first attack or scout");
        cs=think(controller,w,4*60*30);
        check(attacks(cs)==9,"Easy attacks with at most eight units, and a scout goes ahead");
        for (const auto& c:cs) sim::applyCommand(w,empty,c);
        check(attacks(think(controller,w,5*60*30))==0,"Easy waits between attack waves");
        check(attacks(think(controller,w,6*60*30))==8,"Easy resumes attacking after two minutes");
    }
    {
        // Passive goes nowhere for an enemy it has not seen: no scout, and neither
        // the army nor a wave heads for a start position, however large it grows.
        sim::World w;terrain(w);w.player(0).income=100;
        for(int i=0;i<35;++i)w.spawn(&soldier,400+float(i%7)*40,800+float(i/7)*40,0,0);
        sight(w);
        ai::Controller controller(0,empty,profile,1,ai::Difficulty::Passive,{{3400,800}});
        check(controller.difficulty()==ai::Difficulty::Passive,"a Passive controller reports its own level");
        check(attacks(think(controller,w,3*60*30))==0,"Passive leaves four minutes before anything");
        bool quiet=true;
        for (uint32_t minute=4;minute<30;++minute) quiet=quiet && attacks(think(controller,w,minute*60*30))==0;
        check(quiet,"Passive never looks for an enemy it has not seen");
        // An enemy it has seen far from its buildings is left alone.
        auto base=soldier;base.id="base";base.maxVel={};base.sight=300;
        sim::World far;terrain(far);far.player(0).income=100;
        far.spawn(&base,300,800,0,0);
        for(int i=0;i<35;++i)far.spawn(&soldier,400+float(i%7)*40,800+float(i/7)*40,0,0);
        far.spawn(&soldier,1260,800,0,0);   // a sentry there sees it
        far.spawn(&soldier,1300,800,0,1);
        far.setTeam(1,1);sight(far);pastStart(far);
        ai::Controller distant(0,empty,profile,1,ai::Difficulty::Passive,{{3400,800}});
        bool home=true;
        for (uint32_t minute=4;minute<10;++minute) home=home && attacks(think(distant,far,minute*60*30))==0;
        check(home,"Passive never goes out after an enemy it has seen away from its base");
        // An enemy it has seen near its base gets Easy's wave on Easy's timing.
        sim::World seen;terrain(seen);seen.player(0).income=100;
        seen.spawn(&base,300,800,0,0);
        for(int i=0;i<35;++i)seen.spawn(&soldier,400+float(i%7)*40,800+float(i/7)*40,0,0);
        seen.spawn(&soldier,690,800,0,1);
        seen.setTeam(1,1);sight(seen);pastStart(seen);
        ai::Controller known(0,empty,profile,1,ai::Difficulty::Passive,{{3400,800}});
        auto cs=think(known,seen,3*60*30);
        check(attacks(cs)==0,"Passive waits four minutes to attack what it has seen, as Easy does");
        cs=think(known,seen,4*60*30);
        check(attacks(cs)==8,"Passive attacks what it has seen with Easy's wave of eight");
        for (const auto& c:cs) sim::applyCommand(seen,empty,c);
        check(attacks(think(known,seen,5*60*30))==0,"Passive waits between attack waves");
    }
    // An enemy building still going up, far off: every level's strike sends
    // its force there by fight-move, so it fights what it meets on the way,
    // and orders an attack on the site itself only up close. Ordered on it
    // from afar, an Absurd Creon army walked through the Aramon defenders
    // without fighting and died (2026-11-12 replay).
    for (auto level : {ai::Difficulty::Easy,ai::Difficulty::Normal,ai::Difficulty::Hard,ai::Difficulty::Absurd,
                       ai::Difficulty::Defensive,ai::Difficulty::AbsurdlyDefensive}) {
        sim::World w;terrain(w);w.player(0).income=20;
        auto site=soldier;site.id="site";site.maxVel={};
        for (int i=0;i<25;++i) w.spawn(&soldier,400+float(i)*32,800,0,0);
        const int unfinished=w.spawn(&site,3400,800,0,1);w.unit(unfinished)->underConstruction=true;
        // A lookout of its own keeps the site in sight, as the replay's army had it.
        auto eye=site;eye.id="eye";eye.sight=750;eye.weapon.damage=0;eye.weapons.clear();w.spawn(&eye,3000,800,0,0);
        w.setTeam(1,1);sight(w);pastStart(w);
        ai::Controller controller(0,empty,profile,1,level,{{3400,800}});
        bool farOrder=false;int sent=0;
        for (uint32_t minute=0;minute<20 && !sent;++minute) {
            const auto cs=think(controller,w,minute*60*30);
            for (const auto& c:cs) {
                if (c.kind!=net::Cmd::Attack && c.kind!=net::Cmd::AttackMove) continue;
                ++sent;
                const auto* u=w.unit(c.unitId);
                if (c.kind==net::Cmd::Attack && c.targetId==unfinished && u &&
                    std::hypot(u->x.toFloat()-3400.f,u->z.toFloat()-800.f)>1000.f) farOrder=true;
            }
        }
        // The defensive levels attack out only once far ahead: they need not send here.
        const bool defensive=level==ai::Difficulty::Defensive || level==ai::Difficulty::AbsurdlyDefensive;
        check((sent>0 || defensive) && !farOrder,"every level fight-moves at a site being built from afar (attacks it only up close)");
    }
    for (float income : {20.f,200.f}) {
        sim::World w;terrain(w);w.player(0).income=income;
        for (int i=0;i<25;++i) w.spawn(&soldier,400+float(i)*32,800,0,0);
        sight(w);
        ai::Controller controller(0,empty,profile,1,ai::Difficulty::Normal,{{3400,800}});
        const auto cs=think(controller,w);
        check(attacks(cs)==(income==20 ? 25 : 1),"actual income scales the force saved for a heavy attack (one scouts)");
    }
    {
        sim::World w;terrain(w);w.player(0).income=100;
        for (int i=0;i<14;++i) w.spawn(&soldier,400+float(i)*32,800,0,0);
        sight(w);
        ai::Controller controller(0,empty,profile,1,ai::Difficulty::Normal,{{3400,800}});
        // Normal raids three at a time; a lone attack-move beside them is a scout.
        int largest=0,probes=0,scouts=0;
        for (int wave=0;wave<40 && largest<30;++wave) {
            auto cs=think(controller,w,uint32_t(wave*750));int n=attacks(cs);
            largest=std::max(largest,n);if (n>0 && n<30) {probes+=n-n%3;scouts+=n%3;}
            for (const auto& c:cs) sim::applyCommand(w,empty,c);
            // This world is never ticked: the fighters sent out are carried along
            // by hand, or they would stand with their attack-moves and be stopped
            // and sent again as units that make no headway are.
            for (int id=1;const auto* u=w.unit(id);++id)
                if (std::any_of(u->orders.begin(),u->orders.end(),[](const auto& o){return o.attackMove;}))
                    w.unit(id)->x=u->x+sim::Fixed::fromInt(100);
            w.spawn(&soldier,400,800+float(wave)*32,0,0);
        }
        check(probes>0 && probes<=10 && scouts>0,"probing raids preserve the accumulating home force");
        check(largest>=30,"repeated probing still allows a heavy attack to form");
    }
    {
        sim::World w;terrain(w,true);auto air=soldier;air.canFly=true;air.id="air";
        int ground=w.spawn(&soldier,400,800,0,0);int flyer=w.spawn(&air,400,1000,0,0);
        int unfinished=w.spawn(&air,400,1200,0,0);w.unit(unfinished)->underConstruction=true;
        sight(w);
        ai::Controller controller(0,empty,profile,1,ai::Difficulty::Normal,{{3400,800}});
        auto cs=think(controller,w);
        check(attacks(cs)==1 && cs[0].unitId==flyer && cs[0].x==3400,
              "the scout is a flyer that reaches the enemy start, not a stranded or unfinished unit");
        check(w.unit(ground)->orders.empty(),"AI terrain assessment does not mutate navigation");
    }
    {
        sim::World w;terrain(w,true);auto boat=soldier;boat.domain=sim::UnitType::Domain::Water;
        boat.weapon.range=240;
        for (int i=0;i<3;++i) w.spawn(&boat,2000,800+float(i)*80,0,0);
        sight(w);
        ai::Controller controller(0,empty,profile,1,ai::Difficulty::Normal,{{2624,800}});
        const auto cs=think(controller,w);
        check(attacks(cs)==3 && std::all_of(cs.begin(),cs.end(),[](const auto& c){return c.x<2560;}),
              "naval attacks choose reachable water within range of the enemy shore");
    }
    {
        sim::World w;terrain(w,true);auto boat=soldier;boat.domain=sim::UnitType::Domain::Water;
        for (int i=0;i<3;++i) w.spawn(&boat,2000,800+float(i)*80,0,0);
        sight(w);
        ai::Controller controller(0,empty,profile,1,ai::Difficulty::Normal,{{3400,800}});
        const auto cs=think(controller,w);
        check(attacks(cs)==3 && std::all_of(cs.begin(),cs.end(),[](const auto& c){return c.x<2560;}),
              "ships approach an enemy coast even when the starting position lies beyond weapon range");
    }
    {
        sim::World w;std::vector<uint8_t> heights(256*256,100);
        for(int z=0;z<256;++z)for(int x=0;x<256;++x)
            if((x>=48 && x<80) || (x>=144 && x<176))heights[z*256+x]=0;
        w.setTerrain(std::move(heights),256,256,20);
        auto boat=soldier;boat.domain=sim::UnitType::Domain::Water;
        w.spawn(&boat,1000,800,0,0);
        pastStart(w);
        ai::Controller controller(0,empty,profile,1,ai::Difficulty::Normal,{{3000,800}});
        check(attacks(think(controller,w))==0,"naval coastal projection rejects a disconnected body of water");
    }
    {
        // The ship's lake lies within 1536 px of the enemy start, but a separate
        // lake is the enemy's nearest coast (0.9.0: a Navy Yard in a landlocked
        // pond). Only the nearest wet ring, or the one after it, may count.
        sim::World w;std::vector<uint8_t> heights(256*256,100);
        for(int z=0;z<256;++z)for(int x=0;x<256;++x)
            if((x>=48 && x<80) || (x>=112 && x<128))heights[z*256+x]=0;
        w.setTerrain(std::move(heights),256,256,20);
        auto boat=soldier;boat.domain=sim::UnitType::Domain::Water;
        w.spawn(&boat,1000,800,0,0);
        pastStart(w);
        ai::Controller controller(0,empty,profile,1,ai::Difficulty::Normal,{{2700,800}});
        check(attacks(think(controller,w))==0,"naval coastal projection takes the enemy's nearest coast, not a farther lake");
    }
    for (auto difficulty:{ai::Difficulty::Defensive,ai::Difficulty::Easy,ai::Difficulty::Normal}) {
        sim::World w;terrain(w,true);w.player(0).income=200;
        auto boat=soldier;boat.domain=sim::UnitType::Domain::Water;
        const int count=difficulty==ai::Difficulty::Easy ? 2 : 3;
        for (int i=0;i<count;++i) w.spawn(&boat,2000,800+float(i)*80,0,0);
        sight(w);
        ai::Controller controller(0,empty,profile,1,difficulty,{{3400,800}});
        const auto cs=think(controller,w,4*60*30);
        check(attacks(cs)==(difficulty==ai::Difficulty::Defensive ? 0 : count),
              "small fleets deploy despite a large land-army budget; Defensive never attacks");
    }
    {
        sim::World w;terrain(w);
        int a=w.spawn(&soldier,400,800,0,0),b=w.spawn(&soldier,450,800,0,1);
        w.attack(a,b,false);w.unit(a)->orders.front().autoTarget=true;
        w.setStance(a,2);
        check(w.unit(a)->orders.empty() && w.unit(a)->fireState==0,"Passive cancels an existing automatic engagement");
        w.attack(a,b,false);w.setStance(a,2);
        check(!w.unit(a)->orders.empty() && w.unit(a)->orders.front().targetId==b,"Passive retains an explicit player attack");
        w.player(0).defensiveAi=true;int d=w.spawn(&soldier,800,800,0,0);
        check(w.unit(d)->moveState==0 && w.unit(d)->fireState==2,"Passive AI units defend from their first tick");
        for (auto defaults : {std::pair{0,0},std::pair{0,2},std::pair{1,2}}) for (bool placed : {false,true}) {
            sim::World prod;terrain(prod);auto output=soldier;
            output.defaultMove=defaults.first;output.defaultFire=defaults.second;
            auto factory=soldier;if (!placed) factory.maxVel={};
            factory.isBuilder=true;factory.workerTime=1000;factory.buildDist=100;
            int f=prod.spawn(&factory,800,800,0,0);
            if (placed) prod.startBuild(f,&output,880,800,sim::World::Approach::None);
            else prod.train(f,&output,1);
            for (int t=0;t<60;++t) prod.tick(1.f/30.f);
            const sim::Unit* built=nullptr;for (const auto& u:prod.units()) if (u.type==&output) built=&u;
            check(built && !built->underConstruction && built->moveState==defaults.first && built->fireState==defaults.second,
                  "completed factory production and placed conjuring retain type standing orders");
        }
    }
    // Absurd doubles recurring income. Reclaim (features, ordered corpses)
    // pays nothing at all in retail (406250, 494fd0), so there is nothing for
    // it to double; the targets still go. An idle builder beside a body (source
    // 3) clears nothing: retail reclaims only by order.
    for (int source=0;source<4;++source) {
        double earned[2]{};bool cleared[2]{};
        for (int bonus=0;bonus<2;++bonus) {
            sim::World w;terrain(w);auto builder=soldier;
            builder.isBuilder=true;builder.canReclaim=true;builder.storage=10000;
            builder.weapons.clear();builder.weapon.damage=0;
            builder.income=source==0 ? 10 : 0;
            int id=w.spawn(&builder,800,800,0,0);
            w.player(0).mana=0;w.player(0).manaMult=bonus ? 2 : 1;
            if (source==1) {
                w.addFeature(101,840,800,60,1,1,false);
                w.reclaim(id,101,false);
            }
            int dead=0;
            if (source>=2) {
                sim::FeatType corpse;corpse.energy=60;corpse.reclaimable=true;
                w.setFeatureTypes({corpse});w.mapCorpse(&soldier,0);
                dead=w.spawn(&soldier,840,800,0,1);
                auto* u=w.unit(dead);u->hp={};u->deadFor=120;u->corpseUntil=2000;
                if (!sim::RetailReplayProbe::corpse(w,*u,0)) check(false,"test corpse installs");
                if (source==2) w.reclaim(id,-dead,false);
            }
            for (int tick=0;tick<240;++tick) w.tick(1.f/30.f);
            earned[bonus]=w.player(0).mana;
            cleared[bonus]=source==1 ? !w.feature(101)->alive :
                source>=2 ? w.unit(dead)==nullptr || w.unit(dead)->corpseUntil==0 : true;
        }
        if (source==0)
            check(earned[0]>0 && std::abs(earned[1]-earned[0]*2)<0.001,"Absurd doubles recurring income");
        else if (source<3)
            check(earned[0]==0 && earned[1]==0 && cleared[0] && cleared[1],
                  "feature and ordered-corpse reclaims clear their target for no mana");
        else
            check(earned[0]==0 && earned[1]==0 && !cleared[0] && !cleared[1],
                  "an idle builder beside a body leaves it (no automatic reclaim)");
    }
    levelWiring();
    classicLevel();
    {
        // Self-contained production tree: profile caps must not stop a defensive
        // or aggressive AI while mana and the actual game cap allow more troops.
        auto files=std::make_shared<hpi::Vfs::Files>();
        auto file=[&](const std::string& path,const std::string& text) {
            std::string lines;
            for (char ch : text) { lines += ch; if (ch == ';' || ch == '{' || ch == '}') lines += '\n'; }
            (*files)[path]={lines.begin(),lines.end()};
        };
        auto type=[&](const char* id,const char* fields) {
            file(std::string("units/")+id+".fbi",std::string("[UNITINFO]{unitname=")+id+
                ";objectname="+id+";maxdamage=100;footprintx=2;footprintz=2;buildtime=30;buildcost=10;"+fields+"}");
        };
        type("soldier","canmove=1;maxvelocity=1;");
        type("scout","canmove=1;maxvelocity=1;");
        type("unique","canmove=1;maxvelocity=1;totalallowed=1;");
        type("factory","builder=1;workertime=30;");
        // A Zhon-like producer: the early-army boost below is for a side with no factory buildings.
        type("handler","side=ZON;builder=1;workertime=30;canmove=1;maxvelocity=1;");
        type("lode","mogriumincome=20;");
        type("worker","builder=1;workertime=30;canmove=1;maxvelocity=1;");
        file("canbuild/worker/lode.tdf","[CANBUILD]{sortbias=0;}");
        for (const char* parent : {"factory","handler"})
            for (const char* child : {"soldier","scout","unique"})
                file(std::string("canbuild/")+parent+"/"+child+".tdf","[CANBUILD]{sortbias=0;}");
        file("canbuild/handler/lode.tdf","[CANBUILD]{sortbias=0;}");
        hpi::Vfs vfs;vfs.setMapFiles(files);
        sim::TypeRegistry reg;reg.loadDir(vfs,"units");reg.loadBuildTree(vfs,"canbuild");
        // The soldier fights: Hard and Absurd lift the profile limit only of troops
        // their army routines command (a primary weapon that does damage); an
        // unarmed type keeps its limit (tools/ai_economy_test.cpp).
        {
            auto* soldier=const_cast<sim::UnitType*>(reg.find("soldier"));
            soldier->weapon.damage=10;soldier->weapon.range=120;soldier->weapons={soldier->weapon};
        }
        for (auto difficulty : {ai::Difficulty::Defensive,ai::Difficulty::Normal,ai::Difficulty::Hard,ai::Difficulty::Absurd}) {
            sim::World w;terrain(w);w.buildNavClasses(reg);w.setUnitCap(250);
            w.player(0).mana=10000;w.player(0).income=200;
            w.spawn(reg.find("factory"),400,800,0,0);
            for (int i=0;i<190;++i)w.spawn(reg.find("soldier"),1600+float(i%20)*40,1600+float(i/20)*40,0,0);
            ai::Profile p;p.weight["soldier"]=100;p.limit["soldier"]=1;
            ai::Controller controller(0,reg,p,1,difficulty);
            auto trains=[](const auto& cs){return std::count_if(cs.begin(),cs.end(),[](const auto& c){return c.kind==net::Cmd::Train;});};
            check(trains(think(controller,w))==(difficulty==ai::Difficulty::Normal ? 0 : 1),
                  "sustained modes keep producing past old army/profile ceilings; Normal retains its target");
            w.setUnitCap(191);
            check(trains(think(controller,w))==0,"continued production respects the actual player cap");
            w.setUnitCap(250);p.limit["soldier"]=0;
            check(trains(think(controller,w))==0,"disabled profile entries stay disabled");
            p.weight["soldier"]=0;p.weight["unique"]=100;
            w.spawn(reg.find("unique"),3000,2000,0,0);
            check(trains(think(controller,w))==0,"actual per-type gameplay caps remain enforced");
        }
        {
            sim::World w;terrain(w);w.buildNavClasses(reg);
            w.player(0).mana=10000;w.player(0).income=25;
            const int handler=w.spawn(reg.find("handler"),800,800,0,0);
            ai::Profile p;p.weight["soldier"]=100;p.weight["lode"]=100;
            ai::Controller controller(0,reg,p,1,ai::Difficulty::Normal);
            auto cs=think(controller,w);
            check(std::any_of(cs.begin(),cs.end(),[&](const auto& c){return c.unitId==handler && c.kind==net::Cmd::Build && std::string(c.type)=="soldier";}),
                  "mobile production creature makes an opening army before more economy");
            const int defender=w.spawn(reg.find("soldier"),880,800,0,0);
            ai::Profile none;ai::Controller passive(0,reg,none,1,ai::Difficulty::Defensive);
            cs=think(passive,w);
            check(attacks(cs)==0 && std::any_of(cs.begin(),cs.end(),[&](const auto& c){return c.unitId==defender && c.kind==net::Cmd::Move;}),
                  "defensive troops clear mobile production sites without an attack order");
            for (int i=0;i<3;++i)w.spawn(reg.find("soldier"),1200+float(i)*40,800,0,0);
            int worker=w.spawn(reg.find("worker"),1600,800,0,0);
            sim::Order approach;approach.x=sim::Fixed::fromInt(2000);approach.z=sim::Fixed::fromInt(800);
            sim::Order build=approach;build.buildType=reg.find("lode");
            w.unit(worker)->orders={approach,build};
            cs=think(controller,w);
            check(std::any_of(cs.begin(),cs.end(),[&](const auto& c){return c.unitId==handler && c.kind==net::Cmd::Build && std::string(c.type)=="soldier";}),
                  "economy construction behind approach waypoints leaves mobile production making troops");
        }
        // A deposit that a builder is walking to is reserved by the order's SITE.
        // A real MobileBuild order keeps its approach point beside the site in x/z
        // (World::makeBuildOrder); the reservation compared that point, 45 px from
        // the deposit here, against a 32 px match and so never held. In a replayed
        // 4v4 Absurd game two allies sent a twelfth builder to a deposit eleven of
        // theirs were already walking to, and raced each other for home deposits.
        enum class Owner { Self, Ally, Enemy };
        for (auto owner : {Owner::Self,Owner::Ally,Owner::Enemy})
            for (auto difficulty : {ai::Difficulty::Hard,ai::Difficulty::Absurd}) {
                sim::World w;terrain(w);w.buildNavClasses(reg);
                w.setManaSpots({{2000,800},{2000,1600}});
                const int player=owner==Owner::Self ? 0 : 1;
                const int walker=w.spawn(reg.find("worker"),1200,400,0,player);
                const int idle=w.spawn(reg.find("worker"),1800,800,0,0);   // nearer the walker's deposit
                if (player==1) w.setTeam(1,owner==Owner::Ally ? 0 : 1);
                w.player(0).mana=10000;
                net::Command order;order.kind=net::Cmd::Build;order.player=uint8_t(player);order.unitId=walker;
                order.x=2000;order.z=800;std::snprintf(order.type,sizeof order.type,"lode");
                sim::applyCommand(w,reg,order);
                const auto& queued=w.unit(walker)->orders;
                const bool real=!queued.empty() && queued.back().buildType==reg.find("lode") &&
                    std::hypot(queued.back().x.toFloat()-2000,queued.back().z.toFloat()-800)>=32;
                check(real,"a queued lodestone's order point is its approach, away from the deposit");
                ai::Profile p;p.weight["lode"]=100;
                ai::Controller controller(0,reg,p,1,difficulty);
                const auto cs=think(controller,w);
                auto builds=[&](float z) {
                    return std::any_of(cs.begin(),cs.end(),[&](const auto& c) {
                        return c.unitId==idle && c.kind==net::Cmd::Build && c.x==2000 && c.z==z;
                    });
                };
                if (owner==Owner::Enemy)
                    check(builds(800),"an enemy builder's destination does not reserve a deposit");
                else
                    check(builds(1600) && !builds(800),owner==Owner::Self
                          ? "expansion leaves a deposit its own walking builder will take"
                          : "expansion leaves a deposit an ally's walking builder will take");
            }
        // Map-wide expansion leaves a free deposit in a living ally's home area to the
        // ally only while the ally is claiming it: a builder bound for it (never
        // raced), or one of the ally's that can reach it while the deposit has sat
        // unclaimed for less than two minutes of game time. Retail's base-radius
        // search never builds there at all, but leaving a deposit to an ally that
        // never takes it cost a mixed team 20-35% of its income. An enemy's, a
        // defeated ally's and a builderless ally's home deposits are open.
        constexpr uint32_t kClaimTicks=2*60*30;
        enum class Ally { Idle, Bound, Lapsed, NoBuilder, Defeated, Enemy };
        for (auto difficulty : {ai::Difficulty::Hard,ai::Difficulty::Absurd})
            for (auto ally : {Ally::Idle,Ally::Bound,Ally::Lapsed,Ally::NoBuilder,Ally::Defeated,Ally::Enemy}) {
                sim::World w;terrain(w);w.buildNavClasses(reg);
                w.setManaSpots({{2560,800},{1600,1792}});   // the ally's home deposit, a free one
                w.spawn(reg.find("factory"),400,800,0,0);    // our base
                auto monarch=*reg.find("worker");monarch.commander=true;
                const int allyBuilder=ally==Ally::NoBuilder ? 0 : w.spawn(&monarch,3000,800,0,1);   // the ally's start
                if (ally==Ally::NoBuilder) w.spawn(reg.find("factory"),3000,800,0,1);   // its Monarch lost, a building left
                const int idle=w.spawn(reg.find("worker"),2000,800,0,0);   // nearer the ally's deposit
                w.setTeam(1,ally==Ally::Enemy ? 1 : 0);
                if (ally==Ally::Defeated) w.player(1).defeated=true;
                w.player(0).mana=10000;
                if (ally==Ally::Bound || ally==Ally::Lapsed) {
                    net::Command order;order.kind=net::Cmd::Build;order.player=1;order.unitId=allyBuilder;
                    order.x=2560;order.z=800;std::snprintf(order.type,sizeof order.type,"lode");
                    sim::applyCommand(w,reg,order);
                }
                ai::Profile p;p.weight["lode"]=100;
                ai::Controller controller(0,reg,p,1,difficulty);
                // Where the idle builder expands at `tick`: 'A' the ally's deposit, 'F' the free one.
                auto expands=[&](uint32_t tick) {
                    for (const auto& c:think(controller,w,tick))
                        if (c.unitId==idle && c.kind==net::Cmd::Build) return c.x==2560 && c.z==800 ? 'A' : c.x==1600 && c.z==1792 ? 'F' : '?';
                    return '-';
                };
                switch (ally) {
                    case Ally::Idle: {
                        const bool held=expands(0)=='F' && expands(kClaimTicks-20)=='F';
                        check(held,"an ally's home deposit stays its own for two minutes while its idle builder can reach it");
                        check(expands(kClaimTicks)=='A',"an ally's home deposit left unclaimed for two minutes opens to expansion");
                        break;
                    }
                    case Ally::Bound: {
                        // It walks: 100 px nearer by each think (see the stalled builders below).
                        bool held=expands(0)=='F';
                        for (uint32_t tick : {kClaimTicks,2*kClaimTicks}) {
                            w.unit(allyBuilder)->x-=sim::Fixed::fromInt(100);
                            held=held && expands(tick)=='F';
                        }
                        check(held,"expansion never races an ally's builder bound for its home deposit, however long it walks");
                        break;
                    }
                    case Ally::Lapsed: {
                        bool held=expands(0)=='F';
                        w.unit(allyBuilder)->x-=sim::Fixed::fromInt(100);   // still walking at tick 2000
                        held=held && expands(2000)=='F';
                        w.unit(allyBuilder)->orders.clear();   // the claim lapses at tick 2000
                        held=held && expands(kClaimTicks)=='F' && expands(2000+kClaimTicks-20)=='F';
                        check(held,"an ally's home deposit stays its own for two minutes after its last claim lapsed");
                        check(expands(2000+kClaimTicks)=='A',"an ally's home deposit opens two minutes after its last claim lapsed");
                        break;
                    }
                    case Ally::NoBuilder:
                        check(expands(0)=='A',"an ally with no builder left (Monarch Expendable) holds no deposit");
                        break;
                    case Ally::Defeated:
                        check(expands(0)=='A',"expansion claims a defeated ally's home deposit");
                        break;
                    case Ally::Enemy:
                        check(expands(0)=='A',"expansion claims an enemy's home deposit");
                        break;
                }
            }
        // Only a builder that can reach the deposit holds it for the ally.
        for (bool reaches : {true,false}) {
            sim::World w;
            std::vector<uint8_t> h(256*256,100);
            for (int z=0;z<256;++z) for (int x=96;x<160;++x) h[z*256+x]=0;
            w.setTerrain(std::move(h),256,256,60);w.buildNavClasses(reg);   // too deep to wade from x=1536 to 2560
            w.setRoomVision(true,false);
            w.setManaSpots({{2640,800},{3600,2600}});   // the ally's home deposit, a free one
            w.spawn(reg.find("factory"),3800,3200,0,0);  // our base
            w.spawn(reg.find("factory"),2960,800,0,1);   // the ally's base
            w.spawn(reg.find("worker"),reaches ? 3200 : 1000,800,0,1);   // its only builder, across the water or not
            const int idle=w.spawn(reg.find("worker"),3400,1400,0,0);   // nearer the ally's deposit
            w.setTeam(1,0);w.player(0).mana=10000;
            ai::Profile p;p.weight["lode"]=100;
            ai::Controller controller(0,reg,p,1,ai::Difficulty::Absurd);
            const auto cs=think(controller,w);
            const bool claims=std::any_of(cs.begin(),cs.end(),[&](const auto& c) {
                return c.unitId==idle && c.kind==net::Cmd::Build && c.x==2640 && c.z==800;
            });
            check(claims!=reaches,reaches ? "an ally builder that can reach its home deposit holds it"
                                          : "an ally builder that cannot reach its home deposit does not hold it");
        }
        // Once the ally whose home it is no longer holds a deposit -- unclaimed for
        // two minutes, or no builder of its own that can reach it -- the deposit
        // goes to the nearest of the other allies, not to one that would walk across
        // the map and hold it all the way. Every difficulty that reaches it alike.
        for (auto difficulty : {ai::Difficulty::Easy,ai::Difficulty::Normal,ai::Difficulty::Hard,ai::Difficulty::Absurd})
            for (bool ownerBuilds : {true,false})
                for (bool nearer : {true,false}) {
                    sim::World w;terrain(w);w.buildNavClasses(reg);
                    w.setManaSpots({{2560,800},{1600,1792}});   // the owner's home deposit, a free one
                    w.spawn(reg.find("factory"),1200,800,0,0);   // our base, 1360 from the owner's deposit
                    // The owner's start: an idle builder, or a building and no builder left.
                    w.spawn(reg.find(ownerBuilds ? "worker" : "factory"),3000,800,0,1);
                    // A third ally's base, 1092 from the owner's deposit or 3083.
                    w.spawn(reg.find("factory"),nearer ? 3000 : 400,nearer ? 1800 : 3000,0,2);
                    const int idle=w.spawn(reg.find("worker"),2000,800,0,0);
                    w.setTeam(1,0);w.setTeam(2,0);w.player(0).mana=10000;
                    ai::Profile p;p.weight["lode"]=100;
                    ai::Controller controller(0,reg,p,1,difficulty);
                    think(controller,w);
                    const auto cs=think(controller,w,ownerBuilds ? kClaimTicks : 0);
                    const bool claims=std::any_of(cs.begin(),cs.end(),[&](const auto& c) {
                        return c.unitId==idle && c.kind==net::Cmd::Build && c.x==2560 && c.z==800;
                    });
                    check(claims!=nearer,nearer ? (ownerBuilds ? "an ally home deposit open after two minutes is left to a nearer ally"
                                                               : "a builderless ally's home deposit is left to a nearer ally")
                                                : (ownerBuilds ? "an ally home deposit open after two minutes goes to its nearest ally"
                                                               : "a builderless ally's home deposit goes to its nearest ally"));
                }
        // Normal, Easy and Defensive reserve deposits the same way: a site an own or
        // allied builder is walking to, and one planned earlier in the same think.
        // Without it two Normal allies, or two builders of one Normal AI, raced
        // for the nearest free deposit and the later one walked there for nothing.
        for (auto owner : {Owner::Self,Owner::Ally,Owner::Enemy})
            for (auto difficulty : {ai::Difficulty::Defensive,ai::Difficulty::Easy,ai::Difficulty::Normal}) {
                sim::World w;terrain(w);w.buildNavClasses(reg);
                w.setManaSpots({{2000,800},{2000,1200}});
                w.spawn(reg.find("factory"),1800,1000,0,0);   // home, within Defensive's 600
                const int player=owner==Owner::Self ? 0 : 1;
                const int walker=w.spawn(reg.find("worker"),1200,400,0,player);
                const int idle=w.spawn(reg.find("worker"),1900,800,0,0);   // nearer the walker's deposit
                if (player==1) w.setTeam(1,owner==Owner::Ally ? 0 : 1);
                w.player(0).mana=10000;
                net::Command order;order.kind=net::Cmd::Build;order.player=uint8_t(player);order.unitId=walker;
                order.x=2000;order.z=800;std::snprintf(order.type,sizeof order.type,"lode");
                sim::applyCommand(w,reg,order);
                ai::Profile p;p.weight["lode"]=100;
                ai::Controller controller(0,reg,p,1,difficulty);
                const auto cs=think(controller,w);
                auto builds=[&](float z) {
                    return std::any_of(cs.begin(),cs.end(),[&](const auto& c) {
                        return c.unitId==idle && c.kind==net::Cmd::Build && c.x==2000 && c.z==z;
                    });
                };
                if (owner==Owner::Enemy)
                    check(builds(800),"Normal, Easy and Defensive ignore an enemy builder's destination");
                else
                    check(builds(1200) && !builds(800),owner==Owner::Self
                          ? "Normal, Easy and Defensive leave a deposit their own walking builder will take"
                          : "Normal, Easy and Defensive leave a deposit an ally's walking builder will take");
            }
        for (auto difficulty : {ai::Difficulty::Defensive,ai::Difficulty::Normal}) {
            sim::World w;terrain(w);w.buildNavClasses(reg);
            w.setManaSpots({{2000,800},{2000,1200}});
            w.spawn(reg.find("factory"),1800,1000,0,0);
            w.spawn(reg.find("worker"),1900,800,0,0);
            w.spawn(reg.find("worker"),1900,820,0,0);   // both nearest the same deposit
            w.player(0).mana=10000;
            ai::Profile p;p.weight["lode"]=100;
            ai::Controller controller(0,reg,p,1,difficulty);
            std::vector<float> sites;
            for (const auto& c:think(controller,w)) if (c.kind==net::Cmd::Build) sites.push_back(c.z);
            check(sites.size()==2 && sites[0]!=sites[1],"Normal and Defensive builders take different deposits in one think");
        }
        // Passive's builders stay home: a deposit within 900 px of it, never one past
        // that, where Easy's walk out to 1600.
        for (auto difficulty : {ai::Difficulty::Easy,ai::Difficulty::Passive})
            for (float distance : {800.f,1300.f}) {
                sim::World w;terrain(w);w.buildNavClasses(reg);
                w.setManaSpots({{1200+distance,800}});
                w.spawn(reg.find("factory"),1200,800,0,0);
                const int idle=w.spawn(reg.find("worker"),1300,800,0,0);
                w.player(0).mana=10000;
                ai::Profile p;p.weight["lode"]=100;
                ai::Controller controller(0,reg,p,1,difficulty);
                bool built=false;
                for (const auto& c:think(controller,w,0)) built=built || (c.unitId==idle && c.kind==net::Cmd::Build);
                check(built==(difficulty==ai::Difficulty::Easy || distance<900),
                      "Passive takes a deposit near home and leaves one Easy would walk to");
            }
        // ...and leave a living ally's home deposit to it while the ally is claiming
        // it. Normal's and Easy's builders reach 1600 from home, far enough to take a
        // close ally's deposits before the ally gets to them.
        for (auto difficulty : {ai::Difficulty::Easy,ai::Difficulty::Normal})
            for (auto ally : {Ally::Idle,Ally::Bound,Ally::Enemy}) {
                sim::World w;terrain(w);w.buildNavClasses(reg);
                w.setManaSpots({{2560,800},{1600,1792}});   // the ally's home deposit, a free one
                w.spawn(reg.find("factory"),1200,800,0,0);   // our base, 1360 from the ally's deposit
                auto monarch=*reg.find("worker");monarch.commander=true;
                const int allyBuilder=w.spawn(&monarch,3000,800,0,1);   // the ally's start
                const int idle=w.spawn(reg.find("worker"),2000,800,0,0);   // nearer the ally's deposit
                w.setTeam(1,ally==Ally::Enemy ? 1 : 0);
                w.player(0).mana=10000;
                if (ally==Ally::Bound) {
                    net::Command order;order.kind=net::Cmd::Build;order.player=1;order.unitId=allyBuilder;
                    order.x=2560;order.z=800;std::snprintf(order.type,sizeof order.type,"lode");
                    sim::applyCommand(w,reg,order);
                }
                ai::Profile p;p.weight["lode"]=100;
                ai::Controller controller(0,reg,p,1,difficulty);
                const uint32_t lastHeld=kClaimTicks-uint32_t(ai::paramsFor(difficulty).thinkPeriod);
                auto expands=[&](uint32_t tick) {
                    for (const auto& c:think(controller,w,tick))
                        if (c.unitId==idle && c.kind==net::Cmd::Build) return c.x==2560 && c.z==800 ? 'A' : c.x==1600 && c.z==1792 ? 'F' : '?';
                    return '-';
                };
                switch (ally) {
                    case Ally::Idle:
                        check(expands(0)=='F' && expands(lastHeld)=='F' && expands(kClaimTicks)=='A',
                              "Normal and Easy leave a close ally's home deposit to it for two minutes, then take it");
                        break;
                    case Ally::Bound: {
                        bool held=expands(0)=='F';
                        for (uint32_t tick : {kClaimTicks,2*kClaimTicks}) {
                            w.unit(allyBuilder)->x-=sim::Fixed::fromInt(100);   // walking
                            held=held && expands(tick)=='F';
                        }
                        check(held,"Normal and Easy never race an ally's builder bound for its home deposit");
                        break;
                    }
                    default:
                        check(expands(0)=='A',"Normal and Easy take an enemy's home deposit");
                        break;
                }
            }
        // A builder holds its deposit only while it is getting there: one that has
        // come no nearer by 64 px for a minute of game time no longer reserves it,
        // for its own AI or an ally, and its own AI stops it and never sends it
        // back there. In a 4v4 a Zhon handler bound for a human ally's deposit
        // moved 109 px in 440 s and kept every ally off it for the rest of the game.
        constexpr uint32_t kStallTicks=60*30;
        for (auto owner : {Owner::Self,Owner::Ally})
            for (auto difficulty : {ai::Difficulty::Normal,ai::Difficulty::Absurd})
                for (bool walks : {true,false}) {
                    sim::World w;terrain(w);w.buildNavClasses(reg);
                    w.setManaSpots({{2000,800},{2000,1600}});
                    w.spawn(reg.find("factory"),1800,1000,0,0);
                    const int player=owner==Owner::Self ? 0 : 1;
                    const int walker=w.spawn(reg.find("worker"),1000,800,0,player);
                    const int idle=w.spawn(reg.find("worker"),1800,800,0,0);   // nearer the walker's deposit
                    if (player==1) w.setTeam(1,0);
                    w.player(0).mana=10000;
                    net::Command order;order.kind=net::Cmd::Build;order.player=uint8_t(player);order.unitId=walker;
                    order.x=2000;order.z=800;std::snprintf(order.type,sizeof order.type,"lode");
                    sim::applyCommand(w,reg,order);
                    ai::Profile p;p.weight["lode"]=100;
                    ai::Controller controller(0,reg,p,1,difficulty);
                    // A think every 20 s, the walker 100 px nearer by each when it walks.
                    auto takes=[&](const std::vector<net::Command>& out) {
                        return std::any_of(out.begin(),out.end(),[&](const auto& c) {
                            return c.unitId==idle && c.kind==net::Cmd::Build && c.x==2000 && c.z==800;
                        });
                    };
                    bool heldBefore=true,stopped=false;
                    std::vector<net::Command> cs;
                    for (uint32_t tick=0;tick<=kStallTicks;tick+=600) {
                        if (walks && tick) w.unit(walker)->x+=sim::Fixed::fromInt(100);
                        cs=think(controller,w,tick);
                        if (tick<kStallTicks && takes(cs)) heldBefore=false;
                        stopped=stopped || std::any_of(cs.begin(),cs.end(),[&](const auto& c) {
                            return c.unitId==walker && c.kind==net::Cmd::Stop;
                        });
                    }
                    if (walks) {
                        check(heldBefore && !takes(cs) && !stopped,"a builder still gaining on its deposit keeps it reserved");
                        continue;
                    }
                    check(heldBefore && takes(cs),owner==Owner::Self
                          ? "a deposit its own builder has stalled short of for a minute goes to another builder"
                          : "a deposit an ally's builder has stalled short of for a minute goes to another builder");
                    check(stopped==(owner==Owner::Self),owner==Owner::Self ? "the AI stops its own stalled builder"
                                                                           : "the AI leaves an ally's stalled builder alone");
                    if (owner!=Owner::Self) continue;
                    net::Command stop;stop.kind=net::Cmd::Stop;stop.player=0;stop.unitId=walker;
                    sim::applyCommand(w,reg,stop);
                    char walkerSite='-';
                    for (const auto& c:think(controller,w,kStallTicks+60))
                        if (c.unitId==walker && c.kind==net::Cmd::Build) walkerSite=c.z==800 ? 'S' : 'F';
                    check(walkerSite=='F',"a stopped builder is sent to another deposit, not back to the one it stalled on");
                }
        // ...but one at work on an earlier order -- here a repair within reach, its
        // lodestone queued behind -- holds it however long the work takes. The
        // sim assigns the repair before the builder gets there, so one assigned it
        // but getting no nearer is stalled all the same.
        for (auto difficulty : {ai::Difficulty::Normal,ai::Difficulty::Absurd})
            for (bool atWork : {true,false}) {
                sim::World w;terrain(w);w.buildNavClasses(reg);
                w.setManaSpots({{2000,800},{2000,1600}});
                w.spawn(reg.find("factory"),1800,1000,0,0);
                const int repairer=w.spawn(reg.find("worker"),1000,800,0,1);
                const int hurt=w.spawn(reg.find("soldier"),atWork ? 1040 : 400,atWork ? 800 : 1600,0,1);
                const int idle=w.spawn(reg.find("worker"),1800,800,0,0);
                w.setTeam(1,0);w.player(0).mana=10000;
                w.unit(hurt)->hp=sim::Fixed::fromInt(50);
                w.repair(repairer,hurt,false);
                w.queueBuild(repairer,reg.find("lode"),2000,800,true);
                w.unit(repairer)->repairId=hurt;
                w.unit(repairer)->workScriptWorking=atWork;   // in reach and repairing, or not there yet
                ai::Profile p;p.weight["lode"]=100;
                ai::Controller controller(0,reg,p,1,difficulty);
                bool heldBefore=w.unit(repairer)->orders.size()==2,takenAtStall=false;
                for (uint32_t tick=0;tick<=kStallTicks;tick+=600) {
                    const auto cs=think(controller,w,tick);
                    const bool takes=std::any_of(cs.begin(),cs.end(),[&](const auto& c) {
                        return c.unitId==idle && c.kind==net::Cmd::Build && c.x==2000 && c.z==800;
                    });
                    if (tick<kStallTicks) heldBefore=heldBefore && !takes; else takenAtStall=takes;
                }
                check(heldBefore && takenAtStall!=atWork,atWork
                      ? "an ally's builder repairing before its queued lodestone keeps the deposit reserved"
                      : "an ally's builder sent to repair that gets no nearer gives up its queued deposit");
            }
        // Retail builds a lodestone whenever it runs short of mana -- store under
        // 30% full, spending outpacing income -- with none unfinished and no
        // builder already on one; that lodestone then replaces the planner's
        // ordinary pick (icd 0x410810, 0x40c890, 0x40c830). Normal's income target
        // alone held it to one lodestone for a whole game on "4x4 team", where a
        // deposit pays 100 a second, however short of mana it ran.
        enum class Store { Short, Starved, Full, Covered, Walking };
        for (auto difficulty : {ai::Difficulty::Defensive,ai::Difficulty::Easy,ai::Difficulty::Normal})
            for (auto store : {Store::Short,Store::Starved,Store::Full,Store::Covered,Store::Walking}) {
                sim::World w;terrain(w);w.buildNavClasses(reg);
                w.setManaSpots({{1200,800},{1200,1200},{1600,800}});
                w.spawn(reg.find("factory"),800,800,0,0);
                w.spawn(reg.find("lode"),1200,800,0,0);   // the first lodestone, already paying
                const int worker=w.spawn(reg.find("worker"),1000,1000,0,0);
                auto& me=w.player(0);
                me.income=110;me.storage=6000;   // income far past 25 + 20 per factory
                me.mana=store==Store::Full ? 3000 : store==Store::Starved ? 10 : 100;
                if (store==Store::Starved)   // broke with construction under way: no ordinary build
                    w.unit(w.spawn(reg.find("soldier"),800,1200,0,0))->underConstruction=true;
                for (auto& sample:me.displayResources.samples)   // per tick: earned, spent
                    sample={110.f/30,store==Store::Covered ? 110.f/30 : 10.f};
                if (store==Store::Walking) {
                    const int other=w.spawn(reg.find("worker"),1400,800,0,0);
                    net::Command order;order.kind=net::Cmd::Build;order.player=0;order.unitId=other;
                    order.x=1600;order.z=800;std::snprintf(order.type,sizeof order.type,"lode");
                    sim::applyCommand(w,reg,order);
                }
                ai::Profile p;p.weight["lode"]=100;
                ai::Controller controller(0,reg,p,1,difficulty);
                // Easy acts with one producer a think, in turn; tick 90 is the worker's.
                const auto cs=think(controller,w,difficulty==ai::Difficulty::Easy ? 90 : 0);
                const bool builds=std::any_of(cs.begin(),cs.end(),[&](const auto& c) {
                    return c.unitId==worker && c.kind==net::Cmd::Build && std::string(c.type)=="lode";
                });
                switch (store) {
                    case Store::Short:
                        check(builds,"Normal, Easy and Defensive build a lodestone when short of mana, whatever their income");
                        break;
                    case Store::Starved:
                        check(builds,"a starved AI still starts that lodestone, though nothing else");
                        break;
                    case Store::Full:
                        check(!builds,"no lodestone while the store is 30% full or more");
                        break;
                    case Store::Covered:
                        check(!builds,"no lodestone while income covers spending");
                        break;
                    case Store::Walking:
                        check(!builds,"no second lodestone while a builder is on its way to one");
                        break;
                }
            }
        // ...within retail's base radius of home (0x40cf30): 800 px for a builder,
        // 560 for the Monarch, while the base is small. Searching Normal's own 1600
        // instead sent a builder 1250 px into an enemy base on Inner Circle for a
        // freed home deposit, where it died with the lodestone.
        for (auto difficulty : {ai::Difficulty::Defensive,ai::Difficulty::Normal})
            for (bool monarch : {false,true})
                for (bool inside : {true,false}) {
                    sim::World w;terrain(w);w.buildNavClasses(reg);
                    w.spawn(reg.find("factory"),800,800,0,0);
                    w.spawn(reg.find("lode"),1200,800,0,0);   // home is the buildings' centre, (1000,800)
                    const float d=monarch ? (inside ? 500.f : 700.f) : (inside ? 700.f : 1100.f);
                    w.setManaSpots({{1200,800},{1000,800+d}});
                    auto king=*reg.find("worker");king.commander=monarch;
                    const int worker=w.spawn(&king,1000,1000,0,0);
                    auto& me=w.player(0);
                    me.income=110;me.storage=6000;me.mana=100;
                    for (auto& sample:me.displayResources.samples) sample={110.f/30,10.f};
                    ai::Profile p;p.weight["lode"]=100;
                    ai::Controller controller(0,reg,p,1,difficulty);
                    const auto cs=think(controller,w);
                    const bool builds=std::any_of(cs.begin(),cs.end(),[&](const auto& c) {
                        return c.unitId==worker && c.kind==net::Cmd::Build && std::string(c.type)=="lode";
                    });
                    // Defensive's builders claim deposits as far as Normal's; its Monarch stays home.
                    const bool expected=inside;
                    check(builds==expected,monarch
                          ? (inside ? "the Monarch builds the short-of-mana lodestone within 560 px of home"
                                    : "the Monarch leaves a deposit beyond 560 px of home to the ordinary ladder")
                          : (inside ? "a builder builds the short-of-mana lodestone within 800 px of home"
                                    : "a builder leaves a deposit beyond 800 px of home to the ordinary ladder"));
                }
    }
    if (argc>1) {
        auto vfs=hpi::mountRetailRoot(argv[1]);
        for (bool crusades : {false,true}) {
            sim::TypeRegistry reg;sim::setupRegistry(reg,vfs,crusades);
            for (const char* monarch : sim::kMonarchs) {
                auto builderType=*reg.find(monarch);builderType.canFly=true;
                for (auto shape : {std::pair{2,2},std::pair{7,12},std::pair{12,7}})
                for (float offset : {0.f,32.f,64.f}) {
                    sim::World w;terrain(w);w.buildNavClasses(reg);
                    const int builderId=w.spawn(&builderType,800,800,0,0);
                    sim::UnitType building;building.id=shape.first==7 ? "arakeep" : "reserved-site-test";
                    building.side=builderType.side;building.maxHp=100;building.maxVel=sim::Fixed();
                    building.footX=shape.first;building.footZ=shape.second;
                    ai::Controller controller(0,reg,profile,1,ai::Difficulty::Normal);
                    float x=0,z=0;
                    check(ai::PlacementProbe::place(controller,w,&building,*w.unit(builderId),x,z),
                          "AI placement baseline exists without mana spots");
                    // The candidate would cover this deposit; the planner must
                    // choose another site, preserving every faction's upgrade size.
                    const float mx=x+offset,mz=z;
                    w.setManaSpots({{mx,mz}});
                    check(ai::PlacementProbe::place(controller,w,&building,*w.unit(builderId),x,z),
                          "AI finds another site instead of occupying a mana spot");
                    w.spawn(&building,x,z,0,0);
                    w.unit(builderId)->deadFor=0; // remove temporary mobile occupancy
                    for (const auto& [id,lode]:reg.types())
                        if (lode.onMana && lode.isStructure() && lode.side==builderType.side)
                            check(w.canPlace(&lode,mx,mz),
                                  "AI structure preserves space for lodestones and upgrades");
                }
            }
            {
                // Absurd starts with enough income to pick a Keep before a
                // lodestone. Its first short-side approach used to walk the
                // Monarch into the placement exclusion and retry forever.
                sim::World w;
                sim::MatchConfig config;config.vfs=&vfs;
                config.mapPath=hpi::findMap(vfs,"ulasem arena");
                config.slots={{true,0,0,1.0f},{true,0,1,2.0f,true,false}};
                const auto starts=sim::setupMatch(w,reg,config);
                const auto& openingProfile=ai::builtinProfile(ai::Difficulty::Absurd);
                ai::Controller controller(1,reg,openingProfile,0x1234,
                                          ai::Difficulty::Absurd,{starts.front()});
                bool started=false;
                for (uint32_t tick=0;tick<900 && !started;++tick) {
                    controller.tick(w,tick,[&](const auto& command){sim::applyCommand(w,reg,command);});
                    w.tick(1.f/30.f);
                    started=std::any_of(w.units().begin(),w.units().end(),[](const auto& unit) {
                        return unit.alive() && unit.player==1 && unit.type && unit.type->id=="arakeep";
                    });
                }
                check(started,"Absurd Aramon starts its first Keep without repeating rejected approaches");
            }
            for (auto difficulty : {ai::Difficulty::Defensive,ai::Difficulty::Normal,ai::Difficulty::Hard,ai::Difficulty::Absurd}) {
                sim::World w;terrain(w);w.buildNavClasses(reg);w.player(0).mana=10000;
                w.setManaSpots({{2800,800}});
                auto king=*reg.find("araking");king.commander=false; // expansion builder using the same menu
                w.spawn(&king,400,800,0,0);
                ai::Profile p;p.weight["aralode"]=100;
                // The enemy starts 1,100 px beyond the deposit: clear of its base,
                // whose deposits Hard and Absurd leave alone (tools/ai_economy_test.cpp).
                ai::Controller controller(0,reg,p,1,difficulty,{{3900,800}});
                auto cs=think(controller,w);
                bool build=std::any_of(cs.begin(),cs.end(),[](const auto& c){return c.kind==net::Cmd::Build;});
                check(build==(difficulty==ai::Difficulty::Hard || difficulty==ai::Difficulty::Absurd),"Hard and Absurd expand beyond the normal and defensive home limits");
            }
            for(auto difficulty:{ai::Difficulty::Hard,ai::Difficulty::Absurd}) {
                sim::World w;terrain(w);w.buildNavClasses(reg);w.player(0).mana=10000;
                w.setManaSpots({{2800,800},{2800,1200}});
                auto builder=*reg.find("araking");builder.commander=false;
                const int first=w.spawn(&builder,400,800,0,0);
                const int second=w.spawn(&builder,400,1200,0,0);
                ai::Profile p;p.weight["aralode"]=100;
                ai::Controller controller(0,reg,p,1,difficulty);
                const auto cs=think(controller,w);
                std::vector<net::Command> sites;
                for(const auto& c:cs)if(c.kind==net::Cmd::Build)sites.push_back(c);
                check(sites.size()==2 && (sites[0].x!=sites[1].x || sites[0].z!=sites[1].z),
                      "expansion builders reserve different mana spots before lockstep applies orders");
                // The real order list: a move, then the build queued behind it.
                net::Command move;move.kind=net::Cmd::Move;move.player=0;move.unitId=first;move.x=1600;move.z=800;
                sim::applyCommand(w,reg,move);
                net::Command build=move;build.kind=net::Cmd::Build;build.x=2800;build.queue=1;
                std::snprintf(build.type,sizeof build.type,"aralode");
                sim::applyCommand(w,reg,build);
                check(w.unit(first)->orders.size()==2 && w.unit(first)->orders.back().buildType==reg.find("aralode"),
                      "the expansion route holds the build behind its move");
                // Idle beside the reserved deposit, which is nearer than the free one.
                w.unit(second)->x=sim::Fixed::fromInt(2400);w.unit(second)->z=sim::Fixed::fromInt(800);
                const auto later=think(controller,w,uint32_t(ai::paramsFor(difficulty).thinkPeriod));
                check(std::any_of(later.begin(),later.end(),[](const auto& c){return c.kind==net::Cmd::Build && c.z==1200;}),
                      "expansion respects a mana site reserved behind movement waypoints");
            }
            {
                sim::World w;terrain(w);w.buildNavClasses(reg);w.player(0).mana=5000;
                w.spawn(reg.find("araking"),400,800,0,0);
                ai::Profile p;p.weight["aralode"]=100;
                ai::Controller controller(0,reg,p,1,ai::Difficulty::Hard);
                check(think(controller,w).empty(),"AI does not buy nonproducing lodestones on a map without mana spots");
                w.setManaSpots({{800,800}});
                const auto cs=think(controller,w,20);
                check(std::any_of(cs.begin(),cs.end(),[](const auto& c){return c.kind==net::Cmd::Build;}),
                      "AI still builds a lodestone where a mana deposit exists");
            }
            {
                sim::World w;terrain(w);w.buildNavClasses(reg);
                w.spawn(reg.find("arakeep"),800,800,0,0);
                const int guard=w.spawn(reg.find("araarch"),800,880,0,0);
                ai::Profile p;
                ai::Controller controller(0,reg,p,1,ai::Difficulty::Defensive,{{3400,800}});
                const auto cs=think(controller,w);
                check(std::any_of(cs.begin(),cs.end(),[&](const auto& c){
                    return c.kind==net::Cmd::Move && c.unitId==guard &&
                        (c.x-800)*(c.x-800)+(c.z-800)*(c.z-800)<=600*600;
                }) && attacks(cs)==0,"defensive AI clears factory exits with local moves, never attacks");
            }
            for (const auto& fixture : {std::array{"cresage","cresmit","creauto"},
                                       std::array{"araking","arakeep","araarch"},
                                       std::array{"tarnecro","tardung","tararch"},
                                       std::array{"vermage","verkeep","versword"}}) {
                const auto* builderType=reg.find(fixture[0]);
                const auto* factoryType=reg.find(fixture[1]);
                const auto* outputType=reg.find(fixture[2]);
                if (!builderType || !factoryType || !outputType) {
                    check(false,"factory orientation fixture types exist");continue;
                }
                sim::World w;terrain(w);w.buildNavClasses(reg);w.player(0).mana=100000;
                const float reach=float(std::max(factoryType->footX,factoryType->footZ))*8+24;
                const int builder=w.spawn(builderType,1000,800+reach,0,0);
                const int factory=w.startBuild(builder,factoryType,1000,800,sim::World::Approach::None);
                check(factory!=0,"factory construction accepts a reachable site");
                if (factory) {
                    for (int tick=0;tick<9000 && w.unit(factory)->underConstruction;++tick) {
                        w.player(0).mana=100000;w.tick(1.f/30.f);
                    }
                    check(!w.unit(factory)->underConstruction,"factory finishes construction");
                    w.order(builder,700,800,false);
                    for (int tick=0;tick<180;++tick) w.tick(1.f/30.f);
                    w.train(factory,outputType,1);
                    for (int tick=0;tick<600;++tick) { w.player(0).mana=100000;w.tick(1.f/30.f); }
                    bool produced=false;
                    for (const auto& u:w.units()) if (u.type==outputType) produced=true;
                    std::printf("  factory %s: ",fixture[1]);
                    check(produced,"new construction produces through its script-oriented yard");
                }
            }
            {
                // Production doors (2026-10-25, a Creon Absurd base on a generated
                // map): Academy 17 went up 48 px in front of Smithy 5's south door,
                // full width, from that door, by the Smithy's newborn Mechanic, and
                // a Fire Wagon stood behind the working Mechanic for 2.5 minutes.
                const auto* smithy=reg.find("cresmit");
                const auto* academy=reg.find("creacad");
                const auto* mechanic=reg.find("cremech");
                if (!smithy || !academy || !mechanic) check(false,"door fixture types exist");
                else {
                    const auto strips=ai::PlacementProbe::doors(
                        ai::Controller(0,reg,profile,1,ai::Difficulty::Absurd),smithy,1032,1328);
                    std::printf("  cresmit doors:");
                    for (const auto& d:strips) std::printf(" (%.0f,%.0f)-(%.0f,%.0f)",d.x0,d.z0,d.x1,d.z1);
                    std::printf("\n");
                    check(strips.size()==1 && strips[0].z0==1392 && strips[0].z1-strips[0].z0>=80 &&
                          strips[0].x0<=1032-40 && strips[0].x1>=1032+40,
                          "the Smithy's door is its south yard edge, a strip at least 80 px deep");
                    for (bool front:{true,false}) {
                        sim::World w;terrain(w);w.buildNavClasses(reg);
                        w.spawn(smithy,1032,1328,0,0);
                        // The replay's 48 px gap, in front of the door or beside the
                        // Smithy's closed east wall (with the Academy's own door free;
                        // its odd footprint centres on a cell, z 1336 for 1264..1408).
                        const float ax=front ? 1032 : 1032+(7+9)*8+48,az=front ? 1328+(8+9)*8+48 : 1336;
                        const int builder=w.spawn(mechanic,ax+250,az,0,0);
                        for (auto level:{ai::Difficulty::Normal,ai::Difficulty::Absurd}) {
                            ai::Controller controller(0,reg,profile,1,level,{{3400,3400}});
                            const bool ok=ai::PlacementProbe::usable(controller,w,academy,*w.unit(builder),ax,az);
                            check(ok!=front,front ? "a site 48 px in front of a factory door is refused"
                                                  : "the same gap beside the factory's closed wall is still used");
                        }
                    }
                    // The newborn Mechanic at the Smithy's door: its site and its
                    // working position stay out of that door.
                    {
                        sim::World w;terrain(w);w.buildNavClasses(reg);
                        w.spawn(smithy,1032,1328,0,0);
                        const int builder=w.spawn(mechanic,1036,1416,0,0);
                        ai::Controller controller(0,reg,profile,1,ai::Difficulty::Absurd,{{3400,3400}});
                        float x=0,z=0;
                        const bool found=ai::PlacementProbe::placeFrom(controller,w,academy,*w.unit(builder),1036,1416,x,z);
                        const sim::Fixed fx=sim::Fixed::fromFloat(x),fz=sim::Fixed::fromFloat(z);
                        const int ox=sim::footprintOrigin(fx,academy->footX),oz=sim::footprintOrigin(fz,academy->footZ);
                        const bool inDoor=found && float(ox*16)<strips[0].x1 && float((ox+academy->footX)*16)>strips[0].x0 &&
                                          float(oz*16)<strips[0].z1 && float((oz+academy->footZ)*16)>strips[0].z0;
                        std::printf("  academy from the door -> (%.0f,%.0f)\n",x,z);
                        check(found && !inDoor,"a newborn builder does not build in its own factory's door");
                    }
                    // The door-clearing ring: Smithy 12's first ring point (r 176,
                    // 292.5 degrees) lay in Smithy 27's open yard, and eight units
                    // sent there over the game jammed 27's door.
                    {
                        sim::World w;terrain(w);w.buildNavClasses(reg);
                        const float s12x=1048,s12z=1344;   // the replay's (8856,3344), shifted
                        const float s27x=s12x+64,s27z=s12z-208;
                        w.spawn(smithy,s12x,s12z,0,0);
                        w.spawn(smithy,s27x,s27z,0,0);
                        // Door clearing starts each unit's ring probe at angle id%16;
                        // the replay's units started at 13 (292.5 degrees).
                        int filler=0;
                        do filler=w.spawn(mechanic,3600,600+float(filler%64)*40,std::nullopt,1);
                        while (filler%16!=12);
                        const int unit=w.spawn(reg.find("creauto"),s12x+90,s12z+20,0,0);
                        check(unit%16==13,"door-clearing fixture probes the replay's ring point first");
                        ai::Profile none;
                        ai::Controller controller(0,reg,none,1,ai::Difficulty::Normal,{{3400,3400}});
                        const auto cs=think(controller,w);
                        std::vector<decltype(strips)::value_type> blocked;
                        for (const auto& [sx,sz]:{std::pair{s12x,s12z},std::pair{s27x,s27z}}) {
                            blocked.push_back({sx-56,sz-64,sx+56,sz+64});
                            for (const auto& d:ai::PlacementProbe::doors(controller,smithy,sx,sz)) blocked.push_back(d);
                        }
                        bool moved=false,intoYard=false;
                        for (const auto& c:cs) if (c.kind==net::Cmd::Move && c.unitId==unit) {
                            moved=true;
                            const auto* type=w.unit(unit)->type;
                            const float hx=float(type->footX*8),hz=float(type->footZ*8);
                            for (const auto& r:blocked)
                                intoYard=intoYard || (c.x-hx<r.x1 && c.x+hx>r.x0 && c.z-hz<r.z1 && c.z+hz>r.z0);
                            std::printf("  door clearing -> (%.1f,%.1f)\n",c.x,c.z);
                        }
                        check(moved && !intoYard,"door clearing sends no unit into a building's open yard or a factory door");
                    }
                }
            }
            {
                // The same check, an Aramon Priest conjuring a Dragon from inside its
                // Castle's open yard, where it stood for the whole conjure.
                const auto* castle=reg.find("aracastl");
                const auto* priest=reg.find("arapries");
                if (!castle || !priest) check(false,"conjurer fixture types exist");
                else for (bool inYard:{true,false}) {
                    sim::World w;terrain(w);w.buildNavClasses(reg);
                    w.player(0).mana=100000;w.player(0).income=1000;
                    w.spawn(castle,1000,1000,0,0);
                    // The Castle's open rows are its bottom four (z 1048..1112).
                    const int id=w.spawn(priest,1000,inYard ? 1080 : 1400,0,0);
                    ai::Profile p;p.weight["aradrag"]=100;
                    ai::Controller controller(0,reg,p,1,ai::Difficulty::Normal,{{3400,3400}});
                    const auto cs=think(controller,w);
                    const bool conjured=std::any_of(cs.begin(),cs.end(),[&](const auto& c) {
                        return c.unitId==id && c.kind==net::Cmd::Build;
                    });
                    check(conjured!=inYard,inYard ? "a conjurer in a factory's open yard does not conjure there"
                                                  : "the same conjurer clear of the factory conjures");
                }
            }
            if (crusades) {
                // Navy Yard doors (2026-10-25, the same game, crusades balance):
                // Creon's yard 33 at (9016,2392) opens onto a 3-4 row shore pocket at
                // the head of an inlet, its ships circling the yard to get out;
                // yards 35 (9080,4424) and 476 (7880,5816) open onto the sea.
                sim::World w;
                sim::MatchConfig config;config.vfs=&vfs;config.roomMapRevealed=true;   // FOG OF WAR: EXPLORED, the room default
                config.mapPath="~gen1~0a0001000000000000000300030003020000ffff000104000000496e74656c20393331302044656c6c";
                config.slots={{true,4,0,2.0f},{true,0,1,2.0f}};
                config.startMode=sim::StartMode::BySlot;config.startSeed=1853210448u;
                const auto starts=sim::setupMatch(w,reg,config);
                const auto* yard=reg.find("crenavy");
                const auto* iron=reg.find("creiron");
                const auto* mechanic=reg.find("cremech");
                check(starts.size()==2 && yard && iron && mechanic,"generated coast fixture loads");
                if (starts.size()==2 && yard && iron && mechanic) {
                    const auto& absurd=ai::builtinProfile(ai::Difficulty::Absurd);
                    ai::Controller controller(0,reg,absurd,1,ai::Difficulty::Absurd,{starts[1]});
                    const int pocket=ai::PlacementProbe::doorSteps(controller,w,yard,iron,9016,2392);
                    const int open35=ai::PlacementProbe::doorSteps(controller,w,yard,iron,9080,4424);
                    const int open476=ai::PlacementProbe::doorSteps(controller,w,yard,iron,7880,5816);
                    std::printf("  navy yard door steps: (9016,2392)=%d (9080,4424)=%d (7880,5816)=%d\n",
                                pocket,open35,open476);
                    check(pocket==-1 && open35>=0 && open476>=0,
                          "a yard door onto a shore pocket is not clear; doors onto the sea are");
                    // Mechanic 11 ordered yard 33 from here at tick 5780.
                    const int builder=w.spawn(mechanic,9327,3017,0,0);
                    float x=0,z=0;
                    const bool found=ai::PlacementProbe::placeFrom(controller,w,yard,*w.unit(builder),9327,3017,x,z);
                    const int chosen=found ? ai::PlacementProbe::doorSteps(controller,w,yard,iron,x,z) : -3;
                    std::printf("  navy yard from (9327,3017) -> (%.0f,%.0f) door steps %d\n",x,z,chosen);
                    check(found && !(x==9016 && z==2392) && chosen>=0,
                          "the AI sites its Navy Yard with open water out of the door");
                }
            }
            {
                // Start from the actual inland Veruna spawn, with the ordinary
                // land-factory budget already filled. The AI must find a coast,
                // construct the Sea Fort and complete a ship through normal orders.
                sim::World naval;
                sim::MatchConfig config;config.vfs=&vfs;config.roomMapRevealed=true;   // FOG OF WAR: EXPLORED, the room default
                config.mapPath=hpi::findMap(vfs,"Varro Passage");
                config.slots={{true,2,0,1.0f},{true,0,1,1.0f}};
                const auto starts=sim::setupMatch(naval,reg,config);
                naval.spawn(reg.find("verkeep"),256,2800,0,0);
                naval.spawn(reg.find("vercastl"),256,2100,0,0);
                ai::Profile p;p.weight["verasy"]=100;p.weight["verscout"]=100;
                p.weight["verflag"]=10000; // tempting larger ship whose launch footprint is blocked
                ai::Controller controller(0,reg,p,1,ai::Difficulty::Normal,{starts.back()});
                naval.player(0).mana=100000;naval.player(0).income=40;
                const auto cs=think(controller,naval);
                check(std::any_of(cs.begin(),cs.end(),[](const auto& c) {
                    return c.kind==net::Cmd::Build && std::string(c.type)=="verasy";
                }),"Veruna establishes a shipyard beyond the old search radius despite full land production");
                for(const auto& c:cs)sim::applyCommand(naval,reg,c);
                bool ship=false;
                for(uint32_t tick=1;tick<=18000 && !ship;++tick) {
                    naval.player(0).mana=100000;naval.player(0).income=40;
                    controller.tick(naval,tick,[&](const auto& c){sim::applyCommand(naval,reg,c);});
                    naval.tick(1.f/30.f);
                    ship=std::any_of(naval.units().begin(),naval.units().end(),[](const auto& u) {
                        return u.alive() && !u.underConstruction && u.type && u.type->id=="verscout";
                    });
                }
                check(ship,"AI completes a Sea Fort and launches a Scout on Varro Passage");
            }
            {
                sim::World naval;
                sim::MatchConfig config;config.vfs=&vfs;
                config.mapPath=hpi::findMap(vfs,"Varro Passage");
                config.slots={{true,2,0,1.0f},{true,0,1,1.0f}};config.startSeed=1;
                const auto starts=sim::setupMatch(naval,reg,config);
                const auto& p=ai::builtinProfile(ai::Difficulty::Normal);
                ai::Controller controller(0,reg,p,1,ai::Difficulty::Normal,{starts.back()});
                bool ship=false;
                for(uint32_t tick=0;tick<12000 && !ship;++tick) {
                    controller.tick(naval,tick,[&](const auto& c){sim::applyCommand(naval,reg,c);});
                    naval.tick(1.f/30.f);
                    ship=std::any_of(naval.units().begin(),naval.units().end(),[](const auto& u) {
                        return u.player==0 && u.alive() && !u.underConstruction && u.type &&
                            u.type->domain==sim::UnitType::Domain::Water;
                    });
                }
                check(ship,"Normal AI produces boats from the actual opening economy and retail build profile");
            }
            {
                const sim::UnitType* factory=nullptr;const sim::UnitType* ship=nullptr;
                for (const auto& [id,type]:reg.types()) {
                    if (!type.isStructure()) continue;
                    for (const auto& child:reg.buildable(id)) {
                        const auto* t=reg.find(child);
                        if (t && !t->isStructure() && !t->isBuilder && t->domain==sim::UnitType::Domain::Water && t->weapon.damage>0) {
                            factory=&type;ship=t;break;
                        }
                    }
                    if (factory) break;
                }
                check(factory && ship,"retail registry supplies a naval production fixture");
                if (factory && ship) for (bool water : {false,true}) {
                    sim::World naval;terrain(naval,water);naval.buildNavClasses(reg);
                    naval.player(0).mana=100000;naval.player(0).income=100;
                    naval.spawn(factory,2000,800,0,0);
                    ai::Profile p;p.weight[ship->id]=100;
                    ai::Controller controller(0,reg,p,1,ai::Difficulty::Normal,{{2624,800}});
                    const auto cs=think(controller,naval);
                    bool train=std::any_of(cs.begin(),cs.end(),[](const auto& c){return c.kind==net::Cmd::Train;});
                    check(train==water,"naval production requires usable water and an accessible enemy shore");
                }
            }
            sim::World w;terrain(w);w.buildNavClasses(reg);
            bool defaults=true;
            for (const auto& [id,type]:reg.types()) {
                int uid=w.spawn(&type,800,800,0,0);const auto* u=w.unit(uid);
                defaults &= u->moveState==type.defaultMove && u->fireState==type.defaultFire;
            }
            check(defaults,"all shipped units receive their retail stance defaults in this balance");
        }
    }
    std::printf("ai_behavior_test: %d failures\n",fails);return fails ? 1 : 0;
}
