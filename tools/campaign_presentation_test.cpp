#include "campaign/campaign.h"
#include "hpi/hpi.h"
#include "sim/sim.h"
#include "sim/mission.h"
#include <cstring>
#include "sim/matchsetup.h"
#include <cstdio>
#include <stdexcept>

int main(int argc,char** argv) {
    auto check=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
    try {
        using namespace ta::sim;
        for(bool sameOwner:{false,true})for(bool allied:{false,true})for(bool unfinished:{false,true}) {
            UnitType attacker,victim;attacker.maxHp=victim.maxHp=100;
            World world;world.setTerrain(std::vector<uint8_t>(64*64,0),64,64,0);
            world.setMapPlacementFeatures(std::vector<uint16_t>(64*64,0xffff),{});
            const int from=world.spawn(&attacker,200,200,0,0);
            const int to=world.spawn(&victim,400,400,0,sameOwner?0:1);
            if(allied)world.setTeam(1,world.player(0).team);
            world.unit(to)->underConstruction=unfinished;
            world.unit(to)->lastHitBy=from;world.unit(to)->lastHitPlayer=0;world.unit(to)->hp=Fixed();
            // Ownership can change between impact and death accounting.
            world.unit(from)->player=1;
            world.tick(1.f/30.f);
            // TotalA.exe's death switch (0x4868b3..0x48690e): the hitter player's kill,
            // an owner test (an ally's unit counts), finished victims only; TA keeps no
            // per-kill score.
            check(world.player(0).kills==(sameOwner || unfinished?0:1) && world.player(0).score==0,
                  "a kill must count for the hitter player, excluding self but not allied owner");
            world.tick(1.f/30.f);
            check(world.player(0).kills==(sameOwner || unfinished?0:1),"a kill must count only once per death");
        }
        for(uint32_t slot:{0u,1u}) {
            // Real mission VM: SET40(GET40(slot)); native GET uses raw zero-based slot.
            const std::vector<uint32_t> code={0x10021001,40,0x10021001,40,
                0x10021001,slot,0x10021001,0,0x10021001,0,0x10021001,0,
                0x10043000,0x10082000,0x10021001,0,0x10065000};
            std::vector<uint8_t> cob(256);
            auto put=[&](size_t pos,uint32_t v){std::memcpy(cob.data()+pos,&v,4);};
            put(0,6);put(4,1);put(12,uint32_t(code.size()));put(24,52);put(28,56);
            put(36,128);put(44,60);put(52,0);put(56,72);
            std::memcpy(cob.data()+72,"Start",6);
            for(size_t i=0;i<code.size();++i)put(128+i*4,code[i]);
            TypeRegistry registry;ta::tdf::Node header;World world;
            world.player(0).score=2500;world.player(1).score=-17;
            world.setMission(std::make_unique<MissionScript>(cob,header,registry,0,"score fixture"));
            world.mission()->step(world,1.f/30);
            check(world.player(0).score==(slot==0?2500:-17),"mission GET40 must read selected score, not elapsed time");
            check(world.scoreAutomaticDisabled(),"mission SET40 must disable automatic accrual");
            auto hash=world.stateHash();++world.player(1).score;
            check(world.stateHash()!=hash,"campaign score must be authoritative hash input");
            UnitType type;type.maxHp=100;
            const int victim=world.spawn(&type,0,0,0,1);
            world.unit(victim)->lastHitPlayer=0;world.unit(victim)->hp=Fixed();
            hash=world.stateHash();world.unit(victim)->lastHitPlayer=1;
            check(world.stateHash()!=hash,"campaign future score attribution must be hashed");
            world.unit(victim)->lastHitPlayer=0;
            world.tick(1.f/30);
            check(world.player(0).score==(slot==0?2500:-17),"SET40 must prevent subsequent kill points");
            world.resetForReplay();check(!world.scoreAutomaticDisabled(),"replay reset must restore automatic scoring");
        }
        if(argc<2) {std::puts("PASS: native score accumulation and one-shot death accounting");return 0;}
        // With an install: TA's campaigns load (camps/*.tdf, docs/retail-campaign.md
        // section 1), each with its missions. Kingdoms' book screen (BOD art, chapter
        // paging) has no TA counterpart: TA picks a campaign on NEWGAME (tafrontend).
        const auto vfs=ta::hpi::mountRetailRoot(argv[1]);
        const auto camps=ta::loadCampaigns(vfs);check(camps.size()>=2,"campaign roster absent");
        for(const auto& c:camps)check(c.count()>0,"a campaign without missions");
        std::puts("PASS: TA campaign roster and score accounting");return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
