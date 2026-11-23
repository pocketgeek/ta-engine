// Exercise the renderer-facing VM with every loaded unit script. The reference
// integer VM is separately checked against KINGDOMS.icd by the Python oracles.
#include "cob/vm.h"
#include "client/retailaim.h"
#include "cob/retailstate.h"
#include "sim/matchsetup.h"
#include "sim/retailrng.h"
#include "sim/retailanimationqueries.h"
#include "client/retaildeathsfx.h"
#include "hpi/hpi.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <string_view>

struct Host {
    int phase=0;
    uint32_t seed=12345;
    uint32_t random(int32_t bound) { return tak::sim::retailRandom(seed,bound); }
    uint32_t get(int id,const std::array<uint32_t,4>&) {
        switch(id) {
        case 4:return phase==3 ? 25:100;
        case 18:return 1;
        case 28:return phase==2;
        case 29:return phase==1 || phase==2 ? 100:0;
        case 32:return phase==3 ? 10:0;
        case 33:return phase==1 ? 100:0;
        case 34:return phase==1;
        case 46:return phase==3;
        default:return 0;
        }
    }
    void set(int,int) {}
    uint32_t sound(int,int32_t) {return 0;}
    void effect(uint32_t,int,int32_t) {}
};
int main(int argc,char**argv) {
    const std::string_view mode=argc>1 ? argv[1] : "";
    if((argc==3 || argc==5) &&
       (mode=="--death-sfx-timeline" || mode=="--death-render-timeline")) {
        const bool renderHost=mode=="--death-render-timeline";
        auto file=tak::cob::load(std::filesystem::path(argv[2]));
        const int severity=argc==5 ? std::stoi(argv[3]) : 100;
        const int damageType=argc==5 ? std::stoi(argv[4]) : 1;
        tak::cob::Vm dying(file);dying.enableRetailAnimation();
        int tick=0;
        bool ownerVmStopRequested=false;
        dying.onEmitSfx=[&](int piece,int32_t code) {
            if(code>=260 && code<=262)
                std::cout<<"E "<<tick<<' '<<piece<<' '<<code<<'\n';
        };
        dying.onSetUnitValue=[&](int32_t id,int32_t value) {
            std::cout<<"U "<<tick<<' '<<id<<' '<<value<<'\n';
            if(renderHost && tak::retailOwnerVmStopsOnSetUnitValue(id))
                ownerVmStopRequested=true;
        };
        dying.onGet=[](int query,const std::vector<int32_t>&) {
            return query==4 ? 100 : query==18 ? 1 : 0;
        };
        dying.start("Create");
        for(int frame=0;frame<30;++frame)dying.tick(1.f/30);
        dying.reset();dying.setStatic(0,0);
        dying.start("Killed",{severity,0,damageType});
        if(!dying.start("Dying",{damageType}))dying.start("death");
        for(int frame=0;frame<600;++frame) {
            tick=frame+1;
            // GameView lets both death call-ins start synchronously, then its
            // render-host VM loop stops on native SET26/31 owner retirement.
            if(!renderHost || tak::retailOwnerVmMayAdvance(ownerVmStopRequested))
                dying.tick(1.f/30);
        }
        return 0;
    }
    if(argc==2 && std::string_view(argv[1])=="--standing-order") {
        unsigned enabled,standing,move,fire,request;
        while(std::cin>>enabled>>standing>>move>>fire>>request) {
            tak::sim::UnitType type;type.canSetStance=enabled;
            type.defaultStandingOrder=standing;type.defaultMove=move;type.defaultFire=fire;
            tak::sim::World world;
            const int id=world.spawn(&type,100,100,0,0);
            if(request<3)world.setStance(id,2-int(request));
            const auto& u=*world.unit(id);
            std::cout<<unsigned(u.standingOrder)<<' '<<unsigned(u.moveState)<<' '
                     <<unsigned(u.fireState)<<'\n';
        }
        return 0;
    }
    if(argc!=2)return 2;
    auto vfs=tak::hpi::mountRetailRoot(argv[1]);
    tak::sim::TypeRegistry registry;tak::sim::setupRegistry(registry,vfs,false);
    int scripts=0,failures=0;
    // Aradrag has 3s, 2.5s, and 5s weapons. The renderer used to notify
    // the first slot's 3000ms, shortening the script's restore delay.
    if(const auto* dragon=registry.find("aradrag");!dragon || dragon->maxWeaponReloadMs!=5000) {
        std::printf("FAIL Aradrag SetMaxReloadTime must include its third weapon\n");
        ++failures;
    }
    size_t serialFrames=0,parallelFrames=0,explosions=0;
    std::map<int32_t,size_t> emissions;
    for(const auto& [name,type]:registry.types()) {
        const auto* file=type.script();if(!file)continue;
        ++scripts;
        std::map<int32_t,size_t> deathEmissions;
        std::map<int32_t,int> deathLatestTick;
        int deathTick=0;
        bool captureDeathSfx=false;
        tak::cob::Vm display(*file);display.enableRetailAnimation();
        display.onEmitSfx=[&](int,int32_t code){++emissions[code];};
        const auto reachability=tak::cob::explosionReachability(*file);
        bool serial=true;
        display.onExplode=[&](int,int32_t) {
            ++explosions;
            if(!serial) {std::printf("FAIL %s explosion classified parallel\n",name.c_str());++failures;}
            return true;
        };
        tak::cob::RetailScriptState reference(*file);Host host;
        display.onGet=[&](int id,const std::vector<int32_t>&){return int32_t(host.get(id,{}));};
        auto notify=[&](const char* name,std::vector<int32_t> args={}) {
            const int script=file->scriptIndex(name);if(script<0)return;
            serial=true; // Client notifications execute on the main thread.
            display.start(name,args);
            std::array<uint32_t,4> values{};
            for(size_t i=0;i<args.size();++i)values[i]=uint32_t(args[i]);
            reference.startArguments(*file,script,values,unsigned(args.size()));
            reference.tick(*file,0,host);
        };
        notify("Create");notify("SetMaxReloadTime",{type.maxWeaponReloadMs});
        bool matches=true;
        for(int tick=0;tick<1200 && matches;++tick) {
            host.phase=(tick/300)%4;
            if(tick==30){notify("Activate");notify("BeginFlight");notify("setSFXoccupy",{5});}
            if(tick==150)notify("StartBuilding");
            if(tick==300){notify("StopBuilding");notify("MoveRate",{2});notify("TurnDirection",{5});}
            if(tick==450){notify("AimWeapon",{0,0,0,0});notify("FireWeapon",{0});}
            if(tick==600){notify("TargetCleared",{0});notify("WindChange",{10,8192});}
            if(tick==750)notify("StartCloaking");
            if(tick==900){notify("StopCloaking");notify("BeginLanding");notify("setSFXoccupy",{0});}
            if(tick==1050)notify("Deactivate");
            // Two display frames must advance exactly one retail simulation tick.
            for(int frame=0;frame<2;++frame) {
                serial=display.mayReachExplosion(reachability);
                ++(serial?serialFrames:parallelFrames);
                display.tick(1.f/60);
            }
            reference.tick(*file,1,host);
            for(size_t i=0;i<reference.pieces.size() && matches;++i) {
                const auto& a=display.pieces()[i];const auto& b=reference.pieces[i];
                matches=a.visible==b.visible;
                for(int axis=0;axis<3;++axis)
                    matches=matches && std::abs(a.move[axis]-float(b.move[axis])/65536)<0.0001f &&
                        std::abs(a.rot[axis]-float(b.turn[axis])*(6.28318530717959f/65536))<0.0001f;
            }
            if(!matches)std::printf("FAIL %s at tick %d\n",name.c_str(),tick);
        }
        failures+=!matches;
        // Death sequences include branches and delayed child scripts that are
        // dormant during the normal callback timeline above.
        for(int severity:{1,100,1000})for(int damageType:{0,1}) {
            tak::cob::Vm dying(*file);dying.enableRetailAnimation();
            dying.onEmitSfx=[&](int,int32_t code){
                ++emissions[code];
                if(captureDeathSfx) {++deathEmissions[code];deathLatestTick[code]=deathTick;}
            };
            bool deathSerial=true;
            dying.onGet=[](int query,const std::vector<int32_t>&) {return query==4?100:0;};
            dying.onExplode=[&](int,int32_t) {
                ++explosions;
                if(!deathSerial) {
                    std::printf("FAIL %s delayed death explosion classified parallel\n",name.c_str());
                    ++failures;
                }
                return true;
            };
            // Match the production death edge: Create has started its background
            // threads, then reset() stops them before Killed/Dying are installed.
            dying.start("Create");
            for(int frame=0;frame<30;++frame)dying.tick(1.f/30);
            dying.reset();dying.setStatic(0,0);
            captureDeathSfx=true;deathTick=0;
            dying.start("Killed",{severity,0,damageType});
            if(!dying.start("Dying",{damageType}))dying.start("death");
            for(int frame=0;frame<600;++frame) {
                deathTick=frame+1;
                deathSerial=dying.mayReachExplosion(reachability);
                ++(deathSerial?serialFrames:parallelFrames);
                dying.tick(1.f/30);
            }
            captureDeathSfx=false;
        }
        const auto requiresDeathEffect=[&](int32_t code) {
            const bool required=(name=="tarmage" && code==260) ||
                                (name=="tarhel" && code==261) ||
                                (name=="crefire" && code==262);
            if(required && !deathEmissions[code]) {
                std::printf("FAIL %s Killed timeline omitted native SFX code %d\n",
                            name.c_str(),code);
                ++failures;
            }
        };
        for(int32_t code:{260,261,262})requiresDeathEffect(code);
        for(const auto& [code,count]:deathEmissions) {
            const auto family=tak::retailDeathSfxFamily(code);
            if(count && family && *family!=tak::RetailDeathSfxFamily::DamageFlame) {
                std::printf("FAIL %s reset-separated death timeline emitted live-only SFX code %d\n",
                            name.c_str(),code);
                ++failures;
            }
        }
        for(const auto& [code,lastTick]:deathLatestTick) {
            const auto family=tak::retailDeathSfxFamily(code);
            if(family && *family!=tak::RetailDeathSfxFamily::Detached && lastTick>=120) {
                std::printf("FAIL %s death SFX %d emitted at tick %d, after owner handoff\n",
                            name.c_str(),code,lastTick);
                ++failures;
            }
        }
    }
    // GET 46 is standingunitorder, independently of current movement/targets.
    // These two shipped scripts holster in passive stance, even with an explicit
    // target, and draw in either active stance even while idle.
    for(const char* name:{"verbers","vercrus"}) {
        const auto* type=registry.find(name);
        if(!type || !type->script()){++failures;continue;}
        tak::sim::World world;const int id=world.spawn(type,100,100,0,0);
        tak::cob::Vm display(*type->script());display.enableRetailAnimation();
        display.onGet=[&](int query,const std::vector<int32_t>&) {
            if(query==46)return int32_t(world.unit(id)->standingOrder);
            return query==4 ? 100:0;
        };
        display.start("Create");
        for(int stance:{0,2,1,2,0}) {
            world.setStance(id,stance);
            world.unit(id)->orders.clear();
            if(stance==2) {
                tak::sim::Order attack;attack.targetId=123;
                world.unit(id)->orders.push_back(attack);
            }
            for(int tick=0;tick<600;++tick)display.tick(1.f/30);
            if(display.getStatic(7)!=(stance!=2)) {
                std::printf("FAIL %s holster state in stance %d\n",name,stance);++failures;
            }
        }
    }
    if(const auto* tower=registry.find("vertower");tower && tower->script()) {
        const auto& file=*tower->script();
        {
            tak::cob::Vm display(file);display.enableRetailAnimation();
            tak::RetailAimState aim;
            display.onGet=[](int query,const std::vector<int32_t>&){return query==4 ? 100:0;};
            display.onSetUnitValue=[&](int query,int slot){if(slot==0)aim.set(query);};
            display.start("Create");
            int starts=0;
            auto advance=[&](uint16_t heading) {
                if(aim.start(heading,0)) {
                    ++starts;display.start("AimWeapon",{heading,0,0});
                }
                aim.ready(heading,0,1024,true,true);
                display.tick(1.f/30);
            };
            // The tower needs several seconds to turn: preserve its waiting
            // thread, then retain readiness while the target stays stationary.
            for(int tick=0;tick<300;++tick)advance(49152);
            if(starts!=1 || !(aim.flags&8)) {
                std::puts("FAIL tower stationary aim was restarted or never became ready");++failures;
            }
            // An out-of-tolerance target consumes seven refresh steps before
            // the next AimWeapon. Its new turn must again finish undisturbed.
            for(int tick=0;tick<7;++tick)advance(32768);
            if(starts!=1 || (aim.flags&0xe0)) {
                std::puts("FAIL tower aim refresh countdown");++failures;
            }
            for(int tick=0;tick<300;++tick)advance(32768);
            if(starts!=2 || !(aim.flags&8)) {
                std::puts("FAIL tower re-aim handshake");++failures;
            }
            display.start("TargetCleared",{0});
            if(aim.flags&0xf8) {
                std::puts("FAIL tower target-clear did not reset aim state");++failures;
            }
            advance(49152);
            if(starts!=3) {
                std::puts("FAIL tower reacquisition did not start aiming");++failures;
            }
        }
        auto cannon=std::find(file.pieces.begin(),file.pieces.end(),"cannon");
        if(cannon==file.pieces.end())++failures;
        else for(int height:{-100,0,100}) {
            tak::cob::Vm display(file);display.enableRetailAnimation();
            display.onGet=[](int query,const std::vector<int32_t>&){return query==4 ? 100:0;};
            int ready=-1;
            display.onSetUnitValue=[&](int query,int value){if(query==22)ready=value;};
            display.start("Create");
            const auto angles=tak::sim::retailDirectAim(0,height*65536,100*65536,32768);
            display.start("AimWeapon",{angles[0],angles[1],0});
            for(int tick=0;tick<300;++tick)display.tick(1.f/30);
            const auto& pose=display.retailPieces()[size_t(cannon-file.pieces.begin())];
            if(uint16_t(pose.turn[0])!=angles[1] || ready!=0) {
                std::printf("FAIL vertower pitch/ready at target height %d\n",height);++failures;
            }
        }
    } else ++failures;
    if(const auto* mortar=registry.find("vermort");mortar && mortar->script()) {
        for(bool high:{false,true}) {
            tak::cob::Vm display(*mortar->script());display.enableRetailAnimation();
            display.onGet=[](int query,const std::vector<int32_t>&){return query==4 ? 100:0;};
            int ready=-1;display.onSetUnitValue=[&](int query,int value){if(query==22)ready=value;};
            display.start("Create");
            const auto pitch=tak::retailBallisticPitch(0,0,100,10,1,high);
            display.start("AimWeapon",{32768,pitch,0});
            for(int tick=0;tick<900;++tick)display.tick(1.f/30);
            const auto& names=mortar->script()->pieces;
            const auto found=std::find(names.begin(),names.end(),"mortar");
            if(found==names.end() || ready!=0 ||
               uint16_t(display.retailPieces()[size_t(found-names.begin())].turn[0])!=uint16_t(pitch-16384)) {
                std::printf("FAIL mortar ballistic elevation high=%d\n",int(high));++failures;
            }
        }
    } else ++failures;
    std::printf("%d scripts, 1200-tick display callback timelines, %d failures\n",scripts,failures);
    std::printf("explosion scheduling: %zu callbacks, %zu serial and %zu parallel frame classifications\n",
                explosions,serialFrames,parallelFrames);
    std::printf("observed script emissions:");
    for(const auto& [code,count]:emissions)std::printf(" %d=%zu",code,count);
    std::puts("");
    return failures || scripts<200 ? 1:0;
}
