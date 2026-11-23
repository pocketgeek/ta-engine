#include "sim/mission.h"
#include "sim/sim.h"
#include "hpi/hpi.h"
#include "client/retailsound.h"
#include <SDL.h>
#include <cstring>
#include <iostream>
#include <set>

int main(int argc,char**argv) {
    if(argc==2 && std::string(argv[1])=="--routes") {
        int flags,free;
        while(std::cin>>flags>>free) {
            const int priority=tak::retailSoundPriority(flags);
            std::cout<<(priority>1 || free ? priority : -1)<<'\n';
        }
        return 0;
    }
    int failures=0;
    auto check=[&](bool good,const char* why){if(!good){++failures;std::cerr<<"FAIL "<<why<<'\n';}};
    // A real mission VM emits two different names in the same Start callback.
    const std::vector<uint32_t> code={0x10021001,10,0x10072000,0,0x10024000,
        0x10021001,7,0x10072000,1,0x10024000,0x10021001,0,0x10065000};
    std::vector<uint8_t> cob(512);
    auto put=[&](size_t offset,uint32_t value){std::memcpy(cob.data()+offset,&value,4);};
    put(0,6);put(4,1);put(12,uint32_t(code.size()));put(24,52);put(28,56);
    put(36,128);put(44,60);put(48,2);put(52,0);put(56,72);put(60,80);put(64,96);
    std::memcpy(cob.data()+72,"Start",6);std::memcpy(cob.data()+80,"line_a.wav",11);
    std::memcpy(cob.data()+96,"line_b",7);
    for(size_t i=0;i<code.size();++i)put(128+i*4,code[i]);
    tak::sim::TypeRegistry registry;tak::tdf::Node header;
    header.values["victorytimerrunsout"]="100";
    tak::sim::World world;
    tak::sim::MissionScript mission(cob,header,registry,0,"audio fixture");
    mission.start(world);
    check(world.takeSoundRequests().empty(),"mission Start audio waits for first script tick");
    mission.step(world,1.f/30);
    const auto hash=world.stateHash();
    auto sounds=world.takeSoundRequests();
    check(sounds.size()==2,"same-tick mission lines survive independently");
    if(sounds.size()==2) {
        check(sounds[0].name=="line_a.wav" && sounds[0].flags==10,"authored first name and flags retained");
        check(sounds[1].name=="line_b" && sounds[1].flags==7,"authored second name and flags retained");
        check(sounds[0].seq+1==sounds[1].seq && sounds[0].tick==sounds[1].tick,"event order and mission tick retained");
    }
    check(world.takeSoundRequests().empty(),"consumed sounds do not replay");
    check(world.stateHash()==hash,"audio consumption cannot change authoritative state");
    for(int i=0;i<300;++i)world.requestSound(std::to_string(i));
    sounds=world.takeSoundRequests();
    check(sounds.size()==256 && sounds.front().name=="44" && sounds.back().name=="299",
          "headless audio storage remains bounded with latest requests in order");
    world.requestSound("stale.wav",4);
    world.resetForReplay();
    check(world.takeSoundRequests().empty() && world.soundRequest().seq==0,
          "replay reset discards previous mission audio and sequence");
    world.requestSound("new.wav",2);
    sounds=world.takeSoundRequests();
    check(sounds.size()==1 && sounds.front().seq==1 && sounds.front().tick==0 && sounds.front().name=="new.wav",
          "new replay starts a clean sound timeline");
    // Objective soundplayed is independent of completion: the polling rule can
    // become false again, whereas its sound must remain once-only.
    {
        tak::sim::World w;tak::sim::UnitType type;type.id="fixture";
        const int enemy=w.spawn(&type,100,100,0,1);
        tak::tdf::Node rules;rules.values={{"destroyallunits","1"},{"victorytimerrunsout","100"},{"deathtimerrunsout","100"}};
        tak::sim::MissionScript m({},rules,registry,0,"objective sound");m.start(w);
        m.step(w,1.f/30);check(w.takeSoundRequests().empty(),"incomplete objective is silent");
        w.unit(enemy)->deadFor=0;m.step(w,1.f/30);
        auto cues=w.takeSoundRequests();
        check(cues.size()==1 && cues[0].name=="Victory Condition" && cues[0].flags==7,
              "completed non-timer objective requests native cue");
        w.unit(enemy)->deadFor=-1;m.step(w,1.f/30);
        w.unit(enemy)->deadFor=0;m.step(w,1.f/30);
        check(w.takeSoundRequests().empty(),"objective sound does not repeat after false/true transition");
    }
    {
        tak::sim::World w;tak::sim::UnitType type;type.id="commander";
        const int enemy=w.spawn(&type,100,100,0,1);
        tak::tdf::Node rules;rules.values={{"killenemycommander","1"},{"victorytimerrunsout","100"},{"deathtimerrunsout","100"}};
        tak::sim::MissionScript m({},rules,registry,0,"event sound");m.setPlayerCommanders({&type,&type});m.start(w);
        w.unit(enemy)->deadFor=0;m.unitDied(w,enemy);
        check(w.takeSoundRequests().size()==1,"death objective emits at event before next poll");
        m.unitDied(w,enemy);m.step(w,1.f/30);
        check(w.takeSoundRequests().empty(),"event plus poll cannot duplicate objective cue");
    }
    {
        tak::sim::World w;tak::tdf::Node rules;
        rules.values={{"victorytimerrunsout","1"},{"deathtimerrunsout","1"}};
        tak::sim::MissionScript m({},rules,registry,0,"timer sound");m.start(w);
        for(int i=0;i<30;++i)m.step(w,1.f/30);
        check(w.takeSoundRequests().empty(),"timer victory and defeat have no invented success cue");
    }
    if(argc==2) {
        SDL_SetMainReady();auto vfs=tak::hpi::mountRetailRoot(argv[1]);
        std::set<std::string> referenced;int scripts=0,decoded=0,missing=0;
        for(const auto& path:vfs.list("missions")) {
            if(!path.ends_with(".cob"))continue;
            auto file=tak::cob::load(vfs.read(path),path);bool hasSound=false;
            for(size_t pc=0;pc+1<file.code.size();++pc)if(file.code[pc]==0x10072000) {
                const auto index=file.code[pc+1];if(index>=file.names.size())continue;
                hasSound=true;auto name=file.names[index];
                std::transform(name.begin(),name.end(),name.begin(),::tolower);
                if(name.ends_with(".wav"))name.resize(name.size()-4);
                referenced.insert(name);
            }
            scripts+=hasSound;
        }
        const auto scriptReferences=referenced.size();
        referenced.insert("victory condition");
        for(const auto& name:referenced) {
            const auto path="sounds/"+name+".wav";
            if(!vfs.has(path)) {++missing;std::cout<<"Absent authored mission WAV "<<path<<'\n';continue;}
            const auto bytes=vfs.read(path);SDL_AudioSpec spec{};Uint8* pcm=nullptr;Uint32 length=0;
            check(SDL_LoadWAV_RW(SDL_RWFromConstMem(bytes.data(),int(bytes.size())),1,&spec,&pcm,&length) && length,
                  "present mission WAV decodes");
            if(pcm)SDL_FreeWAV(pcm);
            ++decoded;
        }
        std::cout<<scripts<<" mission scripts reference "<<scriptReferences<<" clips, plus native objective cue: "<<decoded<<" decoded, "<<missing<<" absent\n";
    }
    std::cout<<(failures?"FAIL":"PASS")<<": campaign audio events ("<<failures<<" failures)\n";
    return failures?1:0;
}
