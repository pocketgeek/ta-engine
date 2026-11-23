#include "client/sound.h"
#include "hpi/hpi.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>

// In-game audio through SoundBank at the 44100 Hz mix rate: music and effects keep
// their own rate and channels (stepped through the windowed-sinc interpolator, which
// stops the images above a source's Nyquist and bypasses 1:1 bit for bit), stereo effects
// keep their image and still pan, multichannel sources map channel for channel onto
// the output and fold down only what it lacks, a mod's music/track<n>.wav replacement
// plays at its own 44.1 kHz stereo, and the stream tap receives the 44100 Hz mix
// unchanged. No device: mix()/mixThunk are driven directly.
struct SoundBankTestAccess {
    static void layout(SoundBank& bank,int channels) {bank.chan_=channels;bank.musicVol_=256;}
    static std::optional<SoundBank::Pcm> convert(const std::vector<uint8_t>& wav) {
        return SoundBank::convertWav(wav.data(),wav.size());
    }
    static void setMusic(SoundBank& bank,SoundBank::Pcm pcm) {
        bank.musicStep_=SoundBank::stepFor(double(pcm.rate)/SoundBank::kMixRate);
        bank.music_=std::move(pcm);bank.musicPos_=0;bank.musicDone_=false;
    }
    static const SoundBank::Pcm& music(const SoundBank& bank) {return bank.music_;}
    static bool musicDone(const SoundBank& bank) {return bank.musicDone_;}
    static void silence(SoundBank& bank) {for(auto& c:bank.channels_)c=SoundBank::Channel{};}
    static std::vector<int16_t> mix(SoundBank& bank,int frames) {
        std::vector<int16_t> out(size_t(frames)*size_t(bank.chan_));
        bank.mix(out.data(),int(out.size()));return out;
    }
    static std::vector<int16_t> mixThunk(SoundBank& bank,int frames) {
        std::vector<int16_t> out(size_t(frames)*size_t(bank.chan_));
        SoundBank::mixThunk(&bank,reinterpret_cast<Uint8*>(out.data()),int(out.size()*2));
        return out;
    }
    static bool decodeSfx(SoundBank& bank,const std::string& name,const std::vector<uint8_t>& wav) {
        auto pcm=bank.decodeWav(wav.data(),wav.size());
        if(!pcm)return false;
        bank.cache_[name]=std::move(*pcm);bank.index_[name]=name;return true;
    }
    static SoundBank::Pcm cached(SoundBank& bank,const std::string& name) {return bank.cache_[name];}
    // playAt needs a device to admit a voice; a paused dummy one never calls back.
    static bool openPaused(SoundBank& bank) {
        SDL_AudioSpec want{};want.freq=SoundBank::kMixRate;want.format=AUDIO_S16SYS;
        want.channels=1;want.samples=1024;
        bank.dev_=SDL_OpenAudioDevice(nullptr,0,&want,&bank.spec_,0);
        return bank.dev_!=0;
    }
};

namespace {
// A canonical PCM RIFF/WAVE file in memory (interleaved samples, SDL channel order).
std::vector<uint8_t> wavFile(int rate,int channels,int bits,const std::vector<int>& samples) {
    std::vector<uint8_t> out;
    auto u32=[&](uint32_t v){for(int i=0;i<4;++i)out.push_back(uint8_t(v>>(8*i)));};
    auto u16=[&](uint16_t v){out.push_back(uint8_t(v));out.push_back(uint8_t(v>>8));};
    const uint32_t bytes=uint32_t(samples.size())*uint32_t(bits/8);
    out.insert(out.end(),{'R','I','F','F'});u32(36+bytes);out.insert(out.end(),{'W','A','V','E'});
    out.insert(out.end(),{'f','m','t',' '});u32(16);u16(1);u16(uint16_t(channels));u32(uint32_t(rate));
    u32(uint32_t(rate*channels*bits/8));u16(uint16_t(channels*bits/8));u16(uint16_t(bits));
    out.insert(out.end(),{'d','a','t','a'});u32(bytes);
    for(int s:samples) {if(bits==8)out.push_back(uint8_t(s));else u16(uint16_t(int16_t(s)));}
    return out;
}
// `frames` frames of freqs.size() channels, channel c a sine of amplitude A at freqs[c].
std::vector<int> tones(int rate,int frames,const std::vector<double>& freqs,double A=8000) {
    std::vector<int> s;
    for(int i=0;i<frames;++i)for(double f:freqs)s.push_back(int(A*std::sin(2*M_PI*f*i/rate)));
    return s;
}
// Goertzel magnitude of one channel of interleaved PCM at `freq`, normalised so a
// full-scale-A sine reads ~A.
double tone(const std::vector<int16_t>& pcm,int channels,int channel,double freq,int rate) {
    const size_t n=pcm.size()/size_t(channels);
    const double w=2*M_PI*freq/rate,c=2*std::cos(w);
    double s1=0,s2=0;
    for(size_t i=0;i<n;++i){double s=pcm[i*size_t(channels)+size_t(channel)]+c*s1-s2;s2=s1;s1=s;}
    return std::sqrt(s1*s1+s2*s2-c*s1*s2)*2/double(n);
}
int lastNonZero(const std::vector<int16_t>& pcm,int channels) {
    int last=-1;
    for(size_t i=0;i<pcm.size();++i)if(pcm[i]!=0)last=int(i/size_t(channels));
    return last;
}
std::string num(double v) {return std::to_string(std::lround(v));}
}

int main(int argc,char** argv) {
    int failures=0;
    const auto check=[&](bool ok,const std::string& message) {if(!ok){++failures;std::cerr<<"FAIL "<<message<<'\n';}};
    SDL_SetMainReady();
    SDL_setenv("SDL_AUDIODRIVER","dummy",1);
    if(SDL_Init(SDL_INIT_AUDIO)!=0)return 2;
    constexpr int R=SoundBank::kMixRate;
    static_assert(R==44100);
    {
        // Retail-format 22050 Hz stereo track: an 8 kHz tone on the left, 440 Hz on
        // the right. Cached at 22050 (no resample), it lasts twice the frames at the
        // 44100 Hz mix and keeps both tones, each on its own side. 8 kHz is 0.363 of the
        // source rate, inside the sinc's passband (-1 dB at 0.386): linear interpolation
        // left it ~3 dB down (> 5300), the sinc ~0.5 dB. Its image at 22050-8000 Hz and
        // the 440 Hz one at 21610 Hz lie in the stopband (>= 81 dB down): below -60 dB.
        constexpr int N=4410;
        auto pcm=SoundBankTestAccess::convert(wavFile(22050,2,16,tones(22050,N,{8000,440})));
        check(pcm && pcm->rate==22050 && pcm->channels==2 && pcm->frames()==size_t(N),
              "22050 Hz stereo track is cached at its own rate");
        SoundBank bank;SoundBankTestAccess::layout(bank,2);
        if(pcm)SoundBankTestAccess::setMusic(bank,*pcm);
        auto out=SoundBankTestAccess::mix(bank,2*N);
        const double l8=tone(out,2,0,8000,R),r8=tone(out,2,1,8000,R);
        const double l4=tone(out,2,0,440,R),r4=tone(out,2,1,440,R);
        std::cerr<<"22050 stereo: L8k="<<l8<<" R8k="<<r8<<" L440="<<l4<<" R440="<<r4<<'\n';
        const double img8=tone(out,2,0,22050-8000,R),img4=tone(out,2,1,22050-440,R);
        std::cerr<<"22050 stereo images: 14.05k="<<img8<<" 21.61k="<<img4<<'\n';
        check(l8>7000 && r8<200,"8 kHz left-channel content reaches only the left speaker");
        check(img8<0.001*l8 && img4<0.001*r4,"22050 Hz track: images above 11 kHz below -60 dB");
        check(r4>7900 && l4<200,"440 Hz right-channel content reaches only the right speaker");
        check(!SoundBankTestAccess::musicDone(bank),"track still playing on its last frame");
        SoundBankTestAccess::mix(bank,1);
        check(SoundBankTestAccess::musicDone(bank),"track ends after exactly twice its frames");
    }
    {
        // A mod's 44100 Hz stereo track plays frame for frame, 15 kHz intact.
        constexpr int N=4410;
        auto pcm=SoundBankTestAccess::convert(wavFile(R,2,16,tones(R,N,{15000,1000})));
        check(pcm && pcm->rate==R && pcm->channels==2,"44100 Hz stereo track is cached as authored");
        SoundBank bank;SoundBankTestAccess::layout(bank,2);
        if(pcm)SoundBankTestAccess::setMusic(bank,*pcm);
        auto out=SoundBankTestAccess::mix(bank,N);
        const double l15=tone(out,2,0,15000,R),r15=tone(out,2,1,15000,R),r1=tone(out,2,1,1000,R);
        std::cerr<<"44100 stereo: L15k="<<l15<<" R15k="<<r15<<" R1k="<<r1<<'\n';
        check(l15>7900 && r15<50 && r1>7900,"44100 Hz track keeps 15 kHz on the left, 1 kHz on the right");
        check(!SoundBankTestAccess::musicDone(bank),"44100 Hz track still playing on its last frame");
        SoundBankTestAccess::mix(bank,1);
        check(SoundBankTestAccess::musicDone(bank),"44100 Hz track ends after exactly its frames");
    }
    {
        // Music volume applies per channel; mono output folds L and R.
        SoundBank bank;SoundBankTestAccess::layout(bank,2);bank.setMusicVolume(128);
        SoundBankTestAccess::setMusic(bank,{{8192,-4096,8192,-4096},R,2});
        auto out=SoundBankTestAccess::mix(bank,2);
        check(out[0]==4096 && out[1]==-2048,"music volume scales both channels");
        SoundBankTestAccess::layout(bank,1);SoundBankTestAccess::setMusic(bank,{{8192,-4096},R,2});
        check(SoundBankTestAccess::mix(bank,1)[0]==2048,"mono output gets (L+R)/2");
    }
    {
        // 5.1 output, stereo track: L/R halved on front and rear, a quarter of each in
        // the centre, and the LFE only the low-passed mono sum.
        SoundBank bank;SoundBankTestAccess::layout(bank,6);
        std::vector<int16_t> pcm;for(int i=0;i<R;++i){pcm.push_back(8192);pcm.push_back(-4096);}
        SoundBankTestAccess::setMusic(bank,{pcm,R,2});
        auto out=SoundBankTestAccess::mix(bank,R);
        check(out[0]==4096 && out[1]==-2048 && out[2]==1024 && out[4]==4096 && out[5]==-2048,
              "5.1 music layout: FL/FR/BL/BR halved, centre (L+R)/4");
        check(out[3]>0 && out[3]<100,"LFE starts low-passed, not the full feed");
        const int settled=out[size_t(R-1)*6+3];
        check(settled>=1420 && settled<=1434,"LFE settles to the sub gain of (L+R)/2 = "+std::to_string(settled));
        // ~110 Hz one-pole at 44100 Hz: 63% of the step after 1/(2*pi*110) s = 64 frames.
        const int tau=out[64*6+3];
        check(tau>800 && tau<1000,"LFE cutoff stays ~110 Hz at 44100 Hz ("+std::to_string(tau)+")");
    }
    // A 5.1 source, one tone per channel (SDL order FL FR FC LFE BL BR).
    const std::vector<double> six={500,700,900,60,1100,1300};
    constexpr double h=0.70710678,w=1+2*h;   // a side's ITU fold (1 + .707 + .707), unity-weighted
    {
        // 5.1 music: on a 6-channel layout every channel keeps its own speaker, its
        // LFE to the LFE; on stereo it folds ITU-style, LFE dropped; a device switch
        // between the two re-lays it at once.
        constexpr int N=R/2;
        auto pcm=SoundBankTestAccess::convert(wavFile(R,6,16,tones(R,2*N,six)));
        check(pcm && pcm->channels==6 && pcm->rate==R,"5.1 track is cached with its six channels");
        if(!pcm)return 1;
        SoundBank bank;SoundBankTestAccess::layout(bank,6);SoundBankTestAccess::setMusic(bank,*pcm);
        auto surround=SoundBankTestAccess::mix(bank,N);
        for(int o=0;o<6;++o)for(int k=0;k<6;++k) {
            const double v=tone(surround,6,o,six[size_t(k)],R);
            const std::string what="5.1 music channel "+std::to_string(k);
            if(o==k)check(v>7800,what+" reaches its own speaker ("+num(v)+")");
            else check(v<120,what+" stays off speaker "+std::to_string(o)+" ("+num(v)+")");
        }
        SoundBankTestAccess::layout(bank,2);
        auto stereo=SoundBankTestAccess::mix(bank,N/2);
        const double fl=tone(stereo,2,0,500,R),fc=tone(stereo,2,0,900,R),bl=tone(stereo,2,0,1100,R);
        const double lfe=tone(stereo,2,0,60,R)+tone(stereo,2,1,60,R);
        const double cross=tone(stereo,2,0,700,R)+tone(stereo,2,0,1300,R)+tone(stereo,2,1,500,R);
        const double fcR=tone(stereo,2,1,900,R),br=tone(stereo,2,1,1300,R);
        std::cerr<<"5.1->2.0: FL="<<fl<<" FC="<<fc<<"/"<<fcR<<" BL="<<bl<<" BR="<<br
                 <<" LFE="<<lfe<<" cross="<<cross<<'\n';
        check(std::abs(fl-8000/w)<150 && std::abs(fc-8000*h/w)<150 && std::abs(bl-8000*h/w)<150,
              "stereo fold: L = (FL + .707 FC + .707 BL) / 2.414");
        check(std::abs(fcR-fc)<50 && std::abs(br-8000*h/w)<150,"stereo fold: centre in both, BR on the right");
        check(lfe<200 && cross<200,"stereo fold drops the LFE and keeps the sides apart");
        SoundBankTestAccess::layout(bank,6);
        auto back=SoundBankTestAccess::mix(bank,N/2);
        check(tone(back,6,3,60,R)>7800 && tone(back,6,3,500,R)<120 && tone(back,6,4,1100,R)>7800,
              "switching back to 6 channels restores the channel-for-channel layout");
    }
    {
        // 44.1 kHz stereo mod SFX: cached as authored (rate, channels, length), and a
        // UI play keeps its image -- 15 kHz on the left, 3 kHz on the right.
        constexpr int N=4410;
        SoundBank bank;
        if(!SoundBankTestAccess::openPaused(bank))return 2;
        SoundBankTestAccess::layout(bank,2);
        check(SoundBankTestAccess::decodeSfx(bank,"modsfx",wavFile(R,2,16,tones(R,N,{15000,3000}))),
              "44100 Hz stereo mod SFX decodes");
        const auto pcm=SoundBankTestAccess::cached(bank,"modsfx");
        check(pcm.rate==R && pcm.channels==2 && pcm.frames()==size_t(N),"44100 Hz stereo SFX is cached as authored");
        bank.play("modsfx");
        auto out=SoundBankTestAccess::mix(bank,N+500);
        std::vector<int16_t> body(out.begin(),out.begin()+2*N);
        const double l15=tone(body,2,0,15000,R),r15=tone(body,2,1,15000,R);
        const double l3=tone(body,2,0,3000,R),r3=tone(body,2,1,3000,R);
        std::cerr<<"stereo SFX: L15k="<<l15<<" R15k="<<r15<<" L3k="<<l3<<" R3k="<<r3<<'\n';
        check(l15>2700 && r15<30,"stereo SFX keeps 15 kHz energy, on the left only");
        check(r3>2700 && l3<30,"stereo SFX keeps 3 kHz on the right only");
        const int last=lastNonZero(out,2);
        check(last>=N-2 && last<=N,"stereo SFX lasts 0.1 s = 4410 frames ("+std::to_string(last+1)+")");
    }
    {
        // An 11025 Hz 8-bit mono unit sound is cached at 11025 and lasts as long as
        // before: four output frames per source frame.
        SoundBank bank;
        if(!SoundBankTestAccess::openPaused(bank))return 2;
        SoundBankTestAccess::layout(bank,1);
        check(SoundBankTestAccess::decodeSfx(bank,"blip",wavFile(11025,1,8,std::vector<int>(1102,0xC0))),
              "11025 Hz 8-bit SFX decodes");
        const auto pcm=SoundBankTestAccess::cached(bank,"blip");
        check(pcm.rate==11025 && pcm.channels==1 && pcm.frames()==1102,"unit SFX is cached at 11025 Hz mono");
        bank.play("blip");
        auto out=SoundBankTestAccess::mix(bank,5000);
        const int last=lastNonZero(out,1);
        // Exactly four output frames per source frame: the voice ends when its position
        // passes the last frame, no tail (the kernel's ring past the end is not played);
        // the last frame reads the sound's end against silence, so it is still non-zero.
        check(last+1==4408,"SFX plays 0.1 s = 4408 frames at 44100 Hz ("+std::to_string(last+1)+")");
        // The level holds once the kernel is inside the sound. The first frame's kernel
        // half-reads the silence before frame 0, so the onset is a band-limited step
        // (~0.92 of the level: the kernel's centre tap is 0.84) where linear
        // interpolation started at the full level.
        constexpr int level=(0xC0-0x80)*256/2;
        check(std::abs(out[100]-level)<=1 && std::abs(out[4000]-level)<=1,"8-bit SFX level unchanged ("
              +std::to_string(out[100])+")");
        check(out[0]>level*3/4 && out[0]<level-200,"SFX onset starts from silence ("+std::to_string(out[0])+")");
    }
    {
        // A 1 kHz tone at 11025 Hz stepped to 44100. Nearest-sample stepping would leave
        // its images at 11025-/+1000 Hz only ~20 dB down and linear interpolation ~40 dB
        // (the old limit was 3%); the sinc stops everything past 5.5 kHz: below -60 dB.
        SoundBank bank;
        if(!SoundBankTestAccess::openPaused(bank))return 2;
        SoundBankTestAccess::layout(bank,1);
        SoundBankTestAccess::decodeSfx(bank,"tone",wavFile(11025,1,16,tones(11025,11025,{1000})));
        bank.play("tone");
        auto out=SoundBankTestAccess::mix(bank,R-100);
        const double f=tone(out,1,0,1000,R),image=tone(out,1,0,10025,R),image2=tone(out,1,0,12025,R);
        std::cerr<<"11025 interpolation: 1k="<<f<<" image10k="<<image<<" image12k="<<image2<<'\n';
        check(f>3800 && image<0.001*f && image2<0.001*f,"11025 Hz tone: images at 10/12 kHz below -60 dB");
    }
    {
        // A 44100 Hz source plays 1:1 past the filter: noise through the music bed at
        // unity comes out sample for sample.
        std::vector<int16_t> noise(2*4096);
        uint32_t seed=12345;
        for(auto& v:noise){seed=seed*1664525u+1013904223u;v=int16_t(seed>>16);}
        SoundBank bank;SoundBankTestAccess::layout(bank,2);bank.setMusicVolume(256);
        SoundBankTestAccess::setMusic(bank,{noise,R,2});
        check(SoundBankTestAccess::mix(bank,4096)==noise,"44100 Hz source is bit-exact through the mixer");
    }
    {
        // A loop: 441 Hz at 11025 Hz is 25 frames a cycle, 1100 frames 44 whole cycles. Past
        // the first wrap the kernel reads across it (the end before frame 0, the start
        // past the end), so the seam is as smooth as the rest of the sine: no sample step
        // larger than the sine's own slope, and the third pass repeats the second.
        SoundBank bank;
        if(!SoundBankTestAccess::openPaused(bank))return 2;
        SoundBankTestAccess::layout(bank,1);
        SoundBankTestAccess::decodeSfx(bank,"loop",wavFile(11025,1,16,tones(11025,1100,{441})));
        bank.playAt("loop",0,0,false,0,0,1.f,1.f,3,true);
        constexpr int P=4*1100;
        auto out=SoundBankTestAccess::mix(bank,3*P+200);
        int jump=0,seam=0,repeat=0;
        for(int i=P/2;i<3*P;++i)jump=std::max(jump,std::abs(out[size_t(i)+1]-out[size_t(i)]));
        for(int i=2*P-50;i<2*P+50;++i)seam=std::max(seam,std::abs(out[size_t(i)+1]-out[size_t(i)]));
        for(int i=P;i<2*P;++i)repeat=std::max(repeat,std::abs(out[size_t(i)+P]-out[size_t(i)]));
        const int slope=int(std::ceil(4000*2*M_PI*441/R));   // amplitude 8000 /2 headroom
        std::cerr<<"loop: max jump="<<jump<<" at seam="<<seam<<" slope="<<slope<<" repeat="<<repeat<<'\n';
        check(jump<=slope+2 && seam<=slope+2,"loop wrap is continuous (no click at the seam)");
        check(repeat<=2,"each loop pass repeats the last, seam included");
    }
    {
        // Pitch: an 11025 Hz 1 kHz tone at rate 2 steps half a frame per output frame;
        // it plays at 2 kHz and the kernel still stops its images at 22050 -/+ 2000 Hz.
        SoundBank bank;
        if(!SoundBankTestAccess::openPaused(bank))return 2;
        SoundBankTestAccess::layout(bank,1);
        SoundBankTestAccess::decodeSfx(bank,"tone",wavFile(11025,1,16,tones(11025,11025,{1000})));
        bank.playAt("tone",0,0,false,0,0,1.f,2.f);
        auto out=SoundBankTestAccess::mix(bank,R/2-100);
        const double f=tone(out,1,0,2000,R),image=tone(out,1,0,20050,R),old=tone(out,1,0,1000,R);
        std::cerr<<"11025 at rate 2: 2k="<<f<<" image20k="<<image<<" 1k="<<old<<'\n';
        check(f>3800 && image<0.001*f && old<0.001*f,"rate 2 on 11025 Hz: tone at 2 kHz, no image");
    }
    {
        // Pitch above the mix rate: a 44100 Hz source at rate 2 (two frames per output
        // frame) has its kernel stretched, cutoff halved. A 15 kHz tone (-> 30 kHz,
        // above Nyquist) must not alias to 44100-30000 = 14.1 kHz; a 5 kHz one -> 10 kHz.
        SoundBank bank;
        if(!SoundBankTestAccess::openPaused(bank))return 2;
        SoundBankTestAccess::layout(bank,1);
        SoundBankTestAccess::decodeSfx(bank,"hi",wavFile(R,1,16,tones(R,R,{15000})));
        SoundBankTestAccess::decodeSfx(bank,"mid",wavFile(R,1,16,tones(R,R,{5000})));
        bank.playAt("hi",0,0,false,0,0,1.f,2.f);
        auto hi=SoundBankTestAccess::mix(bank,R/2-100);
        SoundBankTestAccess::silence(bank);bank.playAt("mid",0,0,false,0,0,1.f,2.f);
        auto mid=SoundBankTestAccess::mix(bank,R/2-100);
        const double alias=tone(hi,1,0,14100,R),ten=tone(mid,1,0,10000,R);
        std::cerr<<"44100 at rate 2: alias14.1k="<<alias<<" 10k="<<ten<<'\n';
        check(alias<4,"rate 2 on 44100 Hz: 15 kHz does not alias to 14.1 kHz (< -60 dB of 4000)");
        check(ten>3800,"rate 2 on 44100 Hz: 5 kHz plays at 10 kHz");
    }
    {
        // A stereo SFX pans with position: on screen centre it keeps its image; off to
        // one side its width narrows to mono on that side's speaker, and it fades.
        SoundBank bank;
        if(!SoundBankTestAccess::openPaused(bank))return 2;
        SoundBankTestAccess::layout(bank,2);
        std::vector<int> s;for(int i=0;i<2000;++i){s.push_back(8000);s.push_back(2000);}
        SoundBankTestAccess::decodeSfx(bank,"pair",wavFile(R,2,16,s));
        bank.setListener(0,0,100,50);
        bank.playWorld("pair",0,0);
        auto c=SoundBankTestAccess::mix(bank,1);
        check(std::abs(c[0]-2828)<=2 && std::abs(c[1]-707)<=2,"centred stereo SFX: L and R each on their own side ("
              +std::to_string(c[0])+","+std::to_string(c[1])+")");
        SoundBankTestAccess::silence(bank);bank.playWorld("pair",100,0);
        auto r=SoundBankTestAccess::mix(bank,1);
        check(r[0]==0 && r[1]==2500,"stereo SFX at the right edge: its mono sum on the right only");
        SoundBankTestAccess::silence(bank);bank.playWorld("pair",-50,0);
        auto l=SoundBankTestAccess::mix(bank,1);
        check(l[0]>l[1] && l[1]>0 && l[0]<4000,"stereo SFX half left: leans left, image narrowed");
        bank.setListener(-200,0,100,50);
        auto far=SoundBankTestAccess::mix(bank,1);
        check(std::abs(far[0])+std::abs(far[1])<std::abs(l[0])+std::abs(l[1]),"stereo SFX fades with distance");
    }
    {
        // A 5.1 UI sound: channel for channel on a 6-channel layout (LFE to LFE), the
        // ITU fold on stereo, LFE dropped, and back after a device switch. A positional
        // 5.1 sound is folded to stereo at its source instead.
        SoundBank bank;
        if(!SoundBankTestAccess::openPaused(bank))return 2;
        const int v[6]={1000,2000,3000,4000,5000,6000};
        std::vector<int> s;for(int i=0;i<3000;++i)for(int k:v)s.push_back(k);
        SoundBankTestAccess::decodeSfx(bank,"ui51",wavFile(R,6,16,s));
        check(SoundBankTestAccess::cached(bank,"ui51").channels==6,"5.1 UI sound keeps six channels");
        SoundBankTestAccess::layout(bank,6);
        bank.play("ui51");
        auto six51=SoundBankTestAccess::mix(bank,1);
        bool exact=true;for(int k=0;k<6;++k)exact&=six51[size_t(k)]==v[k]/2;
        check(exact,"5.1 UI sound on 5.1: each channel on its own speaker, LFE to the LFE");
        SoundBankTestAccess::layout(bank,2);
        auto two=SoundBankTestAccess::mix(bank,1);
        const int wantL=int((1000+h*3000+h*5000)/w)/2,wantR=int((2000+h*3000+h*6000)/w)/2;
        check(std::abs(two[0]-wantL)<=2 && std::abs(two[1]-wantR)<=2,"5.1 UI sound on stereo: ITU fold, LFE dropped ("
              +std::to_string(two[0])+","+std::to_string(two[1])+")");
        SoundBankTestAccess::layout(bank,6);
        auto again=SoundBankTestAccess::mix(bank,1);
        check(again[3]==2000 && again[4]==2500,"5.1 UI sound after switching back to 6 channels");
        SoundBankTestAccess::silence(bank);SoundBankTestAccess::layout(bank,2);
        bank.setListener(0,0,100,50);bank.playWorld("ui51",0,0);
        auto pos=SoundBankTestAccess::mix(bank,1);
        check(std::abs(pos[0]-int(wantL*h))<=2 && std::abs(pos[1]-int(wantR*h))<=2,
              "positional 5.1 sound folds to stereo at the source, then pans");
    }
    {
        // Synthesised tracks are cached at kSfxRate and stepped: 10 s at either rate.
        SoundBank bank;
        if(!SoundBankTestAccess::openPaused(bank))return 2;
        bank.discoAt(0,0);
        const auto d=SoundBankTestAccess::cached(bank,"disco");
        check(d.rate==SoundBank::kSfxRate && d.frames()==size_t(10*SoundBank::kSfxRate),
              "disco track is 10 s at 11025 Hz");
        auto out=SoundBankTestAccess::mix(bank,4*SoundBank::kSfxRate);
        check(lastNonZero(out,1)>4*SoundBank::kSfxRate-100,"disco track plays through the stepping");
    }
    {
        // Tap: the stream gets every 44100 Hz device frame as mixed, channel
        // count and stereo levels unchanged.
        struct Sink {int frames=0,channels=0,calls=0;std::vector<int16_t> pcm;} sink;
        SoundBank bank;SoundBankTestAccess::layout(bank,2);
        std::vector<int16_t> pcm;for(int i=0;i<4000;++i){pcm.push_back(8192);pcm.push_back(-4096);}
        SoundBankTestAccess::setMusic(bank,{pcm,R,2});
        bank.setAudioTap(&sink,[](void* ctx,const int16_t* p,int frames,int channels) {
            auto* s=static_cast<Sink*>(ctx);s->frames+=frames;s->channels=channels;++s->calls;
            s->pcm.insert(s->pcm.end(),p,p+size_t(frames)*size_t(channels));
        });
        SoundBankTestAccess::mixThunk(bank,1024);
        check(sink.frames==1024 && sink.channels==2,"tap gets every frame, same channels");
        SoundBankTestAccess::mixThunk(bank,1023);SoundBankTestAccess::mixThunk(bank,1);
        check(sink.frames==2048,"odd-sized callbacks pass through ("+std::to_string(sink.frames)+")");
        check(sink.pcm[2]==8192 && sink.pcm[3]==-4096 && sink.pcm[4094]==8192 && sink.pcm[4095]==-4096,
              "tap keeps the stereo levels");
        bank.setAudioTap(nullptr,nullptr);
    }
    {
        // A cosmetic mod pack replacing music/track3.wav with a 44.1 kHz stereo file
        // wins the VFS over the install's Music/ copy and plays at its own quality.
        namespace fs=std::filesystem;
        const fs::path root=fs::temp_directory_path()/("game_music_mod_"+std::to_string(std::random_device{}()));
        const fs::path pack=root/"mods"/"cosmetic"/"always_enabled"/"hq"/"music";
        fs::create_directories(root/"Music");fs::create_directories(pack);
        auto put=[](const fs::path& p,const std::vector<uint8_t>& d) {
            std::ofstream(p,std::ios::binary)
                .write(reinterpret_cast<const char*>(d.data()),std::streamsize(d.size()));
        };
        constexpr int N=R/4;
        put(root/"Music"/"track3.wav",wavFile(22050,2,16,tones(22050,N/2,{440,440})));
        put(pack/"track3.wav",wavFile(R,2,16,tones(R,N,{15000,1000})));
        {
            auto vfs=tak::hpi::mountPlayVfs(root,{},{});
            SoundBank bank;
            if(!SoundBankTestAccess::openPaused(bank))return 2;
            SoundBankTestAccess::layout(bank,2);
            bank.startMusic(vfs,{3});
            const auto& m=SoundBankTestAccess::music(bank);
            check(m.rate==R && m.channels==2 && m.frames()==size_t(N),
                  "mod track replaces the install's track3 at 44.1 kHz stereo");
            auto out=SoundBankTestAccess::mix(bank,N);
            const double l15=tone(out,2,0,15000,R),r15=tone(out,2,1,15000,R),r1=tone(out,2,1,1000,R);
            std::cerr<<"mod track: L15k="<<l15<<" R15k="<<r15<<" R1k="<<r1<<'\n';
            check(l15>7900 && r15<50 && r1>7900,"mod track plays at full rate, in stereo");
            check(!SoundBankTestAccess::musicDone(bank),"mod track still playing on its last frame");
            SoundBankTestAccess::mix(bank,1);
            check(SoundBankTestAccess::musicDone(bank),"mod track ends on time");
        }
        std::error_code ec;fs::remove_all(root,ec);
    }
    if(argc==2) {
        // Retail tracks are 22050 Hz stereo: cached as they are, frame for frame.
        auto vfs=tak::hpi::mountRetailRoot(argv[1]);
        int tracks=0;
        for(int t=1;t<=20;++t) {
            const std::string path="music/track"+std::to_string(t)+".wav";
            if(!vfs.has(path))continue;
            auto raw=vfs.read(path);
            SDL_AudioSpec spec{};Uint8* buf=nullptr;Uint32 len=0;
            if(!SDL_LoadWAV_RW(SDL_RWFromConstMem(raw.data(),int(raw.size())),1,&spec,&buf,&len)) {
                check(false,path+" loads");continue;
            }
            SDL_FreeWAV(buf);
            check(spec.freq==22050 && spec.channels==2,path+" is 22050 Hz stereo");
            auto pcm=SoundBankTestAccess::convert(raw);
            check(pcm && pcm->samples.size()==size_t(len/2) && pcm->rate==22050,
                  path+" keeps its own rate and channels");
            ++tracks;
        }
        check(tracks>0,"retail music tracks found");
    }
    SDL_Quit();
    if(failures)return 1;
    std::cout<<"game music ok\n";
    return 0;
}
