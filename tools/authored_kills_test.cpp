// The engine's authored veterancy is TA's kill count. A scenario (.crt),
// Cartographer or a mission's `v F` clause gives a placed unit a starting
// kill count; an old rank N reads as N x 5 kills (crt::killsForRank), and
// TA's rules read that count alone: the damage the unit takes, amount x
// (25 - min(kills/5, 5)) x 4 / 100 (TotalA.exe 0x489bf3), and its aim, the
// lead past 5 kills (0x48a324). No Kingdoms rank multiplier is left.
//
//   authored_kills_test             the .crt format and Cartographer round trips
//   authored_kills_test <ta data>   also a rank-2 placement in a real match setup
#include "cartographer/units.h"
#include "crt/crt.h"
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/mission.h"
#include "sim/retailveterancy.h"
#include "sim/sim.h"
#include "tnt/tnt.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace ta;
namespace ta::sim { struct RetailReplayProbe {
    static void hit(World& w,const Weapon& weapon,float x,float z,int player,int from,Unit* victim) {
        w.applyHit(weapon,x,z,player,from,victim);
    }
    static std::array<int32_t,3> aimPoint(World& w,Unit& u,Unit& target,const Weapon& weapon) {
        return w.taTargetPoint(u,{&target,{}},weapon);
    }
    static std::array<int32_t,3> leadPoint(World& w,const Unit& u,const Unit& target,const Weapon& weapon) {
        return w.retailLeadPoint(u,target,&weapon);
    }
    static std::array<int32_t,3> sweetSpot(World& w,int target) { return w.queryUnitScriptPoint(target,true); }
}; }
using sim::RetailReplayProbe;

static void check(bool ok,const std::string& what) {if(!ok)throw std::runtime_error(what);}
static int32_t le32(const std::vector<uint8_t>& d,size_t o) {int32_t v;std::memcpy(&v,&d[o],4);return v;}

// The byte offsets of a one-type, one-unit .crt: version, the type count, the
// type record (stat[3] at +0x10c), the unit count, the unit record (+0x220).
constexpr size_t kTypeStat3=4+4+0x10c, kUnitKills=4+4+272+4+0x220;

static crt::Scenario scene(float version,int unitKills,int typeKills) {
    crt::Scenario s;s.version=version;s.players.resize(9);
    crt::CustomType t;t.name="ARMPW";t.stat[3]=typeKills;s.customTypes.push_back(t);
    crt::Unit u;u.objectName="ARMPW";u.uniqueName="Veteran";u.x=10;u.z=12;u.kills=unitKills;
    s.units.push_back(u);
    return s;
}

static void formats() {
    // An old (1.0) file holds the rank: rank 2 and a type rank 3 load as 10
    // and 15 kills, and an unchanged old file writes back byte for byte.
    const auto old=crt::write(scene(crt::kVersionRank,10,15));
    check(le32(old,kUnitKills)==2 && le32(old,kTypeStat3)==3,"a 1.0 file does not hold the rank");
    const auto parsed=crt::parse(old);
    check(parsed.version==crt::kVersionRank && parsed.units.size()==1 && parsed.units[0].kills==10 &&
          parsed.customTypes[0].stat[3]==15,"an old rank 2 / type rank 3 did not load as 10 / 15 kills");
    check(crt::write(parsed)==old,"an old rank file does not round-trip");
    // A new (2.0) file holds the kill count itself, any count.
    const auto fresh=crt::write(scene(crt::kVersionKills,13,7));
    check(le32(fresh,0)==0x40000000 && le32(fresh,kUnitKills)==13 && le32(fresh,kTypeStat3)==7,
          "a 2.0 file does not hold the kill counts");
    const auto back=crt::parse(fresh);
    check(back.units[0].kills==13 && back.customTypes[0].stat[3]==7 && crt::write(back)==fresh,
          "a 2.0 file does not round-trip its kill counts");
    check(crt::Scenario{}.version==crt::kVersionCurrent,"a new scenario is not a kill-count, ten-player (3.0) file");
    auto bad=fresh;const float v4=4.0f;std::memcpy(bad.data(),&v4,4);
    check(crt::parse(bad).version==0,"an unknown .crt version was accepted");
    check(crt::killsForRank(9)==45 && crt::killsForRank(0)==0,"rank to kills");
    std::puts("PASS: .crt 1.0 rank converts to kills (x5) and round-trips; 2.0 keeps the count");
}

static void cartographer() {
    // Cartographer's working list carries the kill count; its save writes a
    // 2.0 file, and loading that file gives the same count back. An old rank
    // file opened in Cartographer shows rank x 5 and is saved upgraded.
    cart::PlacedUnit p;p.type="ARMPW";p.player=1;p.x=168;p.z=200;p.kills=37;p.name="Old Hand";
    const auto saved=cart::saveScenario(crt::Scenario{},{p});
    const auto s=crt::parse(saved);
    check(s.version==crt::kVersionCurrent && s.units.size()==1 && s.units[0].kills==37,"Cartographer did not save the kills");
    const auto placed=cart::toPlaced(s);
    check(placed.size()==1 && placed[0].kills==37 && placed[0].name=="Old Hand","Cartographer did not load the kills");
    check(cart::saveScenario(s,placed)==saved,"Cartographer save/load/save is not stable");
    const auto legacy=cart::toPlaced(crt::parse(crt::write(scene(crt::kVersionRank,15,0))));
    check(legacy.size()==1 && legacy[0].kills==15,"an old rank 3 did not open as 15 kills");
    const auto upgraded=crt::parse(cart::saveScenario(crt::parse(crt::write(scene(crt::kVersionRank,15,0))),legacy));
    check(upgraded.version==crt::kVersionCurrent && upgraded.units[0].kills==15,"an old file was not saved upgraded");
    p.kills=70000;
    check(crt::parse(cart::saveScenario(crt::Scenario{},{p})).units[0].kills==65535,"kills past the word were not clamped");
    std::puts("PASS: Cartographer saves and loads the kill count; an old rank opens as rank x 5");
}

// A match on a flat synthetic map whose .crt is an OLD (1.0) file placing an
// ARMPW of rank `rank` for player 0 and a plain ARMPW for player 1.
static std::unique_ptr<sim::World> placedMatch(const hpi::Vfs& base,const sim::TypeRegistry& reg,int rank) {
    tnt::Map map;map.width=map.height=64;map.blocksX=map.blocksY=32;map.seaLevel=0;
    map.heights.assign(4096,40);map.features.assign(4096,0xffff);
    map.tileCount=1;map.tileGfx.assign(1024,7);map.tileIndex.assign(1024,0);
    auto files=std::make_shared<hpi::Vfs::Files>();
    (*files)["maps/vettest.tnt"]=map.save();
    const std::string ota="[GlobalHeader]\n{\nmissionname=vettest;\nhasscenario=1;\n}\n[TAPLAYTEST]\n{\nauthoredscenario=1;\n}\n";
    (*files)["maps/vettest.ota"]={ota.begin(),ota.end()};
    crt::Scenario s;s.version=crt::kVersionRank;s.players.resize(9);
    crt::Unit vet;vet.objectName="ARMPW";vet.uniqueName="vet";vet.player=0;vet.x=20;vet.z=20;vet.kills=crt::killsForRank(rank);
    crt::Unit foe=vet;foe.player=1;foe.x=24;foe.kills=0;
    s.units={vet,foe};
    const auto bytes=crt::write(s);
    check(le32(bytes,4+4+4+0x220)==rank,"the fixture is not an old rank file");
    (*files)["maps/vettest.crt"]=bytes;
    auto view=std::make_shared<hpi::Vfs>(&base);view->setMapFiles(files);
    sim::MatchConfig cfg;cfg.vfs=view.get();cfg.mapPath="maps/vettest.tnt";
    cfg.slots.resize(2);cfg.slots[0]={true,0,0,1,false,false};cfg.slots[1]={true,1,1,1,false,false};
    auto w=std::make_unique<sim::World>();
    sim::setupMatch(*w,reg,cfg);
    check(w->units().size()==2,"the scenario did not place its two units");
    static std::vector<std::shared_ptr<hpi::Vfs>> keep;keep.push_back(view);   // the world reads through it
    return w;
}

static void match(const char* dataDir) {
    auto base=hpi::mountRetailRoot(dataDir);
    sim::TypeRegistry reg;sim::setupRegistry(reg,base);
    auto plain=placedMatch(base,reg,0),veteran=placedMatch(base,reg,2);
    auto id=[](sim::World& w,int player) {
        for(const auto& u:w.units())if(u.player==player)return u.id;
        throw std::runtime_error("no unit for the player");
    };
    sim::Unit& v=*veteran->unit(id(*veteran,0));
    check(v.kills==10,"a rank 2 placement did not start with 10 kills ("+std::to_string(v.kills)+")");
    check(plain->unit(id(*plain,0))->kills==0,"a rank 0 placement has kills");
    check(veteran->unit(id(*veteran,1))->kills==0,"the plain foe has kills");
    // Damage: the foe hits each; the same CRT draw on the 10-kill unit takes
    // (25 - 2) x 4 / 100 = 92%, 8% less (0x489bf3); nothing else scales it.
    const sim::Weapon& gun=v.type->weapons.at(0);
    auto hit=[&](sim::World& w) {
        sim::Unit& target=*w.unit(id(w,0));
        const int before=target.hp.floorInt();
        RetailReplayProbe::hit(w,gun,target.x.toFloat(),target.z.toFloat(),1,id(w,1),&target);
        return before-target.hp.floorInt();
    };
    const int fresh=hit(*plain),scaled=hit(*veteran);
    check(fresh>0 && scaled==sim::retailTaVeteranDamage(fresh,10) && scaled==fresh*92/100,
          "a 10-kill unit did not take 8% less ("+std::to_string(fresh)+" vs "+std::to_string(scaled)+")");
    // Its own hits are not scaled by its kills (no Kingdoms attack multiplier).
    auto shoot=[&](sim::World& w) {
        sim::Unit& target=*w.unit(id(w,1));
        const int before=target.hp.floorInt();
        RetailReplayProbe::hit(w,gun,target.x.toFloat(),target.z.toFloat(),0,id(w,0),&target);
        return before-target.hp.floorInt();
    };
    check(shoot(*plain)==shoot(*veteran),"a 10-kill shooter's hit was scaled");
    // Accuracy: more than 5 kills leads the target (0x48a324); 0 kills, and
    // 5 (rank 1), aim at the SweetSpot.
    {
        sim::World& w=*veteran;sim::Unit& u=*w.unit(id(w,0));sim::Unit& t=*w.unit(id(w,1));
        t.speed=sim::Fixed::fromInt(2);   // a moving target: the lead is off the SweetSpot
        check(RetailReplayProbe::leadPoint(w,u,t,gun)!=RetailReplayProbe::sweetSpot(w,t.id),"the fixture's lead is the SweetSpot");
        check(RetailReplayProbe::aimPoint(w,u,t,gun)==RetailReplayProbe::leadPoint(w,u,t,gun),
              "a 10-kill shooter did not lead its target");
        u.kills=5;
        check(RetailReplayProbe::aimPoint(w,u,t,gun)==RetailReplayProbe::sweetSpot(w,t.id),
              "a 5-kill shooter led its target");
        u.kills=10;
    }
    // A mission's `v F` is the same kill count: v 1.2 is rank 2, 10 kills.
    check(sim::MissionScript::missionVeteranKills(1.2f)==10,"v 1.2 is not 10 kills");
    std::puts("PASS: a rank 2 placement (old .crt) starts with 10 kills, takes 8% less and leads its aim");
}

int main(int argc,char** argv) try {
    formats();
    cartographer();
    if(argc>1)match(argv[1]);
    return 0;
} catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
