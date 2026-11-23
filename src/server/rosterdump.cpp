#include "server/rosterdump.h"
#ifndef NDEBUG
#include "ai/ai.h"
#include "sim/sim.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
namespace tak::srv {
namespace {
struct Dump {
    std::FILE* file=nullptr;
    ~Dump() {if(file)std::fclose(file);}
};
struct Settings {
    const char* path=std::getenv("TAK_ROSTER_DUMP");
    uint32_t every=[] {
        const char* e=std::getenv("TAK_ROSTER_EVERY");
        const long v=e?std::strtol(e,nullptr,10):30;
        return uint32_t(v>0?v:30);
    }();
};
const Settings& settings() {static const Settings s;return s;}
// Room workers run on their own threads; each room has its own file.
std::mutex gMutex;
std::map<uint32_t,std::unique_ptr<Dump>> gDumps;
bool gFirstTaken=false;

const char* controllerOf(int player,const std::vector<ai::Controller>& controllers) {
    for(const auto& c:controllers)if(c.player()==player)return "ai";
    return "human";
}
// Writes retail's construction-remaining fraction: 1 = just placed, 0 = finished.
double remaining(const sim::Unit& u) {
    return u.underConstruction?1.0-double(u.constructionFraction()):0.0;
}
}

void rosterDump(uint32_t roomId,const sim::World& world,const std::vector<ai::Controller>& controllers) {
    const Settings& s=settings();
    if(!s.path || !*s.path)return;
    const uint32_t tick=world.tickCount();
    const bool first=tick==1;
    if(!first && tick%s.every!=0)return;
    std::FILE* f=nullptr;
    {
        std::lock_guard lock(gMutex);
        auto& slot=gDumps[roomId];
        if(!slot) {
            slot=std::make_unique<Dump>();
            std::string path=s.path;
            if(gFirstTaken)path+=".room"+std::to_string(roomId);
            gFirstTaken=true;
            slot->file=std::fopen(path.c_str(),"w");
            if(!slot->file) {std::fprintf(stderr,"TAK_ROSTER_DUMP: cannot write %s\n",path.c_str());return;}
            std::fprintf(slot->file,"{\"kind\":\"meta\",\"schema\":1,\"engine\":\"tak\",\"tick_rate\":30,"
                         "\"every\":%u,\"room\":%u,\"players\":[",s.every,roomId);
            for(int p=0;p<world.numPlayers();++p) {
                int level=-1;
                for(const auto& c:controllers)if(c.player()==p)level=int(c.difficulty());
                std::fprintf(slot->file,"%s{\"p\":%d,\"controller\":\"%s\",\"level\":%d}",p?",":"",p,
                             controllerOf(p,controllers),level);
            }
            std::fprintf(slot->file,"]}\n");
        }
        f=slot->file;
    }
    if(!f)return;
    std::fprintf(f,"{\"kind\":\"sample\",\"tick\":%u,\"players\":[",tick);
    for(int p=0;p<world.numPlayers();++p) {
        const sim::Player& pl=world.player(p);
        std::fprintf(f,"%s{\"p\":%d,\"controller\":\"%s\",\"mana\":%.1f,\"storage\":%.1f,\"defeated\":%s}",
                     p?",":"",p,controllerOf(p,controllers),pl.mana,double(pl.manaCapacity()),
                     pl.defeated?"true":"false");
    }
    std::fprintf(f,"],\"units\":[");
    bool comma=false;
    for(const sim::Unit& u:world.units()) {
        if(!u.alive() || !u.type)continue;
        std::fprintf(f,"%s[%d,%d,\"%s\",%.3f,%d,%d]",comma?",":"",u.id,u.player,u.type->id.c_str(),
                     remaining(u),int(std::floor(u.x.toFloat())),int(std::floor(u.z.toFloat())));
        comma=true;
    }
    // Squads: none yet. The Classic brain will add {"p","squads":[...]} here in
    // retail_ai_poll.py's layout once it has them.
    std::fprintf(f,"]}\n");
    std::fflush(f);
}
}
#endif
