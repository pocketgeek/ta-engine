#include "client/streaming.h"
#include "client/cursorpath.h"
#include "client/dev.h"
#include "client/hudcorner.h"
#include "client/hidpi.h"
#include "client/uitext.h"
#include "client/gpuvram.h"
#include "client/hotkeys.h"
#include "client/settings.h"
#include "client/streamkey.h"
#include "client/streamcapture_d3d.h"
#include "client/uploadwatch.h"
#include "video/youtubebitrate.h"
#include <SDL_opengl.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstring>
#include <vector>
#include <utility>

namespace tak {
namespace {
// Double-buffered OpenGL readback: only map after the fence signals. Direct3D
// shrinks on the GPU (streamcapture_d3d.h). SDL's Metal/software backends use a
// throttled portable readback fallback.
// The two PBOs and the scaling renderbuffer are counted in gpuvram's total (only
// while a stream runs: 2 x w*h*4, plus w*h*4 when scaled) but never refused by its
// cap -- the player asked to stream, and the capture cannot work without them.
class Capture {
public:
    ~Capture() { clear(); }
    void reset() { clear(); d3d_.reset(); w_=h_=0; checked_=available_=false; context_=nullptr; std::vector<uint8_t>().swap(pixels_); }
    // `cursor`: the hardware cursor beside this capture (a default one: none). The
    // OpenGL readback delivers a frame or two later, so each pixel buffer keeps the
    // cursor it was captured with; the other paths deliver the frame they capture.
    bool get(SDL_Renderer* ren, int w, int h, video::Stream& stream, const video::StreamCursor& cursor) {
        if (w<=0 || h<=0 || w>8192 || h>8192) return false;
        SDL_RendererInfo info{}; SDL_GetRendererInfo(ren,&info);
        bool gl = info.name && !std::strcmp(info.name,"opengl");
        if (gl && !checked_) { checked_=true; available_=load(); scalable_=loadScale(); context_=SDL_GL_GetCurrentContext(); }
        if (gl && available_ && SDL_GL_GetCurrentContext()==context_) {
            SDL_RenderFlush(ren);
            const int sourceW=w, sourceH=h;
            const auto output=stream.status();
            if(scalable_ && output.width>0 && output.height>0) {
                const double scale=std::min({1.0,double(output.width)/w,double(output.height)/h});
                w=std::max(2,int(w*scale)&~1); h=std::max(2,int(h*scale)&~1);
            }
            if (w!=w_ || h!=h_) {clear();w_=w;h_=h; gen_(2,buffers_.data());}
            GLint old=0, align=0; get_(GL_PIXEL_PACK_BUFFER_BINDING,&old);get_(GL_PACK_ALIGNMENT,&align);
            GLint rowLength=0, skipRows=0, skipPixels=0;
            get_(GL_PACK_ROW_LENGTH,&rowLength); get_(GL_PACK_SKIP_ROWS,&skipRows); get_(GL_PACK_SKIP_PIXELS,&skipPixels);
            store_(GL_PACK_ROW_LENGTH,0); store_(GL_PACK_SKIP_ROWS,0); store_(GL_PACK_SKIP_PIXELS,0);
            store_(GL_PACK_ALIGNMENT,1);
            bool delivered=false;
            // Slots can wrap while an older readback is still pending. Deliver
            // ready frames in capture order so the newest image always wins.
            const std::array<int,2> order = serial_[0] <= serial_[1] ? std::array<int,2>{0,1} : std::array<int,2>{1,0};
            for(int i : order) if(fences_[i]) {
                GLenum result=wait_(fences_[i],GL_SYNC_FLUSH_COMMANDS_BIT,0);
                if(result==GL_ALREADY_SIGNALED || result==GL_CONDITION_SATISFIED) {
                    bind_(GL_PIXEL_PACK_BUFFER,buffers_[i]);
                    const auto* ptr=static_cast<const uint8_t*>(map_(GL_PIXEL_PACK_BUFFER,GL_READ_ONLY));
                    if(ptr) {
                        pixels_.resize(size_t(w)*h*4);
                        for(int y=0;y<h;++y) std::memcpy(pixels_.data()+size_t(y)*w*4,ptr+size_t(h-1-y)*w*4,size_t(w)*4);
                        unmap_(GL_PIXEL_PACK_BUFFER);
                        stream.cursor(cursors_[i]);
                        delivered=stream.video(pixels_,w,h);
                    }
                    deleteSync_(fences_[i]);fences_[i]=nullptr;
                }
            }
            for(int i=0;i<2;++i) if(!fences_[i]) {
                bind_(GL_PIXEL_PACK_BUFFER,buffers_[i]);
                if(!allocated_[i]) {
                    data_(GL_PIXEL_PACK_BUFFER,GLsizeiptr(size_t(w)*h*4),nullptr,GL_STREAM_READ);
                    allocated_[i]=true;count(size_t(w)*h*4);
                }
                GLint oldRead=0, oldDraw=0, oldRenderbuffer=0;
                const bool scale=scalable_ && (sourceW!=w || sourceH!=h);
                if(scale) {
                    get_(GL_READ_FRAMEBUFFER_BINDING,&oldRead);get_(GL_DRAW_FRAMEBUFFER_BINDING,&oldDraw);
                    get_(GL_RENDERBUFFER_BINDING,&oldRenderbuffer);
                    if(!fbo_) {
                        genFbo_(1,&fbo_);genRb_(1,&rb_);
                        bindRb_(GL_RENDERBUFFER,rb_);storage_(GL_RENDERBUFFER,GL_RGBA8,w,h);count(size_t(w)*h*4);
                        bindFbo_(GL_DRAW_FRAMEBUFFER,fbo_);
                        attach_(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,rb_);
                    } else bindFbo_(GL_DRAW_FRAMEBUFFER,fbo_);
                    if(checkFbo_(GL_DRAW_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) {
                        bindFbo_(GL_READ_FRAMEBUFFER,oldRead);bindFbo_(GL_DRAW_FRAMEBUFFER,oldDraw);
                        bindRb_(GL_RENDERBUFFER,oldRenderbuffer);
                        bind_(GL_PIXEL_PACK_BUFFER,GLuint(old));store_(GL_PACK_ALIGNMENT,align);
                        store_(GL_PACK_ROW_LENGTH,rowLength);store_(GL_PACK_SKIP_ROWS,skipRows);store_(GL_PACK_SKIP_PIXELS,skipPixels);
                        clear();scalable_=false;return false; // portable full-size capture next frame
                    }
                    const bool scissor=isEnabled_(GL_SCISSOR_TEST);
                    if(scissor)disable_(GL_SCISSOR_TEST);
                    blit_(0,0,sourceW,sourceH,0,0,w,h,GL_COLOR_BUFFER_BIT,GL_LINEAR);
                    if(scissor)enable_(GL_SCISSOR_TEST);
                    bindFbo_(GL_READ_FRAMEBUFFER,fbo_);
                }
                read_(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
                if(scale) {
                    bindFbo_(GL_READ_FRAMEBUFFER,oldRead);bindFbo_(GL_DRAW_FRAMEBUFFER,oldDraw);
                    bindRb_(GL_RENDERBUFFER,oldRenderbuffer);
                }
                fences_[i]=fence_(GL_SYNC_GPU_COMMANDS_COMPLETE,0);serial_[i]=++nextSerial_;cursors_[i]=cursor;break;
            }
            bind_(GL_PIXEL_PACK_BUFFER,GLuint(old));store_(GL_PACK_ALIGNMENT,align);
            store_(GL_PACK_ROW_LENGTH,rowLength); store_(GL_PACK_SKIP_ROWS,skipRows); store_(GL_PACK_SKIP_PIXELS,skipPixels);
            return delivered;
        }
        stream.cursor(cursor);
        if(!gl) {
            const auto output=stream.status();
            const int d3d=d3d_.get(ren,w,h,output.width,output.height,stream,cursor);
            if(d3d>=0)return d3d>0;
        }
        pixels_.resize(size_t(w)*h*4);
        if(SDL_RenderReadPixels(ren,nullptr,SDL_PIXELFORMAT_RGBA32,pixels_.data(),w*4)!=0)return false;
        return stream.video(pixels_,w,h);
    }
private:
    template<class T> bool proc(T& out,const char* name) {out=reinterpret_cast<T>(SDL_GL_GetProcAddress(name));return out!=nullptr;}
    bool load() {
        if (!SDL_GL_ExtensionSupported("GL_ARB_sync")) return false;
        return proc(gen_,"glGenBuffers") && proc(del_,"glDeleteBuffers") && proc(bind_,"glBindBuffer") &&
          proc(data_,"glBufferData") && proc(map_,"glMapBuffer") && proc(unmap_,"glUnmapBuffer") &&
          proc(fence_,"glFenceSync") && proc(wait_,"glClientWaitSync") && proc(deleteSync_,"glDeleteSync") &&
          proc(read_,"glReadPixels") && proc(get_,"glGetIntegerv") && proc(store_,"glPixelStorei");
    }
    bool loadScale() {
        if(!SDL_GL_ExtensionSupported("GL_ARB_framebuffer_object"))return false;
        return proc(genFbo_,"glGenFramebuffers") && proc(delFbo_,"glDeleteFramebuffers") &&
            proc(bindFbo_,"glBindFramebuffer") && proc(genRb_,"glGenRenderbuffers") &&
            proc(delRb_,"glDeleteRenderbuffers") && proc(bindRb_,"glBindRenderbuffer") &&
            proc(storage_,"glRenderbufferStorage") && proc(attach_,"glFramebufferRenderbuffer") &&
            proc(checkFbo_,"glCheckFramebufferStatus") && proc(blit_,"glBlitFramebuffer") &&
            proc(isEnabled_,"glIsEnabled") && proc(enable_,"glEnable") && proc(disable_,"glDisable");
    }
    void clear() {
        if(context_ && context_==SDL_GL_GetCurrentContext()) {
            for(auto& f:fences_) if(f) {deleteSync_(f);f=nullptr;}
            if(buffers_[0]) del_(2,buffers_.data());
            if(fbo_)delFbo_(1,&fbo_);
            if(rb_)delRb_(1,&rb_);
        }
        buffers_={};fences_={};allocated_={};cursors_={};fbo_=rb_=0;
        gpuvram::removeBuffer(counted_);counted_=0;   // gone with the context otherwise
    }
    void count(size_t b) {gpuvram::addBuffer(b);counted_+=b;}
    size_t counted_=0;
    D3DCapture d3d_;
    bool checked_=false,available_=false,scalable_=false;
    GLuint fbo_=0,rb_=0;
    int w_=0,h_=0;SDL_GLContext context_=nullptr;
    std::array<uint64_t,2> serial_{};uint64_t nextSerial_=0;
    std::array<bool,2> allocated_{};
    std::array<video::StreamCursor,2> cursors_{};
    std::array<GLuint,2> buffers_{};std::array<GLsync,2> fences_{};
    std::vector<uint8_t> pixels_;
    GLboolean (APIENTRY *isEnabled_)(GLenum)=nullptr;
    void (APIENTRY *enable_)(GLenum)=nullptr;
    void (APIENTRY *disable_)(GLenum)=nullptr;
    void (APIENTRY *genFbo_)(GLsizei,GLuint*)=nullptr;
    void (APIENTRY *delFbo_)(GLsizei,const GLuint*)=nullptr;
    void (APIENTRY *bindFbo_)(GLenum,GLuint)=nullptr;
    void (APIENTRY *genRb_)(GLsizei,GLuint*)=nullptr;
    void (APIENTRY *delRb_)(GLsizei,const GLuint*)=nullptr;
    void (APIENTRY *bindRb_)(GLenum,GLuint)=nullptr;
    void (APIENTRY *storage_)(GLenum,GLenum,GLsizei,GLsizei)=nullptr;
    void (APIENTRY *attach_)(GLenum,GLenum,GLenum,GLuint)=nullptr;
    GLenum (APIENTRY *checkFbo_)(GLenum)=nullptr;
    void (APIENTRY *blit_)(GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLbitfield,GLenum)=nullptr;
    void (APIENTRY *gen_)(GLsizei,GLuint*)=nullptr;
    void (APIENTRY *del_)(GLsizei,const GLuint*)=nullptr;
    void (APIENTRY *bind_)(GLenum,GLuint)=nullptr;
    void (APIENTRY *data_)(GLenum,GLsizeiptr,const void*,GLenum)=nullptr;
    void* (APIENTRY *map_)(GLenum,GLenum)=nullptr;
    GLboolean (APIENTRY *unmap_)(GLenum)=nullptr;
    GLsync (APIENTRY *fence_)(GLenum,GLbitfield)=nullptr;
    GLenum (APIENTRY *wait_)(GLsync,GLbitfield,GLuint64)=nullptr;
    void (APIENTRY *deleteSync_)(GLsync)=nullptr;
    void (APIENTRY *read_)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*)=nullptr;
    void (APIENTRY *get_)(GLenum,GLint*)=nullptr;
    void (APIENTRY *store_)(GLenum,GLint)=nullptr;
};
// The OS cursor as the broadcast should show it, or none (client/cursorpath.h): the
// hotspot at the pointer in renderer-output pixels, the w x h space being captured.
video::StreamCursor hardwareCursor(SDL_Renderer* ren,int w,int h) {
    const auto& shown=shownHardwareCursor();
    SDL_Window* window=SDL_RenderGetWindow(ren);
    if(!stampHardwareCursor(shown.image!=nullptr,SDL_ShowCursor(SDL_QUERY)==SDL_ENABLE,
                            shown.cursor && SDL_GetCursor()==shown.cursor,
                            window && SDL_GetMouseFocus()==window))return {};
    int mx=0,my=0;hidpi::mousePixels(ren,mx,my);
    video::StreamCursor c;
    c.image=shown.image;c.x=float(mx);c.y=float(my);c.scale=shown.pixelScale;c.refW=w;c.refH=h;
    return c;
}
}
struct Streaming::Impl {
    SDL_Renderer* ren;
    video::Stream stream;
    video::StreamConfig config;
    Capture capture;
    Settings* settings=nullptr;
    std::function<void()> save;
    std::unique_ptr<StreamKeyMemory> keys;
    int resolution=5; // 720p, 1080p, 1440p, 4K, full window, max width 3840
    bool visible=false,editing=false,wasActive=false,wasTextInput=false;
    bool localCopy=false; // SAVE A LOCAL COPY: settings->streamLocalCopy when there are settings
#ifdef TAK_STREAM_TESTING
    bool testCursorSet=false;video::StreamCursor testCursor;
#endif
    std::string error;
    // Why the bitrate was lowered (YOUTUBE'S ... MAXIMUM), until the next choice.
    std::string notice;
    uint64_t nextCapture=0, rateTime=0, rateBytes=0, rateSend=0;
    uint64_t rateKbps=0;
    UploadWatch upload; // UPLOAD CAN'T KEEP UP (client/uploadwatch.h)
    // Stream::start() sends only to YouTube, so its bitrate cap always applies here.
    const bool youtube=video::isYouTubeIngest(video::kYouTubeIngest);
    // The layout is in the Options screen's unit (u) and text sizes, so this panel grows
    // with the window exactly as Options, Voice Chat and Controls do.
    static constexpr float kW=660,kH=524,kRowPx=2.f,kSmallPx=1.6f; // panel size; row and status text
    float x=0,y=0,u=1;
    // Row 0 is the key; the REMEMBER KEY row sits under it, then rows 1-4, the SAVE A
    // LOCAL COPY row, and row 5 (start/stop).
    SDL_FRect slot(int k) const {return {x+26*u,y+(76+38*k)*u,(kW-52)*u,32*u};}
    SDL_FRect row(int n) const {return slot(n+(n>0)+(n>4));}
    SDL_FRect rememberRect() const {return slot(1);}
    SDL_FRect localCopyRect() const {return slot(6);}
    std::pair<int,int> windowResolution(int w,int h) const {
        if(resolution!=5)return {w&~1,h&~1};
        const int width=std::max(2,std::min(w&~1,3840));
        const int height=std::max(2,int(int64_t(h)*width/std::max(w,1))&~1);
        return {width,height};
    }
    SDL_FRect pasteRect() const {auto r=row(0);r.x+=r.w-110*u;r.w=110*u;return r;}
    SDL_FRect recommendedRect() const {auto r=row(3);r.x+=r.w-170*u;r.w=170*u;return r;}
    // Footer BACK, as Options' and Controls', at the right edge in line with the rows:
    // closes, as Esc.
    SDL_FRect backRect() const {return {x+(kW-26-150)*u,y+(kH-52+10)*u,150*u,32*u};}
    std::pair<int,int> outputSize(int w,int h) const {
        return resolution>=4&&!stream.active() ? windowResolution(w,h) : std::pair<int,int>{config.width,config.height};
    }
    // YouTube's bitrate for what Start Streaming would send now (video/youtubebitrate.h):
    // the cap and the RECOMMENDED value. INT_MAX for a destination that is not YouTube.
    int capKbps(int w,int h) const {
        if(!youtube)return INT_MAX;
        const auto [ow,oh]=outputSize(w,h);
        return video::youtubeRecommendedKbps(ow,oh,config.fps,video::streamCodec(config.encoder,ow,oh));
    }
    // Lower the bitrate to the cap, saying why when the value asked for was above it (a
    // resolution's floor raising it past the cap is not news). True when it changed.
    bool capBitrate(int w,int h,int asked) {
        const int cap=capKbps(w,h);
        if(config.bitrateKbps<=cap)return false;
        config.bitrateKbps=cap;
        if(asked>cap)notice="BITRATE LOWERED TO YOUTUBE'S "+std::to_string(cap)+" KBPS MAXIMUM";
        return true;
    }
    // The BITRATE row's steps: the panel's values up to the cap, then the cap itself.
    std::vector<int> bitrateSteps(int w,int h) const {
        const int cap=capKbps(w,h);
        std::vector<int> steps;
        for(int r:Settings::kStreamBitrates) if(r<cap)steps.push_back(r);
        if(cap<=Settings::kStreamBitrates[std::size(Settings::kStreamBitrates)-1])steps.push_back(cap);
        return steps;
    }
    void pasteKey() {
        char* clipboard=SDL_GetClipboardText();
        if(!clipboard || !*clipboard) {error="CLIPBOARD IS EMPTY";SDL_free(clipboard);return;}
        credstore::wipe(config.key);config.key=clipboard;
        credstore::wipe(clipboard,std::strlen(clipboard));SDL_free(clipboard);
        if(config.key.size()>256)config.key.resize(256);
        error.clear();editing=true;SDL_StartTextInput();
        keys->sync(config.key);
    }
    void toggleRemember() {
        if(!keys->setRemember(!keys->remember(),config.key)) {keys->check();return;} // greyed out: look again
        if(settings) {settings->streamRememberKey=keys->remember();if(save)save();}
    }
    // Remember the panel's current resolution, frame rate, bitrate (including one a
    // resolution change raised) and encoder; saves only when one of them changed.
    void saveChoices() {
        if(!settings)return;
        const bool cpu=!config.encoder.empty();
        if(settings->streamBitrate==config.bitrateKbps && settings->streamResolution==resolution &&
           settings->streamFps==config.fps && settings->streamCpuEncoder==cpu)return;
        settings->streamBitrate=config.bitrateKbps;settings->streamResolution=resolution;
        settings->streamFps=config.fps;settings->streamCpuEncoder=cpu;
        if(save)save();
    }
    void toggleLocalCopy() {
        if(stream.active())return; // greyed out: the choice applies at Start Streaming
        localCopy=!localCopy;
        if(settings) {settings->streamLocalCopy=localCopy;if(save)save();}
    }
    // The local copy's line: what it is doing, and its file -- the name while saving,
    // the whole path once the stream has stopped. Paths keep their end when cut.
    std::string localLine(const video::StreamStatus& s,float maxW) const {
        if(s.local.empty())return {};
        std::string file=s.localFile;
        if(s.active) {const auto cut=file.find_last_of("/\\");if(cut!=std::string::npos)file.erase(0,cut+1);}
        if(file.empty())return uiTextFit(s.local,kSmallPx*u,maxW);
        const std::string head=s.local+": ";
        return head+uiTextFitStart(file,kSmallPx*u,std::max(0.f,maxW-uiTextWidth(head,kSmallPx*u)));
    }
    void pollKeys() {
        keys->poll(config.key);
        if(auto e=keys->takeError();!e.empty())error=e;
        // A refused untick re-ticks the box (the key is still stored): keep the setting true.
        if(settings && settings->streamRememberKey!=keys->remember()) {
            settings->streamRememberKey=keys->remember();if(save)save();
        }
    }
    // The status banner shown while the panel is closed: "" when there is none.
    static constexpr const char* kUploadWarning="UPLOAD CAN'T KEEP UP - LOWER THE BITRATE";
    // The panel's key as shown, "" when it is unbound.
    std::string panelKey() const {
        const KeyChord c=settings ? effectiveChord(settings->hotkeys,Act::StreamingPanel)
                                  : effectiveChord({},Act::StreamingPanel);
        if(!c.bound())return {};
        std::string name=chordName(c);
        for(char& ch:name)ch=char(std::toupper(static_cast<unsigned char>(ch)));
        return name;
    }
    static std::string bannerText(const video::StreamStatus& s,bool uploadWarning,const std::string& panelKey) {
        if(!s.active && !s.state.starts_with("FAILED"))return {};
        std::string text="STREAM " + s.state;
        if(s.active && uploadWarning)text+=std::string(" - ")+kUploadWarning;
        // A copy going well is left out beside the warning, to keep the banner short.
        if(s.active && !s.local.empty() && s.local!="LOCAL COPY SAVED" &&
           !(uploadWarning && s.local=="SAVING LOCAL COPY"))text+=" - "+s.local;
        return panelKey.empty() ? text : text+" - "+panelKey;
    }
    video::StreamStatus bannerStatus(video::StreamStatus s) const {
#ifndef NDEBUG
        // TAK_SHOT_STREAM_BANNER=1: show the banner of a live stream saving a local copy,
        // with no stream at all, so a screenshot can check what it shares the corner with.
        if(!s.active && tak::devFlag("TAK_SHOT_STREAM_BANNER")) {s.active=true;s.state="LIVE";s.local="SAVING LOCAL COPY";}
#endif
        return s;
    }
    void layout(int w,int h) {
        u=std::clamp(std::min(w/1280.0f,h/720.0f),1.0f,3.0f); // Options' unit
        u=std::max(.4f,std::min({u,w*.95f/kW,h*.95f/kH}));      // a window too small to hold it
        x=(w-kW*u)/2;y=(h-kH*u)/2;
    }
    void show(bool on) {
        if(on&&!visible)wasTextInput=SDL_IsTextInputActive();
        if(on)keys->check();else if(visible)keys->sync(config.key);
        visible=on;editing=on&&!stream.active();
        if(editing||(!on&&wasTextInput))SDL_StartTextInput();else SDL_StopTextInput();
    }
    void draw(int w,int h) {
        auto s=stream.status();
        uint64_t now=SDL_GetTicks64();
        if(!s.active || s.bytes<rateBytes || s.sendMicros<rateSend) {
            rateTime=now;rateBytes=s.bytes;rateSend=s.sendMicros;rateKbps=0;upload.reset();
        } else if(now-rateTime>=1000) {
            rateKbps=(s.bytes-rateBytes)*8/(now-rateTime);
            if(s.state.starts_with("LIVE"))upload.sample(s.dropped,s.sendMicros-rateSend,(now-rateTime)*1000);
            else upload.reset(); // connecting or reconnecting: nothing is being sent yet
            rateTime=now;rateBytes=s.bytes;rateSend=s.sendMicros;
        }
        if(!visible) {
            if(const std::string text=bannerText(bannerStatus(s),upload.warning(),panelKey());!text.empty()) {
                const auto box=hudcorner::streamBanner(uiTextWidth(text,1.5f),w); // GameView keeps its overlays below it
                SDL_SetRenderDrawColor(ren,10,12,18,230);SDL_FRect r{box.x,box.y,box.w,box.h};SDL_RenderFillRectF(ren,&r);
                uiText(ren,text,20,20,1.5f,{255,210,100,255});
            }
            return;
        }
        layout(w,h);
        pollKeys();
        // A saved value, or a window resized under FULL / MAX, can be above the cap.
        if(!s.active && capBitrate(w,h,config.bitrateKbps))saveChoices();
        const bool storeOk=keys->state()==StreamKeyMemory::State::Available;
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren,12,17,26,248);SDL_FRect panel{x,y,kW*u,kH*u};SDL_RenderFillRectF(ren,&panel);
        const float rpx=kRowPx*u,spx=kSmallPx*u,left=x+26*u,width=(kW-52)*u;
        auto line=[&](const std::string& t,float py,SDL_Color col=SDL_Color{235,235,235,255}) {uiText(ren,t,left,y+py*u,spx,col);};
        auto rowText=[&](const std::string& t,float tx,const SDL_FRect& r,SDL_Color col) {
            uiText(ren,t,tx,r.y+(r.h-7*rpx)/2,rpx,col);
        };
        const std::string title="YOUTUBE STREAMING";
        uiText(ren,title,x+(kW*u-uiTextWidth(title,3.4f*u))/2,y+13*u,3.4f*u,{235,225,180,255});
        line(storeOk&&keys->remember()?"GAME VIDEO AND AUDIO - KEY SAVED IN PASSWORD STORE":
                                       "GAME VIDEO AND AUDIO - KEY IS NOT SAVED",54);
        const auto [outputW,outputH]=resolution>=4&&!s.active ? windowResolution(w,h) :
            std::pair<int,int>{config.width,config.height};
        std::array<std::string,6> labels={
            "STREAM KEY: " + (config.key.empty()?std::string("NOT SET"):std::string(std::min<size_t>(config.key.size(),24),'*')),
            "RESOLUTION: " + std::string(resolution==5?"MAX 3840 / ":resolution==4?"FULL ":"") +
                std::to_string(outputW)+" X "+std::to_string(outputH),
            "FRAME RATE: " + std::to_string(config.fps),
            "BITRATE: " + std::to_string(config.bitrateKbps)+" KBPS"+
                (youtube&&config.bitrateKbps==capKbps(w,h)?" - YOUTUBE MAX":""),
            "ENCODER: " + std::string(config.encoder.empty()?"AUTOMATIC":"CPU"),
            s.active?"STOP STREAMING":"START STREAMING"};
        for(int i=0;i<6;++i) {
            auto r=row(i);if(i==0)r.w-=118*u;if(i==3&&youtube)r.w-=178*u;
            SDL_SetRenderDrawColor(ren,i==0&&editing?55:35,45,60,255);SDL_RenderFillRectF(ren,&r);
            rowText(uiTextFit(labels[i],rpx,r.w-20*u),r.x+10*u,r,{240,235,210,255});
        }
        const SDL_Color live{240,235,210,255},grey{110,115,120,255},side{170,175,180,255};
        // A tick-box row: box, label, and a note on the right in the smaller text.
        auto tickRow=[&](const SDL_FRect& r,bool ticked,bool on,const char* label,const std::string& note,bool noteOn) {
            SDL_SetRenderDrawColor(ren,35,45,60,255);SDL_RenderFillRectF(ren,&r);
            SDL_FRect box{r.x+10*u,r.y+(r.h-18*u)/2,18*u,18*u};
            SDL_SetRenderDrawColor(ren,on?200:90,on?195:95,on?175:100,255);SDL_RenderDrawRectF(ren,&box);
            if(ticked) {
                SDL_FRect tick{box.x+4*u,box.y+4*u,10*u,10*u};
                SDL_SetRenderDrawColor(ren,on?255:90,on?210:95,on?100:100,255);SDL_RenderFillRectF(ren,&tick);
            }
            const float lx=r.x+38*u;
            rowText(label,lx,r,on?live:grey);
            const float room=r.x+r.w-10*u-(lx+uiTextWidth(label,rpx)+16*u);
            const std::string fit=uiTextFit(note,spx,std::max(0.f,room));
            uiText(ren,fit,r.x+r.w-10*u-uiTextWidth(fit,spx),r.y+(r.h-7*spx)/2,spx,noteOn?side:grey);
        };
        // REMEMBER KEY: greyed out, with the reason, when there is no usable store.
        const std::string where=keys->state()==StreamKeyMemory::State::Unavailable?keys->reason():
            !storeOk?std::string("CHECKING PASSWORD STORE"):keys->busy()?std::string("SAVING..."):keys->storeName();
        tickRow(rememberRect(),keys->remember(),storeOk,"REMEMBER KEY",where,storeOk);
        // SAVE A LOCAL COPY: the same look, greyed out while streaming (applies at start).
        tickRow(localCopyRect(),localCopy,!s.active,"SAVE A LOCAL COPY",
                s.active?"APPLIES AT START":"MP4 IN THE VIDEOS FOLDER",!s.active);
        auto smallButton=[&](const SDL_FRect& r,const std::string& label) {
            SDL_SetRenderDrawColor(ren,s.active?35:50,s.active?40:65,s.active?45:85,255);SDL_RenderFillRectF(ren,&r);
            rowText(label,r.x+(r.w-uiTextWidth(label,rpx))/2,r,s.active?grey:live);
        };
        smallButton(pasteRect(),"PASTE");
        if(youtube)smallButton(recommendedRect(),"RECOMMENDED"); // YouTube's bitrate for these settings
        line(s.state,384,{255,210,100,255});
        line((s.encoder.empty()?"":s.encoder+"  ")+"FRAMES "+std::to_string(s.frames)+"  DROPPED "+std::to_string(s.dropped),406);
        line("REPLACED "+std::to_string(s.replaced)+"  RATE "+std::to_string(rateKbps)+" KBPS",428);
        if(const auto local=localLine(s,width);!local.empty())
            line(local,450,s.local.find("STOPPED")!=std::string::npos||s.local.find("FAILED")!=std::string::npos?
                 SDL_Color{255,150,110,255}:SDL_Color{170,215,170,255});
        // An error or notice shares the footer with BACK, on its left, its text made
        // smaller when it would not fit beside the button.
        const auto back=backRect();
        auto bottom=[&](const std::string& t,SDL_Color col) {
            const float px=uiTextFitPx(t,spx,back.x-16*u-left);
            uiText(ren,t,left,back.y+(back.h-7*px)/2,px,col);
        };
        if(!error.empty())bottom(error,{235,235,235,255});
        else if(s.active&&upload.warning())bottom(kUploadWarning,{255,150,110,255});
        else if(!notice.empty()&&!s.active)bottom(notice,{255,210,100,255});
        // the BACK button every other settings screen has
        SDL_SetRenderDrawColor(ren,60,66,86,255);SDL_RenderFillRectF(ren,&back);
        SDL_SetRenderDrawColor(ren,130,140,170,255);SDL_RenderDrawRectF(ren,&back);
        uiText(ren,"BACK",back.x+(back.w-uiTextWidth("BACK",2.2f*u))/2,back.y+(back.h-7*2.2f*u)/2,2.2f*u,{228,232,242,255});
    }
};
Streaming::Streaming(SDL_Renderer* ren,Settings* settings,std::function<void()> save,
                     std::shared_ptr<credstore::Store> store):p_(std::make_unique<Impl>()) {
    p_->ren=ren;p_->config.fps=60;p_->settings=settings;p_->save=std::move(save);
    p_->localCopy=settings && settings->streamLocalCopy;
    if(settings) { // every value was snapped to its allowed list when the settings loaded
        p_->config.bitrateKbps=settings->streamBitrate;
        p_->config.fps=settings->streamFps;
        p_->config.encoder=settings->streamCpuEncoder?"libx264":"";
        p_->resolution=settings->streamResolution;
        if(p_->resolution<4) { // fixed sizes; FULL / MAX are taken from the window at Start
            constexpr int widths[]={1280,1920,2560,3840},heights[]={720,1080,1440,2160};
            p_->config.width=widths[p_->resolution];p_->config.height=heights[p_->resolution];
            // a hand-edited file may pair a big size with a low bitrate: same floor as choosing it
            const int64_t pixels=int64_t(p_->config.width)*p_->config.height;
            p_->config.bitrateKbps=std::max(p_->config.bitrateKbps,pixels>2560*1440?30000:pixels>1920*1080?12000:6000);
        }
    }
    p_->config.key.reserve(320); // room for 255 bytes + one text event: typing never reallocates (unwiped copies)
    const bool remember=settings && settings->streamRememberKey;
    p_->keys=std::make_unique<StreamKeyMemory>(store?std::move(store):streamKeyStore(),remember);
    if(remember)p_->keys->check(); // load the remembered key in the background now
}
Streaming::~Streaming(){
    if(p_->visible){p_->keys->sync(p_->config.key);SDL_StopTextInput();} // quitting closes the panel
    p_->keys.reset(); // finishes queued writes/deletes
    credstore::wipe(p_->config.key);
}
SDL_FRect Streaming::rememberRect(int w,int h){p_->layout(w,h);return p_->rememberRect();}
SDL_FRect Streaming::localCopyRect(int w,int h){p_->layout(w,h);return p_->localCopyRect();}
SDL_FRect Streaming::bitrateRect(int w,int h){p_->layout(w,h);auto r=p_->row(3);if(p_->youtube)r.w-=178*p_->u;return r;}
SDL_FRect Streaming::recommendedRect(int w,int h){p_->layout(w,h);return p_->recommendedRect();}
const std::string& Streaming::notice() const{return p_->notice;}
bool Streaming::waitKeyStore(int ms){const bool idle=p_->keys->waitIdle(ms);p_->pollKeys();return idle;}
video::Stream& Streaming::stream(){return p_->stream;}
#ifdef TAK_STREAM_TESTING
void Streaming::testCursor(const video::StreamCursor* c){p_->testCursorSet=c!=nullptr;p_->testCursor=c?*c:video::StreamCursor{};}
#endif
bool Streaming::shown() const{return p_->visible;}
bool Streaming::input(const SDL_Event& e,int w,int h) {
    static const std::map<std::string,std::string> noOverrides;
    if((e.type==SDL_KEYDOWN && !e.key.repeat &&
        streamingPanelPress(p_->settings?p_->settings->hotkeys:noOverrides,e.key.keysym.sym,
                            e.key.keysym.mod,SDL_IsTextInputActive())) ||
       (e.type==SDL_USEREVENT && e.user.code==kStreamingEvent)) {p_->show(!p_->visible);return true;}
    if(e.type==SDL_RENDER_TARGETS_RESET || e.type==SDL_RENDER_DEVICE_RESET)p_->capture.reset();
    if(!p_->visible)return false;
    p_->layout(w,h);
    p_->pollKeys();
    if(e.type==SDL_KEYDOWN) {
        if(e.key.keysym.sym==SDLK_ESCAPE){p_->show(false);return true;}
        if(p_->editing&&!p_->stream.active()) {
            if(e.key.keysym.sym==SDLK_BACKSPACE && !p_->config.key.empty())p_->config.key.pop_back();
            if((e.key.keysym.mod & KMOD_CTRL) && e.key.keysym.sym==SDLK_v) {
                p_->pasteKey();
            }
            if((e.key.keysym.mod & KMOD_CTRL) && e.key.keysym.sym==SDLK_a) {
                credstore::wipe(p_->config.key);p_->keys->sync(p_->config.key); // deletes a remembered key
            }
        }
    }
    if(e.type==SDL_TEXTINPUT && p_->editing && !p_->stream.active() && p_->config.key.size()<256)p_->config.key+=e.text.text;
    if(e.type==SDL_MOUSEBUTTONDOWN && (e.button.button==SDL_BUTTON_LEFT || e.button.button==SDL_BUTTON_RIGHT)) {
        const int direction=e.button.button==SDL_BUTTON_LEFT?1:-1;
        SDL_FPoint pt{float(e.button.x),float(e.button.y)};
        if(const auto back=p_->backRect();SDL_PointInFRect(&pt,&back)) {if(direction>0)p_->show(false);return true;}
        const auto paste=p_->pasteRect();
        if(SDL_PointInFRect(&pt,&paste)) {
            if(direction>0 && !p_->stream.active())p_->pasteKey();
            return true;
        }
        const auto remember=p_->rememberRect();
        if(SDL_PointInFRect(&pt,&remember)) {if(direction>0)p_->toggleRemember();return true;} // left only: untick deletes
        const auto copyRow=p_->localCopyRect();
        if(SDL_PointInFRect(&pt,&copyRow)) {if(direction>0)p_->toggleLocalCopy();return true;}
        const auto recommended=p_->recommendedRect();
        if(p_->youtube && SDL_PointInFRect(&pt,&recommended)) {
            if(direction>0 && !p_->stream.active()) {
                p_->config.bitrateKbps=p_->capKbps(w,h);p_->notice.clear();p_->saveChoices();
            }
            return true;
        }
        for(int i=0;i<6;++i) {auto r=p_->row(i);if(!SDL_PointInFRect(&pt,&r))continue;
            if(direction<0 && (i==0 || i==5))return true;
            if(i==5) {
                if(p_->stream.active())p_->stream.stop();
                else {
                    if(p_->resolution>=4){
                        const auto size=p_->windowResolution(w,h);
                        p_->config.width=size.first;p_->config.height=size.second;
                    }
                    if(p_->capBitrate(w,h,p_->config.bitrateKbps))p_->saveChoices();
                    p_->config.localCopyDir=p_->localCopy?videoDir():std::string();
                    if(w<2 || h<2 || p_->config.width>8192 || p_->config.height>8192)
                        p_->error="RESOLUTION MUST BE BETWEEN 2 AND 8192 PIXELS";
                    else if(p_->localCopy && p_->config.localCopyDir.empty())
                        p_->error="NO USER FOLDER FOR A LOCAL COPY - UNTICK SAVE A LOCAL COPY";
                    else if(!p_->stream.start(p_->config))p_->error="ENTER A VALID YOUTUBE STUDIO STREAM KEY";
                    else {p_->error.clear();p_->notice.clear();p_->editing=false;SDL_StopTextInput();p_->keys->sync(p_->config.key);}
                }
            } else if(!p_->stream.active()) {
                if(i>0)p_->notice.clear();
                p_->editing=i==0;if(p_->editing)SDL_StartTextInput();else SDL_StopTextInput();
                if(i==1){
                    p_->resolution=(p_->resolution+direction+6)%6;
                    constexpr int widths[]={1280,1920,2560,3840};
                    constexpr int heights[]={720,1080,1440,2160};
                    if(p_->resolution>=4){
                        const auto size=p_->windowResolution(w,h);
                        p_->config.width=size.first;p_->config.height=size.second;
                    } else {
                        p_->config.width=widths[p_->resolution];p_->config.height=heights[p_->resolution];
                    }
                    const int64_t pixels=int64_t(p_->config.width)*p_->config.height;
                    const int asked=p_->config.bitrateKbps;
                    p_->config.bitrateKbps=std::max(asked,pixels>2560*1440?30000:pixels>1920*1080?12000:6000);
                    p_->capBitrate(w,h,asked);
                    p_->saveChoices();
                }
                if(i==2){p_->config.fps=p_->config.fps==30?60:30;p_->capBitrate(w,h,p_->config.bitrateKbps);p_->saveChoices();}
                if(i==3){
                    const auto rates=p_->bitrateSteps(w,h);
                    if(direction>0) {
                        const auto next=std::upper_bound(std::begin(rates),std::end(rates),p_->config.bitrateKbps);
                        p_->config.bitrateKbps=next==std::end(rates)?rates[0]:*next;
                    } else {
                        auto prev=std::lower_bound(std::begin(rates),std::end(rates),p_->config.bitrateKbps);
                        if(prev==std::begin(rates))prev=std::end(rates);
                        p_->config.bitrateKbps=*--prev;
                    }
                    p_->saveChoices();
                }
                if(i==4){
                    p_->config.encoder=p_->config.encoder.empty()?"libx264":"";
                    p_->capBitrate(w,h,p_->config.bitrateKbps);p_->saveChoices();
                }
            }
        }
    }
    // Don't send setup keystrokes/clicks as game orders. Still pass window events.
    return e.type==SDL_KEYDOWN||e.type==SDL_KEYUP||e.type==SDL_TEXTINPUT||e.type==SDL_MOUSEBUTTONDOWN||e.type==SDL_MOUSEBUTTONUP||e.type==SDL_MOUSEMOTION||e.type==SDL_MOUSEWHEEL;
}
void Streaming::draw(int w,int h) { p_->draw(w,h); }
float Streaming::bannerBottom(int w) const {
    if(p_->visible)return 0;
    const std::string text=Impl::bannerText(p_->bannerStatus(p_->stream.status()),p_->upload.warning(),p_->panelKey());
    if(text.empty())return 0;
    const auto box=hudcorner::streamBanner(uiTextWidth(text,1.5f),w);
    return box.y+box.h;
}
void Streaming::frame(int w,int h,bool drawPanel) {
    uint64_t now=SDL_GetTicks64()*1000;
    bool active=p_->stream.active();
    if(!active && p_->wasActive)p_->capture.reset();
    p_->wasActive=active;
    if(active&&now>=p_->nextCapture&&w>0&&h>0){
        video::StreamCursor cursor=hardwareCursor(p_->ren,w,h);
#ifdef TAK_STREAM_TESTING
        if(p_->testCursorSet)cursor=p_->testCursor;
#endif
        p_->capture.get(p_->ren,w,h,p_->stream,cursor);
        const uint64_t interval=1000000/p_->stream.status().fps;
        // Keep a stable cadence across uneven render frames; now + interval
        // loses a whole render frame whenever the deadline is slightly late.
        p_->nextCapture+=interval;
        if(p_->nextCapture<=now)p_->nextCapture=now+interval;
    }
    if(drawPanel) p_->draw(w,h);
}
}
