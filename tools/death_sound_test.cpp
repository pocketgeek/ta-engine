#include "client/sound.h"
#include "client/monarchalert.h"
#include "client/retaildeathsfx.h"
#include "cob/vm.h"
#include "sim/matchsetup.h"
#include <cmath>
#include <iostream>
#include <set>

// Paused dummy device: exercise production admission and PCM mixing without
// racing the audio callback or depending on physical audio hardware.
struct SoundBankTestAccess {
    static bool prepare(SoundBank& bank) {
        SDL_AudioSpec want{};want.freq=SoundBank::kMixRate;want.format=AUDIO_S16SYS;
        want.channels=1;want.samples=256;
        bank.dev_=SDL_OpenAudioDevice(nullptr,0,&want,&bank.spec_,0);
        bank.chan_=1;
        bank.index_["quiet"]="quiet";bank.index_["cry"]="cry";
        bank.cache_["quiet"]={std::vector<int16_t>(64,20)};
        bank.cache_["cry"]={std::vector<int16_t>(64,400)};
        return bank.dev_!=0;
    }
    static int sample(SoundBank& bank) {int16_t value=0;bank.mix(&value,1);return value;}
    static bool decodes(SoundBank& bank,const std::vector<uint8_t>& data) {
        auto pcm=bank.decodeWav(data.data(),data.size());return pcm && pcm->frames()>0;
    }
    static void setEvenGain(SoundBank& bank,const char* name,float gain) {bank.cache_[name].evenGain=gain;}
    // EVEN OUT SOUND LEVELS' boost for a WAV, as SoundBank::load measures it.
    static float evenGain(SoundBank& bank,const std::vector<uint8_t>& data) {
        auto pcm=bank.decodeWav(data.data(),data.size());
        return pcm ? tak::evenOutGain(pcm->samples.data(),pcm->samples.size(),pcm->rate,pcm->channels) : 0.f;
    }
};

int main(int argc,char** argv) {
    if(argc==2 && std::string(argv[1])=="--victims") {
        int incoming;
        while(std::cin>>incoming) {
            std::array<tak::SoundVoice,32> voices;
            for(auto& voice:voices)std::cin>>voice.active>>voice.looping>>voice.priority>>voice.started;
            std::cout<<tak::retailSoundVoice(voices,incoming)<<'\n';
        }
        return 0;
    }
    if(argc==2 && std::string(argv[1])=="--routes") {
        int flags;bool visible,selected,free;
        while(std::cin>>flags>>visible>>selected>>free) {
            const int priority=tak::retailSoundPriority(flags);
            const bool admit=tak::retailUnitSoundAudible(flags,visible,selected) && (priority>1 || free);
            std::cout<<(admit?(priority==7?2:1):0)<<'\n';
        }
        return 0;
    }
    int failures=0;
    const auto check=[&](bool ok,const char* message) {if(!ok){++failures;std::cerr<<"FAIL "<<message<<'\n';}};
    {
        tak::MonarchAlert alert;
        check(!alert.admit(2,0,false,100), "other player's monarch does not alert");
        check(!alert.admit(1,0,true,100), "spectators do not hear a local monarch alarm");
        check(alert.admit(1,0,false,100), "first local monarch hit alerts immediately");
        check(!alert.admit(1,0,false,15099), "monarch alarm cooldown is 15 real seconds");
        check(alert.admit(1,0,false,15100), "monarch alarm repeats at cooldown boundary");
        check(!alert.admit(0,0,false,40000), "cooldown expiry alone does not play an alarm");
    }
    SDL_SetMainReady();
    SDL_setenv("SDL_AUDIODRIVER","dummy",1);
    if(SDL_Init(SDL_INIT_AUDIO)!=0)return 2;
    for(int priority:{3,4,5}) {
        SoundBank bank;
        if(!SoundBankTestAccess::prepare(bank))return 2;
        for(int i=0;i<32;++i)bank.playWorld("quiet",0,0,priority);
        bank.playWorld("cry",0,0,4);
        check(SoundBankTestAccess::sample(bank)==(priority<4?510:320),
              "death cry replaces lower-priority audio but preserves equal/higher priority voices");
    }
    {
        SoundBank bank;if(!SoundBankTestAccess::prepare(bank))return 2;
        for(int i=0;i<32;++i)bank.play("quiet",1,1,1,true);
        bank.playWorld("cry",0,0,4);
        for(int i=0;i<130;++i)check(SoundBankTestAccess::sample(bank)==320,
                                   "looping voices wrap and remain protected from eviction");
    }
    {
        // Test the actual PCM output, including camera movement after playback
        // begins. Mono isolates attenuation from left/right speaker panning.
        SoundBank bank;if(!SoundBankTestAccess::prepare(bank))return 2;
        bank.setListener(0,0,100,50);
        bank.playWorld("cry",100,0,4);
        check(SoundBankTestAccess::sample(bank)==200,"on-screen edge retains full volume");
        bank.setListener(-25,0,100,50);
        check(SoundBankTestAccess::sample(bank)==112,"offscreen sound fades continuously from nearest edge");
        bank.setListener(-50,0,100,50);
        check(SoundBankTestAccess::sample(bank)==50,"sound halfway through fade has quarter amplitude");
        bank.setListener(-100,0,100,50);
        check(SoundBankTestAccess::sample(bank)==0,"distant ongoing sound becomes silent");
        bank.setListener(0,0,200,100);
        check(SoundBankTestAccess::sample(bank)==200,"zooming out restores on-screen volume");
    }
    for(const auto point : {std::pair{150.f,0.f},std::pair{-150.f,0.f},
                           std::pair{0.f,100.f},std::pair{0.f,-100.f},std::pair{130.f,90.f}}) {
        SoundBank bank;if(!SoundBankTestAccess::prepare(bank))return 2;
        bank.setListener(0,0,100,50);bank.playWorld("cry",point.first,point.second,4);
        check(SoundBankTestAccess::sample(bank)==50,"initial attenuation treats all edges and corner distance consistently");
    }
    {
        SoundBank bank;if(!SoundBankTestAccess::prepare(bank))return 2;
        bank.setListener(0,0,100,50);
        for(int i=0;i<32;++i)bank.playWorld("quiet",0,0,3);
        bank.playWorld("cry",10000,0,7);
        check(SoundBankTestAccess::sample(bank)==320,"inaudible remote sound cannot evict an audible voice");
    }
    {
        SoundBank bank;if(!SoundBankTestAccess::prepare(bank))return 2;
        bank.setListener(0,0,100,50);bank.play("cry");
        bank.setListener(10000,10000,100,50);
        check(SoundBankTestAccess::sample(bank)==200,"nonpositional alerts and UI do not attenuate with the camera");
    }
    {
        SoundBank bank;if(!SoundBankTestAccess::prepare(bank))return 2;
        bank.setListener(0,0,100,50);bank.playAt("cry",0,0,true,200,0,1,1,3,true);
        for(int i=0;i<130;++i)check(SoundBankTestAccess::sample(bank)==0,"inaudible loop keeps its clock silently");
        bank.setListener(200,0,100,50);
        check(SoundBankTestAccess::sample(bank)==200,"loop becomes audible when camera reaches its source");
    }
    {
        // SOUND VOICES: past retail's 32 a cry gets a voice of its own instead of
        // being refused by 32 equal/higher-class voices; lowering the limit stops the
        // voices past it.
        SoundBank bank;if(!SoundBankTestAccess::prepare(bank))return 2;
        bank.setVoiceLimit(64);
        for(int i=0;i<32;++i)bank.playWorld("quiet",0,0,5);
        bank.playWorld("cry",0,0,4);
        check(SoundBankTestAccess::sample(bank)==520,"a larger voice pool admits the cry beside 32 higher-class voices");
        for(int i=0;i<8;++i)bank.playWorld("quiet",0,0,5);
        bank.setVoiceLimit(32);
        check(SoundBankTestAccess::sample(bank)==320,"lowering the voice pool stops the voices past it");
        bank.setVoiceLimit(1000);
        check(bank.voiceLimit()==SoundBank::kMaxVoices,"the voice pool is capped");
    }
    // EVEN OUT SOUND LEVELS: a sound plays raised by its measured gain only with the
    // option on. (One bank at a time: the dummy driver opens one device.)
    for(bool on:{false,true}) {
        SoundBank bank;if(!SoundBankTestAccess::prepare(bank))return 2;
        SoundBankTestAccess::setEvenGain(bank,"cry",1.5f);
        bank.setEvenOut(on);bank.playWorld("cry",0,0,4);
        check(SoundBankTestAccess::sample(bank)==(on?300:200),
              on?"on: the sound is raised by its even-out gain":"off: the file's own level, as retail");
    }
    {
        // The measured gain: a tone 20 dB down rises to the -15 dBFS target (+5 dB), one 35
        // dB down by the +10 dB cap only, a loud one and silence stay as they are, and a
        // peaky one stops at its peak cap.
        const auto tone=[](float rms,int frames,int spike=0) {
            std::vector<int16_t> s(static_cast<size_t>(frames));
            for(int i=0;i<frames;++i)s[size_t(i)]=int16_t(std::lround(std::sqrt(2.f)*rms*32767.f*std::sin(float(i)*0.3f)));
            if(spike)s[0]=int16_t(spike);
            return s;
        };
        const auto gain=[](const std::vector<int16_t>& s) {return tak::evenOutGain(s.data(),s.size(),22050,1);};
        check(std::abs(gain(tone(0.1f,22050))-1.778f)<0.02f,"a sound 20 dB down is raised to the target");
        check(std::abs(gain(tone(0.0178f,22050))-3.162f)<0.02f,"a sound far down is raised by at most +10 dB");
        check(gain(tone(0.4f,22050))==1.f,"a loud sound is left alone");
        check(gain(std::vector<int16_t>(2205,0))==1.f,"silence is left alone");
        check(std::abs(gain(tone(0.05f,22050,20000))-0.95f*32768.f/20000.f)<0.01f,"the boost stops short of clipping the peak");
    }
    check(!tak::retailUnitSoundAudible(4,false,true),"hidden positional death cry is suppressed");
    check(tak::retailUnitSoundAudible(7,false,false),"global authored sound bypasses visibility");
    check(!tak::retailUnitSoundAudible(1,true,false),"unselected chatter is suppressed");
    check(tak::retailUnitSoundAudible(1,true,true),"selected chatter is eligible");
    for(bool retail:{false,true}) {
        tak::cob::File file;file.names={"cry"};file.scripts={{"Dying",0}};
        file.numStatics=1;
        file.code={0x10021001,4,0x10072000,0,0x10023004,0,0x10021001,0,0x10065000};
        tak::cob::Vm vm(std::move(file));if(retail)vm.enableRetailAnimation();
        int calls=0;
        vm.onPlaySound=[&](int32_t index,int32_t flags) {
            ++calls;check(index==0 && flags==4,"VM preserves death sound name and authored priority");
        };
        vm.start("Dying");vm.tick(1.f/30);
        check(calls==1 && vm.getStatic(0)==0,"unit sound host returns native zero result");
    }
    if(argc==2) {
        auto vfs=tak::hpi::mountRetailRoot(argv[1]);
        std::set<std::string> checkedSounds;
        SoundBank decoder;
        check(vfs.has("sounds/alarmmon.wav"), "retail monarch alarm exists");
        check(SoundBankTestAccess::decodes(decoder,vfs.read("sounds/alarmmon.wav")),
              "retail monarch alarm decodes to PCM");
        // The Creon cries the option exists for: mastered -19 to -20 dBFS, raised +3.4 to
        // +5.1 dB (gains 1.48 / 1.78 / 1.80); a cannon (-8) is not.
        for(const char* cry:{"sounds/creautodie1.wav","sounds/creautodie2.wav","sounds/creautodie3.wav"}) {
            const float g=SoundBankTestAccess::evenGain(decoder,vfs.read(cry));
            std::cout<<cry<<" even-out gain "<<g<<'\n';
            check(g>1.4f && g<2.2f,"an Automaton death cry is raised by EVEN OUT SOUND LEVELS");
        }
        check(SoundBankTestAccess::evenGain(decoder,vfs.read("sounds/cannon8.wav"))==1.f,"a loud cannon is left alone");
        int decoded=0,missingShipped=0;
        const auto auditSound=[&](std::string sound) {
            std::transform(sound.begin(),sound.end(),sound.begin(),::tolower);
            check(!sound.empty(),"death callback resolves a sound name");
            if(!checkedSounds.insert(sound).second)return;
            const auto path="sounds/"+sound+".wav";
            if(!vfs.has(path)) {
                // Shipped Gatling Crossbow/Giant Barracuda scripts reference absent WAVs.
                // Retail's failed lookup is silent; do not substitute another unit.
                if(sound=="cregatldie1" || sound=="zonbardie1") {++missingShipped;return;}
                ++failures;std::cerr<<"Missing death sound "<<path<<'\n';return;
            }
            check(SoundBankTestAccess::decodes(decoder,vfs.read(path)),"authored death sound decodes to PCM");
            ++decoded;
        };
        for(bool crusades:{false,true}) {
            tak::sim::TypeRegistry registry;tak::sim::setupRegistry(registry,vfs,crusades);
            int scripts=0,events=0;
            for(const auto& [name,type]:registry.types()) {
                const auto* file=type.script();if(!file)continue;
                ++scripts;
                // Check every authored death-cry variant, including branches not
                // selected by this deterministic callback timeline's random draws.
                for(auto sound:file->names) {
                    std::transform(sound.begin(),sound.end(),sound.begin(),::tolower);
                    if(sound.find("die")!=std::string::npos)auditSound(sound);
                }
                for(int deathType:{0,1,2,3,5,7,8,14,15}) {
                    tak::cob::Vm vm(*file);vm.enableRetailAnimation();
                    bool stop=false,capture=false;
                    vm.onGet=[&](int query,const std::vector<int32_t>&){return query==4 && !capture ? 100:0;};
                    vm.onSetUnitValue=[&](int32_t id,int32_t){if(tak::retailOwnerVmStopsOnSetUnitValue(id))stop=true;};
                    vm.onPlaySound=[&](int32_t index,int32_t flags) {
                        if(!capture)return;
                        ++events;
                        check(flags==4 || flags==7,"death cry uses an authored combat/global class");
                        auditSound(file->name(uint32_t(index)));
                    };
                    vm.start("Create");for(int t=0;t<30;++t)vm.tick(1.f/30);
                    stop=false;capture=true;
                    const bool callbacks=(deathType>=1 && deathType<=5);
                    if(callbacks) {
                        if(deathType!=5)vm.call("Killed",{100,0,deathType});
                        if(!vm.start("Dying",{deathType}))stop=true;
                    }
                    for(int t=0;t<600 && !stop && callbacks;++t)vm.tick(1.f/30);
                }
            }
            std::cout<<(crusades?"Crusades":"Standard")<<": "<<scripts<<" scripts, "<<events<<" death sound events\n";
        }
        std::cout<<decoded<<" distinct death WAVs decoded; "<<missingShipped<<" known absent shipped WAVs remain silent\n";
    }
    SDL_Quit();
    std::cout<<(failures?"FAIL":"PASS")<<": death sound delivery and mixer admission ("<<failures<<" failures)\n";
    return failures?1:0;
}
