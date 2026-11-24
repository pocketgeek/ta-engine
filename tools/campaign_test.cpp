#include "campaign/campaign.h"
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>

int main(int argc,char** argv) {
    int failures=0;
    auto check=[&](bool valid,const char* why){if(!valid){std::fprintf(stderr,"FAIL %s\n",why);++failures;}};
    ta::Campaign campaign;
    campaign.missions={{"mission39a","","First title"},{"mission39","","Second title"},{"final","",""}};
    campaign.altFinal="otherfinal";campaign.altTitle="Alternate title";
    auto first=ta::campaignChapter(campaign,"mission39a");
    check(first && first->index==0 && first->title=="First title" && first->nextStem=="mission39",
          "campaign order follows authored list, not filename suffix");
    auto last=ta::campaignChapter(campaign,"final");
    check(last && last->title=="CHAPTER 3" && last->nextStem.empty() && !last->alternate,
          "ordinary finale is terminal with a useful missing-title fallback");
    auto alternate=ta::campaignChapter(campaign,"otherfinal");
    check(alternate && alternate->alternate && alternate->index==2 && alternate->title=="Alternate title" && alternate->nextStem.empty(),
          "alternate finale has authored title and no next chapter");
    check(!ta::campaignChapter(campaign,"") && !ta::campaignChapter(campaign,"unknown"),"unknown chapter cannot advance progress");
    {
        // TA_FORCE_WIN / TA_FORCE_LOSE's rewrite: the forced rule heads [GlobalHeader];
        // the replaced rules are renamed out of the parser's sight; nothing else moves.
        const std::string ota="[GlobalHeader]\n\t{\n\tmissionname=1: A Hero Returns;\n"
            "\tMoveUnitToRadius=ANYTYPE, 992, 656, 64;\n\tAllUnitsKilled=1;\n\tkillenemycommander = 1;\n"
            "\tDeathTimerRunsOut=600;\n\t[Schema 0]\n\t\t{\n\t\tType=Easy;\n\t\t}\n\t}\n";
        const std::string won=ta::forceMissionOutcome(ota,true,5);
        check(won=="[GlobalHeader]\n\t{\n\tVictoryTimerRunsOut=5;\n\tmissionname=1: A Hero Returns;\n"
              "\tForcedMoveUnitToRadius=ANYTYPE, 992, 656, 64;\n\tAllUnitsKilled=1;\n\tForcedkillenemycommander = 1;\n"
              "\tDeathTimerRunsOut=600;\n\t[Schema 0]\n\t\t{\n\t\tType=Easy;\n\t\t}\n\t}\n",
              "a forced win's only victory rule is its timer");
        const std::string lost=ta::forceMissionOutcome(ota,false,7);
        check(lost=="[GlobalHeader]\n\t{\n\tDeathTimerRunsOut=7;\n\tmissionname=1: A Hero Returns;\n"
              "\tMoveUnitToRadius=ANYTYPE, 992, 656, 64;\n\tAllUnitsKilled=1;\n\tkillenemycommander = 1;\n"
              "\tForcedDeathTimerRunsOut=600;\n\t[Schema 0]\n\t\t{\n\t\tType=Easy;\n\t\t}\n\t}\n",
              "a forced loss adds its death timer and keeps the victory rules");
        check(ta::forceMissionOutcome("[Other]\n{\nMoveUnitToRadius=1;\n}\n",true,5)=="[Other]\n{\nMoveUnitToRadius=1;\n}\n",
              "no [GlobalHeader]: unchanged");
        // Without TA_FORCE_WIN / TA_FORCE_LOSE (and in every release build) no view is made.
        if(!std::getenv("TA_FORCE_WIN") && !std::getenv("TA_FORCE_LOSE"))
            check(!ta::forcedOutcomeMission(ta::hpi::Vfs{},"AC01.ota:1"),"no forced mission unless asked");
    }
    if(argc>1) {
        const auto vfs=ta::hpi::mountRetailRoot(argv[1]);
        const auto campaigns=ta::loadCampaigns(vfs);
        int chapters=0;
        for(const auto& c:campaigns) {
            for(size_t i=0;i<c.missions.size();++i) {
                const auto chapter=ta::campaignChapter(c,c.missions[i].stem);
                check(chapter && chapter->index==int(i) && !chapter->title.empty(),"mounted chapter identity and title");
                ++chapters;
            }
            if(!c.altFinal.empty()) {
                const auto chapter=ta::campaignChapter(c,c.altFinal);
                check(chapter && chapter->alternate && !c.altTitle.empty(),"mounted alternate title retained");
                ++chapters;
            }
        }
        // docs/retail-campaign.md section 1.1: Arm and Core 25 each, Core
        // Contingency 12 each, Battle Tactics 46 Arm + 54 Core, Krogoth
        // Encounter 1. No TA campaign has an alternate finale.
        check(chapters==175,"installed base, expansion and Battle Tactics chapter count");
        std::printf("mounted chapters: %d\n",chapters);
        {
            // Player=1 placements are player index 0, the human; Player=2 is
            // index 1, the first computer (docs/retail-campaign.md section 2.4,
            // TotalA.exe 0x436c30). AC01 places both.
            ta::sim::TypeRegistry registry;
            ta::sim::setupRegistry(registry,vfs);
            auto seatCheck=[&](const ta::hpi::Vfs& from,const std::string& stem,int aiSlot,const char* what) {
                ta::sim::World world;world.setSerialThreads(true);
                int human=-1;
                ta::sim::MissionSetup ms;
                const bool loaded=ta::sim::setupMission(world,registry,from,stem,human,&ms);
                check(loaded,what);
                if(!loaded)return;
                const bool aiOnHuman=std::find(ms.aiSlots.begin(),ms.aiSlots.end(),0)!=ms.aiSlots.end();
                const bool aiOnEnemy=std::find(ms.aiSlots.begin(),ms.aiSlots.end(),aiSlot)!=ms.aiSlots.end();
                int humanUnits=0;
                for(const auto& u:world.units())if(u.alive() && u.player==0)++humanUnits;
                std::printf("seat %s: human=%d ai=%zu humanUnits=%d allied(0,%d)=%d\n",stem.c_str(),human,
                            ms.aiSlots.size(),humanUnits,aiSlot,int(world.allied(0,aiSlot)));
                check(human==0,"the human is Player1, world slot 0");
                check(!aiOnHuman && aiOnEnemy,"the human gets no AI; the Player=2 computer keeps one");
                check(!world.allied(0,aiSlot),"the human is not teamed with the mission's opponent");
                check(humanUnits>0,"Player=1 placements belong to the human");
            };
            seatCheck(vfs,"ac01",1,"ac01 loads");
        }
        if(argc>2 && std::string(argv[2])=="--startup") {
            // A condition rewrite must not silently end real missions as soon
            // as their initial script runs. Exercise every chapter's actual
            // placements/conditions, not only synthetic predicate fixtures.
            {
                ta::sim::TypeRegistry registry;
                ta::sim::setupRegistry(registry,vfs);
                for(const auto& c:campaigns) {
                    std::vector<std::string> stems;
                    for(const auto& m:c.missions)stems.push_back(m.stem);
                    if(!c.altFinal.empty())stems.push_back(c.altFinal);
                    for(const auto& stem:stems) {
                        ta::sim::World world;world.setSerialThreads(true);
                        world.setDoubleSight(true); // prior skirmish setting must not leak into a mission
                        int human=0;
                        const bool loaded=ta::sim::setupMission(world,registry,vfs,stem,human);
                        check(loaded,"mounted mission startup loads");
                        if(!loaded)continue;
                        ta::sim::UnitType eye;eye.sight=123;eye.radar=234;
                        check(world.sightDistance(eye)==123 && world.radarDistance(eye)==234,
                              "campaign resets skirmish sight/radar multiplier");
                        for(int tick=0;tick<30 && !world.missionOutcome();++tick)world.tick(1.f/30.f);
                        std::printf("startup %s tick=%u outcome=%d units=%zu\n",stem.c_str(),
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
