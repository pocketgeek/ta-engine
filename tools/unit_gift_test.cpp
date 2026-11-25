#include "sim/matchsetup.h"
#include "net/protocol.h"
#include <cstdio>
#include <memory>
using namespace ta;
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
    // TA's auto-share (TotalA.exe 0x457d30): every 60 ticks a player whose
    // ShareMetal / ShareEnergy switch is on and who stores more than the
    // threshold gives the LAST allied human (by index) storing less
    // min((stored - threshold) * 1/3 (metal) or 1/2 (energy), their room),
    // through the manual give: it leaves the giver at once and lands at the
    // receiver's next settle.
    auto economy=std::make_unique<sim::World>();auto& e=*economy;
    e.setPlayerCount(5);
    sim::UnitType bank;bank.id="bank";bank.maxHp=100;bank.metalStorage=1000;bank.energyStorage=1000;
    e.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
    for(int p=0;p<5;++p) {e.setTeam(p,p==3?1:0);e.spawn(&bank,128+64*p,256,std::nullopt,p);}
    e.setStartResources(0,900,900);e.setStartResources(1,300,300);e.setStartResources(2,600,100);
    e.setStartResources(3,0,0);e.setStartResources(4,0,0);
    e.player(4).automaticGates=true;   // a computer ally (owner kind 2)
    auto runTo=[&](uint32_t tick){while(e.tickCount()<tick)e.tick(1.0f/30);};
    auto third=[](float stored,float threshold){return float(double(stored-threshold)*double(1.0f/3.0f));};
    runTo(95);
    check(e.books(0).stored[1]==900 && e.books(2).stored[1]==100,"sharing is off by default");
    // The switch is a sequenced command: wire round trip, hashed.
    net::Command sharing;sharing.kind=net::Cmd::ShareResources;sharing.player=0;
    sharing.targetId=1;sharing.queue=1;   // econ::M, on
    const auto originalHash=e.stateHash();
    net::Writer sharingWire;sharingWire.cmd(sharing);
    net::Reader sharingReader(sharingWire.b.data(),sharingWire.b.size());
    sim::applyCommand(e,reg,sharingReader.cmd());
    check(sharingReader.ok && e.player(0).shareResources==1,"share-metal command round trip");
    check(e.stateHash()!=originalHash,"sharing switch participates in lockstep hash");
    // Tick 120: P0 stores 900 > 0. The poorer allied humans are P1 (300) and
    // P2 (100); the last, P2, gets min(900 / 3, 1200 - 100). P3 is an enemy,
    // P4 a computer (poorer still, never a recipient).
    runTo(125);
    const float give1=third(900,0);
    check(e.books(0).stored[1]==900-give1 && e.books(2).stored[1]==100,
          "the gift leaves the giver at once, not yet landed");
    runTo(155);
    check(e.books(2).stored[1]==100+give1 && e.books(1).stored[1]==300 && e.books(4).stored[1]==0 &&
          e.books(3).stored[1]==0,"metal lands at the last poorer allied human's next settle");
    check(e.books(0).stored[0]==900 && e.books(2).stored[0]==600,"energy switch off: no energy shared");
    // Tick 180: P0 600 against P1 300, P2 400: P2 again, a third of 600.
    runTo(215);
    const float give2=third(900-give1,0);
    check(e.books(0).stored[1]==900-give1-give2 && e.books(2).stored[1]==100+give1+give2,
          "next round: a third of what is left");
    // Energy at 1/2 above a threshold (SetShareEnergy stores min(N, cap)).
    sharing.targetId=0;sharing.queue=3;sharing.x=700;   // econ::E, on, threshold 700
    sim::applyCommand(e,reg,sharing);
    check(e.player(0).shareResources==3 && e.books(0).shareThreshold[0]==700,"energy switch and threshold");
    // Metal: P0 400 is no longer above P2 (600) but is above P1 (300): P1
    // gets a third. Energy: P2 (600, the last poorer) gets (900 - 700) / 2.
    runTo(275);
    const float give3=third(900-give1-give2,0);
    check(e.books(1).stored[1]==300+give3 && e.books(0).stored[0]==800 && e.books(2).stored[0]==700,
          "energy shares half the excess over its threshold; metal skips a richer ally");
    sharing.queue=0;sim::applyCommand(e,reg,sharing);
    check(e.player(0).shareResources==1,"energy switch off again");
    // Bounds: an unknown resource or player changes nothing.
    e.setResourceSharing(0,7,true);e.setResourceSharing(99,1,false);
    check(e.player(0).shareResources==1,"invalid resource and player rejected");
    std::printf("unit gift: %d failures\n",failures);return failures?1:0;
}
