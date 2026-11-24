#include "client/campaignscreen.h"

#include <algorithm>
#include <cctype>
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
#include "client/taguiview.h"
#include "gaf/gaf.h"
#include "gui/gui.h"
#include "gui/tagui.h"
#include "hpi/hpi.h"
#include "sim/sides.h"

namespace ta {
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
                    frames.push_back({ta::art::makeTexture(ren,frame.rgba,frame.width,frame.height),frame.width,frame.height});
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
// ---- Total Annihilation: the any-mission NEWGAME screen ------------------------
namespace {
std::string upperOf(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string trimmed(const std::string& s) {
    const auto a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
// "camps/Arm Campaign.tdf" -> "Arm Campaign": the name TaCampaign / loadTaCampaign take.
std::string campsStem(const std::string& file) {
    std::string n = file.substr(file.find_last_of('/') + 1);
    if (const auto dot = n.rfind('.'); dot != std::string::npos) n.resize(dot);
    return n;
}
}  // namespace

struct CampaignScreen::TaPicker {
    SDL_Renderer* ren;
    const Settings& settings;
    std::vector<Campaign> all;             // every camps/*.tdf
    std::vector<std::string> sideNames;    // sidedata order (ARM, CORE)
    std::vector<size_t> shown;             // the side's campaigns (indices into all)
    std::unique_ptr<TaGuiView> view;
    TaFont rows;                           // the mission rows (a .FNT takes a tint)
    int side = 0, camp = 0, campScroll = 0, sel = 0, scroll = 0, nextUp = -1, difficulty = 1;
    std::vector<std::string> campNames;    // the Choose Campaign rows
    bool picked = false;
    std::string stem, campaign;
    // The plates of playanygame4 (measured on the backdrop, 640x480): Choose Campaign
    // and Choose Game. NEWGAME.GUI's own Campaign list spans both (it is laid out for
    // newcampaign4x, where the one plate is the campaign list), and its GAF font fits
    // two rows in the small plate: both lists are drawn here, with the .FNT font.
    static constexpr float kCampX = 34, kCampY = 307, kCampW = 318, kCampH = 46;
    static constexpr float kMisX = 34, kMisY = 387, kMisW = 318, kMisH = 62;
    static constexpr float kStatusW = 40;   // DONE / PLAY column

    TaPicker(SDL_Renderer* r, const hpi::Vfs& vfs, const Settings& s, int initialSide)
        : ren(r), settings(s), all(loadCampaigns(vfs)) {
        for (int i = 0; i < sides::count(); ++i) sideNames.push_back(sides::at(i).name);
        if (sideNames.empty()) sideNames = {"ARM", "CORE"};
        view = std::make_unique<TaGuiView>(ren, vfs, tagui::load(vfs, "newgame"), true);
        // docs/retail-gui.md "New campaign": playanygame4 is NEWGAME's any-mission backdrop.
        view->setBackdrop("bitmaps/playanygame4.pcx");
        view->setStage("Difficulty", difficulty);
        for (const char* g : {"Campaign", "Missions", "MissionsKnob", "CampaignKnob"}) view->setVisible(g, false);
        try {
            if (vfs.has("fonts/hatt12.fnt")) rows.loadFnt(ren, vfs.read("fonts/hatt12.fnt"), SDL_Color{255, 255, 255, 255});
        } catch (...) {}
        if (!rows.height())
            try {
                rows.loadGaf(ren, vfs, "hattfont12",
                             gaf::Palette::fromBytes(vfs.read("bitmaps/playanygame4.pcx"), "playanygame4.pcx"));
            } catch (...) {}
        selectSide(std::clamp(initialSide, 0, int(sideNames.size()) - 1));
    }
    // Completion is keyed by the campaign id trimmed: settings.ini drops a key's
    // trailing blank ("core campaign - core contingency ").
    bool done(const Campaign& c, int mission) const { return settings.missionCompleted(trimmed(c.id), mission); }
    int rowH() const { return std::max(10, rows.height() + 1); }
    int visibleRows() const { return std::max(1, int(kMisH) / rowH()); }
    int campRows() const { return std::max(1, int(kCampH) / rowH()); }
    const Campaign* current() const {
        return camp >= 0 && camp < int(shown.size()) ? &all[shown[size_t(camp)]] : nullptr;
    }
    void selectSide(int s) {
        side = s;
        shown.clear();
        const std::string want = upperOf(sideNames[size_t(side)]);
        for (size_t i = 0; i < all.size(); ++i) {
            const std::string cs = upperOf(trimmed(all[i].side));
            if (cs == want || cs == "ALL") shown.push_back(i);   // 0x476a60
        }
        // 0x476a60 lists the side's campaigns sorted by name.
        std::sort(shown.begin(), shown.end(), [&](size_t a, size_t b) { return all[a].id < all[b].id; });
        campNames.clear();
        for (size_t i : shown) campNames.push_back(trimmed(campsStem(all[i].file)));
        campScroll = 0;
        view->setPressed("Arm", side == 0);
        view->setPressed("Core", side == 1);
        view->setText("SIDENAME", sideNames[size_t(side)]);
        // Not retail: open on the side's own campaign ("Core Campaign" sorts after
        // "Battle Tactics - Core ...").
        int first = 0;
        for (size_t i = 0; i < shown.size(); ++i)
            if (upperOf(all[shown[i]].id) == want + " CAMPAIGN") first = int(i);
        selectCampaign(first);
    }
    void selectCampaign(int c) {
        camp = shown.empty() ? -1 : std::clamp(c, 0, int(shown.size()) - 1);
        if (camp >= 0) {
            if (camp < campScroll) campScroll = camp;
            if (camp >= campScroll + campRows()) campScroll = camp - campRows() + 1;
        }
        nextUp = -1;
        if (const auto* cp = current())
            for (int i = 0; i < cp->count(); ++i)
                if (!done(*cp, i)) { nextUp = i; break; }
        // The next-up mission is selected; with every mission done, the first.
        sel = nextUp >= 0 ? nextUp : 0;
        scroll = 0;
        reveal();
    }
    int count() const { const auto* c = current(); return c ? c->count() : 0; }
    void reveal() {
        const int n = visibleRows();
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + n) scroll = sel - n + 1;
        scroll = std::clamp(scroll, 0, std::max(0, count() - n));
    }
    bool choose() {
        const auto* c = current();
        if (!c || sel < 0 || sel >= c->count()) return false;
        campaign = "ta:" + campsStem(c->file);
        stem = c->missions[size_t(sel)].otaFile + ":" + std::to_string(difficulty);
        picked = true;
        return true;
    }
    // A gadget the view fired (or a scripted press); true when the picker is done.
    bool hit(const std::string& name) {
        const std::string h = upperOf(name);
        if (h == "PREVMENU") return true;
        if (h == "START") return choose();
        if (h == "ARM" || h == "SIDE0") selectSide(0);
        else if ((h == "CORE" || h == "SIDE1") && sideNames.size() > 1) selectSide(1);
        else if (h == "DIFFICULTY") difficulty = view->stage("Difficulty");
        return false;
    }
    bool input(const SDL_Event& e, int w, int h) {
        const GuiLayout L(w, h);
        auto overRows = [&](float x, float y) { return L.hit(kMisX, kMisY, kMisW, kMisH, x, y); };
        auto overCamps = [&](float x, float y) { return L.hit(kCampX, kCampY, kCampW, kCampH, x, y); };
        if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT &&
            overCamps(float(e.button.x), float(e.button.y))) {
            const int row = campScroll + int((float(e.button.y) - L.py(kCampY)) / L.scale) / rowH();
            if (row >= 0 && row < int(shown.size()) && row != camp) selectCampaign(row);
            return false;
        }
        if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT &&
            overRows(float(e.button.x), float(e.button.y))) {
            const int row = scroll + int((float(e.button.y) - L.py(kMisY)) / L.scale) / rowH();
            if (row >= 0 && row < count()) {
                sel = row;
                if (e.button.clicks >= 2) return choose();   // a double-click plays it
            }
            return false;
        }
        if (e.type == SDL_MOUSEWHEEL) {
            int mx = 0, my = 0;
            hidpi::mousePixels(ren, mx, my);
            if (overRows(float(mx), float(my))) {
                scroll = std::clamp(scroll - e.wheel.y, 0, std::max(0, count() - visibleRows()));
                return false;
            }
            if (overCamps(float(mx), float(my))) {
                campScroll = std::clamp(campScroll - e.wheel.y, 0, std::max(0, int(shown.size()) - campRows()));
                return false;
            }
        }
        if (e.type == SDL_KEYDOWN) {
            const auto k = e.key.keysym.sym;
            if (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_PAGEUP || k == SDLK_PAGEDOWN || k == SDLK_HOME || k == SDLK_END) {
                const int n = count();
                if (k == SDLK_UP) sel = std::max(0, sel - 1);
                if (k == SDLK_DOWN) sel = std::min(std::max(0, n - 1), sel + 1);
                if (k == SDLK_PAGEUP) sel = std::max(0, sel - visibleRows());
                if (k == SDLK_PAGEDOWN) sel = std::min(std::max(0, n - 1), sel + visibleRows());
                if (k == SDLK_HOME) sel = 0;
                if (k == SDLK_END) sel = std::max(0, n - 1);
                reveal();
                return false;
            }
            if (k == SDLK_TAB && !shown.empty()) { selectCampaign((camp + 1) % int(shown.size())); return false; }
        }
        const std::string fired = view->handle(e, L);
        return !fired.empty() && hit(fired);
    }
    void render(int w, int h) {
        const GuiLayout L(w, h);
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        view->draw(L);
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        const SDL_Color plain{220, 220, 220, 255}, doneInk{120, 220, 120, 255}, nextInk{255, 232, 64, 255};
        const int ih = rowH();
        // A thin bar at a plate's right edge when its list runs past it.
        auto scrollBar = [&](float x, float y, float h, int first, int rows, int total) {
            if (total <= rows) return;
            const SDL_FRect track = L.rect(x - 4, y, 3, h);
            SDL_SetRenderDrawColor(ren, 60, 70, 50, 200);
            SDL_RenderFillRectF(ren, &track);
            const SDL_FRect knob = L.rect(x - 4, y + h * float(first) / float(total), 3, h * float(rows) / float(total));
            SDL_SetRenderDrawColor(ren, 170, 190, 150, 230);
            SDL_RenderFillRectF(ren, &knob);
        };
        for (int r = 0; r < campRows() && campScroll + r < int(campNames.size()); ++r) {
            const int i = campScroll + r;
            const float y = kCampY + float(r * ih);
            if (i == camp) {
                const SDL_FRect hl = L.rect(kCampX, y, kCampW - 6, float(ih));
                SDL_SetRenderDrawColor(ren, 60, 60, 160, 160);
                SDL_RenderFillRectF(ren, &hl);
            }
            // A campaign with every mission done reads DONE, like its missions.
            const Campaign& cp = all[shown[size_t(i)]];
            int won = 0;
            for (int m = 0; m < cp.count(); ++m) won += done(cp, m);
            rows.draw(ren, L, kCampX + 3, y + 1, campNames[size_t(i)], plain);
            if (won == cp.count() && won > 0)
                rows.draw(ren, L, kCampX + kCampW - 10 - float(rows.width("DONE")), y + 1, "DONE", doneInk);
        }
        scrollBar(kCampX + kCampW, kCampY + 1, kCampH - 2, campScroll, campRows(), int(campNames.size()));
        if (campNames.size() > size_t(campRows())) {
            const std::string where = std::to_string(camp + 1) + " of " + std::to_string(campNames.size());
            rows.draw(ren, L, kCampX + kCampW - 10 - float(rows.width(where)), 290, where, plain);
        }
        const auto* c = current();
        if (!c) {
            rows.draw(ren, L, kMisX + 4, kMisY + 2, "No campaign for this side.", SDL_Color{255, 220, 120, 255});
            return;
        }
        const int n = visibleRows();
        for (int r = 0; r < n && scroll + r < c->count(); ++r) {
            const int i = scroll + r;
            const float y = kMisY + float(r * ih);
            const bool won = done(*c, i);
            if (i == sel) {   // the selected row: TaGuiView's list highlight
                const SDL_FRect hl = L.rect(kMisX, y, kMisW - 6, float(ih));
                SDL_SetRenderDrawColor(ren, 60, 60, 160, 160);
                SDL_RenderFillRectF(ren, &hl);
            } else if (i == nextUp) {   // the next-up mission, when not selected
                const SDL_FRect hl = L.rect(kMisX, y, kMisW - 6, float(ih));
                SDL_SetRenderDrawColor(ren, 120, 100, 20, 110);
                SDL_RenderFillRectF(ren, &hl);
            }
            const auto& m = c->missions[size_t(i)];
            std::string title = m.title.empty() ? trimmed(m.stem) : m.title;
            const float room = kMisW - kStatusW - 14;
            if (float(rows.width(title)) > room) {
                while (title.size() > 1 && float(rows.width(title + "...")) > room) title.pop_back();
                title += "...";
            }
            rows.draw(ren, L, kMisX + 3, y + 1, title, i == nextUp ? nextInk : plain);
            const char* tag = won ? "DONE" : i == nextUp ? "PLAY" : "";
            if (*tag) rows.draw(ren, L, kMisX + kMisW - 10 - float(rows.width(tag)), y + 1, tag, won ? doneInk : nextInk);
        }
        // How far the campaign is: beside the Choose Game caption.
        int doneCount = 0;
        for (int i = 0; i < c->count(); ++i) doneCount += done(*c, i);
        const std::string tally = std::to_string(doneCount) + " of " + std::to_string(c->count()) + " done";
        rows.draw(ren, L, kMisX + kMisW - 10 - float(rows.width(tally)), 368, tally, doneCount ? doneInk : plain);
        scrollBar(kMisX + kMisW, kMisY + 1, kMisH - 2, scroll, n, c->count());
    }
};

CampaignScreen::CampaignScreen(SDL_Renderer* r,const hpi::Vfs& v,const Settings& s,int tab,
                               std::function<void(const std::string&)> sound,
                               const std::filesystem::path& install)
{
    // Total Annihilation data: TA's own screen (no Kingdoms book art to draw).
    if(!v.has("guis/bod.gui") && v.has("guis/newgame.gui"))ta_=std::make_unique<TaPicker>(r,v,s,tab);
    else {d_=std::make_unique<Impl>(r,v,s,tab,install);d_->playSound=std::move(sound);}
}
CampaignScreen::~CampaignScreen()=default;
bool CampaignScreen::picked()const{return ta_?ta_->picked:d_->picked;}
const std::string& CampaignScreen::pickedStem()const{return ta_?ta_->stem:d_->stem;}
const std::string& CampaignScreen::pickedCampaign()const{return ta_?ta_->campaign:d_->campaign;}
int CampaignScreen::pickedMission()const{return ta_?ta_->sel:0;}
int CampaignScreen::pickedSide()const{return ta_?ta_->side:0;}
int CampaignScreen::pickedDifficulty()const{return ta_?ta_->difficulty:1;}
void CampaignScreen::setDifficulty(int stage) {
    if(!ta_)return;
    ta_->difficulty=std::clamp(stage,0,2);ta_->view->setStage("Difficulty",ta_->difficulty);
}
TaGuiView* CampaignScreen::taView(){return ta_?ta_->view.get():nullptr;}
bool CampaignScreen::press(const std::string& gadget) {
    if(!ta_)return false;
    const std::string fired=ta_->view->activate(gadget);
    return !fired.empty() && ta_->hit(fired);
}

bool CampaignScreen::input(const SDL_Event& e,int w,int h) {
    if(ta_)return ta_->input(e,w,h);
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
    if(ta_){ta_->render(w,h);return;}
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
} // namespace ta
