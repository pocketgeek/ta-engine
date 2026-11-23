// The front-end click (client/menuclick.h): which WAV a .gui button names, and the
// level it plays at under the MASTER / SOUND EFFECTS sliders.
#include "client/menuclick.h"
#include <cstdio>
#include <stdexcept>
static void check(bool yes,const char* message) {if(!yes)throw std::runtime_error(message);}
int main() try {
    namespace mc=tak::menuclick;
    tak::gui::Gadget g;
    check(mc::soundOf(g).empty(),"no states, no click");
    // A button's states: "Default"-style entries and empties around the one WAV.
    g.states={"","Default","OK.WAV",""};
    check(mc::soundOf(g)=="ok.wav","the WAV entry, lowercased (sounds/ is looked up by name)");
    g.states={"cancel.wav","ok.wav"};
    check(mc::soundOf(g)=="cancel.wav","the first WAV wins");
    g.states={"wav","x.wave","Default"};
    check(mc::soundOf(g).empty(),"only a .wav extension names a click");
    // Level: the UI level (100/128 around silence) at full sliders, as the title menu
    // always played; scaled by both sliders; silent at either zero.
    const std::vector<uint8_t> pcm{0,64,128,192,255};
    const auto full=mc::level(pcm,256,256);
    for(size_t i=0;i<pcm.size();++i)
        check(full[i]==uint8_t(128+(int(pcm[i])-128)*100/128),"full sliders keep the UI level");
    check(mc::level(pcm,0,256)==std::vector<uint8_t>(pcm.size(),128),"MASTER at zero is silence");
    check(mc::level(pcm,256,0)==std::vector<uint8_t>(pcm.size(),128),"SOUND EFFECTS at zero is silence");
    const auto half=mc::level(pcm,128,256);
    check(half[0]==128-50 && half[4]==128+(127*100*128/(128*256)),"half MASTER halves the swing");
    check(mc::level(pcm,128,256)==mc::level(pcm,256,128),"the two sliders multiply");
    check(mc::level(pcm,999,-5)==mc::level(pcm,256,0),"levels clamp to 0..256");
    std::puts("menuclick ok");
    return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
