// Naval sight must not confuse water depth with a wall, including shore attacks.
// With --legion the moving cases (attack-move, attack) run in
// Legion mode on a placement plane, as a loaded map has, and must be routed
// by Legion.
#include "hpi/hpi.h"
#include "sim/legion.h"
#include "sim/matchsetup.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string_view>

int main(int argc,char**argv) {
    if (argc!=2 && argc!=3) return 2;
    const bool legion=argc==3 && std::string_view(argv[2])=="--legion";
    if (argc==3 && !legion) return 2;
    auto vfs=ta::hpi::mountRetailRoot(argv[1]);
    int failures=0,cases=0;
    // Legion plans boats on the map's placement plane (feature-free here).
    auto legionMode=[&](ta::sim::World& w,int width,int height) {
        if (!legion) return;
        w.setPathfindingMode(ta::sim::PathfindingMode::Legion);w.setPathService(true);
        w.setMapPlacementFeatures(std::vector<uint16_t>(size_t(width)*height,0xffff),{});
    };
    auto served=[&](ta::sim::World& w,int id,bool& seen) {
        if (auto* l=w.legionNavigator(); l && w.unit(id))
            seen|=l->mission(*w.unit(id))!=ta::sim::LegionMission::None;
    };
    auto check=[&](bool ok,const char* label) {
        std::printf("[%s] %s\n",ok?"PASS":"FAIL",label);
        if (!ok) ++failures;
    };
    {
        ta::sim::TypeRegistry registry;ta::sim::setupRegistry(registry,vfs);
        std::vector<std::string> ships;
        for (const auto& [id,t]:registry.types())
            if (t.domain==ta::sim::UnitType::Domain::Water && !t.isStructure() &&
                t.weapon.damage>0) ships.push_back(id);
        std::sort(ships.begin(),ships.end());
        for (const auto& name:ships) for (bool shore:{false,true}) for (int order=0;order<3;++order) {
            const auto* type=registry.find(name);
            // Stay outside minimum range and inside both range and the idle leash.
            const float distance=std::max(200.f,float(type->weapon.minRange)+80.f);
            if (distance>float(type->weapon.range)*0.85f ||
                (order==0 && type->leash>0 && distance>type->leash)) continue;
            ta::sim::World w;
            std::vector<uint8_t> heights(512*256,10);
            if (shore) for (int z=0;z<256;++z) for (int x=256;x<512;++x)
                heights[size_t(z)*512+x]=70;
            w.setTerrain(heights,512,256,64);w.buildNavClasses(registry);legionMode(w,512,256);
            auto victim=*registry.find(shore?"armguard":"armtship");
            victim.maxHp=30000;victim.healTime=0;victim.weapon.damage=0;
            victim.weapons.clear();victim.maxVel={};
            // A shore target is a Guardian (ShootMe=1): a human player's sweep
            // takes only a structure with shootme (40f330, UnitDef+0x260 bit
            // 15), so a solar collector would be left alone.
            const float tx=shore?4160.f:3800.f,tz=1800.f;
            // A fight-move (order 1) takes only what its player sees (Move
            // response mode -1, 4d8370), so it starts 300 px past the nearer
            // of its range and its sight; an explicit attack past its range.
            const float initial=order==0?distance:order==1?
                float(std::min<int>(type->weapon.range,type->sight))+300.f:float(type->weapon.range)+300.f;
            const int a=w.spawn(type,tx-initial,tz,0,0),b=w.spawn(&victim,tx,tz,0,1);
            
            if (order==1) w.attackMove(a,shore?4048.f:tx,tz,false);
            if (order==2) w.attack(a,b,false);
            bool fired=false,routed=false,seen=false;
            for (int tick=0;tick<1800 && !fired;++tick) {
                w.tick(1.f/30.f);fired=w.unit(a)->justFired;served(w,a,routed);
                seen=seen || w.playerSees(0,*w.unit(b));
            }
            const float actual=ta::sim::fxLen(w.unit(a)->x-w.unit(b)->x,w.unit(a)->z-w.unit(b)->z).toFloat();
            std::printf("unit=%s shore=%d order=%d fired=%d seen=%d distance=%.1f range=%d\n",
                name.c_str(),shore,order,fired,seen,actual,type->weapon.range);
            // Idle, the ship has only Standby's scan and the sweep, and both
            // take only an enemy its player sees (4d8370, 40f330: list +5): a
            // submarine whose sight falls short of the target leaves it alone.
            if (order==0 && !seen) { ++cases;check(!fired,"an idle ship leaves an enemy its player cannot see");continue; }
            // So does a fight-move's scan: an enemy its player never sees is
            // left alone. One it sees inside its minimum range it backs away
            // from and then shoots (engine deviation; retail never fired).
            // No TA weapon has a minrange, so that branch guards modded data.
            if (order==1 && !seen) {
                ++cases;check(!fired,"a fight-moving ship leaves an enemy its player never sees");
                continue;
            }
            if (order==1 && type->sight<=type->weapon.minRange) {
                ++cases;check(fired && actual>=float(type->weapon.minRange)-8.f,
                              "a fight-moving ship backs out of its minimum range to shoot what it saw");
                continue;
            }
            ++cases;check(fired,"ship acquires, attack-moves, and attacks across open water/shore");
            if (order==0) check(actual>=distance-20,"in-range ship turns and fires without charging the target");
            if (legion && order>0) check(routed,"Legion routes the ship's approach");
        }
        // Shore defenders must also be able to see and fire at ships. Face the
        // fixture toward its target: this check isolates the reverse sight ray.
        {
            ta::sim::World w;
            std::vector<uint8_t> heights(128*128,10);
            for (int z=0;z<128;++z) for (int x=64;x<128;++x) heights[size_t(z)*128+x]=70;
            w.setTerrain(heights,128,128,64);w.buildNavClasses(registry);
            auto defender=*registry.find("armroy");defender.domain=ta::sim::UnitType::Domain::Ground;
            defender.maxVel={};defender.weapon.lobPreferred=false;
            for (auto& weapon:defender.weapons) weapon.lobPreferred=false;
            auto victim=*registry.find("armtship");victim.maxHp=30000;
            victim.weapon.damage=0;victim.weapons.clear();victim.maxVel={};
            const int a=w.spawn(&defender,1100,800,-1.570796327f,0);
            // 200 px off, inside the Crusader's 330 sight: a structure fires only
            // through the sweep (Guard_NoMove picks nothing, 401a90), at what
            // its player sees (40f330, list +5); and a second unit of the
            // defender's, passive, as the sweep's walk skips a player's only
            // unit.
            w.spawn(&victim,900,800,0,1);
            const int mate=w.spawn(registry.find("armpw"),1500,1500,0,0);
            w.unit(mate)->fireState=0;w.unit(mate)->moveState=0;
            bool fired=false;
            for (int tick=0;tick<300 && !fired;++tick) { w.tick(1.f/30.f);fired=w.unit(a)->justFired; }
            check(fired,"shore defender acquires and fires at a ship across water");
        }
        // A straight-shooting ship must still respect an actual obstacle. This
        // exercises acquisition and explicit fire gates, with default ground
        // combat and ground navigation checked separately below.
        for (bool cliff:{false,true}) for (bool attack:{false,true}) {
            ta::sim::World w;
            std::vector<uint8_t> heights(128*128,10);
            std::vector<uint16_t> features(128*128,0xFFFF);
            for (int z=0;z<128;++z) for (int x=53;x<59;++x)
                if (cliff) heights[size_t(z)*128+x]=220;
                else features[size_t(z)*128+x]=0xFFFC;
            w.setTerrain(heights,128,128,64,&features);w.buildNavClasses(registry);
            // The Crusader's Heavy Cannon is Ballistic, which takes no 2D
            // line test since 0.9.3 (its arc decides): make its weapons
            // line-of-sight shots, the kind the test still applies to.
            auto type=*registry.find("armroy");type.weapon.lobPreferred=false;
            for (auto& wp:type.weapons) { wp.lobPreferred=false;wp.ballistic=false;wp.beam=true; }
            type.weapon=type.weapons.front();
            auto victim=*registry.find("armtship");victim.maxHp=30000;
            victim.weapon.damage=0;victim.weapons.clear();victim.maxVel={};
            const int a=w.spawn(&type,800,800,1.570796327f,0),b=w.spawn(&victim,1000,800,0,1);
            if (attack) w.attack(a,b,false);
            bool fired=false;
            int shots=0,lastShot=-1;
            const float hp0=w.unit(b)->hp.toFloat();
            for (int tick=0;tick<(cliff?900:300);++tick) {
                w.tick(1.f/30.f);fired|=w.unit(a)->justFired;
                if (w.unit(a)->justFired) { ++shots;lastShot=tick; }
            }
            if (cliff) {
                // Raised land is no wall to the 2D sight test (only the
                // obstacle overlay is; World::combatLineOfSight): TotalA.exe
                // fires on range alone and its shots stop on the land
                // (0x49b090). Three of them on the cliff write the target off
                // from here (noteShotImpact), and the ship fires no more.
                std::printf("cliff attack=%d: %d shots, last at tick %d, target hp lost %.0f, blocked=%d\n",attack,shots,
                            lastShot,hp0-w.unit(b)->hp.toFloat(),int(w.unit(a)->blockedShots));
                check(shots<=3 && lastShot<300 && w.unit(b)->hp.toFloat()==hp0 && w.unit(a)->blockedShots>=3,
                      "raised land stops the naval shots, and three on it write the target off");
            } else check(!fired,"explicit wall blocks naval acquisition and fire");
            if (!cliff) {
                w.blockCells(53,0,6,128,false);
                for (int tick=0;tick<600 && !fired;++tick) { w.tick(1.f/30.f);fired=w.unit(a)->justFired; }
                check(fired,"removing a shared obstacle immediately opens naval sight");
            }
        }
    }
    // A Crusader's Heavy Cannon (Ballistic, range 660) against a Guardian
    // (3x3, ShootMe) on an island (first seen in 0.9.1 with a Kingdoms
    // mortar sub). 52bc60 rules a target out of range when the shell's arc
    // cannot reach it and tests the centre distance with no footprint pad;
    // our footprint pad let ships just past their range lob shells flat into
    // the water at their own bow. Never in Legion: no movement here
    // that Legion routes differently matters to what is checked.
    if (!legion) {
        ta::sim::TypeRegistry registry;ta::sim::setupRegistry(registry,vfs);
        const auto* sub=registry.find("armroy");
        const auto* mana=registry.find("armguard");
        check(sub && mana && sub->weapon.ballistic,"shipped armroy (ballistic) and armguard load");
        if (sub && mana) {
            const float range=float(sub->weapon.range);
            // island: cells [isl0,isl1) square, land 66 over sea level 64.
            struct Shot { int fired=0,hits=0,bowSplashes=0,acquired=0; bool holding=false; float closest=1e9f,moved=0,lastMoves=0; };
            auto run=[&](float distance,int moveState,int islandHalf,int ticks,uint8_t land=66) {
                ta::sim::World w;
                const int size=256;
                std::vector<uint8_t> heights(size_t(size)*size,10);
                const int c=size/2;
                for (int z=c-islandHalf;z<c+islandHalf;++z) for (int x=c-islandHalf;x<c+islandHalf;++x)
                    heights[size_t(z)*size+x]=land;
                w.setTerrain(heights,size,size,64);w.buildNavClasses(registry);
                auto victim=*mana;victim.maxHp=60000;victim.healTime=0;
                const float tx=float(c*16),tz=float(c*16);
                const int b=w.spawn(&victim,tx,tz,0,1);
                // A spotter of the ship's off the island: its sight is 330
                // and its range 660, and the sweep and the scans take only
                // what the player sees.
                static ta::sim::UnitType spotter;
                spotter=*sub;spotter.id="spotter";spotter.weapons.clear();spotter.weapon={};
                spotter.sight=400;spotter.maxVel={};
                w.spawn(&spotter,tx,tz-240,0,0);
                const int a=w.spawn(sub,tx-distance,tz,1.570796327f,0);
                w.unit(a)->moveState=moveState;
                
                Shot out;
                std::vector<std::pair<float,float>> trail;
                bool attacking=false;
                for (int tick=0;tick<ticks;++tick) {
                    w.tick(1.f/30.f);
                    const auto* u=w.unit(a);
                    if (!u || !w.unit(b)) break;
                    out.fired+=u->justFired;
                    const bool now=!u->orders.empty() && u->orders.front().targetId==b;
                    out.acquired+=now && !attacking;
                    attacking=out.holding=now;
                    trail.push_back({u->x.toFloat(),u->z.toFloat()});
                    out.closest=std::min(out.closest,ta::sim::fxLen(u->x-w.unit(b)->x,u->z-w.unit(b)->z).toFloat());
                    for (const auto& h:w.hits()) {
                        if (h.fromPlayer!=0) continue;
                        if (h.victimId==b) { ++out.hits;continue; }
                        const float dx=h.x-u->x.toFloat(),dz=h.z-u->z.toFloat();
                        if (dx*dx+dz*dz<300.f*300.f) ++out.bowSplashes;
                    }
                }
                for (size_t i=1;i<trail.size();++i) {
                    const float step=std::hypot(trail[i].first-trail[i-1].first,trail[i].second-trail[i-1].second);
                    out.moved+=step;
                    if (i+600>=trail.size()) out.lastMoves+=step;
                }
                return out;
            };
            auto show=[&](const char* what,float d,const Shot& s) {
                std::printf("armroy %-27s from %.0f px (range %.0f): fired=%d hits=%d bow-splashes=%d acquired=%d holding=%d closest=%.0f moved=%.0f last20s=%.0f\n",
                    what,d,range,s.fired,s.hits,s.bowSplashes,s.acquired,int(s.holding),s.closest,s.moved,s.lastMoves);
            };
            // In arc and in range: it hits from where it lies.
            const Shot near=run(range-60,0,8,900);show("hold, near",range-60,near);
            check(near.hits>0 && near.bowSplashes==0,"a ship well inside its range lobs onto the Guardian");
            // Past the range: it never lobs a shell flat into the water at its
            // bow. Idle, even on Roam, it does not close in: Standby's scan
            // (4d8370 -> 412620) takes a ranged slot's target only within the
            // slot's range (52ad60), so a seen enemy just past it is left be.
            for (float d:{range+30,range+40,range+48}) {
                const Shot open=run(d,2,8,1800);show("roam, open water",d,open);
                check(open.bowSplashes==0,"no flat shell into the water from beyond the arc's reach");
                check(open.acquired==0 && open.hits==0,"an idle ship does not close on a seen enemy past its range");
            }
            // The band where the origin arc reaches but the muzzle-to-SweetSpot
            // solve may not: a shell is lobbed onto the target or not at all.
            for (float d:{range-15,range-5,range-1}) {
                const Shot band=run(d,0,8,900);show("hold, edge of range",d,band);
                check(band.bowSplashes==0 && (band.fired==0 || band.hits>0),
                      "at the edge of its range a ship hits the Guardian or holds its fire");
            }
            // A Guardian on a raised island, in the ship's 2D range: the
            // origin arc (52bc60) may reach while the muzzle-to-SweetSpot
            // solve does not, so the aimed ship has no shot. A Hold ship cannot
            // close in: after kBlockedShots reloads without an arc it writes
            // the target off and drops it (before: it held it for ever with
            // its slots off auto), and it is not acquired again from there.
            for (uint8_t land:{uint8_t(70),uint8_t(80),uint8_t(100),uint8_t(120)}) {
                const Shot high=run(range-5,0,8,900,land);
                std::printf("  (island height %d)\n",int(land));show("hold, raised island",range-5,high);
                check(high.bowSplashes==0 && high.acquired<=1 && (high.hits>0 || !high.holding),
                      "a Hold ship with no arc drops the target and does not take it up again");
            }
            // Past the range with the shore in the way: it cannot close, so it
            // holds: no shell, and no churning at the shoreline.
            for (float d:{range+30,range+48}) {
                const Shot shore=run(d,2,int(range+8)/16,1800);show("roam, shore-bound",d,shore);
                check(shore.fired==0 && shore.bowSplashes==0,"a ship kept past its range by the shore does not fire");
                check(shore.lastMoves<64,"a shore-bound ship holds instead of churning");
            }
        }
    }
    // A submerged Triton (an amphibious tank on the sea floor, its SweetSpot
    // under the surface) is no target for a battleship's cannons, which would
    // strike the surface short of it: the ship neither picks it nor is ordered
    // onto it (engine deviation, World::aimPointSubmerged). Standing in the
    // shallows of a shore, its SweetSpot above the surface, it is shot as
    // before. (The Battleship carries no depth charge, which would reach it.)
    if (!legion) {
        ta::sim::TypeRegistry registry;ta::sim::setupRegistry(registry,vfs);
        const auto* ship=registry.find("armbats");
        auto mer=*registry.find("armcroc");
        check(ship && registry.find("armcroc"),"the Battleship and Triton load");
        if (ship) for (bool shallow:{false,true}) {
            mer.weapons.clear();mer.weapon.damage=0;mer.maxVel={};mer.healTime=0;mer.maxHp=100000;
            ta::sim::World w;
            std::vector<uint8_t> heights(256*256,10);
            // The shallows: ground 1 under the sea, so the tank stands on it
            // with its SweetSpot above the surface.
            if (shallow) for (int z=0;z<256;++z) for (int x=128;x<256;++x) heights[size_t(z)*256+x]=57;
            w.setTerrain(heights,256,256,58);w.buildNavClasses(registry);
            const int a=w.spawn(ship,1900,2000,0,0),b=w.spawn(&mer,2200,2000,0,1);
            for (int i=0;i<60;++i) w.tick(1.f/30);
            const auto hp=w.unit(b)->hp;
            w.attack(a,b,false);
            bool targeted=false;
            for (int i=0;i<600;++i) {
                w.tick(1.f/30);
                for (int s=0;s<3;++s) targeted|=w.unit(a)->slotTarget[size_t(s)]==b;
            }
            const bool hurt=!w.unit(b) || w.unit(b)->hp<hp;
            if (shallow) check(hurt,"a ship shoots a Triton standing in the shallows");
            else check(!hurt && !targeted && (w.unit(a)->orders.empty() || w.unit(a)->orders.front().targetId!=b),
                       "a ship neither targets nor is ordered onto a submerged Triton");
        }
    }
    // No endless shots (engine deviations, 2026-11-12). A submarine facing
    // either way is hit by a destroyer's depth charges; a ship whose shells
    // stop in the bank under a unit on the shore sails to a spot with a clear
    // shot (its chord starts at its own height) or, finding none, gives the
    // attack up.
    if (!legion) {
        ta::sim::TypeRegistry registry;ta::sim::setupRegistry(registry,vfs);
        auto still=[&](const char* id) {
            auto t=*registry.find(id);
            t.weapons.clear();t.weapon.damage=0;t.maxVel={};t.healTime=0;t.maxHp=100000;
            return t;
        };
        auto sea=[&](bool shore) {
            std::vector<uint8_t> heights(256*256,10);
            if (shore) for (int z=0;z<256;++z) for (int x=128;x<256;++x) heights[size_t(z)*256+x]=70;
            return heights;
        };
        auto hurtBy=[&](const char* attacker,ta::sim::UnitType& victim,float ax,float tx,bool shore,float heading) {
            ta::sim::World w;w.setTerrain(sea(shore),256,256,58);w.buildNavClasses(registry);
            const int a=w.spawn(registry.find(attacker),ax,2000,0,0),b=w.spawn(&victim,tx,2000,heading,1);
            for (int i=0;i<60;++i) w.tick(1.f/30);
            const auto hp=w.unit(b)->hp;
            w.attack(a,b,false);
            int orderTicks=0;
            for (int i=0;i<1200;++i) {
                w.tick(1.f/30);
                if (!w.unit(b)) break;
                orderTicks+=!w.unit(a)->orders.empty() && w.unit(a)->orders.front().targetId==b;
            }
            return std::pair{!w.unit(b) || w.unit(b)->hp<hp,orderTicks};
        };
        for (float heading:{0.f,1.5708f,3.1416f,4.7124f}) {
            auto sub=still("corsub");
            check(hurtBy("armroy",sub,1700,1850,false,heading).first,"a destroyer's depth charges hit a Snake submarine");
        }
        auto peewee=still("armpw");
        const auto [hurt,held]=hurtBy("armroy",peewee,2144-150,2144,true,0);
        check(hurt || held<300,"a ship whose shells stop in the bank moves to a clear shot or gives the attack up");
    }
    // The naval combat mask changes no movement cell or ground raycast.
    ta::sim::World w;w.setTerrain(std::vector<uint8_t>(64*64,10),64,64,64);
    ta::sim::UnitType boat;boat.domain=ta::sim::UnitType::Domain::Water;
    check(!w.nav().losBetween(200,200,600,200),"ground-only sight across deep water is unchanged");
    check(!w.nav().walkable(20,20) && w.navFor(&boat).walkable(20,20),"ground and boat movement grids are unchanged");
    std::printf("naval combat: %d cases, %d failures\n",cases,failures);
    return failures?1:0;
}
