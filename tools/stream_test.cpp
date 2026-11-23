#include "video/stream.h"
extern "C" {
#include <libavformat/avformat.h>
}
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
static void require(bool value,const char* message) {if(!value){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
static uint32_t be24(const unsigned char* p){return uint32_t(p[0])<<16|uint32_t(p[1])<<8|p[2];}
static uint32_t be32(const unsigned char* p){return uint32_t(p[0])<<24|be24(p+1);}
static uint64_t be64(const unsigned char* p){return uint64_t(be32(p))<<32|be32(p+4);}
static std::vector<unsigned char> slurp(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(file),{}};
}
// Coded video frames in an FLV file (the stream's own packets).
static int flvVideoFrames(const std::vector<unsigned char>& bytes) {
    int videos=0;
    for(size_t pos=13;pos+15<=bytes.size();) {
        const size_t len=be24(&bytes[pos+1]);
        if(pos+11+len+4>bytes.size())break;
        const auto* p=&bytes[pos+11];
        if(bytes[pos]==9&&len>=5) {
            const bool enhanced=(p[0]&0x80)!=0;
            const int packet=enhanced?(p[0]&15):p[1];
            if(packet==1 || (enhanced && packet==3))++videos;
        }
        pos+=11+len+4;
    }
    return videos;
}
// A fragmented MP4, read box by box (the shipped FFmpeg has no MP4 demuxer): its
// tracks' sample entries and timescales, and per track the samples and decode time
// the moof fragments add up to. Anything malformed fails the walk.
struct Mp4Track { std::string handler, entry; uint32_t timescale=0, defaultDuration=0; uint64_t samples=0, end=0; };
struct Mp4 { bool ftyp=false, mvex=false, trailer=false; int fragments=0; uint64_t mdat=0; std::map<uint32_t,Mp4Track> tracks; };
static bool walk(const unsigned char* p,size_t n,Mp4& m,uint32_t track,const std::string& parent) {
    size_t pos=0;
    uint32_t trafTrack=0,trafDefault=0;uint64_t trafBase=0,trafDuration=0;
    while(pos+8<=n) {
        uint64_t size=be32(p+pos);size_t head=8;
        const std::string type(reinterpret_cast<const char*>(p+pos+4),4);
        if(size==1){if(pos+16>n)return false;size=be64(p+pos+8);head=16;}
        else if(size==0)size=n-pos;
        if(size<head||size>n-pos)return false;
        const unsigned char* b=p+pos+head;const size_t len=size_t(size)-head;
        auto need=[&](size_t k){return len>=k;};
        if(type=="ftyp")m.ftyp=true;
        else if(type=="mdat")m.mdat+=len;
        else if(type=="mfra")m.trailer=true;
        else if(type=="mvex"){m.mvex=true;if(!walk(b,len,m,0,type))return false;}
        else if(type=="moov"||type=="mdia"||type=="minf"||type=="stbl"){if(!walk(b,len,m,track,type))return false;}
        else if(type=="trak") {
            // tkhd comes first: find the track id, then read the rest under it.
            if(!need(24)||std::string(reinterpret_cast<const char*>(b+4),4)!="tkhd")return false;
            const uint32_t id=be32(b+8+(b[8]==1?20:12));
            m.tracks[id];if(!walk(b,len,m,id,type))return false;
        } else if(type=="hdlr"){if(!need(12))return false;m.tracks[track].handler.assign(reinterpret_cast<const char*>(b+8),4);}
        else if(type=="mdhd"){if(!need(24))return false;m.tracks[track].timescale=be32(b+(b[0]==1?20:12));}
        else if(type=="stsd"){if(!need(16))return false;m.tracks[track].entry.assign(reinterpret_cast<const char*>(b+12),4);}
        else if(type=="trex"){if(!need(24))return false;m.tracks[be32(b+4)].defaultDuration=be32(b+12);}
        else if(type=="moof"){++m.fragments;if(!walk(b,len,m,0,type))return false;}
        else if(type=="traf") {
            if(!walk(b,len,m,0,type))return false;
        } else if(type=="tfhd"&&parent=="traf") {
            if(!need(8))return false;
            const uint32_t flags=be32(b)&0xffffff;size_t at=8;
            trafTrack=be32(b+4);trafDefault=m.tracks[trafTrack].defaultDuration;
            if(flags&1)at+=8;
            if(flags&2)at+=4;
            if(flags&8){if(!need(at+4))return false;trafDefault=be32(b+at);}
        } else if(type=="tfdt"&&parent=="traf") {
            if(!need(b[0]==1?12:8))return false;
            trafBase=b[0]==1?be64(b+4):be32(b+4);
        } else if(type=="trun"&&parent=="traf") {
            if(!need(8)||!trafTrack)return false;
            const uint32_t flags=be32(b)&0xffffff,count=be32(b+4);size_t at=8;
            if(flags&1)at+=4;
            if(flags&4)at+=4;
            const size_t each=4*(((flags>>8)&1)+((flags>>9)&1)+((flags>>10)&1)+((flags>>11)&1));
            if(!need(at+each*count))return false;
            for(uint32_t i=0;i<count;++i)trafDuration+=(flags&0x100)?be32(b+at+i*each):trafDefault;
            auto& t=m.tracks[trafTrack];t.samples+=count;
            t.end=std::max(t.end,trafBase+trafDuration);
        }
        pos+=size_t(size);
    }
    return pos==n;
}
static bool readMp4(const std::filesystem::path& path,Mp4& m) {
    const auto bytes=slurp(path);
    return !bytes.empty() && walk(bytes.data(),bytes.size(),m,0,"");
}
static const Mp4Track* trackOf(const Mp4& m,const char* handler) {
    for(auto& [id,t]:m.tracks)if(t.handler==handler)return &t;
    return nullptr;
}
static void waitStopped(tak::video::Stream& stream) {
    const auto deadline=std::chrono::steady_clock::now()+5s;
    while(stream.active() && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);
    require(!stream.active(),"stream stops");
}
// Feeds moving pixels and a tone for `seconds`, requiring the stream to stay up.
static void feed(tak::video::Stream& stream,std::chrono::milliseconds length) {
    std::vector<uint8_t> pixels(640*480*4);
    std::array<int16_t,1024*2> audio{};
    int samples=0,frame=0;
    const auto begin=std::chrono::steady_clock::now();
    while(std::chrono::steady_clock::now()-begin<length) {
        require(stream.active(),stream.status().state.c_str());
        for(size_t i=0;i<pixels.size();i+=4){pixels[i]=uint8_t(i/7+frame);pixels[i+1]=uint8_t(i/13);pixels[i+2]=80;pixels[i+3]=255;}
        ++frame;
        stream.video(pixels.data(),640,480,640*4);
        for(int i=0;i<1024;++i)audio[i*2]=audio[i*2+1]=int16_t(std::sin(double(samples++)*6.283185307179586*440/44100)*12000);
        stream.audio(audio.data(),1024,2);
        std::this_thread::sleep_for(23ms);
    }
}
// SAVE A LOCAL COPY: the stream's packets, also in a fragmented MP4 beside it. Returns
// false (skipped) when this FFmpeg build has no MP4 writer -- tools/build-ffmpeg-bink.sh
// predates it -- which CI never allows.
static bool localCopy(tak::video::StreamConfig config) {
    if(!av_guess_format("mp4",nullptr,nullptr)) {
        std::printf("SKIP local copy: this static FFmpeg has no MP4 muxer; rerun tools/build-ffmpeg-bink.sh\n");
        require(!std::getenv("CI"),"CI FFmpeg has the MP4 muxer");
        return false;
    }
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-stream-copy-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto videos=root/"videos"; // missing: the stream creates it
    const auto flv=root/"stream.flv";
    fs::create_directories(root);
    config.key.clear();config.localCopyDir=videos.string();
    {
        tak::video::Stream stream;
        require(stream.startRecording(config,flv.string()),"start with a local copy");
        feed(stream,2500ms);
        auto s=stream.status();
        require(s.local=="SAVING LOCAL COPY","status says SAVING LOCAL COPY");
        require(fs::path(s.localFile).parent_path()==videos,"the copy is in the videos folder");
        const auto name=fs::path(s.localFile).filename().string();
        require(name.size()==23 && name.starts_with("TAK-") && name[12]=='-' && name.ends_with(".mp4"),"named TAK-YYYYMMDD-HHMMSS.mp4");
        stream.stop();waitStopped(stream);
        s=stream.status();
        require(s.local=="LOCAL COPY SAVED" && fs::exists(s.localFile),"status says LOCAL COPY SAVED, with the path");
        Mp4 m;
        require(readMp4(s.localFile,m),"the copy is a well-formed MP4");
        const auto* v=trackOf(m,"vide");const auto* a=trackOf(m,"soun");
        require(m.ftyp && m.mvex && m.fragments>=1 && m.trailer,"fragmented MP4 with its trailer");
        require(v && (v->entry=="avc1"||v->entry=="hvc1") && v->timescale,"H.264/HEVC video track");
        require(a && a->entry=="mp4a" && a->timescale,"AAC audio track");
        const double vs=double(v->end)/v->timescale,as=double(a->end)/a->timescale;
        std::printf("local copy %s: %d fragments, video %llu samples %.2fs, audio %.2fs, %llu bytes\n",s.localFile.c_str(),
                    m.fragments,(unsigned long long)v->samples,vs,as,(unsigned long long)fs::file_size(s.localFile));
        require(m.mdat>10000 && v->samples>=30,"video samples written");
        require(vs>1.8 && vs<3.5 && std::abs(vs-as)<0.2,"duration matches the stream");
        const int flvFrames=flvVideoFrames(slurp(flv));
        std::printf("stream video frames %d, copy %llu\n",flvFrames,(unsigned long long)v->samples);
        require(uint64_t(flvFrames)==v->samples,"the copy holds exactly the stream's packets");
    }
    {
        // A full disk mid-stream: the copy stops, the stream does not.
        tak::video::Stream stream;
        require(stream.startRecording(config,flv.string()),"start with a local copy (disk fills)");
        stream.testFailLocalAfter(2, 20);   // a finished fragment first, whatever the runner's speed
        feed(stream,4000ms);
        auto s=stream.status();
        require(stream.active() && s.state=="RECORDING","a failed local write leaves the stream running");
        require(s.local.starts_with("LOCAL COPY STOPPED"),"status says the local copy stopped");
        const auto before=s.frames;
        feed(stream,500ms);
        require(stream.status().frames>before,"frames keep encoding after the copy stopped");
        stream.stop();waitStopped(stream);
        require(stream.status().local.starts_with("LOCAL COPY STOPPED"),"stop keeps the stopped reason");
        Mp4 m;
        require(readMp4(stream.status().localFile,m),"the cut-off copy is still well-formed (no trailer)");
        const auto* v=trackOf(m,"vide");
        require(!m.trailer && m.fragments>=1 && v && v->samples>=30,"its finished fragments remain");
    }
    {
        // A folder that cannot be created: the copy fails, the stream runs.
        tak::video::Stream stream;
        std::ofstream(root/"file").put('x');
        config.localCopyDir=(root/"file"/"videos").string();
        require(stream.startRecording(config,flv.string()),"start with an impossible local copy");
        feed(stream,800ms);
        const auto s=stream.status();
        require(s.state=="RECORDING" && s.local.starts_with("LOCAL COPY FAILED"),"unwritable folder: copy failed, stream runs");
        stream.stop();waitStopped(stream);
        config.localCopyDir="rtmps://example.invalid/x";
        require(!stream.startRecording(config,flv.string()),"a local copy cannot be a network destination");
    }
    fs::remove_all(root);
    return true;
}
// A capture much larger than the stream, as MAX 3840 sends a 7680x2160 window at
// 3840x1080: the worker must resize every frame within its frame slot. Before the
// threaded scaler, swscale's bilinear took ~50 ms per 7680x2160 frame and a 60 FPS
// stream encoded ~18 FPS, the rest DROPPED. The output is kept small so the encoder
// (x264 on machines without a GPU) is not what is measured. Steady state only: the
// first half second (encoder start-up) is not counted.
static int pacing(const std::string& encoder,int sw,int sh,int ow,int oh,int fps) {
    // Measured: a 3-core machine sits near the 10% limit and 2 cores fail it (the
    // encoder, the scaler and this feeder share them). Not a verdict on the scaler.
    if(std::thread::hardware_concurrency()<4){std::printf("SKIP pacing: fewer than 4 hardware threads\n");return 77;}
    tak::video::StreamConfig config;
    config.encoder=encoder;config.width=ow;config.height=oh;config.fps=fps;config.bitrateKbps=3000;
    auto path=std::filesystem::temp_directory_path()/("tak-stream-pacing-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".flv");
    std::array<std::vector<uint8_t>,2> frames;
    for(int f=0;f<2;++f) {
        frames[f].resize(size_t(sw)*sh*4);
        for(int y=0;y<sh;++y)for(int x=0;x<sw;++x){uint8_t* p=&frames[f][(size_t(y)*sw+x)*4];p[0]=uint8_t(x/7+f*40);p[1]=uint8_t(y/5);p[2]=uint8_t((x^y)>>3);p[3]=255;}
    }
    tak::video::StreamStatus warm{},end{};
    {
        tak::video::Stream stream;
        require(stream.startRecording(config,path.string()),"start pacing recording");
        std::array<int16_t,735*2> audio{};
        const auto begin=std::chrono::steady_clock::now();
        bool warmed=false;
        std::vector<uint8_t> buffer;
        for(int n=0;;++n) {
            const auto now=std::chrono::steady_clock::now();
            if(now-begin>=3500ms)break;
            require(stream.active(),stream.status().state.c_str());
            if(!warmed && now-begin>=500ms){warm=stream.status();warmed=true;}
            // As the capture does: fill a buffer, then hand it over (the copy
            // happens outside the stream's lock; a buffer comes back to reuse).
            buffer.assign(frames[n&1].begin(),frames[n&1].end());
            stream.video(buffer,sw,sh);
            stream.audio(audio.data(),735,2);
            std::this_thread::sleep_until(begin+std::chrono::microseconds(int64_t(n+1)*1000000/60));
        }
        end=stream.status();
        stream.stop();
        const auto deadline=std::chrono::steady_clock::now()+5s;
        while(stream.active()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);
    }
    std::filesystem::remove(path);
    const uint64_t frames3=end.frames-warm.frames, dropped=end.dropped-warm.dropped;
    std::printf("pacing %s %dx%d -> %dx%d@%d: %llu frames, %llu dropped in 3 s of steady state\n",end.encoder.c_str(),sw,sh,ow,oh,fps,
                (unsigned long long)frames3,(unsigned long long)dropped);
    // 3 s at fps: allow for a loaded test machine, not for a scaler that misses most slots.
    require(dropped*10<=uint64_t(fps)*3,"no more than 10% of the frame slots dropped");
    require(frames3*10>=uint64_t(fps)*3*8,"at least 80% of the frame slots encoded");
    return 0;
}
// The hardware cursor end to end through the worker (video/streamcursor.h): a 400x300
// capture into a 320x180 stream letterboxes to a 240x180 picture, so a cursor at the
// window's centre must land at the picture's centre at 0.6x, and only while one is set.
// A cursor set AFTER a submission belongs to the next one: frames carry their own.
static int cursorStamp(const std::string& encoder) {
    tak::video::StreamConfig config;
    config.encoder=encoder;config.width=320;config.height=180;config.fps=30;config.bitrateKbps=1000;
    auto path=std::filesystem::temp_directory_path()/("tak-stream-cursor-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".flv");
    const int sw=400,sh=300;
    std::vector<uint8_t> frame(size_t(sw)*sh*4,60);
    for(size_t i=3;i<frame.size();i+=4)frame[i]=255;
    auto image=std::make_shared<tak::video::CursorImage>();
    image->w=20;image->h=20;image->hotX=0;image->hotY=0;image->rgba.assign(20*20*4,255);
    tak::video::StreamCursor cursor;
    cursor.image=image;cursor.x=200;cursor.y=150;cursor.scale=1;cursor.refW=sw;cursor.refH=sh;
    int ok=0;
    {
        tak::video::Stream stream;
        require(stream.startRecording(config,path.string()),"start cursor recording");
        // Wait for a picture showing `want` (true: the cursor), feeding frames meanwhile.
        auto await=[&](bool want,const char* what) {
            const auto begin=std::chrono::steady_clock::now();
            while(std::chrono::steady_clock::now()-begin<5s) {
                require(stream.active(),stream.status().state.c_str());
                std::vector<uint8_t> buffer(frame);
                stream.cursor(want?cursor:tak::video::StreamCursor{});
                stream.video(buffer,sw,sh);
                if(!want)stream.cursor(cursor); // set after the submission: not this frame's
                std::this_thread::sleep_for(40ms);
                int w=0,h=0;const auto pic=stream.testLastPicture(w,h);
                if(w!=240||h!=180||pic.empty())continue;
                auto px=[&](int x,int y){return pic[(size_t(y)*w+x)*4];};
                // Centre 120,90; the 20px sprite becomes 12px: 120..131 x 90..101.
                const bool white=px(125,95)==255 && px(120,90)==255 && px(131,101)==255;
                const bool clean=px(119,90)==60 && px(132,95)==60 && px(125,102)==60 && px(125,89)==60;
                if(want && white && clean) return true;
                if(!want && px(125,95)==60 && px(120,90)==60) return true;
            }
            std::fprintf(stderr,"FAIL: %s\n",what);return false;
        };
        ok+=await(true,"cursor stamped at the picture's centre, scaled 0.6x, nothing around it");
        ok+=await(false,"no cursor -> no stamp (a later cursor does not leak into an earlier frame)");
        ok+=await(true,"cursor back");
        stream.stop();
        const auto deadline=std::chrono::steady_clock::now()+5s;
        while(stream.active()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);
    }
    std::filesystem::remove(path);
    if(ok!=3)return 1;
    std::printf("cursor stamp: placed, scaled and removed through the %s worker\n",encoder.c_str());
    return 0;
}
int main(int argc,char** argv) {
    if(argc>2 && std::string(argv[2])=="cursor")return cursorStamp(argv[1]);
    if(argc>2 && std::string(argv[2])=="pacing")
        return pacing(argv[1],argc>3?std::atoi(argv[3]):7680,argc>4?std::atoi(argv[4]):2160,
                      argc>5?std::atoi(argv[5]):1280,argc>6?std::atoi(argv[6]):360,argc>7?std::atoi(argv[7]):60);
    tak::video::StreamConfig config;
    config.encoder=argc>1?argv[1]:"libx264";
    if(config.encoder=="auto")config.encoder.clear();
    auto path=std::filesystem::temp_directory_path()/("tak-stream-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".flv");
    {
        tak::video::Stream stream;
        require(!stream.start(config),"missing key rejected without connection");
        config.key="invalid/secret?key";
        require(!stream.start(config),"URL injection rejected without connection");
        config.width=1;require(!stream.startRecording(config,path.string()),"invalid dimensions rejected");config.width=1280;
        require(!stream.startRecording(config,"rtmps://example.invalid/test"),"recording cannot contact a network destination");
        config.width=8194;require(!stream.startRecording(config,path.string()),"oversized dimensions rejected");
        config.width=1279;require(!stream.startRecording(config,path.string()),"odd dimensions rejected");
        config.width=1280;
        if(argc>2 && std::string(argv[2])=="4k") {config.width=3840;config.height=2160;config.bitrateKbps=30000;}
        if(argc>2 && std::string(argv[2])=="native") {config.width=1582;config.height=934;}
        if(argc>2 && std::string(argv[2])=="wide") {config.width=7680;config.height=2160;config.bitrateKbps=30000;}
        if(argc>3)config.fps=std::atoi(argv[3]);
        config.key.clear();
        require(stream.startRecording(config,path.string()),"start recording");
        require(!stream.startRecording(config,path.string()),"cannot start twice");
        std::vector<uint8_t> pixels(640*480*4);
        for(int y=0;y<480;++y)for(int x=0;x<640;++x){size_t p=(y*640+x)*4;pixels[p]=uint8_t(x/3);pixels[p+1]=uint8_t(y/2);pixels[p+2]=80;pixels[p+3]=255;}
        std::array<int16_t,1024*6> audio{};
        int samples=0;
        bool delayed=false;
        const bool overload=argc>2 && std::string(argv[2])=="overload";
        auto begin=std::chrono::steady_clock::now();
        while(std::chrono::steady_clock::now()-begin<(overload?4500ms:2500ms)) {
            require(stream.active(),stream.status().state.c_str());
            if(overload && !delayed && stream.status().frames>=15) {
                stream.testDelayOnce(1300); delayed=true;
            }
            stream.video(pixels.data(),640,480,640*4);
            for(int i=0;i<1024;++i) {
                int16_t s=int16_t(std::sin(double(samples++)*6.283185307179586*440/44100)*12000);
                for(int c=0;c<6;++c)audio[i*6+c]=s;
            }
            stream.audio(audio.data(),1024,6);
            std::this_thread::sleep_for(23ms);
        }
        auto status=stream.status();
        std::printf("encoder=%s frames=%llu dropped=%llu bytes=%llu\n",status.encoder.c_str(),
                    (unsigned long long)status.frames,(unsigned long long)status.dropped,(unsigned long long)status.bytes);
        if(overload) {require(delayed,"overload injected");require(status.dropped>=30,"late video slots skipped");require(status.state=="RECORDING","encoder stall did not restart stream");}
        require(status.frames>=30,"video frames encoded");require(status.bytes>10000,"packets written");
        uint64_t pausedFrames=0;
        if(overload) {
            // Deterministically stop after the worker has entered an iteration
            // but before audio catch-up. Previously it still encoded a video
            // frame at the new timestamp after cancellation stopped the audio.
            stream.testDelayOnce(10000);
            const auto deadline=std::chrono::steady_clock::now()+5s;
            while(!stream.testDelayInProgress() && std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(1ms);
            require(stream.testDelayInProgress(),"worker paused before cancellation");
            pausedFrames=stream.status().frames;
            std::this_thread::sleep_for(200ms);
            require(stream.testDelayInProgress(),"worker remains paused at stop boundary");
        }
        auto stop=std::chrono::steady_clock::now();stream.stop();
        require(std::chrono::steady_clock::now()-stop<50ms,"stop does not block UI");
        {
            const auto deadline=std::chrono::steady_clock::now()+5s;
            while(stream.active() && std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(1ms);
            require(!stream.active(),"cancel wakes paused encoder and finishes drain");
            require(stream.testRetainedVideoBytes()==0,"stopped stream releases video buffers");
            if(overload)require(stream.status().frames==pausedFrames,"no video submitted after stop interrupted audio");
        }
    }
    std::ifstream file(path,std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)),{});
    file.close(); // Windows cannot remove an output while our reader still holds it.
    require(bytes.size()>13 && bytes[0]=='F'&&bytes[1]=='L'&&bytes[2]=='V',"valid FLV header");
    require((bytes[4]&5)==5,"both audio and video advertised");
    size_t pos=13;int videos=0,audios=0,keys=0;uint32_t vt=0,at=0;
    bool vc=false,ac=false;
    while(pos+15<=bytes.size()) {
        unsigned type=bytes[pos];size_t len=be24(&bytes[pos+1]);
        uint32_t ts=be24(&bytes[pos+4])|(uint32_t(bytes[pos+7])<<24);
        require(pos+11+len+4<=bytes.size(),"complete packet");
        auto* p=&bytes[pos+11];
        if(type==9&&len>=5) {
            const bool enhanced=(p[0]&0x80)!=0;
            if(enhanced)require(std::string(reinterpret_cast<const char*>(p+1),4)=="hvc1","HEVC fourcc");
            else require((p[0]&15)==7,"H264 video");
            const int packet=enhanced?(p[0]&15):p[1];
            if(packet==0)vc=true;
            if(packet==1 || (enhanced && packet==3)) {
                require(ts>=vt,"monotonic video timestamp");vt=ts;++videos;
                if(((p[0]>>4)&7)==1)++keys;
            }
        }
        if(type==8&&len>=2) {
            require((p[0]>>4)==10,"AAC audio");
            if(p[1]==0)ac=true;
            if(p[1]==1){require(ts>=at,"monotonic audio timestamp");at=ts;++audios;}
        }
        pos+=11+len+4;
    }
    require(vc&&ac,"both codec configuration headers present");
    require(videos>=30&&audios>=40&&keys>=1,"decodable video and audio packets");
    std::printf("audio/video end_ms=%u/%u\n",vt,at);
    require(std::abs(int(vt)-int(at))<100,"audio/video end times within 100 ms");
    std::printf("file=%s video=%d audio=%d keyframes=%d end_ms=%u/%u\n",path.string().c_str(),videos,audios,keys,vt,at);
    if(argc<2 || (argc>2 && std::string(argv[2])=="overload"))std::filesystem::remove(path);
    path += ".restart";
    // Failure is local, reported without throwing through the host or exposing a key.
    {
        tak::video::Stream stream;
        auto bad=path/"missing"/"output.flv";
        require(stream.startRecording(config,bad.string()),"failed destination scheduled asynchronously");
        auto limit=std::chrono::steady_clock::now()+5s;
        while(stream.active()&&std::chrono::steady_clock::now()<limit)std::this_thread::sleep_for(10ms);
        require(!stream.active()&&stream.status().state.starts_with("FAILED"),"I/O failure reported");
        require(stream.startRecording(config,path.string()),"can restart after failure");stream.stop();
    }
    std::filesystem::remove(path);
    if(argc<3 && !localCopy(config))return 77; // the plain run only: CTest reports a skip
    return 0;
}
