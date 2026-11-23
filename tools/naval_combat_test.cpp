// Naval sight must not confuse water depth with a wall, including shore attacks.
// With --legion the moving cases (attack-move, attack, melee closing) run in
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
    auto vfs=tak::hpi::mountRetailRoot(argv[1]);
    int failures=0,cases=0;
    // Legion plans boats on the map's placement plane (feature-free here).
    auto legionMode=[&](tak::sim::World& w,int width,int height) {
        if (!legion) return;
        w.setPathfindingMode(tak::sim::PathfindingMode::Legion);w.setPathService(true);
        w.setMapPlacementFeatures(std::vector<uint16_t>(size_t(width)*height,0xffff),{});
    };
    auto served=[&](tak::sim::World& w,int id,bool& seen) {
        if (auto* l=w.legionNavigator(); l && w.unit(id))
            seen|=l->mission(*w.unit(id))!=tak::sim::LegionMission::None;
    };
    auto check=[&](bool ok,const char* label) {
        std::printf("[%s] %s\n",ok?"PASS":"FAIL",label);
        if (!ok) ++failures;
    };
    for (bool crusades:{false,true}) {
        tak::sim::TypeRegistry registry;tak::sim::setupRegistry(registry,vfs,crusades);
        std::vector<std::string> ships;
        for (const auto& [id,t]:registry.types())
            if (t.domain==tak::sim::UnitType::Domain::Water && !t.isStructure() &&
                t.weapon.damage>0 && !t.weapon.melee) ships.push_back(id);
        std::sort(ships.begin(),ships.end());
        for (const auto& name:ships) for (bool shore:{false,true}) for (int order=0;order<3;++order) {
            const auto* type=registry.find(name);
            // Stay outside minimum range and inside both range and the idle leash.
            const float distance=std::max(200.f,float(type->weapon.minRange)+80.f);
            if (distance>float(type->weapon.range)*0.85f ||
                (order==0 && type->leash>0 && distance>type->leash)) continue;
            tak::sim::World w;
            std::vector<uint8_t> heights(512*256,10);
            if (shore) for (int z=0;z<256;++z) for (int x=256;x<512;++x)
                heights[size_t(z)*512+x]=70;
            w.setTerrain(heights,512,256,64);w.buildNavClasses(registry);legionMode(w,512,256);
            auto victim=*registry.find(shore?"arakeep":"aratrans");
            victim.maxHp=30000;victim.healTime=0;victim.weapon.damage=0;
            victim.weapons.clear();victim.maxVel={};
            const float tx=shore?4160.f:3800.f,tz=1800.f;
            // A fight-move (order 1) takes only what its player sees (Move
            // response mode -1, 4d8370), so it starts 300 px past the nearer
            // of its range and its sight; an explicit attack past its range.
            const float initial=order==0?distance:order==1?
                float(std::min<int>(type->weapon.range,type->sight))+300.f:float(type->weapon.range)+300.f;
            const int a=w.spawn(type,tx-initial,tz,0,0),b=w.spawn(&victim,tx,tz,0,1);
            w.player(0).mana=30000;
            if (order==1) w.attackMove(a,shore?4048.f:tx,tz,false);
            if (order==2) w.attack(a,b,false);
            bool fired=false,routed=false,seen=false;
            for (int tick=0;tick<1800 && !fired;++tick) {
                w.tick(1.f/30.f);fired=w.unit(a)->justFired;served(w,a,routed);
                seen=seen || w.playerSees(0,*w.unit(b));
            }
            const float actual=tak::sim::fxLen(w.unit(a)->x-w.unit(b)->x,w.unit(a)->z-w.unit(b)->z).toFloat();
            std::printf("balance=%d unit=%s shore=%d order=%d fired=%d seen=%d distance=%.1f range=%d\n",
                crusades,name.c_str(),shore,order,fired,seen,actual,type->weapon.range);
            // Idle, the ship has only Standby's scan and the sweep, and both
            // take only an enemy its player sees (4d8370, 40f330: list +5): a
            // submarine whose sight falls short of the target leaves it alone.
            if (order==0 && !seen) { ++cases;check(!fired,"an idle ship leaves an enemy its player cannot see");continue; }
            // So does a fight-move's scan: an enemy its player never sees is
            // left alone. One it sees inside its minimum range (a Crusades
            // submarine's sight, 80, falls short of its minimum range, 180)
            // it backs away from and then shoots (engine deviation; retail
            // never fired).
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
        // Melee water creatures still close to adjacency rather than using their
        // misleading authored ranged radius. No shoreline target beyond reach.
        for (const auto& [name,type]:registry.types()) {
            if (type.domain!=tak::sim::UnitType::Domain::Water || type.isStructure() ||
                type.weapon.damage<=0 || !type.weapon.melee) continue;
            for (int order=0;order<3;++order) {
                tak::sim::World w;
                w.setTerrain(std::vector<uint8_t>(128*128,10),128,128,64);w.buildNavClasses(registry);legionMode(w,128,128);
                auto victim=*registry.find("aratrans");victim.maxHp=30000;victim.healTime=0;
                victim.weapon.damage=0;victim.weapons.clear();victim.maxVel={};
                const float distance=order==0?40.f:500.f;
                const int a=w.spawn(&type,1000-distance,1000,0,0),b=w.spawn(&victim,1000,1000,0,1);
                if (order==1) w.attackMove(a,1000,1000,false);
                if (order==2) w.attack(a,b,false);
                bool fired=false,routed=false;
                for (int tick=0;tick<1800 && !fired;++tick) { w.tick(1.f/30.f);fired=w.unit(a)->justFired;served(w,a,routed); }
                std::printf("melee balance=%d unit=%s order=%d fired=%d\n",crusades,name.c_str(),order,fired);
                ++cases;check(fired,"melee water creature closes and attacks");
                // An explicit attack on this mover-less target is Attack_Melee's
                // rectangle approach (protocol 307), which the original-style
                // navigator serves in both modes; the attack-move is Legion's.
                if (legion && order==1) check(routed,"Legion routes the water creature's approach");
            }
        }
        // Shore defenders must also be able to see and fire at ships. Face the
        // fixture toward its target: this check isolates the reverse sight ray.
        {
            tak::sim::World w;
            std::vector<uint8_t> heights(128*128,10);
            for (int z=0;z<128;++z) for (int x=64;x<128;++x) heights[size_t(z)*128+x]=70;
            w.setTerrain(heights,128,128,64);w.buildNavClasses(registry);
            auto defender=*registry.find("verflag");defender.domain=tak::sim::UnitType::Domain::Ground;
            defender.maxVel={};defender.weapon.lobPreferred=false;
            for (auto& weapon:defender.weapons) weapon.lobPreferred=false;
            auto victim=*registry.find("aratrans");victim.maxHp=30000;
            victim.weapon.damage=0;victim.weapons.clear();victim.maxVel={};
            const int a=w.spawn(&defender,1100,800,-1.570796327f,0);
            // 200 px off, inside the flag's 230 sight: a structure fires only
            // through the sweep (Guard_NoMove picks nothing, 401a90), at what
            // its player sees (40f330, list +5); and a second unit of the
            // defender's, passive, as the sweep's walk skips a player's only
            // unit.
            w.spawn(&victim,900,800,0,1);
            const int mate=w.spawn(registry.find("verman"),1500,1500,0,0);
            w.unit(mate)->fireState=0;w.unit(mate)->moveState=0;
            bool fired=false;
            for (int tick=0;tick<300 && !fired;++tick) { w.tick(1.f/30.f);fired=w.unit(a)->justFired; }
            check(fired,"shore defender acquires and fires at a ship across water");
        }
        // A straight-shooting ship must still respect an actual obstacle. This
        // exercises acquisition and explicit fire gates, with default ground
        // combat and ground navigation checked separately below.
        for (bool cliff:{false,true}) for (bool attack:{false,true}) {
            tak::sim::World w;
            std::vector<uint8_t> heights(128*128,10);
            std::vector<uint16_t> features(128*128,0xFFFF);
            for (int z=0;z<128;++z) for (int x=53;x<59;++x)
                if (cliff) heights[size_t(z)*128+x]=220;
                else features[size_t(z)*128+x]=0xFFFC;
            w.setTerrain(heights,128,128,64,&features);w.buildNavClasses(registry);
            // The flagship's Bow and Arrows are Ballistic, which takes no 2D
            // line test since 0.9.3 (its arc decides): make them a line-of-sight
            // shot, the kind the test still applies to.
            auto type=*registry.find("verflag");type.weapon.lobPreferred=false;
            for (auto& wp:type.weapons) { wp.lobPreferred=false;wp.ballistic=false;wp.beam=wp.straight=true; }
            type.weapon=type.weapons.front();
            auto victim=*registry.find("aratrans");victim.maxHp=30000;
            victim.weapon.damage=0;victim.weapons.clear();victim.maxVel={};
            const int a=w.spawn(&type,800,800,1.570796327f,0),b=w.spawn(&victim,1000,800,0,1);
            if (attack) w.attack(a,b,false);
            bool fired=false;
            for (int tick=0;tick<300;++tick) { w.tick(1.f/30.f);fired|=w.unit(a)->justFired; }
            check(!fired,cliff?"raised land blocks naval acquisition and fire":"explicit wall blocks naval acquisition and fire");
            if (!cliff) {
                w.blockCells(53,0,6,128,false);
                for (int tick=0;tick<600 && !fired;++tick) { w.tick(1.f/30.f);fired=w.unit(a)->justFired; }
                check(fired,"removing a shared obstacle immediately opens naval sight");
            }
        }
    }
    // A Creon Submersible's Marine Mortar (Ballistic, lobpreferred,
    // gravityadjustment 2.8) against an Aramon Divine Lodestone (3x3) on an
    // island, as in the user's 0.9.1 game. 52bc60 rules a target out of range
    // when the shell's arc cannot reach it and tests the centre distance with
    // no footprint pad; our footprint pad let subs 822-848 px off lob shells
    // flat into the water at their own bow. Never in Legion: no movement here
    // that Legion routes differently matters to what is checked.
    if (!legion) {
        tak::sim::TypeRegistry registry;tak::sim::setupRegistry(registry,vfs,true);   // range 800, as in the game
        const auto* sub=registry.find("cresubm");
        const auto* mana=registry.find("aramana");
        check(sub && mana && sub->weapon.ballistic,"shipped cresubm (ballistic) and aramana load");
        if (sub && mana) {
            const float range=float(sub->weapon.range);
            // island: cells [isl0,isl1) square, land 66 over sea level 64.
            struct Shot { int fired=0,hits=0,bowSplashes=0,acquired=0; bool holding=false; float closest=1e9f,moved=0,lastMoves=0; };
            auto run=[&](float distance,int moveState,int islandHalf,int ticks,uint8_t land=66) {
                tak::sim::World w;
                const int size=256;
                std::vector<uint8_t> heights(size_t(size)*size,10);
                const int c=size/2;
                for (int z=c-islandHalf;z<c+islandHalf;++z) for (int x=c-islandHalf;x<c+islandHalf;++x)
                    heights[size_t(z)*size+x]=land;
                w.setTerrain(heights,size,size,64);w.buildNavClasses(registry);
                auto victim=*mana;victim.maxHp=60000;victim.healTime=0;
                const float tx=float(c*16),tz=float(c*16);
                const int b=w.spawn(&victim,tx,tz,0,1);
                // A spotter of the sub's off the island: the sub's sight is 80
                // and its range 800, and the sweep and the scans take only
                // what the player sees.
                static tak::sim::UnitType spotter;
                spotter=*sub;spotter.id="spotter";spotter.weapons.clear();spotter.weapon={};
                spotter.sight=400;spotter.maxVel={};
                w.spawn(&spotter,tx,tz-240,0,0);
                const int a=w.spawn(sub,tx-distance,tz,1.570796327f,0);
                w.unit(a)->moveState=moveState;
                w.player(0).mana=30000;
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
                    out.closest=std::min(out.closest,tak::sim::fxLen(u->x-w.unit(b)->x,u->z-w.unit(b)->z).toFloat());
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
                std::printf("cresubm %-26s from %.0f px (range %.0f): fired=%d hits=%d bow-splashes=%d acquired=%d holding=%d closest=%.0f moved=%.0f last20s=%.0f\n",
                    what,d,range,s.fired,s.hits,s.bowSplashes,s.acquired,int(s.holding),s.closest,s.moved,s.lastMoves);
            };
            // In arc and in range: it hits from where it lies.
            const Shot near=run(range-60,0,8,900);show("hold, near",range-60,near);
            check(near.hits>0 && near.bowSplashes==0,"a sub well inside its range lobs onto the lodestone");
            // Past the range: it never lobs a shell flat into the water at its
            // bow. Idle, even on Roam, it does not close in: Standby's scan
            // (4d8370 -> 412620) takes a ranged slot's target only within the
            // slot's range (52ad60), so a seen enemy just past it is left be.
            for (float d:{range+30,range+40,range+48}) {
                const Shot open=run(d,2,8,1800);show("roam, open water",d,open);
                check(open.bowSplashes==0,"no flat shell into the water from beyond the arc's reach");
                check(open.acquired==0 && open.hits==0,"an idle sub does not close on a seen enemy past its range");
            }
            // The band where the origin arc reaches but the muzzle-to-SweetSpot
            // solve may not: a shell is lobbed onto the target or not at all.
            for (float d:{range-15,range-5,range-1}) {
                const Shot band=run(d,0,8,900);show("hold, edge of range",d,band);
                check(band.bowSplashes==0 && (band.fired==0 || band.hits>0),
                      "at the edge of its range a sub hits the lodestone or holds its fire");
            }
            // A lodestone on a raised island, in the sub's 2D range: the
            // origin arc (52bc60) may reach while the muzzle-to-SweetSpot
            // solve does not, so the aimed sub has no shot. A Hold sub cannot
            // close in: after kBlockedShots reloads without an arc it writes
            // the target off and drops it (before: it held it for ever with
            // its slots off auto), and it is not acquired again from there.
            for (uint8_t land:{uint8_t(70),uint8_t(80),uint8_t(100),uint8_t(120)}) {
                const Shot high=run(range-5,0,8,900,land);
                std::printf("  (island height %d)\n",int(land));show("hold, raised island",range-5,high);
                check(high.bowSplashes==0 && high.acquired<=1 && (high.hits>0 || !high.holding),
                      "a Hold sub with no arc drops the target and does not take it up again");
            }
            // Past the range with the shore in the way: it cannot close, so it
            // holds: no shell, and no churning at the shoreline.
            for (float d:{range+30,range+48}) {
                const Shot shore=run(d,2,int(range+8)/16,1800);show("roam, shore-bound",d,shore);
                check(shore.fired==0 && shore.bowSplashes==0,"a sub kept past its range by the shore does not fire");
                check(shore.lastMoves<64,"a shore-bound sub holds instead of churning");
            }
        }
    }
    // A swimming Mer Warrior (a wading body at sea - waterline, its SweetSpot
    // as deep) is no target for a flagship's arrows, which would strike the
    // surface short of it: the ship neither picks it nor is ordered onto it
    // (engine deviation, World::aimPointSubmerged). Standing in the shallows
    // of a shore, its SweetSpot above the surface, it is shot as before.
    if (!legion) {
        tak::sim::TypeRegistry registry;tak::sim::setupRegistry(registry,vfs,true);
        const auto* ship=registry.find("verflag");
        auto mer=*registry.find("vermer");
        check(ship && registry.find("vermer"),"the Crusades flagship and Mer Warrior load");
        if (ship) for (bool shallow:{false,true}) {
            mer.weapons.clear();mer.weapon.damage=0;mer.maxVel={};mer.healTime=0;
            tak::sim::World w;
            std::vector<uint8_t> heights(256*256,10);
            // The shallows: ground 4 under the sea, so the body stands on it.
            if (shallow) for (int z=0;z<256;++z) for (int x=128;x<256;++x) heights[size_t(z)*256+x]=54;
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
            const bool hurt=w.unit(b) && w.unit(b)->hp<hp;
            if (shallow) check(hurt,"a ship shoots a Mer Warrior standing in the shallows");
            else check(!hurt && !targeted && (w.unit(a)->orders.empty() || w.unit(a)->orders.front().targetId!=b),
                       "a ship neither targets nor is ordered onto a swimming Mer Warrior");
        }
    }
    // No endless shots (engine deviations, 2026-11-12). A Thirsha with its
    // Lightning selected refuses a swimming Mer Warrior (only the selected
    // weapon fires); a Crusades Wrath of Tammuz's storm runs on the surface
    // and hurts a ship over deep water; a Creon Submersible facing either
    // way is hit by a ship's arrows; a ship whose arrows stop in the bank
    // under a cow on the shore sails to a spot with a clear shot (its chord
    // starts at its own height) or, finding none, gives the attack up.
    if (!legion) {
        tak::sim::TypeRegistry registry;tak::sim::setupRegistry(registry,vfs,true);
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
        {
            auto mer=still("vermer");
            tak::sim::World w;w.setTerrain(sea(false),256,256,58);w.buildNavClasses(registry);
            const int a=w.spawn(registry.find("zonhunt"),1900,2000,0,0),b=w.spawn(&mer,2050,2000,0,1);
            for (int i=0;i<60;++i) w.tick(1.f/30);
            w.attack(a,b,false);
            for (int i=0;i<30;++i) w.tick(1.f/30);
            check(w.unit(a)->orders.empty() || w.unit(a)->orders.front().targetId!=b,
                  "a Thirsha with Lightning selected refuses a swimming Mer Warrior");
        }
        // An order the weapon that would fire cannot serve is refused: a
        // Basilisk's stone on a ship that cannot be stoned, a Fallen Angel's
        // Death Sword (its selected weapon) on a ship it does no damage to.
        for (const char* attacker:{"zonbasil","tarang"}) {
            auto flagship=still("verflag");
            tak::sim::World w;w.setTerrain(sea(false),256,256,58);w.buildNavClasses(registry);
            const int a=w.spawn(registry.find(attacker),1900,2000,0,0),b=w.spawn(&flagship,2050,2000,0,1);
            for (int i=0;i<60;++i) w.tick(1.f/30);
            w.attack(a,b,false);
            for (int i=0;i<30;++i) w.tick(1.f/30);
            check(w.unit(a)->orders.empty() || w.unit(a)->orders.front().targetId!=b,
                  "an attack its firing weapon cannot serve is refused");
        }
        // A computer's switcher picks a weapon that serves (engine deviation,
        // retailAiWeaponSwitch / aiSlotServes): a Mage Archer by an enemy
        // Barracks no longer flips to its Paralyze Arrow (0 damage to a fort)
        // and back for good, and a Fallen Angel ordered onto a ship takes its
        // Fire Bomb, its Soul Stealer doing nothing to naval.
        {
            auto barracks=still("arakeep");
            tak::sim::World w;w.setTerrain(sea(true),256,256,58);w.buildNavClasses(registry);
            w.player(0).automaticGates=true;
            const int a=w.spawn(registry.find("arabow"),2570,2000,0,0),b=w.spawn(&barracks,2400,2000,0,1);
            const auto hp=w.unit(b)->hp;
            int switches=0,last=w.unit(a)->weaponSlot;
            for (int i=0;i<900;++i) {
                w.tick(1.f/30);
                if (w.unit(a)->weaponSlot!=last) { ++switches;last=w.unit(a)->weaponSlot; }
            }
            check(switches<20 && w.unit(b)->hp<hp,"a computer's Mage Archer settles on a weapon that hurts a Barracks");
        }
        {
            auto flagship=still("verflag");
            tak::sim::World w;w.setTerrain(sea(false),256,256,58);w.buildNavClasses(registry);
            w.player(0).automaticGates=true;
            const int a=w.spawn(registry.find("tarang"),1700,2000,0,0),b=w.spawn(&flagship,1850,2000,0,1);
            w.unit(a)->mana=float(w.unit(a)->type->maxMana);
            for (int i=0;i<30;++i) w.tick(1.f/30);
            const auto hp=w.unit(b)->hp;
            w.attack(a,b,false);
            for (int i=0;i<900 && w.unit(b);++i) w.tick(1.f/30);
            check(!w.unit(b) || w.unit(b)->hp<hp,"a computer's Fallen Angel ordered onto a ship switches to a weapon that hurts it");
        }
        auto hurtBy=[&](const char* attacker,tak::sim::UnitType& victim,float ax,float tx,bool shore,float heading) {
            tak::sim::World w;w.setTerrain(sea(shore),256,256,58);w.buildNavClasses(registry);
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
        auto ship=still("verflag");
        check(hurtBy("zongod",ship,1700,1850,false,0).first,"a Wrath of Tammuz storm hurts a ship over deep water");
        for (float heading:{0.f,1.5708f,3.1416f,4.7124f}) {
            auto sub=still("cresubm");
            check(hurtBy("verflag",sub,1700,1850,false,heading).first,"a ship's arrows hit a Creon Submersible");
        }
        // A unit ordered onto a target inside its minimum range backs off to
        // its range band and fires (engine deviation): a Crusades Trebuchet
        // Ship 250 px from a boat (min 500), a Crusades Catapult 90 px from a
        // swordsman (min 180).
        {
            auto boat=still("aratrans");
            check(hurtBy("vertre",boat,1600,1850,false,0).first,"a Trebuchet Ship backs out of its minimum range and fires");
            auto sword=still("arasword");
            check(hurtBy("arapult",sword,2400,2490,true,0).first,"a Catapult backs out of its minimum range and fires");
        }
        auto cow=still("lifcow");
        const auto [hurt,held]=hurtBy("npcrixx",cow,2144-150,2144,true,0);
        check(hurt || held<300,"a ship whose arrows stop in the bank moves to a clear shot or gives the attack up");
    }
    // The naval combat mask changes no movement cell or ground raycast.
    tak::sim::World w;w.setTerrain(std::vector<uint8_t>(64*64,10),64,64,64);
    tak::sim::UnitType boat;boat.domain=tak::sim::UnitType::Domain::Water;
    check(!w.nav().losBetween(200,200,600,200),"ground-only sight across deep water is unchanged");
    check(!w.nav().walkable(20,20) && w.navFor(&boat).walkable(20,20),"ground and boat movement grids are unchanged");
    std::printf("naval combat: %d cases, %d failures\n",cases,failures);
    return failures?1:0;
}
