#include "sim/sim.h"
#include "client/renderframe.h"
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
using namespace tak::sim;
namespace tak::sim { struct RetailReplayProbe {
 static void shoot(World& w,int from,int target){w.fire(*w.unit(from),*w.unit(target),0);}
}; }
static void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
static void setup(World& w){w.setPlayerCount(8);w.setTerrain(std::vector<uint8_t>(128*128,40),128,128,0);}
static UnitType type(){UnitType t;t.id="retirement";t.maxHp=100;t.footX=t.footZ=1;return t;}
int main(int argc,char** argv)try {
 auto t=type();
 if(argc==2 && std::string(argv[1])=="--benchmark") {
  World w;setup(w);
  for(int i=0;i<300;++i)w.spawn(&t,float(32+(i%60)*24),float(32+(i/60)*24),0,i%8);
  for(int phase=0;phase<3;++phase){
   if(phase)for(int wave=0;wave<30;++wave){for(int i=0;i<1000;++i){int id=w.spawn(&t,1800,1800,0,i%8);auto*u=w.unit(id);u->hp={};u->deadFor=World::kRetiredTicks;u->corpseUntil=0;}w.tick(1.f/30);}
   for(int i=0;i<60;++i)w.tick(1.f/30);
   auto a=std::chrono::steady_clock::now();uint64_t h=0;
   for(int i=0;i<300;++i){w.tick(1.f/30);if(i%30==29)h^=w.stateHash();}
   double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-a).count();
   std::printf("historical=%d active_records=%zu tick_and_hash_ms=%.4f checksum=%llx\n",phase*30000,w.units().size(),ms/300,(unsigned long long)h);
  }
  return 0;
 }
 World a,b;setup(a);setup(b);
 for(World* w:{&a,&b}) {
  for(int i=0;i<8;++i)w->spawn(&t,float(64+i*32),64,0,i);
  for(int i=0;i<240;++i){int id=w->spawn(&t,1200,1200,0,i%8);w->scenarioDestroy(id);}
  const int corpse=w->spawn(&t,1300,1300,0,0);auto*u=w->unit(corpse);
  u->hp={};u->deadFor=0;u->corpseStatue=0;u->corpseUntil=600;
 }
 for(int tick=0;tick<60;++tick){a.tick(1.f/30);b.tick(1.f/30);check(a.stateHash()==b.stateHash(),"compaction desynchronized peers");}
 check(a.units().size()==9,"final records not removed or active corpse removed");
 check(!a.unit(9) && a.unit(249) && !a.unit(249)->alive(),"retired ID resolves or active corpse lost");
 for(int id=1;id<=8;++id)check(a.unit(id)&&a.unit(id)->player==id-1,"moved slot lost its ID");
 const int next=a.spawn(&t,1500,1500,0,0);check(next==250&&a.unit(next),"new ID reused retired unit or lookup failed");
 // A projectile or delayed injury may kill after its original attacker retires.
 auto* victim=a.unit(2);victim->hp={};victim->lastHitBy=9;victim->lastHitPlayer=0;
 const auto kills=a.player(0).kills;a.tick(1.f/30);
 check(a.player(0).kills==kills+1,"late kill lost retired attacker attribution");
 {
  World delayed,retained;setup(delayed);setup(retained);auto caster=t;
  Weapon spell;spell.kind=Weapon::Kind::Remote;spell.buildUp=4;spell.buildUpTicks=120;spell.decayTicks=15;spell.damage=10;spell.aoe=32;spell.range=1000;
  caster.weapons.push_back(spell);
  const int from=delayed.spawn(&caster,128,128,0,0),target=delayed.spawn(&t,512,512,0,1);
  delayed.unit(from)->scenarioWeapon=2;
  RetailReplayProbe::shoot(delayed,from,target);
  delayed.scenarioDestroy(from);
  const int retainedFrom=retained.spawn(&caster,128,128,0,0),retainedTarget=retained.spawn(&t,512,512,0,1);
  retained.unit(retainedFrom)->scenarioWeapon=2;
  RetailReplayProbe::shoot(retained,retainedFrom,retainedTarget);
  retained.scenarioDestroy(retainedFrom);
  retained.unit(retainedFrom)->deadFor=0;retained.unit(retainedFrom)->corpseUntil=600;
  for(int i=0;i<90;++i)delayed.tick(1.f/30);
  check(delayed.unit(from),"pending effect lost its retired attacker's modifiers");
  for(int i=0;i<90;++i)delayed.tick(1.f/30);
  check(!delayed.unit(from),"completed effect retained its retired source forever");
  for(int i=0;i<180;++i)retained.tick(1.f/30);
  // 52a35c: a dead source's effect lands without its weapon multiplier (x2
  // here), whether or not its record was compacted.
  check(delayed.unit(target)->hp==retained.unit(retainedTarget)->hp && delayed.unit(target)->hp.floorInt()>=88 &&
        delayed.unit(target)->hp.floorInt()<100,"delayed effect changed source damage after death");
 }
 Frame frame;frame.gen=3;frame.units.resize(2);frame.unitSlots.resize(251,-1);frame.unitSlots[1]=0;frame.unitSlots[250]=1;
 for(size_t i=0;i<2;++i){frame.units[i].id=i?250:1;frame.units[i].gen=3;frame.units[i].type=&t;}
 check(frame.unit(250)==&frame.units[1]&&!frame.unit(9),"sparse render ID lookup failed");
 frame.units[0].id=251;check(!frame.unit(1),"retired snapshot leaked into frame lookup");
 a.resetForReplay();setup(a);check(a.units().empty()&&!a.unit(9),"replay retained records");
 check(a.spawn(&t,64,64,0,0)==1,"replay did not reset unit IDs");
 std::puts("PASS: final retirement, active corpses, stable IDs, delayed credit, sparse snapshots and peer hashes");return 0;
}catch(const std::exception&e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
