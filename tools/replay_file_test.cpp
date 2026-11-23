// Real file loader: another format or protocol, and corrupt inner/outer records,
// must not silently launch a different simulation.
#include "client/replayfile.h"
#include "legion_scn.h"
#include "net/crypto.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include "util/u8path.h"
using namespace tak::net;
namespace {
int checks=0;
void check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
Writer recording(uint32_t protocol=kNetVersion,const Command& command=Command{},uint8_t event=0,bool innerTrailing=false,
                 tak::sim::PathfindingMode mode=tak::sim::PathfindingMode::Retail,const char* mission="",uint32_t format=kReplayFormat){
 Writer file;ReplayHeader header;header.mapId="Synthetic map";header.unitCap=2000;
 header.pathfindingMode=mode;header.mission=mission;
 header.slotType[0]=header.slotType[1]=1;header.slotFaction[1]=1;header.slotTeam[1]=1;
 writeReplayHeader(file,header);
 for(int i=0;i<4;++i){file.b[4+i]=uint8_t(format>>(8*i));file.b[8+i]=uint8_t(protocol>>(8*i));}
 Writer bundle;bundle.u32(0);bundle.u32(1);bundle.cmd(command);bundle.u32(event?1:0);
 if(event){bundle.u8(event);bundle.u8(0);}if(innerTrailing)bundle.u8(99);
 file.u32(1);file.u32(uint32_t(bundle.b.size()));file.b.insert(file.b.end(),bundle.b.begin(),bundle.b.end());
 file.u32(1);file.u32(0);file.u64(1234);file.u64(5678);return file;
}
void run(){
 const auto root=std::filesystem::temp_directory_path()/("tak-replay-file-"+tak::crypto::toHex(tak::crypto::randomVec(12)));
 std::filesystem::create_directories(root);struct Cleanup{std::filesystem::path p;~Cleanup(){std::error_code ec;std::filesystem::remove_all(p,ec);}}cleanup{root};
 const auto path=root/tak::u8path("replay-\xc3\xa9.takrep");
 const auto utf=path.u8string();const std::string name(utf.begin(),utf.end());
 auto load=[&](const Writer& value,ReplayFile* decoded=nullptr){std::ofstream out(path,std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char*>(value.b.data()),std::streamsize(value.b.size()));out.close();ReplayFile replay;const bool ok=loadReplayFile(name,replay);if(ok && decoded)*decoded=std::move(replay);return ok;};
 check(load(recording()),"current simulation recording refused");
 ReplayFile decoded;
 for(auto mode:{tak::sim::PathfindingMode::Retail,tak::sim::PathfindingMode::Legion})
  check(load(recording(kNetVersion,Command{},0,false,mode),&decoded)&&decoded.cfg.pathfindingMode==mode,
        "recorded pathfinder did not reach match configuration");
 check(load(recording(kNetVersion,Command{},0,false,tak::sim::PathfindingMode::Legion,"campaign"),&decoded)&&
       decoded.cfg.pathfindingMode==tak::sim::PathfindingMode::Retail,"campaign replay used Legion");
 // 1-3 are reserved pathfinder values.
 for(int removed:{1,2,3,5})
  check(!load(recording(kNetVersion,Command{},0,false,tak::sim::PathfindingMode(removed))),"unknown pathfinder accepted");
 check(!load(recording(kNetVersion,Command{},0,false,tak::sim::PathfindingMode(255))),"invalid pathfinder byte accepted");
 Command area;area.kind=Cmd::BuildManaArea;area.unitId=1;area.x=100;area.z=200;area.x2=800;area.z2=900;
 std::snprintf(area.type,sizeof area.type,"aralode");
 check(load(recording(kNetVersion,area),&decoded) && decoded.cfg.patrolRepairs &&
       decoded.bundles.size()==1 && decoded.bundles.front().cmds.front().x2==800 &&
       decoded.bundles.front().cmds.front().z2==900,"current lodestone area replay lost its endpoints or patrol policy");
 // Only the current format under the current protocol plays; anything else is refused with a reason.
 auto refused=[&](const Writer& value,const char* what){
  std::ofstream out(path,std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char*>(value.b.data()),std::streamsize(value.b.size()));out.close();
  ReplayFile replay;check(!loadReplayFile(name,replay)&&!replay.error.empty(),what);return replay;};
 for(uint32_t protocol:{kNetVersion-1,kNetVersion+1,219u}){
  const auto r=refused(recording(protocol),"another protocol's recording accepted");
  check(r.protocolVersion==protocol&&r.error.find("protocol v"+std::to_string(protocol))!=std::string::npos,"refusal does not name the protocol");}
 for(uint32_t format:{kReplayFormat-1,kReplayFormat+1,11u,0u}){
  const auto r=refused(recording(kNetVersion,Command{},0,false,tak::sim::PathfindingMode::Retail,"",format),
                       "another replay format accepted");
  check(r.formatVersion==format&&r.error.find("format "+std::to_string(format))!=std::string::npos,"refusal does not name the format");}
 auto outer=recording();outer.u8(99);check(!load(outer),"outer trailing bytes accepted");
 check(!load(recording(kNetVersion,Command{},0,true)),"inner trailing bytes accepted");
 auto shortFile=recording();shortFile.b.pop_back();check(!load(shortFile),"truncated checkpoint accepted");
 Command bad;bad.kind=Cmd(255);check(!load(recording(kNetVersion,bad)),"unknown command accepted");
 bad=Command{};bad.x=std::numeric_limits<float>::quiet_NaN();check(!load(recording(kNetVersion,bad)),"nonfinite command accepted");
 bad=Command{};bad.player=kMaxSlots;check(!load(recording(kNetVersion,bad)),"invalid command player accepted");
 check(!load(recording(kNetVersion,Command{},255)),"unknown event accepted");
 // Kind 3 is reserved: refused like any unknown kind.
 check(!load(recording(kNetVersion,Command{},3)),"reserved event kind 3 accepted");
 check(load(recording(kNetVersion,Command{},uint8_t(Event::Kind::AiTakeover))),"AI takeover event refused");
 check(load(recording(),&decoded)&&decoded.formatVersion==kReplayFormat&&decoded.protocolVersion==kNetVersion&&
       decoded.checks.size()==1&&decoded.checks[0].hash==1234&&decoded.checks[0].posDigest==5678,
       "checkpoint lost its hash or position digest");
 {auto noDigest=recording();noDigest.b.resize(noDigest.b.size()-8);
  check(!load(noDigest),"a checkpoint without its position digest accepted");}
 // The tracker separates a hash-layout change from changed play.
 {using C=ReplayCheck;
  const C ck[3]={{0,10,100},{300,20,200},{600,30,300}};
  ReplayCheckTracker clean;for(const auto& c:ck)clean.observe(c,c.hash,[&]{return c.posDigest;});
  check(!clean.stateDiverged()&&clean.summary().empty(),"matching playback reported a divergence");
  ReplayCheckTracker layout;for(const auto& c:ck)layout.observe(c,c.hash+1,[&]{return c.posDigest;});
  check(layout.stateDiverged()&&layout.stateTick()==0&&!layout.posDiverged()&&
        layout.summary().find("hash layout change only")!=std::string::npos,"layout-only divergence not identified");
  ReplayCheckTracker play;for(const auto& c:ck)play.observe(c,c.hash+1,[&]{return c.tick<300?c.posDigest:c.posDigest+1;});
  check(play.stateDiverged()&&play.stateTick()==0&&play.posDiverged()&&play.posTick()==300&&
        play.summary()=="state diverged at tick 0; positions/hp/orders match until tick 300 (hash layout changed first, behaviour later)",
        "state-then-position divergence not reported with both ticks");
  ReplayCheckTracker none;for(const auto& c:ck)none.observe(C{c.tick,c.hash,0},c.hash+1,[&]{return 0ull;});
  check(none.stateDiverged()&&!none.haveDigest()&&none.summary().find("no position digest")!=std::string::npos,"checkpoints without digests claimed one");}
 // World::posDigest: stable for equal worlds, sensitive to positions and orders, independent of the hash layout.
 {const char* text="scn 1\nname pd\nticks 60\nmap flat 64 64\nplayers 2\ntype a mover 2 2500 10 1.8\n"
                   "group g 0 a 4 rect 5 5 20 20\nat 1 move all 40 40\n";
  auto digest=[&](bool order,int ticks){
   auto scn=tak::scn::parse(text);if(!order)scn.orders.clear();
   auto built=tak::scn::build(scn,{tak::sim::PathfindingMode::Legion,true,0,nullptr});
   tak::scn::OrderFeed feed(scn,*built);
   for(int t=0;t<ticks;++t){feed.apply(uint32_t(t));built->world->tick(1.f/30);}
   return std::pair<uint64_t,uint64_t>{built->world->posDigest(),built->world->stateHash()};};
  const auto a=digest(true,40),b=digest(true,40),idle=digest(false,40),later=digest(true,41);
  check(a.first==b.first&&a.second==b.second,"posDigest not reproducible");
  check(a.first!=idle.first,"posDigest blind to orders and movement");
  check(a.first!=later.first,"posDigest blind to a tick of movement");
  check(a.first!=a.second,"posDigest equals the state hash");}
 // The picker's look at a file: its first bytes tell whether this build can play it and how long the game is.
 {auto put=[&](const Writer& value){std::ofstream out(path,std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char*>(value.b.data()),std::streamsize(value.b.size()));out.close();};
  ReplayPeek peek;
  put(recording());
  check(peekReplayFile(name,peek)&&peek.playable&&peek.ticks==1&&peek.mapId=="Synthetic map"&&
        peek.formatVersion==kReplayFormat&&peek.protocolVersion==kNetVersion,"a current recording is not listed as playable");
  put(recording(kNetVersion-1));
  check(peekReplayFile(name,peek)&&!peek.playable&&peek.protocolVersion==kNetVersion-1,"another protocol's recording listed as playable");
  put(recording(kNetVersion,Command{},0,false,tak::sim::PathfindingMode::Retail,"",kReplayFormat-1));
  check(peekReplayFile(name,peek)&&!peek.playable&&peek.formatVersion==kReplayFormat-1,"another format's recording listed as playable");
  // A long game's file is not read through: what follows the start of the file is never looked at.
  auto countAt=[](const Writer& file){   // where the tick count stands: right behind the header
   Reader r(file.b.data()+4,file.b.size()-4);ReplayHeader h;uint32_t format=0,protocol=0;
   check(readReplayHeader(r,h,format,protocol),"fixture: the header does not read back");
   return size_t(r.p-file.b.data());};
  {auto longGame=recording();
   const size_t header=countAt(longGame);
   for(int i=0;i<4;++i)longGame.b[header+i]=uint8_t(1945042u>>(8*i));
   longGame.b.resize(header+4);longGame.b.resize(header+4+(1u<<20),0xEE);
   put(longGame);
   check(peekReplayFile(name,peek)&&peek.playable&&peek.ticks==1945042,"the game's length does not come from the header");
   ReplayFile whole;check(!loadReplayFile(name,whole),"fixture: this file's body is rubbish, only its start is a replay");}
  // An unfinished recording has an open count: playable as far as the header says, its length unknown.
  {auto open=recording();
   const size_t header=countAt(open);
   for(int i=0;i<4;++i)open.b[header+i]=0xFF;
   put(open);
   check(peekReplayFile(name,peek)&&peek.playable&&peek.ticks==0,"an open tick count read as a length");}
  {Writer junk;for(int i=0;i<64;++i)junk.u8(uint8_t(i));put(junk);
   check(!peekReplayFile(name,peek),"a file that is no replay was listed");}
  {Writer tiny;for(char ch:{'T','A','K','R'})tiny.u8(uint8_t(ch));put(tiny);
   check(!peekReplayFile(name,peek),"a four-byte file was listed");}
  check(!peekReplayFile(name+".missing",peek),"a missing file was listed");}
}
}
int main(){try{run();std::cout<<"PASS: "<<checks<<" replay file checks\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
