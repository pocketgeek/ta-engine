#include "client/campaignscreen.h"
#include "client/uitext.h"
#include "client/settings.h"
#include "client/gpuvram.h"
#include "campaign/campaign.h"
#include "hpi/hpi.h"
#include "sim/sim.h"
#include "sim/mission.h"
#include <cstring>
#include "sim/matchsetup.h"
#include "util/png.h"
#include <cstdio>
#include <stdexcept>

int main(int argc,char** argv) {
    auto check=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
    try {
        using namespace tak::sim;
        for(bool sameOwner:{false,true})for(bool allied:{false,true})for(bool unfinished:{false,true}) {
            UnitType attacker,victim;attacker.maxHp=victim.maxHp=100;victim.experiencePoints=1234;
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
            check(world.player(0).score==(sameOwner || unfinished?0:1234),"score must use victim value, excluding self but not allied owner");
            world.tick(1.f/30.f);
            check(world.player(0).score==(sameOwner || unfinished?0:1234),"score must award only once per death");
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
            TypeRegistry registry;tak::tdf::Node header;World world;
            world.player(0).score=2500;world.player(1).score=-17;
            world.setMission(std::make_unique<MissionScript>(cob,header,registry,0,"score fixture"));
            world.mission()->step(world,1.f/30);
            check(world.player(0).score==(slot==0?2500:-17),"mission GET40 must read selected score, not elapsed time");
            check(world.scoreAutomaticDisabled(),"mission SET40 must disable automatic accrual");
            auto hash=world.stateHash();++world.player(1).score;
            check(world.stateHash()!=hash,"campaign score must be authoritative hash input");
            UnitType type;type.maxHp=100;type.experiencePoints=1234;
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
        const auto vfs=tak::hpi::mountRetailRoot(argv[1]);
        const auto camps=tak::loadCampaigns(vfs);check(camps.size()>=2,"campaign roster absent");
        for(bool crusades:{false,true}) {
            TypeRegistry types;setupRegistry(types,vfs,crusades);
            const auto* archer=types.find("araarch");
            check(archer && archer->experiencePoints>0,"authored experiencepoints not loaded");
        }
        SDL_setenv("SDL_VIDEODRIVER","dummy",1);check(SDL_Init(SDL_INIT_VIDEO)==0,SDL_GetError());
        auto* surface=SDL_CreateRGBSurfaceWithFormat(0,960,720,32,SDL_PIXELFORMAT_ABGR8888);
        auto* ren=SDL_CreateSoftwareRenderer(surface);check(ren!=nullptr,SDL_GetError());
        tak::Settings settings;
        for(size_t tab=0;tab<2;++tab) {
            const auto before=gpuvram::count();
            {
                std::vector<std::string> sounds;
                tak::CampaignScreen screen(ren,vfs,settings,int(tab),[&](const auto& sound){sounds.push_back(sound);});
                screen.render(960,720);
                check(gpuvram::count()>before+75,"authored book/chapter/font art missing");
                if(argc>2) {
                    const std::string name=std::string(argv[2])+"-"+std::to_string(tab)+".png";
                    std::vector<uint8_t> rgba(960*720*4);
                    SDL_RenderReadPixels(ren,nullptr,SDL_PIXELFORMAT_ABGR8888,rgba.data(),960*4);
                    tak::png::write(name,960,720,rgba);
                }
                SDL_Event e{};e.type=SDL_KEYDOWN;e.key.keysym.sym=SDLK_HOME;
                screen.input(e,960,720);e.key.keysym.sym=SDLK_RIGHT;screen.input(e,960,720);
                e.key.keysym.sym=SDLK_RETURN;check(screen.input(e,960,720),"Enter did not choose chapter");
                check(screen.pickedStem()==camps[tab].missions[1].stem,"chapter paging chose wrong mission");
                check(sounds==std::vector<std::string>{"ok.wav"},"Play must route authored GUI click sound");
            }
            tak::releaseUiTextGlyphs();   // the shared interface-font cache, not the screen's
            check(gpuvram::count()==before,"campaign screen leaked authored textures");
        }
        {
            tak::CampaignScreen screen(ren,vfs,settings,1);
            SDL_Event e{};e.type=SDL_KEYDOWN;e.key.keysym.sym=SDLK_END;screen.input(e,960,720);
            e.key.keysym.sym=SDLK_RETURN;screen.input(e,960,720);
            const auto& c=camps[1];check(screen.pickedStem()==(c.altFinal.empty()?c.missions.back().stem:c.altFinal),"last chapter/alternate inaccessible");
        }
        tak::releaseUiTextGlyphs();SDL_DestroyRenderer(ren);SDL_FreeSurface(surface);SDL_Quit();
        std::puts("PASS: campaign book assets, chapter input, all chapters accessible, texture teardown and score accounting");return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
