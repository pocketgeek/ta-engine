// Authored attack audio must retain its name, flags and callback timing.
#include "cob/vm.h"
#include "cob/retailstate.h"
#include "sim/matchsetup.h"
#include "sim/retailrng.h"
#include "hpi/hpi.h"
#include <SDL.h>
#include <iostream>
#include <set>
#include <tuple>

using Event=std::tuple<int,int,int32_t>;
struct Host {
    uint32_t seed=12345;
    int tick=0;
    std::vector<Event> sounds;
    uint32_t random(int32_t bound){return tak::sim::retailRandom(seed,bound);}
    uint32_t get(int query,const std::array<uint32_t,4>&){return query==4?100:query==18?1:0;}
    void set(int,int){}
    uint32_t sound(int name,int32_t flags){sounds.emplace_back(tick,name,flags);return 0;}
    void effect(uint32_t,int,int32_t){}
};
int main(int argc,char**argv) {
    if(argc!=2)return 2;
    SDL_SetMainReady();
    auto vfs=tak::hpi::mountRetailRoot(argv[1]);
    // Report dormant engine-authored burst audio separately from COB audio.
    // Shipped TAK definitions omit both legacy fields; mods may provide them.
    std::set<std::string> definitions;
    int soundStarts=0,soundTriggers=0;
    for(const char* folder:{"units","unitscb"})for(const auto& path:vfs.list(folder)) {
        if(!path.ends_with(".fbi") || !definitions.insert(path).second)continue;
        const auto bytes=vfs.read(path);
        std::string text(bytes.begin(),bytes.end());
        std::transform(text.begin(),text.end(),text.begin(),::tolower);
        soundStarts+=text.find("soundstart")!=std::string::npos;
        soundTriggers+=text.find("soundtrigger")!=std::string::npos;
    }
    std::cout<<definitions.size()<<" mounted unit definitions: "<<soundStarts
             <<" soundstart, "<<soundTriggers<<" soundtrigger references\n";
    std::set<std::string> decoded;
    int failures=0,missingShipped=0;
    for(bool crusades:{false,true}) {
        tak::sim::TypeRegistry registry;tak::sim::setupRegistry(registry,vfs,crusades);
        int slots=0,events=0,silent=0;
        for(const auto& [name,type]:registry.types()) {
            const auto* file=type.script();if(!file)continue;
            for(size_t slot=0;slot<type.weapons.size();++slot) {
                ++slots;
                tak::cob::Vm display(*file);display.enableRetailAnimation();
                tak::cob::RetailScriptState reference(*file);Host host;
                std::vector<Event> actual;
                display.onGet=[&](int query,const std::vector<int32_t>&){return host.get(query,{});};
                display.onPlaySound=[&](int index,int32_t flags){actual.emplace_back(host.tick,index,flags);};
                auto notify=[&](const char* callback,std::vector<int32_t> args={}) {
                    int index=file->scriptIndex(callback);if(index<0)return;
                    display.start(callback,args);
                    std::array<uint32_t,4> values{};
                    for(size_t i=0;i<args.size();++i)values[i]=uint32_t(args[i]);
                    reference.startArguments(*file,index,values,unsigned(args.size()));
                    reference.tick(*file,0,host);
                };
                notify("Create");notify("SetMaxReloadTime",{type.maxWeaponReloadMs});
                for(host.tick=1;host.tick<=900;++host.tick) {
                    if(host.tick==30){notify("Activate");notify("BeginFlight");notify("setSFXoccupy",{5});}
                    if(host.tick==120) {actual.clear();host.sounds.clear();notify("AimWeapon",{0,0,int(slot)});}
                    if(host.tick==150 || host.tick==450)notify("FireWeapon",{int(slot)});
                    display.tick(1.f/60);display.tick(1.f/60);reference.tick(*file,1,host);
                    if(actual!=host.sounds) {
                        std::cerr<<"FAIL authored sound timeline "<<name<<" slot "<<slot<<" tick "<<host.tick<<'\n';
                        ++failures;break;
                    }
                }
                if(actual.empty())++silent;
                for(auto [tick,index,flags]:actual) {
                    ++events;
                    if(index<0 || size_t(index)>=file->names.size()){++failures;continue;}
                    auto sound=file->names[size_t(index)];
                    std::transform(sound.begin(),sound.end(),sound.begin(),::tolower);
                    if(!decoded.insert(sound).second)continue;
                    const auto path="sounds/"+sound+".wav";
                    if(!vfs.has(path)) {
                        // These shipped script references have no matching WAV.
                        // Failed lookup remains silent in retail and our mixer.
                        if(sound=="archer1" || sound=="archer2" || sound=="archer3") {
                            std::cout<<"Absent shipped WAV "<<path<<" referenced by "<<name<<'\n';
                            ++missingShipped;continue;
                        }
                        std::cerr<<"FAIL missing authored firing WAV "<<path<<" referenced by "<<name<<'\n';
                        ++failures;continue;
                    }
                    auto bytes=vfs.read(path);SDL_AudioSpec spec{};Uint8* pcm=nullptr;Uint32 length=0;
                    if(!SDL_LoadWAV_RW(SDL_RWFromConstMem(bytes.data(),int(bytes.size())),1,&spec,&pcm,&length) || !length) {
                        std::cerr<<"FAIL decoding "<<path<<'\n';++failures;
                    }
                    if(pcm)SDL_FreeWAV(pcm);
                }
            }
        }
        std::cout<<(crusades?"Crusades":"Standard")<<": "<<slots<<" slots, "<<events<<" authored sound events, "<<silent<<" silent timelines\n";
    }
    std::cout<<(failures?"FAIL":"PASS")<<": firing audio ("<<failures<<" failures), "<<decoded.size()-size_t(missingShipped)<<" WAVs decoded, "<<missingShipped<<" known absent references\n";
    return failures?1:0;
}
