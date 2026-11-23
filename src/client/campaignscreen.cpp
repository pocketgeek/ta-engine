#include "client/campaignscreen.h"

#include <algorithm>
#include <array>
#include <map>
#include <sstream>
#include <vector>

#include "campaign/campaign.h"
#include "client/uitext.h"
#include "client/hidpi.h"
#include "client/font.h"
#include "client/guiart.h"
#include "client/menuclick.h"
#include "client/gpuvram.h"
#include "client/settings.h"
#include "client/artscale.h"
#include "gui/gui.h"
#include "hpi/hpi.h"

namespace tak {
struct CampaignScreen::Impl {
    SDL_Renderer* ren;
    const Settings& settings;
    std::vector<Campaign> camps;
    gui::Gui ui;
    struct Image {SDL_Texture* texture=nullptr;int width=0,height=0;};
    std::map<std::string,std::vector<Image>> art;
    std::vector<SDL_Texture*> chapters;
    Font body,heading,decor,label;
    int tab=0,page=0;
    bool picked=false;
    std::string stem,campaign;
    std::function<void(const std::string&)> playSound;
    // Not retail: the always-enabled cosmetic packs (mods/cosmetic/always_enabled/) every game loads,
    // campaigns included, named on the left page (they cannot be switched off here).
    std::string alwaysOn;

    Impl(SDL_Renderer* r,const hpi::Vfs& vfs,const Settings& s,int initial,const std::filesystem::path& install)
        :ren(r),settings(s),camps(loadCampaigns(vfs)) {
        if(!install.empty())try {
            for(const auto& name:hpi::alwaysEnabledPacks(install,hpi::ModKind::Cosmetic))alwaysOn+=(alwaysOn.empty()?"":", ")+name;
        }catch(...) {}
        try {ui=gui::parse(vfs.read("guis/bod.gui"),"guis/bod.gui");}catch(...) {}
        std::map<std::string,std::vector<gaf::Sequence>> banks;
        auto frameFor=[&](const gui::ImgRef& ref)->const gaf::Frame* {
            std::string bank=ref.gaf;
            if(bank.size()>4 && bank.substr(bank.size()-4)==".gaf")bank.resize(bank.size()-4);
            if(!banks.contains(bank))try {
                banks[bank]=gaf::load(vfs.read("anims/"+bank+".gaf"),guiPalette(vfs,bank));
            }catch(...){banks[bank]={};}
            for(const auto& seq:banks[bank])if(seq.name==ref.seq && ref.frame>=0 && size_t(ref.frame)<seq.frames.size())
                return &seq.frames[size_t(ref.frame)];
            return nullptr;
        };
        for(const auto& gadget:ui.gadgets) {
            if(gadget.name=="ChapterImage")continue;
            auto& frames=art[gadget.name];
            for(const auto& im:gadget.imgs) {
                if(const auto* source=frameFor(im)) {
                    auto frame=*source;
                    // The background has black reserves for buttons. Composite
                    // their resting art before filtering, as on the title menu.
                    if(gadget.name=="BOD")for(const auto& button:ui.gadgets) {
                        if(button.type!=4 || button.imgs.empty())continue;
                        const int state=(button.name=="LoadGame" || button.name=="ChangeUser")?2:0;
                        if(const auto* face=frameFor(button.imgs[std::min(size_t(state),button.imgs.size()-1)]))
                            for(int y=0;y<face->height;++y)for(int x=0;x<face->width;++x) {
                                const int dx=button.x+x,dy=button.y+y;
                                if(dx<0 || dy<0 || dx>=frame.width || dy>=frame.height)continue;
                                const auto* src=&face->rgba[(size_t(y)*face->width+x)*4];
                                auto* dst=&frame.rgba[(size_t(dy)*frame.width+dx)*4];
                                for(int c=0;c<3;++c)dst[c]=uint8_t((src[c]*src[3]+dst[c]*(255-src[3])+127)/255);
                                dst[3]=255;
                            }
                    }
                    frames.push_back({tak::art::makeTexture(ren,frame.rgba,frame.width,frame.height),frame.width,frame.height});
                }else frames.push_back({});
            }
        }
        // Native BOD constructor4a89c0 replaces the short GUI state list with
        // the entire authored Story1 sequence, then4a9640 selects its page.
        if(const auto* image=ui.find("ChapterImage");image && !image->imgs.empty()) {
            auto ref=image->imgs.front();
            std::string bank=ref.gaf;
            if(bank.size()>4 && bank.substr(bank.size()-4)==".gaf")bank.resize(bank.size()-4);
            try {
                for(const auto& seq:gaf::load(vfs.read("anims/"+bank+".gaf"),guiPalette(vfs,bank)))
                    if(seq.name==ref.seq)for(const auto& frame:seq.frames)
                        chapters.push_back(art::makeTexture(ren,frame.rgba,frame.width,frame.height));
            }catch(...) {}
        }
        try{body=Font(ren,vfs,"fonts/bodfontbody.gaf",FontRole::Text);}catch(...){}
        // The CHAPTER heading is lettering: "C" (bodfontdecor) and "HAPTER" (bodfontbody)
        // are one word sized to sit over the chapter numeral, so it keeps the retail art
        // under UI FONT MODERN too; Noto's caps stand taller than its small capitals and
        // ran into the numeral. The title below stays reading text.
        try{heading=Font(ren,vfs,"fonts/bodfontbody.gaf",FontRole::Art);}catch(...){}
        try{decor=Font(ren,vfs,"fonts/bodfontdecor.gaf",FontRole::Art);}catch(...){}
        try{label=Font(ren,vfs,"fonts/b_times new roman (100b).gaf",FontRole::Text);}catch(...){}
        selectCampaign(initial);
    }
    ~Impl() {
        for(auto& [name,frames]:art)for(auto& image:frames)if(image.texture)gpuvram::destroy(image.texture);
        for(auto* texture:chapters)if(texture)gpuvram::destroy(texture);
        body.destroyGlyphs();heading.destroyGlyphs();decor.destroyGlyphs();label.destroyGlyphs();
    }
    int count() const {return camps.empty()?0:camps[size_t(tab)].count()+!camps[size_t(tab)].altFinal.empty();}
    void selectCampaign(int index) {
        tab=std::clamp(index,0,std::max(0,int(camps.size())-1));page=0;
        if(camps.empty())return;
        const auto& c=camps[size_t(tab)];
        while(page+1<c.count() && settings.missionCompleted(c.id,page))++page;
    }
    const gui::Gadget* gadget(const char* name)const{return ui.find(name);}
    SDL_FRect rect(const GuiLayout& lay,const char* name)const {
        if(auto* g=gadget(name))return lay.rect(float(g->x),float(g->y),float(g->w),float(g->h));
        return {};
    }
    static bool inside(const SDL_FRect& r,float x,float y) {
        return x>=r.x && x<r.x+r.w && y>=r.y && y<r.y+r.h;
    }
    void image(const GuiLayout& lay,const char* name,int state=0) {
        auto i=art.find(name);if(i==art.end() || i->second.empty())return;
        state=std::clamp(state,0,int(i->second.size())-1);
        const auto& image=i->second[size_t(state)];
        if(image.texture) {
            auto dst=rect(lay,name);dst.w=image.width*lay.scale;dst.h=image.height*lay.scale;
            SDL_RenderCopyF(ren,image.texture,nullptr,&dst);
        }
    }
    void text(const GuiLayout& lay,const Font& font,const std::string& value,
              SDL_FRect box,bool centered=true,float size=1,SDL_Color ink={}) {
        if(value.empty())return;
        float scale=lay.scale*size;
        float width=font.ok()?float(font.width(value,scale)):uiTextWidth(value,scale);
        if(width>box.w && width>0){scale*=box.w/width;width=box.w;}
        float top=0,height=7*scale;
        if(font.ok())font.vbounds(value,scale,top,height);
        float x=box.x+(centered?(box.w-width)*.5f:0);
        float y=box.y+(box.h-height)*.5f-top;
        // ink: MODERN's flat face in this colour (the Times art's own tan, made for dark
        // panels, is faint on the parchment); CLASSIC keeps the glyph art.
        if(font.ok() && ink.a && modernUiText())font.drawSolid(ren,value,x,y,scale,ink);
        else if(font.ok())font.draw(ren,value,x,y,scale);
        else uiText(ren,value,x,y,scale,{238,225,196,255});
    }
    void click(const char* name) {
        if(!playSound)return;
        if(const auto* g=gadget(name))if(auto sound=menuclick::soundOf(*g);!sound.empty())playSound(sound);
    }
    void choose() {
        if(camps.empty() || !count())return;
        const auto& c=camps[size_t(tab)];
        stem=page<c.count()?c.missions[size_t(page)].stem:c.altFinal;
        campaign=c.id;picked=true;
    }
};
CampaignScreen::CampaignScreen(SDL_Renderer* r,const hpi::Vfs& v,const Settings& s,int tab,
                               std::function<void(const std::string&)> sound,
                               const std::filesystem::path& install)
    :d_(std::make_unique<Impl>(r,v,s,tab,install)){
    d_->playSound=std::move(sound);
}
CampaignScreen::~CampaignScreen()=default;
bool CampaignScreen::picked()const{return d_->picked;}
const std::string& CampaignScreen::pickedStem()const{return d_->stem;}
const std::string& CampaignScreen::pickedCampaign()const{return d_->campaign;}

bool CampaignScreen::input(const SDL_Event& e,int w,int h) {
    auto& d=*d_;const GuiLayout lay(w,h);
    if(e.type==SDL_KEYDOWN) {
        const auto key=e.key.keysym.sym;
        if(key==SDLK_ESCAPE){d.click("Previous");return true;}
        if(key==SDLK_RETURN || key==SDLK_KP_ENTER){if(d.count())d.click("Play");d.choose();return d.picked;}
        if(key==SDLK_LEFT || key==SDLK_PAGEUP)d.page=std::max(0,d.page-1);
        if(key==SDLK_RIGHT || key==SDLK_PAGEDOWN)d.page=std::min(std::max(0,d.count()-1),d.page+1);
        if(key==SDLK_HOME)d.page=0;
        if(key==SDLK_END)d.page=std::max(0,d.count()-1);
        if(key==SDLK_TAB && !d.camps.empty())d.selectCampaign((d.tab+1)%int(d.camps.size()));
    }
    if(e.type==SDL_MOUSEWHEEL)d.page=std::clamp(d.page-e.wheel.y,0,std::max(0,d.count()-1));
    if(e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_RIGHT) {
        const float x=float(e.button.x),y=float(e.button.y);
        if(Impl::inside(d.rect(lay,"NextPage"),x,y))d.page=std::max(0,d.page-1);
        if(Impl::inside(d.rect(lay,"PreviousPage"),x,y))d.page=std::min(std::max(0,d.count()-1),d.page+1);
    }
    if(e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT) {
        const float x=float(e.button.x),y=float(e.button.y);
        if(Impl::inside(d.rect(lay,"Previous"),x,y)){d.click("Previous");return true;}
        if(Impl::inside(d.rect(lay,"Play"),x,y)){if(d.count())d.click("Play");d.choose();return d.picked;}
        if(Impl::inside(d.rect(lay,"PreviousPage"),x,y))d.page=std::max(0,d.page-1);
        if(Impl::inside(d.rect(lay,"NextPage"),x,y))d.page=std::min(std::max(0,d.count()-1),d.page+1);
        for(size_t i=0;i<d.camps.size();++i) {
            auto r=lay.rect(188+float(i)*266.f/float(d.camps.size()),412,266.f/float(d.camps.size()),32);
            if(Impl::inside(r,x,y))d.selectCampaign(int(i));
        }
    }
    return false;
}
void CampaignScreen::render(int w,int h) {
    auto& d=*d_;const GuiLayout lay(w,h);
    SDL_SetRenderDrawBlendMode(d.ren,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(d.ren,0,0,0,255);SDL_RenderClear(d.ren);
    d.image(lay,"BOD");
    d.image(lay,"ChangeUser",2);d.image(lay,"LoadGame",2);
    int mx=0,my=0;hidpi::mousePixels(d.ren,mx,my);
    for(const char* name:{"Previous","Play","PreviousPage","NextPage"}) {
        const bool disabled=(std::string(name)=="PreviousPage" && d.page==0) ||
            (std::string(name)=="NextPage" && d.page+1>=d.count()) ||
            (std::string(name)=="Play" && d.count()==0);
        d.image(lay,name,disabled?2:Impl::inside(d.rect(lay,name),float(mx),float(my))?1:0);
    }
    if(d.camps.empty()) {
        d.text(lay,d.label,"No campaigns found",lay.rect(100,180,440,40));return;
    }
    const auto& c=d.camps[size_t(d.tab)];
    const bool alternate=d.page>=c.count();
    const int chapter=alternate?c.count():d.page+1;
    // Retail4a9640: base chapters1..48; frame49 is unknown campaign;
    // Iron Plague and IPalt add49 to their one-based chapter number.
    int image=49;
    if(c.id=="book of darien")image=chapter;
    else if(c.id=="the iron plague")image=chapter+49;
    if(!d.chapters.empty()) {
        image=image<int(d.chapters.size())?image:0;
        const auto dst=d.rect(lay,"ChapterImage");
        if(d.chapters[size_t(image)])SDL_RenderCopyF(d.ren,d.chapters[size_t(image)],nullptr,&dst);
    }
    d.text(lay,d.decor,"C",d.rect(lay,"C"));
    d.text(lay,d.heading,"HAPTER",d.rect(lay,"Hapter"));
    d.text(lay,d.decor,std::to_string(chapter),d.rect(lay,"ChapterNumber"));
    const std::string title=alternate?c.altTitle:c.missions[size_t(d.page)].title;
    const auto box=d.rect(lay,"ChapterText");
    std::vector<std::string> lines;std::istringstream words(title);std::string word,line;
    while(words>>word) {
        const auto next=line.empty()?word:line+" "+word;
        const float width=d.body.ok()?float(d.body.width(next,lay.scale)):uiTextWidth(next,lay.scale);
        if(!line.empty() && width>box.w){lines.push_back(line);line=word;}else line=next;
    }
    if(!line.empty())lines.push_back(line);
    float height=d.body.ok()?float(d.body.height(lay.scale)):14*lay.scale;
    height=std::max(height,20*lay.scale);
    for(size_t i=0;i<lines.size();++i)d.text(lay,d.body,lines[i],
        {box.x,box.y+float(i)*height,box.w,height});
    for(size_t i=0;i<d.camps.size();++i) {
        const auto r=lay.rect(188+float(i)*266.f/float(d.camps.size()),412,266.f/float(d.camps.size()),32);
        if(int(i)==d.tab) {SDL_SetRenderDrawColor(d.ren,210,180,110,160);SDL_RenderDrawRectF(d.ren,&r);}
        d.text(lay,d.label,d.camps[i].title,r,true,1.f);
    }
    std::string help="Enter: Play    Esc: Back";
    if(d.settings.missionCompleted(c.id,alternate?kAltMission:d.page))help="Completed - "+help;
    d.text(lay,d.label,help,d.rect(lay,"HelpText"),true,1.f);
    constexpr float kSize=.8f;
    constexpr SDL_Color kInk{72,56,32,255};   // darker than the title's ink: smaller text
    if(!d.alwaysOn.empty()) {
        // Two short lines on the left page, under the chapter title; the names
        // shorten with "..." rather than shrink past legibility.
        const auto names=lay.rect(100,310,220,12);
        const auto fits=[&](const std::string& t){
            const float scale=lay.scale*kSize;
            return (d.label.ok()?float(d.label.width(t,scale)):uiTextWidth(t,scale))<=names.w;};
        std::string line=d.alwaysOn;
        while(line.size()>8 && !fits(line))line=line.substr(0,line.size()-4)+"...";
        d.text(lay,d.label,"Always-on mods:",lay.rect(100,298,220,12),true,kSize,kInk);
        d.text(lay,d.label,line,names,true,kSize,kInk);
    }
}
} // namespace tak
