#include "client/menumusic.h"
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <tuple>

namespace tak {
// The test uses the ordinary SDL dummy device, without the Options UI, and picks
// the "detected" speaker count itself.
int testOutputChannels=2;
int detectOutputChannels() {return testOutputChannels;}
SDL_AudioDeviceID openAudioDevice(int capture, const SDL_AudioSpec* want,
                                 SDL_AudioSpec* have, int allowed) {
    return SDL_OpenAudioDevice(nullptr,capture,want,have,allowed);
}
struct MenuMusicTestAccess {
    // Decode a WAV as start() does and lay it onto a `channels`-speaker device.
    static void source(MenuMusic& music, const std::vector<uint8_t>& wav, int channels) {
        music.stop();
        if(!music.decode(wav))throw std::runtime_error("menu WAV did not decode");
        music.setLayout(channels);
        music.resetTapConverter();
    }
    static int rate(const MenuMusic& music) {return music.rate_;}
    static int sourceChannels(const MenuMusic& music) {return music.srcChannels_;}
    static int deviceChannels(const MenuMusic& music) {return music.channels_;}
    static int deviceRate(const MenuMusic& music) {return music.openWant_.freq;}
    static size_t position(const MenuMusic& music) {return music.pos_;}
    // Keep a real device from pulling while the test fills by hand.
    static void pause(MenuMusic& music) {
        SDL_LockAudioDevice(music.dev_);SDL_PauseAudioDevice(music.dev_,1);SDL_UnlockAudioDevice(music.dev_);
    }
    static std::vector<int16_t> fill(MenuMusic& music, int frames) {
        std::vector<int16_t> out(size_t(frames*music.channels_));
        music.fill(reinterpret_cast<Uint8*>(out.data()),int(out.size()*2));
        return out;
    }
};
}
namespace {
struct Capture {
    std::vector<int16_t> pcm;
    int channels=0;
    static void tap(void* context,const int16_t* data,int frames,int channels) {
        auto& capture=*static_cast<Capture*>(context);
        if(capture.channels && capture.channels!=channels)throw std::runtime_error("tap changed layout mid-run");
        capture.channels=channels;
        capture.pcm.insert(capture.pcm.end(),data,data+frames*channels);
    }
};
// A RIFF/WAVE file: PCM U8/S16 or IEEE float, interleaved in SDL channel order.
std::vector<uint8_t> wavFile(SDL_AudioFormat format,int rate,int channels,const std::vector<double>& samples) {
    const int bits=SDL_AUDIO_BITSIZE(format);
    std::vector<uint8_t> out;
    auto u32=[&](uint32_t v){for(int i=0;i<4;++i)out.push_back(uint8_t(v>>(8*i)));};
    auto u16=[&](uint16_t v){out.push_back(uint8_t(v));out.push_back(uint8_t(v>>8));};
    const uint32_t bytes=uint32_t(samples.size())*uint32_t(bits/8);
    out.insert(out.end(),{'R','I','F','F'});u32(36+bytes);out.insert(out.end(),{'W','A','V','E'});
    out.insert(out.end(),{'f','m','t',' '});u32(16);u16(SDL_AUDIO_ISFLOAT(format)?3:1);u16(uint16_t(channels));
    u32(uint32_t(rate));u32(uint32_t(rate*channels*bits/8));u16(uint16_t(channels*bits/8));u16(uint16_t(bits));
    out.insert(out.end(),{'d','a','t','a'});u32(bytes);
    for(double s:samples) {   // s in -1..1
        if(format==AUDIO_U8)out.push_back(uint8_t(128+std::lround(s*128)));
        else if(SDL_AUDIO_ISFLOAT(format)){float f=float(s);uint32_t v;std::memcpy(&v,&f,4);u32(v);}
        else u16(uint16_t(int16_t(std::lround(s*32768))));
    }
    return out;
}
double tone(const std::vector<int16_t>& pcm,int channels,int channel,double freq,int rate) {
    const size_t n=pcm.size()/size_t(channels);
    const double w=2*M_PI*freq/rate,c=2*std::cos(w);
    double s1=0,s2=0;
    for(size_t i=0;i<n;++i){double s=pcm[i*size_t(channels)+size_t(channel)]+c*s1-s2;s2=s1;s1=s;}
    return std::sqrt(s1*s1+s2*s2-c*s1*s2)*2/double(n);
}
void require(bool ok,const std::string& what) {if(!ok)throw std::runtime_error(what);}
}
int main(int argc,char**argv) {
    SDL_SetMainReady();SDL_setenv("SDL_AUDIODRIVER","dummy",1);
    if(SDL_Init(SDL_INIT_AUDIO)!=0)return 2;
    using Access=tak::MenuMusicTestAccess;
    constexpr int kTap=tak::audio::kMixRate;
    try {
        tak::MenuMusic music;Capture capture;
        music.setAudioTap(&capture,Capture::tap);
        // One second of a constant level (L .25, R -.125) in several source formats:
        // the tap gets 44100 Hz in the device's layout, scaled by the music volume.
        for(auto [format,rate,channels]: {std::tuple{AUDIO_U8,11025,1},
                std::tuple{AUDIO_S16SYS,44100,2},std::tuple{AUDIO_F32SYS,48000,2}}) {
            std::vector<double> s;
            for(int i=0;i<rate;++i){s.push_back(.25);if(channels==2)s.push_back(-.125);}
            Access::source(music,wavFile(SDL_AudioFormat(format),rate,channels,s),2);
            require(Access::rate(music)==std::min(rate,kTap),"source keeps its rate up to 44100");
            require(Access::sourceChannels(music)==channels,"source keeps its channels");
            music.setVolume(256,128);capture.pcm.clear();capture.channels=0;
            const int chunk=Access::rate(music)/50;
            for(int i=0;i<100;++i)Access::fill(music,chunk);
            require(capture.channels==2,"stereo device: stereo tap");
            const size_t frames=capture.pcm.size()/2;
            if(frames<83500 || frames>88400)
                throw std::runtime_error("wrong 44100 Hz tap duration "+std::to_string(frames));
            const size_t middle=(capture.pcm.size()/4)*2;
            if(std::abs(int(capture.pcm[middle])-4096)>20)
                throw std::runtime_error("music volume not reflected in stream");
            if(std::abs(int(capture.pcm[middle+1])-(channels==1?4096:-2048))>20)
                throw std::runtime_error("stereo channels lost during menu capture");
            music.setVolume(0,128);
            for(int i=0;i<20;++i)Access::fill(music,chunk);
            if(std::abs(int(capture.pcm.back()))>1)throw std::runtime_error("master mute not reflected");
            music.setAudioTap(nullptr,nullptr);const auto count=capture.pcm.size();
            Access::fill(music,chunk);
            if(capture.pcm.size()!=count)throw std::runtime_error("detached tap still called");
            music.setAudioTap(&capture,Capture::tap);
        }
        {
            // A stereo track on a 5.1 device: the game's music bed (halved front and
            // rear, a quarter of each in the centre); the tap carries all six.
            std::vector<double> s;for(int i=0;i<4410;++i){s.push_back(.25);s.push_back(-.125);}
            Access::source(music,wavFile(AUDIO_S16SYS,44100,2,s),6);
            music.setVolume(256,256);capture.pcm.clear();capture.channels=0;
            auto out=Access::fill(music,441);
            require(out[0]==4096 && out[1]==-2048 && out[2]==1024 && out[3]==0 && out[4]==4096 && out[5]==-2048,
                    "stereo menu music on 5.1 matches the in-game layout");
            require(capture.channels==6,"5.1 device: the tap passes the device layout to the stream");
        }
        {
            // A 5.1 track keeps six channels on a 6-speaker device and folds to stereo
            // (ITU, LFE dropped) on a stereo one.
            const double v[6]={.05,.1,.15,.2,.25,.3};
            std::vector<double> s;for(int i=0;i<4410;++i)for(double x:v)s.push_back(x);
            Access::source(music,wavFile(AUDIO_S16SYS,44100,6,s),6);
            music.setVolume(256,256);capture.pcm.clear();capture.channels=0;
            auto six=Access::fill(music,100);
            for(int k=0;k<6;++k)
                require(std::abs(six[size_t(k)]-int(v[k]*32768))<=2,"5.1 menu track channel for channel");
            Access::source(music,wavFile(AUDIO_S16SYS,44100,6,s),2);
            capture.pcm.clear();capture.channels=0;
            auto two=Access::fill(music,100);
            const double h=0.70710678,w=1+2*h;
            require(std::abs(two[0]-int((v[0]+h*v[2]+h*v[4])/w*32768))<=3
                    && std::abs(two[1]-int((v[1]+h*v[2]+h*v[5])/w*32768))<=3,"5.1 menu track folds to stereo");
            // On 6.1 (FL FR FC LFE BC SL SR) the 5.1 surrounds play at full level on SL/SR.
            Access::source(music,wavFile(AUDIO_S16SYS,44100,6,s),7);
            capture.pcm.clear();capture.channels=0;
            auto seven=Access::fill(music,100);
            const double want7[7]={v[0],v[1],v[2],v[3],0,v[4],v[5]};
            for(int k=0;k<7;++k)
                require(std::abs(seven[size_t(k)]-int(want7[k]*32768))<=2,"5.1 menu track on 6.1 keeps its surrounds");
        }
        music.stop();
        if(music.playing())throw std::runtime_error("stopped music still owns audio");
        {
            // A cosmetic mod's 44.1 kHz stereo music/track3.wav replaces the install's
            // 22050 Hz copy: the device opens at 44100 in the device layout, the tap
            // keeps 15 kHz on the left, and a switch to a 5.1 device keeps its place.
            namespace fs=std::filesystem;
            const fs::path root=fs::temp_directory_path()/("menu_music_mod_"+std::to_string(std::random_device{}()));
            const fs::path pack=root/"mods"/"cosmetic"/"always_enabled"/"hq"/"music";
            fs::create_directories(root/"Music");fs::create_directories(pack);
            auto put=[](const fs::path& p,const std::vector<uint8_t>& d) {
                std::ofstream(p,std::ios::binary)
                    .write(reinterpret_cast<const char*>(d.data()),std::streamsize(d.size()));
            };
            std::vector<double> retail,mod;
            for(int i=0;i<22050;++i){retail.push_back(.2);retail.push_back(.2);}
            for(int i=0;i<44100;++i){mod.push_back(.25*std::sin(2*M_PI*15000*i/44100.0));
                                     mod.push_back(.25*std::sin(2*M_PI*1000*i/44100.0));}
            put(root/"Music"/"track3.wav",wavFile(AUDIO_S16SYS,22050,2,retail));
            put(pack/"track3.wav",wavFile(AUDIO_S16SYS,44100,2,mod));
            {
                auto vfs=tak::hpi::mountPlayVfs(root,{},{});
                tak::testOutputChannels=2;
                music.setVolume(256,256);music.start(vfs,3);
                require(music.playing(),"mod track starts");
                Access::pause(music);
                require(Access::rate(music)==44100 && Access::deviceRate(music)==44100,"mod track opens at 44100 Hz");
                require(Access::sourceChannels(music)==2 && Access::deviceChannels(music)==2,"mod track in stereo");
                capture.pcm.clear();capture.channels=0;
                for(int i=0;i<10;++i)Access::fill(music,2205);
                const double l15=tone(capture.pcm,2,0,15000,kTap),r15=tone(capture.pcm,2,1,15000,kTap);
                const double r1=tone(capture.pcm,2,1,1000,kTap);
                std::printf("menu mod track tap: L15k=%.0f R15k=%.0f R1k=%.0f\n",l15,r15,r1);
                require(l15>8000 && r15<50 && r1>8000,"mod track's tap keeps 15 kHz stereo at 44100");
                const size_t at=Access::position(music);
                tak::testOutputChannels=6;music.reopen();Access::pause(music);
                // (The unpaused device may pull a buffer before the test pauses it again.)
                require(Access::deviceChannels(music)==6 && Access::position(music)>=at
                        && Access::position(music)<at+8192,
                        "device switch to 5.1 re-lays the track without restarting it");
                capture.pcm.clear();capture.channels=0;
                auto out=Access::fill(music,4410);
                require(tone(out,6,0,15000,44100)>4000 && tone(out,6,1,15000,44100)<50,"5.1 device keeps the image");
                tak::testOutputChannels=2;
                music.stop();
            }
            std::error_code ec;fs::remove_all(root,ec);
        }
        if(argc==2) {
            auto vfs=tak::hpi::mountRetailRoot(argv[1]);
            capture.pcm.clear();capture.channels=0;
            music.setVolume(256,256);music.start(vfs,15);
            if(!music.playing())throw std::runtime_error("retail title track failed to start");
            SDL_Delay(200);music.reopen();
            if(!music.playing())throw std::runtime_error("music device reopen failed");
            SDL_Delay(200);music.stop();
            // The callback is joined before reading the capture vector.
            bool audible=false;for(auto sample:capture.pcm)audible|=sample!=0;
            if(!audible)throw std::runtime_error("retail title tap remained silent");
        }
        music.setAudioTap(nullptr,nullptr);
        std::cout<<"PASS: menu audio conversion, layout, volume, mute, looping, detach, mod tracks and device lifecycle\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';SDL_Quit();return 1;}
    SDL_Quit();
}
