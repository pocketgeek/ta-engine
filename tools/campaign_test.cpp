#include "campaign/campaign.h"
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"
#include <algorithm>
#include <cstdio>
#include <memory>

int main(int argc,char** argv) {
    int failures=0;
    auto check=[&](bool valid,const char* why){if(!valid){std::fprintf(stderr,"FAIL %s\n",why);++failures;}};
    tak::Campaign campaign;
    campaign.missions={{"mission39a","","First title"},{"mission39","","Second title"},{"final","",""}};
    campaign.altFinal="otherfinal";campaign.altTitle="Alternate title";
    auto first=tak::campaignChapter(campaign,"mission39a");
    check(first && first->index==0 && first->title=="First title" && first->nextStem=="mission39",
          "campaign order follows authored list, not filename suffix");
    auto last=tak::campaignChapter(campaign,"final");
    check(last && last->title=="CHAPTER 3" && last->nextStem.empty() && !last->alternate,
          "ordinary finale is terminal with a useful missing-title fallback");
    auto alternate=tak::campaignChapter(campaign,"otherfinal");
    check(alternate && alternate->alternate && alternate->index==2 && alternate->title=="Alternate title" && alternate->nextStem.empty(),
          "alternate finale has authored title and no next chapter");
    check(!tak::campaignChapter(campaign,"") && !tak::campaignChapter(campaign,"unknown"),"unknown chapter cannot advance progress");
    if(argc>1) {
        const auto vfs=tak::hpi::mountRetailRoot(argv[1]);
        const auto campaigns=tak::loadCampaigns(vfs);
        int chapters=0;
        for(const auto& c:campaigns) {
            for(size_t i=0;i<c.missions.size();++i) {
                const auto chapter=tak::campaignChapter(c,c.missions[i].stem);
                check(chapter && chapter->index==int(i) && !chapter->title.empty(),"mounted chapter identity and title");
                ++chapters;
            }
            if(!c.altFinal.empty()) {
                const auto chapter=tak::campaignChapter(c,c.altFinal);
                check(chapter && chapter->alternate && !c.altTitle.empty(),"mounted alternate title retained");
                ++chapters;
            }
        }
        check(chapters==74,"installed base, expansion and alternate-ending chapter count");
        std::printf("mounted chapters: %d\n",chapters);
        {
            // The human is Player1 (world slot 0) whatever its keywords, as in
            // retail (0x4d290c). takx22_mt marks Player1 `strategic opponent` for
            // the city the player defends; a synthetic mission adds a plain
            // Player2, which the old "first non-AI player" guess would have seated.
            tak::sim::TypeRegistry registry;
            tak::sim::setupRegistry(registry,vfs,false);
            auto seatCheck=[&](const tak::hpi::Vfs& from,const std::string& stem,int aiSlot,const char* what) {
                tak::sim::World world;world.setSerialThreads(true);
                int human=-1;
                tak::sim::MissionSetup ms;
                const bool loaded=tak::sim::setupMission(world,registry,from,stem,human,&ms);
                check(loaded,what);
                if(!loaded)return;
                const bool aiOnHuman=std::find(ms.aiSlots.begin(),ms.aiSlots.end(),0)!=ms.aiSlots.end();
                const bool aiOnEnemy=std::find(ms.aiSlots.begin(),ms.aiSlots.end(),aiSlot)!=ms.aiSlots.end();
                int humanUnits=0;
                for(const auto& u:world.units())if(u.alive() && u.player==0)++humanUnits;
                std::printf("seat %s: human=%d ai=%zu humanUnits=%d allied(0,%d)=%d\n",stem.c_str(),human,
                            ms.aiSlots.size(),humanUnits,aiSlot,int(world.allied(0,aiSlot)));
                check(human==0,"the human is Player1, world slot 0");
                check(!aiOnHuman && aiOnEnemy,"Player1's keywords give the human no AI; the opponent keeps one");
                check(!world.allied(0,aiSlot),"the human is not teamed with the mission's opponent");
                check(!world.player(0).automaticGates,"the human's gates stay manual");
                check(humanUnits>0,"Player=1 placements belong to the human");
            };
            seatCheck(vfs,"takx22_mt",1,"takx22_mt loads");
            const std::string tnt="missions/takx22_mt.tnt";
            if(vfs.has(tnt)) {
                const std::string ota=
                    "[GlobalHeader]\n{\n"
                    "Player1=strategic opponent logo 6 creon;\n"
                    "Player2=logo 3 aramon;\n"
                    "Player3=strategic opponent logo 1 taros;\n"
                    "[Map Data]\n{\n[units]\n{\n"
                    "[unit0]\n{\nUnitname=ARAKNIGH;\nXPos=40;\nZPos=40;\nPlayer=1;\n}\n"
                    "[unit1]\n{\nUnitname=ARAKNIGH;\nXPos=60;\nZPos=40;\nPlayer=2;\n}\n"
                    "[unit2]\n{\nUnitname=ARAKNIGH;\nXPos=80;\nZPos=40;\nPlayer=3;\n}\n"
                    "}\n}\n}\n";
                auto files=std::make_shared<tak::hpi::Vfs::Files>();
                (*files)[tak::hpi::MountSet::key("missions/seattest.tnt")]=vfs.read(tnt);
                (*files)[tak::hpi::MountSet::key("missions/seattest.ota")]=std::vector<uint8_t>(ota.begin(),ota.end());
                tak::hpi::Vfs overlay(&vfs);
                overlay.setModFiles(files);
                seatCheck(overlay,"seattest",2,"synthetic seat mission loads");
                tak::sim::World world;world.setSerialThreads(true);
                int human=-1;
                if(tak::sim::setupMission(world,registry,overlay,"seattest",human)) {
                    int owner[3]={-1,-1,-1};
                    for(const auto& u:world.units())
                        if(u.alive() && u.x.toFloat()<16*90)owner[std::clamp(int(u.x.toFloat())/(16*20)-2,0,2)]=u.player;
                    check(owner[0]==0 && owner[1]==1 && owner[2]==2,"Player1/2/3 placements land in slots 0/1/2");
                    check(world.allied(0,1),"a plain Player2 is a friendly computer player, not the human");
                }
            }
        }
        if(argc>2 && std::string(argv[2])=="--startup") {
            // A condition rewrite must not silently end real missions as soon
            // as their initial script runs. Exercise every chapter's actual
            // placements/conditions, not only synthetic predicate fixtures.
            for(bool crusades:{false,true}) {
                tak::sim::TypeRegistry registry;
                tak::sim::setupRegistry(registry,vfs,crusades);
                for(const auto& c:campaigns) {
                    std::vector<std::string> stems;
                    for(const auto& m:c.missions)stems.push_back(m.stem);
                    if(!c.altFinal.empty())stems.push_back(c.altFinal);
                    for(const auto& stem:stems) {
                        tak::sim::World world;world.setSerialThreads(true);
                        world.setDoubleSight(true); // prior skirmish setting must not leak into a mission
                        int human=0;
                        const bool loaded=tak::sim::setupMission(world,registry,vfs,stem,human);
                        check(loaded,"mounted mission startup loads");
                        if(!loaded)continue;
                        tak::sim::UnitType eye;eye.sight=123;eye.radar=234;
                        check(world.sightDistance(eye)==123 && world.radarDistance(eye)==234,
                              "campaign resets skirmish sight/radar multiplier");
                        for(int tick=0;tick<30 && !world.missionOutcome();++tick)world.tick(1.f/30.f);
                        std::printf("startup %s crusades=%d tick=%u outcome=%d units=%zu\n",stem.c_str(),crusades,
                                    world.tickCount(),world.missionOutcome(),world.units().size());
                        check(world.missionOutcome()==0,"mission must remain playable through its opening second");
                        std::fflush(stdout);
                    }
                }
            }
        }
    }
    std::printf("%s: campaign chapter identity and progression\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
