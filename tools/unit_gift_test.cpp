#include "sim/matchsetup.h"
#include "net/protocol.h"
#include <cstdio>
#include <memory>
using namespace tak;
static int failures=0;
static void check(bool ok,const char* why) {
    if (!ok) { std::fprintf(stderr,"FAIL: %s\n",why); ++failures; }
}
int main() {
    sim::UnitType type;type.id="gift";type.maxHp=100;type.maxVel=sim::Fixed::fromInt(2);
    sim::TypeRegistry reg;
    auto world=std::make_unique<sim::World>();auto& w=*world;
    w.setPlayerCount(3);w.setTeam(0,0);w.setTeam(1,0);w.setTeam(2,1);
    w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
    int id=w.spawn(&type,128,128,std::nullopt,0);
    auto& u=*w.unit(id);
    check(w.canGiveUnit(id,0,1),"ordinary allied gift eligible");
    check(!w.canGiveUnit(id,0,0) && !w.canGiveUnit(id,0,2),"self and enemy rejected");
    check(!w.canGiveUnit(id,1,0) && !w.canGiveUnit(id,0,99),"ownership and player bounds");
    type.commander=true;check(!w.canGiveUnit(id,0,1),"monarch rejected");type.commander=false;
    u.flightGroundMode=2;check(!w.canGiveUnit(id,0,1),"airborne rejected");u.flightGroundMode=1;
    u.inTransport=99;check(!w.canGiveUnit(id,0,1),"passenger rejected");u.inTransport=0;
    u.cargo={99};check(!w.canGiveUnit(id,0,1),"loaded transport rejected");u.cargo.clear();
    u.underConstruction=true;check(!w.canGiveUnit(id,0,1),"unfinished rejected");u.underConstruction=false;
    w.player(1).defeated=true;check(!w.canGiveUnit(id,0,1),"defeated recipient rejected");w.player(1).defeated=false;
    u.hp=sim::Fixed();check(!w.canGiveUnit(id,0,1),"dying unit rejected");u.hp=sim::Fixed::fromInt(20);
    u.squad=-4;u.repeatType=&type;u.buildQueue.push_back(&type);u.selfDestructT=30;
    const auto hp=u.hp;const auto built0=w.player(0).built,built1=w.player(1).built;
    net::Command c;c.kind=net::Cmd::GiveUnit;c.unitId=id;c.player=0;c.targetId=1;
    net::Writer out;out.cmd(c);net::Reader in(out.b.data(), out.b.size());auto wire=in.cmd();
    check(in.ok && wire.kind==c.kind && wire.targetId==1,"gift wire round trip");
    sim::applyCommand(w,reg,wire);
    check(u.player==1 && u.hp==hp,"gift transfers without healing");
    check(u.squad==0 && u.orders.empty() && !u.repeatType && u.buildQueue.empty() &&
          u.rally.empty() && u.selfDestructT==-1,"old group, production and orders cleared");
    check(w.player(0).unitCount==0 && w.player(1).unitCount==1,"counts moved immediately");
    check(w.player(0).built==built0 && w.player(1).built==built1 &&
          w.player(0).kills==0 && w.player(1).kills==0,"gift does not award score");
    sim::applyCommand(w,reg,wire);
    check(w.player(1).unitCount==1,"duplicate stale gift rejected");
    int second=w.spawn(&type,192,128,std::nullopt,0);
    w.setUnitCap(1);check(!w.giveUnit(second,0,1),"recipient population cap");w.setUnitCap(0);
    type.totalAllowed=1;check(!w.giveUnit(second,0,1),"recipient type cap");type.totalAllowed=0;
    check(w.giveUnit(second,0,1),"eligible remainder transfers");
    sim::UnitType structure=type;structure.id="factory";structure.maxVel=sim::Fixed();
    int building=w.spawn(&structure,320,128,std::nullopt,0);
    check(w.giveUnit(building,0,1),"completed building eligible");
    sim::UnitType carrier=type;carrier.id="carrier";carrier.transportCap=4;
    int empty=w.spawn(&carrier,384,128,std::nullopt,0);
    check(w.giveUnit(empty,0,1),"empty landed carrier eligible");
    // Overflow sharing prioritizes lowest percentage full, independent of slot.
    auto economy=std::make_unique<sim::World>();auto& e=*economy;
    e.setPlayerCount(3);
    sim::UnitType bank;bank.id="bank";bank.maxHp=100;bank.storage=1000;
    e.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
    for(int p=0;p<3;++p) {e.setTeam(p,0);e.spawn(&bank,128+64*p,256,std::nullopt,p);}
    e.player(0).mana=800;e.player(1).mana=100;e.player(2).mana=1300;
    e.tick(1.0f/30);
    check(e.player(0).mana==800 && e.player(1).mana==400 && e.player(2).mana==1000,
          "overflow goes to neediest ally, not first slot");
    e.player(0).mana=100;e.player(1).mana=100;e.player(2).mana=1300;e.tick(1.0f/30);
    check(e.player(0).mana==250 && e.player(1).mana==250,"equal need shares fairly");
    sim::UnitType largeBank=bank;largeBank.id="large-bank";largeBank.storage=2000;
    e.unit(2)->type=&largeBank;
    e.player(0).mana=100;e.player(1).mana=200;e.player(2).mana=1300;e.tick(1.0f/30);
    check(e.player(0).mana==200 && e.player(1).mana==400,
          "equal fill ratios share proportionally to storage");
    // Outgoing preferences are sequenced, hashed, and restricted to allies.
    net::Command sharing;sharing.kind=net::Cmd::ShareMana;sharing.player=2;
    sharing.targetId=1;sharing.queue=0;
    const auto originalHash=e.stateHash();
    net::Writer sharingWire;sharingWire.cmd(sharing);
    net::Reader sharingReader(sharingWire.b.data(),sharingWire.b.size());
    sim::applyCommand(e,reg,sharingReader.cmd());
    check(sharingReader.ok && !(e.player(2).manaShareMask & 2),"mana toggle command round trip");
    check(e.stateHash()!=originalHash,"sharing preference participates in lockstep hash");
    e.unit(2)->type=&bank;
    e.player(0).mana=800;e.player(1).mana=100;e.player(2).mana=1300;e.tick(1.0f/30);
    check(e.player(0).mana==1000 && e.player(1).mana==100,"excluded needy ally receives no overflow");
    sharing.queue=1;sim::applyCommand(e,reg,sharing);
    e.player(0).mana=800;e.player(1).mana=100;e.player(2).mana=1300;e.tick(1.0f/30);
    check(e.player(0).mana==800 && e.player(1).mana==400,"re-enabled sharing again feeds the neediest ally");
    e.setManaSharing(2,0,false);e.setManaSharing(2,1,false);
    e.player(0).mana=100;e.player(1).mana=100;e.player(2).mana=1300;e.tick(1.0f/30);
    check(e.player(0).mana==100 && e.player(1).mana==100 && e.player(2).mana==1000,
          "disabled sharing wastes overflow without exceeding the donor cap");
    e.player(0).mana=1300;e.player(1).mana=1000;e.player(2).mana=100;e.tick(1.0f/30);
    check(e.player(2).mana==400,"outgoing sharing toggle does not refuse incoming mana");
    const auto fromMask=w.player(0).manaShareMask;
    w.setManaSharing(0,2,false);w.setManaSharing(0,0,false);w.setManaSharing(0,99,false);
    check(w.player(0).manaShareMask==fromMask,"enemy, self and invalid mana recipients rejected");
    w.player(1).defeated=true;w.setManaSharing(0,1,false);
    check(w.player(0).manaShareMask==fromMask,"defeated mana recipient rejected");
    std::printf("unit gift: %d failures\n",failures);return failures?1:0;
}
