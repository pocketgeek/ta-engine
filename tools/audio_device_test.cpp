#include "client/options.h"
#include <cstdio>

static int devices=0, opens=0;
extern "C" int SDLCALL TAK_Test_GetNumAudioDevices(int) { return devices; }
extern "C" SDL_AudioDeviceID SDLCALL TAK_Test_OpenAudioDevice(
    const char*,int,const SDL_AudioSpec* want,SDL_AudioSpec* got,int) {
    ++opens;
    if(got)*got=*want;
    return 42;
}
int main() {
    SDL_SetMainReady();
    SDL_AudioSpec want{},got{};want.freq=44100;want.channels=2;want.format=AUDIO_S16SYS;
    // Intro, menu music and individual clicks must not enter the backend's
    // blocking endpoint-retry path when enumeration reports no active device.
    for(int i=0;i<8;++i)
        if(tak::openAudioDevice(0,&want,&got,0)!=0 || opens!=0)return 1;
    // Do not permanently disable audio: a later selection after hotplug retries.
    devices=1;
    if(tak::openAudioDevice(0,&want,&got,0)!=42 || opens!=1 || got.freq!=44100)return 2;
    // Some SDL backends cannot enumerate but can open their default device.
    devices=-1;
    if(tak::openAudioDevice(0,&want,&got,0)!=42 || opens!=2)return 3;
    std::puts("PASS: absent audio skips opens; available/unknown devices can open");
}
