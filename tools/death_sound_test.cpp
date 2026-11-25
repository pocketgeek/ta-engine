#include "client/sound.h"
#include "client/commanderalert.h"
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
        return pcm ? ta::evenOutGain(pcm->samples.data(),pcm->samples.size(),pcm->rate,pcm->channels) : 0.f;
    }
};

int main(int argc,char** argv) {
    if(argc==2 && std::string(argv[1])=="--victims") {
        int incoming;
        while(std::cin>>incoming) {
            std::array<ta::SoundVoice,32> voices;
            for(auto& voice:voices)std::cin>>voice.active>>voice.looping>>voice.priority>>voice.started;
            std::cout<<ta::retailSoundVoice(voices,incoming)<<'\n';
        }
        return 0;
    }
    if(argc==2 && std::string(argv[1])=="--routes") {
        int flags;bool visible,selected,free;
        while(std::cin>>flags>>visible>>selected>>free) {
            const int priority=ta::retailSoundPriority(flags);
            const bool admit=ta::retailUnitSoundAudible(flags,visible,selected) && (priority>1 || free);
            std::cout<<(admit?(priority==7?2:1):0)<<'\n';
        }
        return 0;
    }
    int failures=0;
    const auto check=[&](bool ok,const char* message) {if(!ok){++failures;std::cerr<<"FAIL "<<message<<'\n';}};
    {
        ta::CommanderAlert alert;   // TotalA.exe 0x47f850 / 0x47fad0 / 0x47feee
        check(!alert.admit(1,0,false,false,100), "another player's unit does not alert");
        check(!alert.admit(0,0,true,false,100), "spectators hear no alert");
        check(!alert.admit(0,0,false,true,100), "a unit on screen does not alert");
        check(alert.admit(0,0,false,false,100), "first off-screen hit alerts at once");
        check(alert.admit(0,0,false,false,101), "an alert not yet heard starts no cooldown");
        alert.spoken(100);
        check(!alert.admit(0,0,false,false,699), "the alert waits 600 game ticks once heard");
        check(alert.admit(0,0,false,false,700), "and repeats at the boundary");
        // 0x47fdf9 / 0x47ff26: priority 9 against 10 - level (Off|Medium|Full = 0|5|10).
        check(!ta::CommanderAlert::speaks(0) && ta::CommanderAlert::speaks(5) && ta::CommanderAlert::speaks(10),
              "under attack is spoken at Medium and Full speech only");
        check(!ta::CommanderAlert::shows(0) && ta::CommanderAlert::shows(5) && ta::CommanderAlert::shows(10),
              "its line shows at Medium and Full unit chat text only");
        check(ta::CommanderAlert::line("Commander")=="Commander: Under Attack", "the line is \"%s: %s\"");
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
        const auto gain=[](const std::vector<int16_t>& s) {return ta::evenOutGain(s.data(),s.size(),22050,1);};
        check(std::abs(gain(tone(0.1f,22050))-1.778f)<0.02f,"a sound 20 dB down is raised to the target");
        check(std::abs(gain(tone(0.0178f,22050))-3.162f)<0.02f,"a sound far down is raised by at most +10 dB");
        check(gain(tone(0.4f,22050))==1.f,"a loud sound is left alone");
        check(gain(std::vector<int16_t>(2205,0))==1.f,"silence is left alone");
        check(std::abs(gain(tone(0.05f,22050,20000))-0.95f*32768.f/20000.f)<0.01f,"the boost stops short of clipping the peak");
    }
    check(!ta::retailUnitSoundAudible(4,false,true),"hidden positional death cry is suppressed");
    check(ta::retailUnitSoundAudible(7,false,false),"global authored sound bypasses visibility");
    check(!ta::retailUnitSoundAudible(1,true,false),"unselected chatter is suppressed");
    check(ta::retailUnitSoundAudible(1,true,true),"selected chatter is eligible");
    for(bool retail:{false,true}) {
        ta::cob::File file;file.names={"cry"};file.scripts={{"Dying",0}};
        file.numStatics=1;
        file.code={0x10021001,4,0x10072000,0,0x10023004,0,0x10021001,0,0x10065000};
        ta::cob::Vm vm(std::move(file));if(retail)vm.enableRetailAnimation();
        int calls=0;
        vm.onPlaySound=[&](int32_t index,int32_t flags) {
            ++calls;check(index==0 && flags==4,"VM preserves death sound name and authored priority");
        };
        vm.start("Dying");vm.tick(1.f/30);
        check(calls==1 && vm.getStatic(0)==0,"unit sound host returns native zero result");
    }
    if(argc==2) {
        auto vfs=ta::hpi::mountRetailRoot(argv[1]);
        SoundBank decoder;
        // TA's under-attack alert is each unit's sound class underattack= (gamedata/
        // sound.tdf): warning1 for the Commander.
        check(vfs.has("sounds/warning1.wav"), "retail under-attack alert exists");
        check(SoundBankTestAccess::decodes(decoder,vfs.read("sounds/warning1.wav")),
              "retail under-attack alert decodes to PCM");
        // Quietly mastered unit servos (gains 1.56 / 1.91 / 1.91 measured) are what the
        // option is for; a cannon is not raised.
        for(const char* quiet:{"sounds/servsml3.wav","sounds/servsml4.wav","sounds/servsml5.wav"}) {
            const float g=SoundBankTestAccess::evenGain(decoder,vfs.read(quiet));
            std::cout<<quiet<<" even-out gain "<<g<<'\n';
            check(g>1.4f && g<2.2f,"a quiet servo is raised by EVEN OUT SOUND LEVELS");
        }
        check(SoundBankTestAccess::evenGain(decoder,vfs.read("sounds/cannon3.wav"))==1.f,"a loud cannon is left alone");
        // Kingdoms' Dying scripts played authored death cries (PLAY_SOUND). TA's
        // scripts play no sounds: a death is heard as its explosion weapon's soundhit
        // (sound_hit_data covers those).
    }
    SDL_Quit();
    std::cout<<(failures?"FAIL":"PASS")<<": death sound delivery and mixer admission ("<<failures<<" failures)\n";
    return failures?1:0;
}
