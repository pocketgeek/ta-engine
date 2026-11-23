#include "client/streaming.h"
#include "client/presentationpacer.h"
#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
int main(int argc,char** argv) {
    SDL_SetMainReady();
    if(SDL_Init(SDL_INIT_VIDEO)!=0){std::fprintf(stderr,"%s\n",SDL_GetError());return 1;}
    const int w=argc>3?std::atoi(argv[3]):640, h=argc>4?std::atoi(argv[4]):480;
    SDL_Window* win=SDL_CreateWindow("capture test",0,0,w,h,SDL_WINDOW_HIDDEN);
    SDL_Renderer* ren=SDL_CreateRenderer(win,-1,argc>1&&*argv[1]?SDL_RENDERER_ACCELERATED:SDL_RENDERER_SOFTWARE); // "": software
    if(!ren){std::fprintf(stderr,"%s\n",SDL_GetError());return 1;}
    const bool checkVsync=argc>1 && std::string(argv[1])=="gl-vsync";
    if(checkVsync && SDL_RenderSetVSync(ren,1)!=0) {
        std::fprintf(stderr,"VSync unavailable: %s\n",SDL_GetError());return 4;
    }
    const int swapBefore=checkVsync?SDL_GL_GetSwapInterval():0;
    if(checkVsync && swapBefore!=1){std::fprintf(stderr,"native GL VSync not enabled\n");return 4;}
    // A fast/nonblocking present must still be paced. A slow frame must not
    // be followed by another whole interval of delay.
    if(checkVsync) {
        tak::PresentationPacer pacer;
        pacer.paceAtRefresh(60);
        const auto begin=SDL_GetPerformanceCounter();
        for(int i=0;i<6;++i)pacer.paceAtRefresh(60);
        const double elapsed=double(SDL_GetPerformanceCounter()-begin)/SDL_GetPerformanceFrequency();
        if(elapsed<0.095){std::fprintf(stderr,"presentation fallback ran too fast\n");return 7;}
        SDL_Delay(30);
        const auto slow=SDL_GetPerformanceCounter();pacer.paceAtRefresh(60);
        if(double(SDL_GetPerformanceCounter()-slow)/SDL_GetPerformanceFrequency()>0.010) {
            std::fprintf(stderr,"presentation fallback double-paced a slow frame\n");return 8;
        }
    }
    SDL_RendererInfo info{};SDL_GetRendererInfo(ren,&info);
    const bool tempFile=argc<3||!*argv[2]; // "": a temporary file, removed afterwards
    // Unique: CTest runs streaming_capture and streaming_capture_cursor side by side.
    std::string path=!tempFile?argv[2]:(std::filesystem::temp_directory_path()/
        ("tak-stream-capture-"+std::to_string(SDL_GetPerformanceCounter())+".flv")).string();
    {
        tak::Streaming streaming(ren);
        tak::video::StreamConfig c;c.encoder=argc>6?argv[6]:"libx264";
        if(argc>5 && std::string(argv[5])=="native") {c.width=w&~1;c.height=h&~1;c.bitrateKbps=30000;}
        if(argc>5 && std::string(argv[5])=="4k") {c.width=3840;c.height=2160;c.bitrateKbps=30000;}
        if(argc>5 && std::string(argv[5])=="cursor") {c.width=320;c.height=180;c.bitrateKbps=1000;}
        if(argc>7)c.fps=std::atoi(argv[7]);
        if(c.encoder=="auto")c.encoder.clear();
        if(!streaming.stream().startRecording(c,path))return 1;
        SDL_Event e{};e.type=SDL_KEYDOWN;e.key.keysym.sym=SDLK_F9;
        if(!streaming.input(e,w,h)||!streaming.shown())return 2;
        // "cursor": a 640x480 window into a 320x180 stream (a 240x180 picture, 0.375x).
        // A 20x20 white cursor at CURSOR SIZE 2 on the window's centre must arrive as a
        // 15x15 block at 120,90 of the picture, whichever capture backend shrank the
        // frame -- and must be gone once the cursor is cleared.
        const bool cursorCheck=argc>5 && std::string(argv[5])=="cursor";
        auto image=std::make_shared<tak::video::CursorImage>();
        image->w=image->h=20;image->rgba.assign(20*20*4,255);
        tak::video::StreamCursor cursor;cursor.image=image;cursor.x=float(w)/2;cursor.y=float(h)/2;
        cursor.scale=2;cursor.refW=w;cursor.refH=h;
        int cursorPhase=cursorCheck?1:0; // 1: want it stamped; 2: want it gone; 3: done
        if(cursorCheck)streaming.testCursor(&cursor);
        Uint64 start=SDL_GetTicks64();
        uint64_t captureUs=0; unsigned calls=0;
        while(SDL_GetTicks64()-start<4000){
            // Wide red left half, green right half; blue top-left corner lets
            // a decoder check both channel order and vertical orientation.
            SDL_SetRenderDrawColor(ren,255,0,0,255);SDL_RenderClear(ren);
            SDL_SetRenderDrawColor(ren,0,255,0,255);SDL_Rect r{w/2,0,w-w/2,h};SDL_RenderFillRect(ren,&r);
            SDL_SetRenderDrawColor(ren,0,0,255,255);r={0,0,w/6,h/5};SDL_RenderFillRect(ren,&r);
            const auto before=SDL_GetPerformanceCounter();
            streaming.frame(w,h);
            captureUs+=(SDL_GetPerformanceCounter()-before)*1000000/SDL_GetPerformanceFrequency(); ++calls; // private panel drawn AFTER captured image
            if(checkVsync && SDL_GL_GetSwapInterval()!=swapBefore) {
                std::fprintf(stderr,"stream capture changed swap interval\n");return 5;
            }
            SDL_RenderPresent(ren);SDL_Delay(8);
            if(cursorPhase==1||cursorPhase==2) {
                int pw=0,ph=0;const auto pic=streaming.stream().testLastPicture(pw,ph);
                if(pw==240&&ph==180) {
                    auto px=[&](int x,int y){return &pic[(size_t(y)*pw+x)*4];};
                    const bool white=px(127,97)[0]==255&&px(127,97)[1]==255&&px(127,97)[2]==255&&
                                     px(120,90)[1]==255&&px(134,104)[1]==255;
                    const bool redBeside=px(110,97)[0]>200&&px(110,97)[1]<60; // the frame, not the cursor
                    if(cursorPhase==1&&white&&redBeside&&px(135,97)[2]<60&&px(127,105)[2]<60) {
                        std::printf("cursor stamped: 15x15 at 120,90 of the 240x180 picture\n");
                        streaming.testCursor(nullptr);cursorPhase=2;
                    } else if(cursorPhase==2&&!white&&px(127,97)[2]<60) {
                        std::printf("cursor cleared: no stamp\n");cursorPhase=3;
                    }
                }
            }
        }
        if(cursorCheck&&cursorPhase!=3){std::fprintf(stderr,"FAIL: cursor phase %d not reached\n",cursorPhase);return 9;}
        auto s=streaming.stream().status();
        std::printf("capture average_us=%llu calls=%u dropped=%llu\n", (unsigned long long)(captureUs/calls), calls, (unsigned long long)s.dropped);
        if(s.frames<30){std::fprintf(stderr,"%s\n",s.state.c_str());return 3;}
        std::printf("capture backend=%s file=%s frames=%llu\n",info.name,path.c_str(),(unsigned long long)s.frames);
        streaming.stream().stop();
    }
    if(checkVsync && SDL_GL_GetSwapInterval()!=swapBefore)return 6;
    if(checkVsync)std::printf("swap interval unchanged: %d\n",swapBefore);
    SDL_DestroyRenderer(ren);SDL_DestroyWindow(win);SDL_Quit();
    if(tempFile){std::error_code ec;std::filesystem::remove(path,ec);}
}
