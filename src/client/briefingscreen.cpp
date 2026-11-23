#include "client/briefingscreen.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <vector>

#include "campaign/campaign.h"
#include "client/appquit.h"
#include "client/uitext.h"
#include "client/hidpi.h"
#include "client/cursors.h"
#include "client/dev.h"
#include "client/font.h"
#include "client/guiart.h"
#include "client/menumusic.h"
#include "client/hotkeys.h"
#include "client/settings.h"
#include "gui/gui.h"
#include "util/png.h"

namespace tak {
bool BriefingScreen::run(SDL_Renderer* ren,const hpi::Vfs& vfs,const std::string& stem,
                        const std::string& title,Settings* settings,MenuMusic* music,
                        SDL_Texture* background,std::function<bool()> idle,int chapter,
                        const std::string& note) {
    auto objectives=loadObjectives(vfs,stem);
    if(!note.empty()){objectives.push_back("");objectives.push_back(note);}
    gui::Gui ui;
    try {ui=gui::parse(vfs.read("guis/briefing.gui"),"guis/briefing.gui");}catch(...){}
    Font heading,bold,body;
    try{heading=Font(ren,vfs,"fonts/font48.gaf",FontRole::Art);}catch(...){}
    try{bold=Font(ren,vfs,"fonts/b_times new roman (100b).gaf",FontRole::Text);}catch(...){}
    try{body=Font(ren,vfs,"fonts/b_times new roman (100).gaf",FontRole::Text);}catch(...){}
    struct Cleanup {Font& a;Font& b;Font& c;~Cleanup(){a.destroyGlyphs();b.destroyGlyphs();c.destroyGlyphs();}} cleanup{heading,bold,body};
    CursorSet cursors;cursors.load(ren,vfs,settings);
    SDL_PumpEvents();SDL_FlushEvents(SDL_MOUSEBUTTONDOWN,SDL_MOUSEBUTTONUP);
    int scroll=0;
    for(;;) {
        if(termRequested() || (idle && !idle()))return false;
        int w=0,h=0;SDL_GetRendererOutputSize(ren,&w,&h);
        const GuiLayout screen(w,h);
        // Native root510x339 is centered on the in-game desktop. It contains
        // text only: no invented BEGIN/BACK buttons or replacement backdrop.
        GuiLayout lay=screen;lay.ox+=65*screen.scale;lay.oy+=70.5f*screen.scale;
        auto rect=[&](const char* name,SDL_FRect fallback) {
            if(const auto* g=ui.find(name))return lay.rect(float(g->x),float(g->y),float(g->w),float(g->h));
            return lay.rect(fallback.x,fallback.y,fallback.w,fallback.h);
        };
        const auto textRect=rect("Line2",{35,135,440,20});
        std::vector<std::string> lines;
        for(const auto& objective:objectives) {
            std::istringstream words(objective);std::string line,word;
            while(words>>word) {
                const auto next=line.empty()?word:line+" "+word;
                const float width=body.ok()?float(body.width(next,lay.scale)):uiTextWidth(next,lay.scale);
                if(!line.empty() && width>textRect.w){lines.push_back(line);line=word;}else line=next;
            }
            if(!line.empty() || objective.empty())lines.push_back(line);
        }
        scroll=std::clamp(scroll,0,std::max(0,int(lines.size())-9));
        SDL_Event e;
        while(SDL_PollEvent(&e)) {
            if(e.type==SDL_QUIT)continue; // consistent with the game's WM-close policy
            if(settings && toggleFullscreenKey(e,settings->hotkeys,settings->fullscreen)) {saveSettings(*settings);continue;}
            if(e.type==SDL_KEYDOWN) {
                const auto key=e.key.keysym.sym;
                if(key==SDLK_ESCAPE || key==SDLK_RETURN || key==SDLK_KP_ENTER || key==SDLK_SPACE)return true;
                if(key==SDLK_DOWN || key==SDLK_PAGEDOWN)++scroll;
                if(key==SDLK_UP || key==SDLK_PAGEUP)--scroll;
            }
            if(e.type==SDL_MOUSEWHEEL)scroll-=e.wheel.y;
            if(e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT)return true;
        }
        scroll=std::clamp(scroll,0,std::max(0,int(lines.size())-9));
        if(music)music->poll();
        SDL_SetRenderDrawColor(ren,0,0,0,255);SDL_RenderClear(ren);
        if(background)SDL_RenderCopy(ren,background,nullptr,nullptr);
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren,0,0,0,170);SDL_RenderFillRect(ren,nullptr);
        auto text=[&](const Font& font,const std::string& value,SDL_FRect r,bool center=false) {
            float scale=lay.scale;
            float width=font.ok()?float(font.width(value,scale)):uiTextWidth(value,scale);
            if(width>r.w && width>0){scale*=r.w/width;width=r.w;}
            float top=0,height=7*scale;
            if(font.ok())font.vbounds("Ag0123456789",scale,top,height);
            const float x=r.x+(center?(r.w-width)*.5f:0),y=r.y+(r.h-height)*.5f-top;
            if(font.ok())font.draw(ren,value,x,y,scale);
            else uiText(ren,value,x,y,scale,{255,255,255,255});
        };
        text(heading,"Paused",rect("Static0",{0,0,510,63}),true);
        text(bold,chapter>0?"Chapter "+std::to_string(chapter):"",rect("Line0",{35,79,440,20}));
        text(bold,title,rect("Line1",{35,99,440,20}));
        for(int i=0;i<9 && i+scroll<int(lines.size());++i) {
            const std::string name="Line"+std::to_string(i+2);
            text(body,lines[size_t(i+scroll)],rect(name.c_str(),{35,float(135+i*23),440,20}));
        }
        if(cursors.ok()) {
            const int scale=settings?settings->cursorScale:1;
            if(settings && settings->hardwareCursor && cursors.applyHardware(CursorId::Normal,scale))SDL_ShowCursor(SDL_ENABLE);
            else {SDL_ShowCursor(SDL_DISABLE);int mx=0,my=0;hidpi::mousePixels(ren,mx,my);cursors.draw(ren,CursorId::Normal,mx,my,scale);}
        }else SDL_ShowCursor(SDL_ENABLE);
        if(const char* path=devEnv("TAK_SHOT_BRIEFING")) {
            std::vector<uint8_t> pixels(size_t(w)*size_t(h)*4);
            if(SDL_RenderReadPixels(ren,nullptr,SDL_PIXELFORMAT_ABGR8888,pixels.data(),w*4)==0)png::write(path,w,h,pixels);
            return true;
        }
        if(const char* driver=SDL_GetCurrentVideoDriver();driver && !std::strcmp(driver,"dummy"))return true;
        SDL_RenderPresent(ren);SDL_Delay(8);
    }
}
}
