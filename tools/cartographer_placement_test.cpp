// Cartographer placement and robustness tests. Each run drives the real editor
// (cart::runEditor) with synthetic SDL input pushed from the frame hook: the
// events of one frame are handled by the real loop during the next, so every
// placement goes through the same palette-click and canvas-click path a user's
// does. The attached Editor is only read back to check the document.
//
//   cartographer_placement_test <data> starts [world...]
//       Regression: switching to the STARTS browser (or changing the start list
//       while it shows) crashed -- the browser drew the previous tool's items.
//       Blank, generated, New-dialog and shipped maps of every world.
//   ... rescan            the browser after its library is rescanned (a Maps/
//                         archive's features vanish when a map opens from disk)
//   ... terrain [world...]  every section: browser pick, stamp by click, undo, redo
//   ... features [world...] every feature: pick, place, undo, redo, erase, undo
//   ... units [world...]    every unit type, on terrain the game accepts
//   ... slots [world...]    start slots 1-8 (a ninth is refused) and a region
//   ... feature-ops [world...]  every feature operation on every feature: select,
//                         shift-select, drag, box select, properties, group drag,
//                         copy/paste/duplicate/cut/delete, live symmetry (all kinds),
//                         Symmetrize, stamps with objects protected or not, Clear
//                         Area, terrain clipboard, resize crop/grow, overlays, Check
//                         Map, Test Map's snapshot, cross-world paste, a generated
//                         map's scatter and a dense field of 10k-58k features
//   ... blocked           the BLOCKED tool on a New Map: overlay, click, brush sizes,
//                         gap-free strokes past a feature, Shift+drag rectangle,
//                         Erase, undo/redo, live symmetry, [ ] and B, save/reopen
//                         with the minimaps unchanged
//   ... smoke [world...]  every tool x mode, browser tab, category, terrain group,
//                         faction, menu row (but Test Map) and shortcut, the
//                         scripting buttons, regions list, Use Only, then Exit
// Each placement mode then saves the map, reopens it through Open and compares.
// Every frame checks that the browser lists only items of the current list.
// Without data every mode exits 77 (skipped).

#include "cartographer/editor.h"
#include "cartographer/document.h"
#include "cartographer/editorstate.h"
#include "hpi/hpi.h"
#include "net/mappackage.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"
#include "tdf/tdf.h"
#include "tnt/ota.h"
#include "util/png.h"

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "util/u8path.h"

namespace fs=std::filesystem;
using Editor=cart::Editor;
using cart::lowerText;

namespace {

std::vector<std::string> failures;
bool check(bool ok,const std::string& what) {
    if(!ok) {failures.push_back(what);std::cerr<<"FAIL: "<<what<<'\n';}
    return ok;
}

const std::vector<std::string> kWorlds={"aramon","veruna","taros","zhon","creon"};

// Synthetic input in the editor's logical coordinates (scaled like the real
// window: the loop divides event positions by kUIScale).
struct Input {
    SDL_Window* window=nullptr;SDL_Renderer* renderer=nullptr;
    float scale() const {float sx=1,sy=1;SDL_RenderGetScale(renderer,&sx,&sy);return sx;}
    int physical(int v) const {return int(std::ceil(v*scale()));}
    void button(Uint32 type,int x,int y,Uint8 which,Uint8 clicks) const {
        SDL_Event e{};e.type=type;e.button.button=which;e.button.clicks=clicks;
        e.button.state=type==SDL_MOUSEBUTTONDOWN?SDL_PRESSED:SDL_RELEASED;
        e.button.x=physical(x);e.button.y=physical(y);SDL_PushEvent(&e);
    }
    void click(int x,int y,Uint8 which=SDL_BUTTON_LEFT,Uint8 clicks=1) const {
        button(SDL_MOUSEBUTTONDOWN,x,y,which,clicks);button(SDL_MOUSEBUTTONUP,x,y,which,clicks);
    }
    void motion(int x,int y,Uint32 state) const {
        SDL_Event e{};e.type=SDL_MOUSEMOTION;e.motion.state=state;e.motion.x=physical(x);e.motion.y=physical(y);SDL_PushEvent(&e);
    }
    void drag(int x,int y,int tx,int ty) const {
        button(SDL_MOUSEBUTTONDOWN,x,y,SDL_BUTTON_LEFT,1);motion((x+tx)/2,(y+ty)/2,SDL_BUTTON_LMASK);
        motion(tx,ty,SDL_BUTTON_LMASK);button(SDL_MOUSEBUTTONUP,tx,ty,SDL_BUTTON_LEFT,1);
    }
    void key(SDL_Keycode k,Uint16 mod=0) const {
        SDL_Event e{};e.type=SDL_KEYDOWN;e.key.state=SDL_PRESSED;e.key.keysym.sym=k;e.key.keysym.mod=mod;SDL_PushEvent(&e);
        e.type=SDL_KEYUP;e.key.state=SDL_RELEASED;SDL_PushEvent(&e);
    }
    void text(const std::string& value) const {
        for(size_t i=0;i<value.size();i+=20) {
            SDL_Event e{};e.type=SDL_TEXTINPUT;const auto part=value.substr(i,20);SDL_strlcpy(e.text.text,part.c_str(),sizeof(e.text.text));SDL_PushEvent(&e);
        }
    }
    // Move the real pointer (paste targets and wheel routing read SDL_GetMouseState).
    void pointer(int x,int y) const {SDL_WarpMouseInWindow(window,physical(x),physical(y));}
};

// With TAK_CARTO_CAPTURE=<folder>, a PNG of the frame just drawn (for review).
void capture(const Input& in,const std::string& name) {
    const char* folder=std::getenv("TAK_CARTO_CAPTURE");if(!folder || !*folder)return;
    int w=0,h=0;SDL_GetRendererOutputSize(in.renderer,&w,&h);std::vector<uint8_t> pixels(size_t(w)*h*4);
    if(SDL_RenderReadPixels(in.renderer,nullptr,SDL_PIXELFORMAT_RGBA32,pixels.data(),w*4)==0)tak::png::write((fs::path(folder)/(name+".png")).string(),w,h,pixels);
}

// Toolbar and canvas geometry, from the editor's own layout constants.
void tool(const Input& in,Editor::Tool t) {in.click(Editor::kToolX+int(t)*Editor::kToolW+30,32);}
void mode(const Input& in,Editor::EditMode m) {in.click(Editor::kModeX+int(m)*Editor::kModeW+30,32);}
void regions(const Input& in) {in.click(Editor::kRegionsX+Editor::kRegionsW/2,32);}
SDL_Point cellScreen(const Editor& e,float wx,float wz) {   // floor: at 1/16 a cell is one pixel
    return {Editor::kPaletteW+int(std::floor((wx-e.mapView.offX())*e.mapView.zoom())),Editor::kMenuH+int(std::floor((wz-e.mapView.offY())*e.mapView.zoom()))};
}
// Centre the 100% view on a world point, at a canvas spot clear of the minimap.
SDL_Point focus(Editor& e,float wx,float wz) {
    e.mapView.setZoom(1);e.mapView.setOffset(wx-160,wz-160);
    return cellScreen(e,wx,wz);
}
// A browser point whose row is `row`, found through the editor's own hit test.
std::optional<SDL_Point> paletteRow(const Editor& e,int row) {
    for(int y=Editor::kPaletteTop+2;y<e.h-Editor::kStatusH-2;y+=2)
        for(int x=6;x<Editor::kPaletteW-6;x+=4)if(e.paletteRowAt(x,y)==row)return SDL_Point{x,y};
    return std::nullopt;
}

// The browser must only ever list items of the current tool's source list.
std::string browserProblem(const Editor& e) {
    const size_t count=e.tool==Editor::UNITS?e.unitTypes.size():e.tool==Editor::FEATURES?e.features.list().size():
        e.tool==Editor::STARTS?e.scenario.starts.size():e.tool==Editor::BLOCKED?cart::kBlockedBrushSizes.size():e.sections.list().size();
    for(const int i:e.paletteItems)if(i<0 || size_t(i)>=count)
        return std::string(Editor::kToolNames[int(e.tool)])+" browser lists item "+std::to_string(i)+" of "+std::to_string(count);
    if(e.tool==Editor::TERRAIN)for(const auto& variants:e.paletteVariants)for(const int i:variants)if(i<0 || size_t(i)>=e.sections.list().size())   // only the terrain browser reads them
        return "terrain browser variant "+std::to_string(i)+" of "+std::to_string(e.sections.list().size());
    if(e.selected>=int(e.sections.list().size()))return "selected section out of range";
    if(e.selectedFeat>=int(e.features.list().size()))return "selected feature out of range";
    if(e.selectedType>=int(e.unitTypes.size()))return "selected unit type out of range";
    if(e.blockedBrush<0 || e.blockedBrush>=int(cart::kBlockedBrushSizes.size()))return "blocked brush out of range";
    return {};
}

std::string tempRoot(const std::string& tag) {
    const char* base=std::getenv("TMPDIR");
    const auto root=fs::path(base && *base?base:fs::temp_directory_path().string())/("tak-carto-"+tag+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);return root.string();
}
// Point every editor profile lookup at a private folder (see cartographer_ui_test).
void isolateProfile(const std::string& root) {
    SDL_setenv("XDG_CONFIG_HOME",root.c_str(),1);SDL_setenv("XDG_DATA_HOME",root.c_str(),1);
    const auto profile=fs::path(root)/"TAKengine"/"Cartographer";
    std::error_code ec;fs::create_directories(profile,ec);
    SDL_setenv("TAK_CARTOGRAPHER_PROFILE",profile.string().c_str(),1);
}

// One editor session: `frame` runs after every drawn frame and returns false to
// end the session. Each frame also checks the browser invariant.
bool drive(std::vector<std::string> args,const std::function<bool(Editor&,const Input&)>& frame,int seconds,const std::string& label) {
    Editor* editor=nullptr;cart::EditorHooks hooks;hooks.attached=[&](Editor& e){editor=&e;};
    std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    const auto began=std::chrono::steady_clock::now();bool browserOk=true;
    const int result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer* renderer,int) {
        if(std::chrono::steady_clock::now()-began>std::chrono::seconds(seconds)) {
            std::cerr<<"FAIL: "<<label<<" timed out\n";std::_Exit(3);
        }
        if(const auto problem=browserProblem(*editor);!problem.empty() && browserOk)browserOk=check(false,label+": "+problem);
        const Input in{window,renderer};
        if(!frame(*editor,in))editor->running=false;
    },hooks);
    return check(result==0,label+": editor exit code "+std::to_string(result)) && browserOk;
}

// One map of each world in the install: the alphabetically first shipped
// (maps/) one, else the first downloaded one.
std::map<std::string,std::string> mapsByWorld(const std::string& data) {
    std::map<std::string,std::pair<bool,std::string>> found;
    const auto vfs=tak::hpi::mountRetailRoot(tak::u8path(data));
    for(const auto& [name,path]:tak::hpi::listMaps(vfs)) {
        try {
            const auto bytes=vfs.read(path.substr(0,path.rfind('.'))+".ota");
            auto world=tak::tnt::Scenario::parse(std::string(bytes.begin(),bytes.end())).kingdom;
            for(char& c:world)c=char(std::tolower((unsigned char)c));
            const std::pair<bool,std::string> rank{!path.starts_with("maps/"),name};
            if(std::find(kWorlds.begin(),kWorlds.end(),world)!=kWorlds.end() && (!found.count(world) || rank<found[world]))found[world]=rank;
        } catch(const std::exception&) {}
    }
    std::map<std::string,std::string> names;for(const auto& [world,rank]:found)names[world]=rank.second;
    return names;
}

// ---- starts: the crash regression -------------------------------------------

// Tool switches into STARTS and start edits while the STARTS browser is up,
// on one document. `dialogWorld` first creates the map with the New dialog (File >
// New map, Ctrl+N, from the empty editor a bare launch opens).
bool startsCase(const std::string& data,const std::string& label,std::vector<std::string> args,const std::string& dialogWorld) {
    const auto root=tempRoot("starts");isolateProfile(root);
    args.insert(args.begin(),"cartographer");args.insert(args.end(),{"--data",data,"--out",root});
    static constexpr Editor::Tool kWalk[]={Editor::FEATURES,Editor::TERRAIN,Editor::UNITS,Editor::TERRAIN,Editor::STARTS,
        Editor::FEATURES,Editor::STARTS,Editor::UNITS,Editor::STARTS};
    int stage=dialogWorld.empty()?1:-1;size_t before=0,walk=0;int placed=-1,cellX=0,cellZ=0;bool ok=true;
    auto expect=[&](bool value,const std::string& what){ok&=check(value,label+": "+what);};
    const bool ran=drive(args,[&](Editor& e,const Input& in) {
        auto& starts=e.scenario.starts;
        switch(stage++) {
        case -1:   // the bare launch: no document (past the first-run welcome), then Ctrl+N
            if(e.modal==Editor::M_MESSAGE) {in.key(SDLK_RETURN);--stage;return true;}
            expect(e.modal==Editor::M_NONE && e.noDocument && !e.dirty,"a bare launch opens with no document");
            in.key(SDLK_n,KMOD_CTRL);return true;
        case 0: {   // New Map dialog: WORLD is the fourth field, cycled with the arrow keys
            expect(e.modal==Editor::M_NEW,"New Map dialog opens");
            const auto& worlds=e.kWorldOpts;
            const int from=int(std::find(worlds.begin(),worlds.end(),e.mf[3])-worlds.begin());
            const int to=int(std::find(worlds.begin(),worlds.end(),dialogWorld)-worlds.begin());
            for(int i=0;i<3;++i)in.key(SDLK_TAB);
            for(int i=0;i<(to-from+int(worlds.size()))%int(worlds.size());++i)in.key(SDLK_RIGHT);
            in.key(SDLK_RETURN);return true;
        }
        case 1:   // one tool switch per frame, so each browser is drawn right after its switch
            if(!dialogWorld.empty() && walk==0)expect(e.modal==Editor::M_NONE && e.world==dialogWorld,"New Map dialog creates a "+dialogWorld+" map");
            if(walk>0)expect(e.tool==kWalk[walk-1],"toolbar switches tool");
            before=starts.size();
            if(walk<std::size(kWalk)) {tool(in,kWalk[walk++]);--stage;}
            return true;
        case 2:   // and every switch into STARTS within one frame
            expect(e.tool==Editor::STARTS,"STARTS tool chosen");
            capture(in,"starts "+label);
            for(const auto from:{Editor::TERRAIN,Editor::FEATURES,Editor::UNITS}) {tool(in,from);tool(in,Editor::STARTS);}
            for(int i=0;i<Editor::kToolCount;++i)in.key(SDLK_TAB);   // Tab cycles back round to STARTS
            in.key(SDLK_l,KMOD_CTRL);in.key(SDLK_l,KMOD_CTRL);tool(in,Editor::STARTS);
            return true;
        case 3: {   // place a new start on an empty cell away from the others
            expect(e.tool==Editor::STARTS,"STARTS tool after switching round");
            if(!cart::nextStartNumber(starts) && !starts.empty()) {   // all eight used: erase the last first
                const auto p=focus(e,starts.back().xpos*16+8.f,starts.back().zpos*16+8.f);mode(in,Editor::MODE_ERASE);in.click(p.x,p.y);
                before=starts.size()-1;--stage;return true;
            }
            const auto& map=e.mapView.map();
            for(int z=map.height/2,found=0;!found && z<map.height;z+=3)for(int x=map.width/2;x<map.width;x+=3) {
                bool clear=true;for(const auto& s:starts)clear&=std::abs(s.xpos-x)>2 || std::abs(s.zpos-z)>2;
                if(clear) {cellX=x;cellZ=z;found=1;break;}
            }
            const auto p=focus(e,cellX*16+8.f,cellZ*16+8.f);mode(in,Editor::MODE_PLACE);in.click(p.x,p.y);return true;
        }
        case 4: {
            expect(starts.size()==before+1,"start placed by a canvas click");
            for(size_t i=0;i<starts.size();++i)if(starts[i].xpos==cellX && starts[i].zpos==cellZ)placed=int(i);
            expect(placed>=0,"new start at the clicked cell");
            // Click its browser row: the view centres on it.
            const auto row=std::find(e.paletteItems.begin(),e.paletteItems.end(),placed)-e.paletteItems.begin();
            const auto point=paletteRow(e,int(row));
            expect(point.has_value(),"new start listed in the STARTS browser");
            if(point) {e.mapView.setOffset(0,0);in.click(point->x,point->y);}
            return true;
        }
        case 5: {
            expect(std::abs(e.mapView.offX()-(cellX*16-e.canvasW/(2*e.mapView.zoom())))<1,"STARTS browser row centres the view on the start");
            const auto p=focus(e,cellX*16+8.f,cellZ*16+8.f);mode(in,Editor::MODE_ERASE);in.click(p.x,p.y);return true;
        }
        case 6: expect(starts.size()==before,"erase removes the start while the STARTS browser lists it");in.key(SDLK_z,KMOD_CTRL);return true;
        case 7: expect(starts.size()==before+1,"undo restores the erased start");in.key(SDLK_y,KMOD_CTRL);return true;
        case 8:
            expect(starts.size()==before,"redo erases it again");
            if(!starts.empty()) {   // the last browser row is the one a shrinking list loses
                const auto p=focus(e,starts.back().xpos*16+8.f,starts.back().zpos*16+8.f);in.click(p.x,p.y);
            }
            return true;
        case 9:   // back to the placed start (one more undo when an original was erased)
            expect(starts.size()==(before?before-1:0),"erase an existing start");
            in.key(SDLK_z,KMOD_CTRL);if(before)in.key(SDLK_z,KMOD_CTRL);return true;
        case 10: {   // Select mode: drag the placed start two cells right
            expect(starts.size()==before+1,"undo across start edits");
            if(starts.size()!=before+1)return false;
            for(size_t i=0;i<starts.size();++i)if(starts[i].xpos==cellX && starts[i].zpos==cellZ)placed=int(i);
            const auto p=focus(e,cellX*16+8.f,cellZ*16+8.f);mode(in,Editor::MODE_SELECT);in.drag(p.x,p.y,p.x+32,p.y);return true;
        }
        case 11:
            expect(placed>=0 && placed<int(starts.size()) && starts[placed].xpos==cellX+2 && starts[placed].zpos==cellZ,"Select mode drags a start");
            tool(in,Editor::TERRAIN);tool(in,Editor::STARTS);in.key(SDLK_z,KMOD_CTRL);return true;
        default: return false;
        }
    },120,label);
    expect(stage>12,"script finished (stopped at stage "+std::to_string(stage)+")");
    std::error_code ec;fs::remove_all(root,ec);
    if(ran && ok)std::cout<<"ok: "<<label<<'\n';
    return ran && ok;
}

int startsWorkflow(const std::string& data,std::vector<std::string> worlds) {
    if(worlds.empty())worlds=kWorlds;
    const auto shipped=mapsByWorld(data);
    for(const auto& world:worlds) {
        startsCase(data,world+" blank map",{"--new","8x8","--world",world},"");
        startsCase(data,world+" generated map",{"--new","8x8","--world",world,"--random"},"");
        startsCase(data,world+" New Map dialog",{},world);
        if(const auto found=shipped.find(world);found!=shipped.end())startsCase(data,world+" map '"+found->second+"'",{found->second},"");
        else std::cout<<"note: no shipped "<<world<<" map in this install\n";
    }
    if(!failures.empty())return 1;
    std::cout<<"PASS: STARTS browser and start edits on blank, generated, New-dialog and shipped maps of "<<worlds.size()<<" worlds\n";
    return 0;
}

// ---- every browser item, placed through the real click path -----------------

// Choose `index` in the current tool's browser the way a user does: search for
// it (Ctrl+F) in one frame, then click its row (terrain: until the wanted
// variant of the piece is selected), scrolling the list first when the row is
// out of view. click(): 1 = clicks queued, 0 = scrolled (retry next frame),
// -1 = the search does not list the item.
struct BrowserPick {
    int index=-1,scrolls=0;
    void search(const Input& in,int item,const std::string& query) {
        index=item;scrolls=0;in.key(SDLK_f,KMOD_CTRL);in.text(query);in.key(SDLK_ESCAPE);
    }
    int click(const Editor& e,const Input& in) {
        int row=-1,clicks=1;
        if(e.tool==Editor::TERRAIN) {
            for(size_t r=0;r<e.paletteVariants.size() && row<0;++r) {
                const auto& v=e.paletteVariants[r];
                if(std::find(v.begin(),v.end(),index)==v.end())continue;
                // Replay handlePaletteClick: a click selects the shown variant, or steps the selected piece.
                int selected=e.selected,shown=e.paletteItems[r];row=int(r);clicks=0;
                while(selected!=index && clicks<=int(v.size())) {
                    if(shown==selected && v.size()>1) {const auto at=std::find(v.begin(),v.end(),selected);selected=shown=at+1==v.end()?v.front():*(at+1);}
                    else selected=shown;
                    ++clicks;
                }
            }
        } else if(const auto at=std::find(e.paletteItems.begin(),e.paletteItems.end(),index);at!=e.paletteItems.end())row=int(at-e.paletteItems.begin());
        if(row<0)return -1;
        const auto point=paletteRow(e,row);
        if(!point) {   // out of view: wheel over the browser towards it, a few notches a frame
            if(++scrolls>400)return -1;
            in.pointer(Editor::kPaletteW/2,Editor::kPaletteTop+40);
            const bool up=row<e.paletteRowAt(10,Editor::kPaletteTop+4);
            for(int i=0;i<8;++i) {SDL_Event wheel{};wheel.type=SDL_MOUSEWHEEL;wheel.wheel.y=up?1:-1;SDL_PushEvent(&wheel);}
            return 0;
        }
        for(int i=0;i<clicks;++i)in.click(point->x,point->y);
        return 1;
    }
};

// The first difference between two documents' terrain planes; features compare
// by name (a reload keeps 63 bytes of each name, like the game).
std::string planeDifference(const tak::tnt::Map& a,const tak::tnt::Map& b) {
    if(a.width!=b.width || a.height!=b.height || a.blocksX!=b.blocksX || a.blocksY!=b.blocksY)return "map size";
    if(a.tileKeys!=b.tileKeys || a.tileCols!=b.tileCols || a.tileRows!=b.tileRows)return "tile plane";
    if(a.heights!=b.heights)return "heights";
    if(a.features.size()!=b.features.size())return "feature plane size";
    if(a.features==b.features && a.featureNames==b.featureNames)return {};
    auto name=[](const tak::tnt::Map& m,size_t i) {
        const auto v=m.features[i];
        return v>=0xFFFA?"#"+std::to_string(v):v<m.featureNames.size()?m.featureNames[v].substr(0,63):std::string("?");
    };
    for(size_t i=0;i<a.features.size();++i)if(name(a,i)!=name(b,i))
        return "feature at cell "+std::to_string(i%a.width)+","+std::to_string(i/a.width)+": "+name(a,i)+" vs "+name(b,i);
    return {};
}

// An engine world over the document's terrain and features (Check Map's view).
std::unique_ptr<tak::sim::World> engineWorld(const Editor& e) {
    const auto& map=e.mapView.map();auto world=std::make_unique<tak::sim::World>();
    world->setTerrain(map.heights,map.width,map.height,map.seaLevel,&map.features);
    world->buildNavClasses(e.unitRegistry);
    tak::sim::registerMapFeatures(*world,map,e.vfs,&e.unitRegistry);
    return world;
}
// What the game makes of the document: unreadable terrain images and feature
// names it cannot resolve (Check Map's own predicates).
std::vector<std::string> engineProblems(const Editor& e) {
    std::vector<std::string> out;
    const auto& map=e.mapView.map();
    for(const auto& issue:cart::validateTerrainResources(map,e.vfs))out.push_back(issue.message);
    const auto world=engineWorld(e);const auto& types=world->mapPlacementTypes();
    std::set<uint16_t> seen;
    for(const auto v:map.features)if(v<0xFFFA && seen.insert(v).second && (v>=types.size() || types[v].name.empty()))
        out.push_back("the game cannot resolve feature '"+(v<map.featureNames.size()?map.featureNames[v]:std::to_string(v))+"'");
    return out;
}

struct Tally {
    int listed=0,placed=0,undone=0,redone=0,erased=0,restored=0,reloaded=0,refused=0;
    std::map<std::string,int> kinds;   // per-category breakdown for the report
    void print(const std::string& what) const {
        std::cout<<what<<": "<<listed<<" listed, "<<placed<<" placed, "<<undone<<" undone, "<<redone<<" redone";
        if(erased)std::cout<<", "<<erased<<" erased, "<<restored<<" restored";
        std::cout<<", "<<reloaded<<" survived save/reopen";
        if(refused)std::cout<<", "<<refused<<" refused";
        std::cout<<'\n';
        if(!kinds.empty()) {std::cout<<"   ";for(const auto& [kind,count]:kinds)std::cout<<' '<<kind<<'='<<count;std::cout<<'\n';}
    }
};

// Save with Ctrl+S, dismiss the confirmation, reopen the .kmp through Open:
// queue() in one frame; the next frame sees the reopened document.
void saveAndReopen(const Editor& e,const Input& in,const std::string&,const std::string& root) {
    in.key(SDLK_s,KMOD_CTRL);in.key(SDLK_RETURN);
    in.key(SDLK_o,KMOD_CTRL);in.text((fs::path(root)/(e.mapName+".kmp")).string());in.key(SDLK_RETURN);
}
bool reopened(const Editor& e,const std::string& label) {
    return check(e.modal==Editor::M_NONE && !e.dirty && e.mapPath.starts_with("kmap/"),label+": saved map reopens clean (modal "+std::to_string(int(e.modal))+", "+e.mapPath+")");
}

// One session per batch of sections on a fresh 16x16-unit map of `world`: each
// section is stamped by a canvas click in its own slot, undone and redone; the
// filled map is saved, reopened and compared. Returns the next section index.
size_t terrainSession(const std::string& data,const std::string& world,size_t first,Tally& tally) {
    const auto root=tempRoot("terrain");isolateProfile(root);
    enum {START,SEARCH,PICK,PLACED,UNDONE,REDONE,REOPENED} phase=START;
    size_t item=first;BrowserPick pick;tak::tnt::Map expected,prior;
    int slot=0,slotsX=0,slots=0,slotBlocks=1;const tak::tnt::Map* prefab=nullptr;
    const std::string label=world+" terrain";
    drive({"cartographer","--new","16x16","--world",world,"--data",data,"--out",root},[&](Editor& e,const Input& in) {
        const auto& sections=e.sections.list();
        auto fail=[&](const std::string& what) {check(false,label+" '"+sections[item].category+"/"+sections[item].name+"': "+what);};
        for(;;)switch(phase) {
        case START: {
            if(first==0)tally.listed+=int(sections.size());
            // Slots fit the largest section; stampAtMouse snaps each to its own size.
            for(const auto& s:sections)if(const auto* m=e.sections.load(e.vfs,s.path))slotBlocks=std::max({slotBlocks,m->blocksX,m->blocksY});
            slotsX=e.mapView.map().blocksX/slotBlocks;slots=slotsX*(e.mapView.map().blocksY/slotBlocks);
            expected=e.mapView.map();tool(in,Editor::TERRAIN);mode(in,Editor::MODE_PLACE);phase=SEARCH;return true;
        }
        case SEARCH:
            if(item>=sections.size() || slot>=slots) {
                if(item==first) return false;
                for(const auto& problem:engineProblems(e))check(false,label+": "+problem);   // tile images decode, prefab features resolve
                saveAndReopen(e,in,label,root);phase=REOPENED;return true;
            }
            if(const auto d=planeDifference(e.mapView.map(),expected);!d.empty()) {fail("document differs before placing: "+d);return false;}
            prefab=e.sections.load(e.vfs,sections[item].path);
            if(!prefab || prefab->blocksX<=0 || prefab->blocksY<=0) {fail("prefab does not load");++item;continue;}
            ++tally.kinds[std::to_string(prefab->blocksX)+"x"+std::to_string(prefab->blocksY)+"-blocks"];
            if(!e.vfs.has(sections[item].path,true))++tally.kinds["from-downloaded-map"];
            pick.search(in,int(item),sections[item].name);phase=PICK;return true;
        case PICK: {
            const int clicked=pick.click(e,in);
            if(!clicked)return true;
            if(clicked<0) {fail("browser search does not list it");++item;phase=SEARCH;continue;}
            const int sx=(slot%slotsX)*slotBlocks,sz=(slot/slotsX)*slotBlocks;++slot;
            const int ox=sx/prefab->blocksX*prefab->blocksX,oz=sz/prefab->blocksY*prefab->blocksY;   // the click snaps here
            const auto p=focus(e,(ox+prefab->blocksX/2)*32+16.f,(oz+prefab->blocksY/2)*32+16.f);
            in.click(p.x,p.y);
            prior=expected;cart::stampSection(expected,*prefab,ox,oz,e.stampLayers);
            phase=PLACED;return true;
        }
        case PLACED:
            if(!check(e.selected==int(item),label+": browser click selects '"+sections[item].name+"'")) {return false;}
            if(const auto d=planeDifference(e.mapView.map(),expected);!d.empty()) {fail("click did not stamp it at the clicked block: "+d);return false;}
            ++tally.placed;
            if(planeDifference(prior,expected).empty()) {   // the blank map's own fill: no change, no undo step
                ++tally.kinds["same-as-blank-fill"];++tally.undone;++tally.redone;++item;phase=SEARCH;continue;
            }
            in.key(SDLK_z,KMOD_CTRL);phase=UNDONE;return true;
        case UNDONE:
            if(const auto d=planeDifference(e.mapView.map(),prior);!d.empty()) {fail("undo: "+d);return false;}
            ++tally.undone;in.key(SDLK_y,KMOD_CTRL);phase=REDONE;return true;
        case REDONE:
            if(const auto d=planeDifference(e.mapView.map(),expected);!d.empty()) {fail("redo: "+d);return false;}
            ++tally.redone;++item;phase=SEARCH;continue;
        case REOPENED:
            if(reopened(e,label)) {
                if(const auto d=planeDifference(e.mapView.map(),expected);!d.empty())check(false,label+": saved and reopened map differs: "+d);
                else tally.reloaded+=int(item-first);
            }
            return false;
        }
    },900,label);
    std::error_code ec;fs::remove_all(root,ec);
    return item;
}

int terrainWorkflow(const std::string& data,std::vector<std::string> worlds) {
    if(worlds.empty())worlds=kWorlds;
    for(const auto& world:worlds) {
        Tally tally;size_t next=0;
        for(size_t done=SIZE_MAX;next!=done;) {done=next;next=terrainSession(data,world,next,tally);if(!failures.empty())break;}
        tally.print(world+" terrain sections");
        check(tally.placed==tally.listed && tally.reloaded==tally.listed,world+": every terrain section placed and saved");
    }
    if(!failures.empty())return 1;
    std::cout<<"PASS: every terrain section of "<<worlds.size()<<" worlds stamps by click, undoes, redoes and survives save/reopen\n";
    return 0;
}

// The feature TDF fields the report breaks placements down by.
// Where a definition comes from: the install's own archives (world / shared),
// a mod pack (mod / modded), or only a downloaded map in Maps/
// (downloaded-map; downloaded-map-shadowed when such a map replaces a retail
// file). Retail mounts every Maps/ archive globally, so the browser offers them.
std::string definitionSource(const tak::hpi::Vfs& vfs,const tak::hpi::Vfs& plain,const std::string& path,const std::string& sharedDir) {
    const auto full=vfs.tryRead(path),installed=vfs.tryRead(path,true),retail=plain.tryRead(path,true);
    return !installed?"downloaded-map":*installed!=full?"downloaded-map-shadowed":!retail?"mod":*retail!=*installed?"modded":
        !sharedDir.empty() && path.starts_with(sharedDir)?"shared":"world";
}
struct FeatureTraits {bool animated=false,reclaimable=false,blocking=false;int height=0;std::string source;};
std::map<std::string,FeatureTraits> featureTraits(const tak::hpi::Vfs& vfs,const tak::hpi::Vfs& plain,const std::string& world) {
    std::map<std::string,FeatureTraits> out;
    for(const std::string& dir:{"features/"+world,std::string("features/all worlds")})for(const auto& path:vfs.list(dir)) {
        if(!path.ends_with(".tdf"))continue;
        try {
            const auto bytes=vfs.read(path);const auto root=tak::tdf::parseText(std::string(bytes.begin(),bytes.end()),path);
            const auto source=definitionSource(vfs,plain,path,"features/all worlds");
            for(const auto& name:root.childOrder)if(const auto* n=root.child(name)) {
                auto& traits=out[name];
                traits.animated=n->numberOr("animating",0)!=0;traits.reclaimable=n->numberOr("reclaimable",0)!=0;
                traits.blocking=n->numberOr("blocking",0)!=0;traits.height=int(n->numberOr("height",0));traits.source=source;
            }
        } catch(const std::exception&) {}
    }
    return out;
}

// Every feature of `world` on one 8x8-unit map: browser pick, Place click,
// undo, redo, Erase click, undo; then save, reopen, compare, and check the game
// resolves each name with the browser's footprint.
bool featureSession(const std::string& data,const std::string& world,Tally& tally) {
    const auto root=tempRoot("features");isolateProfile(root);
    const auto traits=featureTraits(tak::hpi::mountRetailRoot(tak::u8path(data)),tak::hpi::mountRetailRoot(tak::u8path(data)),world);
    enum {START,SEARCH,PICK,PLACED,UNDONE,REDONE,ERASED,RESTORED,REOPENED} phase=START;
    size_t item=0;int cellX=0,cellZ=0;BrowserPick pick;tak::tnt::Map expected,prior;
    std::vector<std::pair<cart::FeatureRef,size_t>> placed;   // browser entry, cell
    const std::string label=world+" features";bool finished=false;
    drive({"cartographer","--new","8x8","--world",world,"--data",data,"--out",root},[&](Editor& e,const Input& in) {
        const auto& list=e.features.list();const auto& map=e.mapView.map();
        auto fail=[&](const std::string& what) {check(false,label+" '"+list[item].name+"': "+what);};
        auto cellPoint=[&] {return focus(e,cellX*16+8.f,cellZ*16+8.f);};
        for(;;)switch(phase) {
        case START:
            tally.listed+=int(list.size());expected=map;tool(in,Editor::FEATURES);phase=SEARCH;return true;
        case SEARCH: {
            if(item>=list.size()) {
                // What the game makes of them (this install's view, as when the map is
                // played from Maps/): every name resolves, with the browser's footprint.
                for(const auto& problem:engineProblems(e))check(false,label+": "+problem);
                const auto game=engineWorld(e);const auto& types=game->mapPlacementTypes();
                for(const auto& [ref,cell]:placed) {
                    const auto v=map.features[cell];
                    if(v<types.size() && (types[v].footX!=ref.footX || types[v].footZ!=ref.footZ))
                        check(false,label+" '"+ref.name+"': game footprint "+std::to_string(types[v].footX)+"x"+std::to_string(types[v].footZ)+
                              ", browser "+std::to_string(ref.footX)+"x"+std::to_string(ref.footZ));
                }
                saveAndReopen(e,in,label,root);phase=REOPENED;return true;
            }
            const int slot=int(item),perRow=map.width/8;
            if(slot>=perRow*(map.height/8)) {fail("no free slot");return false;}
            cellX=slot%perRow*8+3;cellZ=slot/perRow*8+3;
            const auto& ref=list[item];
            const auto t=traits.find(ref.name);
            if(t!=traits.end()) {
                if(t->second.animated)++tally.kinds["animated"];
                if(t->second.reclaimable)++tally.kinds["reclaimable"];
                if(t->second.blocking)++tally.kinds["blocking"];
                if(t->second.height>=64)++tally.kinds["tall"];
                ++tally.kinds["from-"+t->second.source];
            }
            if(ref.footX*ref.footZ>1)++tally.kinds["multi-cell"];
            if(const auto* sprite=e.features.sprite(e.vfs,ref);!sprite || sprite->w<=0)++tally.kinds["no-art"];
            pick.search(in,int(item),ref.name);phase=PICK;return true;
        }
        case PICK: {
            const int clicked=pick.click(e,in);
            if(!clicked)return true;
            if(clicked<0) {fail("browser search does not list it");++item;phase=SEARCH;continue;}
            const auto p=cellPoint();mode(in,Editor::MODE_PLACE);in.click(p.x,p.y);
            prior=expected;
            const auto& name=list[item].name;
            auto at=std::find(expected.featureNames.begin(),expected.featureNames.end(),name);
            if(at==expected.featureNames.end())at=expected.featureNames.insert(expected.featureNames.end(),name);
            expected.features[size_t(cellZ)*expected.width+cellX]=uint16_t(at-expected.featureNames.begin());
            phase=PLACED;return true;
        }
        case PLACED:
            if(!check(e.selectedFeat==int(item),label+": browser click selects '"+list[item].name+"'"))return false;
            if(const auto d=planeDifference(map,expected);!d.empty()) {fail("click did not place it at the clicked cell: "+d);return false;}
            ++tally.placed;in.key(SDLK_z,KMOD_CTRL);phase=UNDONE;return true;
        case UNDONE:
            if(const auto d=planeDifference(map,prior);!d.empty()) {fail("undo: "+d);return false;}
            ++tally.undone;in.key(SDLK_y,KMOD_CTRL);phase=REDONE;return true;
        case REDONE: {
            if(const auto d=planeDifference(map,expected);!d.empty()) {fail("redo: "+d);return false;}
            ++tally.redone;const auto p=cellPoint();mode(in,Editor::MODE_ERASE);in.click(p.x,p.y);phase=ERASED;return true;
        }
        case ERASED:
            if(map.features[size_t(cellZ)*map.width+cellX]!=0xFFFF) {fail("Erase click left it");return false;}
            ++tally.erased;in.key(SDLK_z,KMOD_CTRL);phase=RESTORED;return true;
        case RESTORED:
            if(const auto d=planeDifference(map,expected);!d.empty()) {fail("undo of the erase: "+d);return false;}
            ++tally.restored;placed.push_back({list[item],size_t(cellZ)*map.width+cellX});++item;phase=SEARCH;continue;
        case REOPENED:
            if(reopened(e,label)) {
                if(const auto d=planeDifference(map,expected);!d.empty())check(false,label+": saved and reopened map differs: "+d);
                else tally.reloaded+=int(placed.size());
                finished=true;
            }
            return false;
        }
    },900,label);
    std::error_code ec;fs::remove_all(root,ec);
    return finished;
}

int featuresWorkflow(const std::string& data,std::vector<std::string> worlds) {
    if(worlds.empty())worlds=kWorlds;
    for(const auto& world:worlds) {
        Tally tally;
        if(featureSession(data,world,tally))
            check(tally.placed==tally.listed && tally.reloaded==tally.listed,world+": every feature placed and saved");
        tally.print(world+" features");
    }
    if(!failures.empty())return 1;
    std::cout<<"PASS: every feature of "<<worlds.size()<<" worlds places by click, undoes, redoes, erases and survives save/reopen\n";
    return 0;
}

// Every unit type on one 8x8-unit map whose right half is first stamped with the
// world's deepest water section through the terrain browser. Each type goes
// where the game accepts it -- land, then water, then the shoreline -- and
// owners cycle through players 1-8: pick, Place click, undo, redo, Erase
// click, undo; then save, reopen and compare (the .crt round trip).
bool unitSession(const std::string& data,const std::string& world,Tally& tally) {
    const auto root=tempRoot("units");isolateProfile(root);
    tak::hpi::Vfs plain=tak::hpi::mountRetailRoot(tak::u8path(data));
    enum {START,WATER,WATERED,SEARCH,PICK,PLACED,UNDONE,REDONE,ERASED,RESTORED,REOPENED} phase=START;
    size_t item=0;int water=-1;BrowserPick pick;tak::tnt::Map expectedMap;
    std::vector<cart::PlacedUnit> expected;std::vector<bool> legal;
    std::vector<SDL_Point> land,sea,shore;size_t usedLand=0,usedSea=0,usedShore=0;
    std::unique_ptr<tak::sim::World> game;int cellX=0,cellZ=0,player=0;
    const std::string label=world+" units";bool finished=false;
    drive({"cartographer","--new","8x8","--world",world,"--data",data,"--out",root},[&](Editor& e,const Input& in) {
        const auto& types=e.unitTypes;const auto& map=e.mapView.map();
        auto fail=[&](const std::string& what) {check(false,label+" '"+types[item]+"': "+what);};
        auto unitPoint=[&](const cart::PlacedUnit& u) {return focus(e,u.x,u.z);};
        auto same=[](const cart::PlacedUnit& a,const cart::PlacedUnit& b) {
            return a.type==b.type && a.player==b.player && a.x==b.x && a.z==b.z && a.footX==b.footX && a.footZ==b.footZ &&
                   a.health==b.health && a.armor==b.armor && a.weapon==b.weapon && a.veteran==b.veteran && a.angle==b.angle;
        };
        auto unitsMatch=[&] {
            if(e.units.size()!=expected.size())return false;
            for(size_t i=0;i<expected.size();++i)if(!same(e.units[i],expected[i]))return false;
            return true;
        };
        for(;;)switch(phase) {
        case START: {
            tally.listed+=int(types.size());expectedMap=map;
            // The deepest water section: the most metres between sea level and its highest cell.
            int best=0;
            for(size_t i=0;i<e.sections.list().size();++i)if(const auto* m=e.sections.load(e.vfs,e.sections.list()[i].path)) {
                if(m->heights.empty())continue;
                const int depth=map.seaLevel-int(*std::max_element(m->heights.begin(),m->heights.end()));
                if(depth>best) {best=depth;water=int(i);}
            }
            if(water<0) {std::cout<<"note: "<<world<<" has no water section; naval units stay on land\n";phase=WATERED;continue;}
            tool(in,Editor::TERRAIN);mode(in,Editor::MODE_PLACE);pick.search(in,water,e.sections.list()[size_t(water)].name);phase=WATER;return true;
        }
        case WATER: {
            const int clicked=pick.click(e,in);
            if(!clicked)return true;
            if(!check(clicked>0,label+": water section listed by the terrain browser"))return false;
            // The whole map in view: every stamp of the right half in one frame.
            const auto* m=e.sections.load(e.vfs,e.sections.list()[size_t(water)].path);
            e.mapView.setZoom(0.125f);e.mapView.setOffset(0,0);
            for(int bz=0;bz+m->blocksY<=map.blocksY;bz+=m->blocksY)for(int bx=map.blocksX/2;bx+m->blocksX<=map.blocksX;bx+=m->blocksX) {
                const auto p=cellScreen(e,(bx+m->blocksX/2)*32+16.f,(bz+m->blocksY/2)*32+16.f);in.click(p.x,p.y);
                cart::stampSection(expectedMap,*m,bx,bz,e.stampLayers);
            }
            phase=WATERED;return true;
        }
        case WATERED: {
            if(water>=0 && !check(planeDifference(map,expectedMap).empty(),label+": water stamped on the right half"))return false;
            // Slots 12 cells apart: land on the left, water on the right, and the shore between.
            for(int z=6;z+8<map.height;z+=12) {
                for(int x=6;x+8<map.width/2;x+=12)land.push_back({x,z});
                for(int x=map.width/2+10;x+8<map.width;x+=12)sea.push_back({x,z});
                shore.push_back({map.width/2-2,z});
            }
            game=engineWorld(e);tool(in,Editor::UNITS);phase=SEARCH;return true;
        }
        case SEARCH: {
            if(item>=types.size()) {saveAndReopen(e,in,label,root);phase=REOPENED;return true;}
            const auto* type=e.unitRegistry.find(lowerText(types[item]));
            const int fx=type?type->footX:1,fz=type?type->footZ:1;
            // The first free slot of the kinds the game accepts this type on.
            bool accepted=false;SDL_Point slot{};
            for(auto* kind:{&land,&sea,&shore}) {
                size_t& used=kind==&land?usedLand:kind==&sea?usedSea:usedShore;
                if(used>=kind->size())continue;
                const auto s=(*kind)[used];
                if(!type || type->canFly || game->canPlace(type,s.x*16.f+fx*8,s.y*16.f+fz*8)) {slot=s;++used;accepted=true;break;}
            }
            if(!accepted) {   // still placed (the editor allows it); Check Map warns
                ++tally.refused;std::cout<<"note: the game accepts "<<types[item]<<" on none of land, deep water or shore ("<<fx<<"x"<<fz<<")\n";
                if(usedLand>=land.size()) {fail("no free land slot");return false;}
                slot=land[usedLand++];
            }
            legal.push_back(accepted);cellX=slot.x;cellZ=slot.y;
            ++tally.kinds[e.unitCategory(int(item))];
            if(type)++tally.kinds["side-"+type->side];
            else {++tally.kinds["no-definition"];std::cout<<"note: "<<types[item]<<" has an .fbi the unit registry does not define (placed 1x1; Check Map reports it)\n";}
            ++tally.kinds["from-"+definitionSource(e.vfs,plain,"units/"+lowerText(types[item])+".fbi","")];
            player=int(item%8);
            for(int p=e.currentPlayer;p!=player;p=(p+1)%8)in.key(SDLK_RIGHTBRACKET);
            pick.search(in,int(item),types[item]);phase=PICK;return true;
        }
        case PICK: {
            const int clicked=pick.click(e,in);
            if(!clicked)return true;
            if(clicked<0) {fail("browser search does not list it");return false;}
            const auto p=focus(e,cellX*16+8.f,cellZ*16+8.f);mode(in,Editor::MODE_PLACE);in.click(p.x,p.y);
            const auto* type=e.unitRegistry.find(lowerText(types[item]));
            cart::PlacedUnit u;u.type=types[item];u.player=player;
            if(type) {u.footX=type->footX;u.footZ=type->footZ;}
            u.x=cellX*16.f+u.footX*8;u.z=cellZ*16.f+u.footZ*8;expected.push_back(u);
            phase=PLACED;return true;
        }
        case PLACED:
            if(!check(e.selectedType==int(item),label+": browser click selects '"+types[item]+"'"))return false;
            if(!unitsMatch()) {fail("click did not place it centred on the clicked cell for player "+std::to_string(player+1));return false;}
            ++tally.placed;in.key(SDLK_z,KMOD_CTRL);phase=UNDONE;return true;
        case UNDONE:
            if(e.units.size()+1!=expected.size()) {fail("undo");return false;}
            ++tally.undone;in.key(SDLK_y,KMOD_CTRL);phase=REDONE;return true;
        case REDONE: {
            if(!unitsMatch()) {fail("redo");return false;}
            ++tally.redone;const auto p=unitPoint(expected.back());mode(in,Editor::MODE_ERASE);in.click(p.x,p.y);phase=ERASED;return true;
        }
        case ERASED:
            if(e.units.size()+1!=expected.size()) {fail("Erase click left it");return false;}
            ++tally.erased;in.key(SDLK_z,KMOD_CTRL);phase=RESTORED;return true;
        case RESTORED:
            if(!unitsMatch()) {fail("undo of the erase");return false;}
            ++tally.restored;++item;phase=SEARCH;continue;
        case REOPENED:
            if(reopened(e,label)) {
                if(!unitsMatch())check(false,label+": saved and reopened units differ");
                else tally.reloaded+=int(expected.size());
                const auto reloaded=engineWorld(e);
                for(size_t i=0;i<expected.size() && i<e.units.size();++i)if(legal[i]) {
                    const auto* type=e.unitRegistry.find(lowerText(e.units[i].type));
                    if(type && !type->canFly && !reloaded->canPlace(type,e.units[i].x,e.units[i].z))
                        check(false,label+" '"+e.units[i].type+"': the game rejects the reopened placement");
                }
                finished=true;
            }
            return false;
        }
    },1200,label);
    std::error_code ec;fs::remove_all(root,ec);
    return finished;
}

int unitsWorkflow(const std::string& data,std::vector<std::string> worlds) {
    if(worlds.empty())worlds=kWorlds;
    for(const auto& world:worlds) {
        Tally tally;
        if(unitSession(data,world,tally))
            check(tally.placed==tally.listed && tally.reloaded==tally.listed,world+": every unit type placed and saved");
        tally.print(world+" units");
    }
    if(!failures.empty())return 1;
    std::cout<<"PASS: every unit type places by click on terrain the game accepts, undoes, redoes, erases and survives save/reopen\n";
    return 0;
}

// Start slots 1-8 and a region on a fresh map: each placed by a canvas click,
// undone, redone, erased and restored; a ninth start is refused with a message;
// then save, reopen and compare (.ota starts, .crt region).
bool slotSession(const std::string& data,const std::string& world,Tally& tally) {
    const auto root=tempRoot("slots");isolateProfile(root);
    enum {START,PLACE,PLACED,UNDONE,REDONE,ERASED,RESTORED,NINTH,REGION,REGION_UNDONE,REGION_REDONE,REGION_ERASED,REGION_RESTORED,REOPENED} phase=START;
    int number=0;std::vector<tak::tnt::StartPos> expected;tak::crt::Region region;
    const std::string label=world+" start slots";bool finished=false;
    drive({"cartographer","--new","8x8","--world",world,"--data",data,"--out",root},[&](Editor& e,const Input& in) {
        const auto& starts=e.scenario.starts;
        auto matches=[&] {
            if(starts.size()!=expected.size())return false;
            for(size_t i=0;i<starts.size();++i)if(starts[i].number!=expected[i].number || starts[i].xpos!=expected[i].xpos || starts[i].zpos!=expected[i].zpos)return false;
            return true;
        };
        auto fail=[&](const std::string& what) {check(false,label+" "+std::to_string(number)+": "+what);};
        auto startPoint=[&](const tak::tnt::StartPos& s) {return focus(e,s.xpos*16+8.f,s.zpos*16+8.f);};
        auto regionPoint=[&](float x,float z) {return cellScreen(e,x*16+8,z*16+8);};
        for(;;)switch(phase) {
        case START: check(starts.empty(),label+": a blank map has no starts");tool(in,Editor::STARTS);mode(in,Editor::MODE_PLACE);phase=PLACE;return true;
        case PLACE: {
            if(++number>8) {   // all slots used: the ninth click is refused
                const auto p=focus(e,200*16+8.f,200*16+8.f);mode(in,Editor::MODE_PLACE);in.click(p.x,p.y);phase=NINTH;return true;
            }
            ++tally.listed;
            expected.push_back({number,20+(number-1)%4*60,30+(number-1)/4*150});
            const auto p=startPoint(expected.back());mode(in,Editor::MODE_PLACE);in.click(p.x,p.y);phase=PLACED;return true;
        }
        case PLACED: if(!matches()) {fail("click did not place StartPos at the clicked cell");return false;}
            ++tally.placed;in.key(SDLK_z,KMOD_CTRL);phase=UNDONE;return true;
        case UNDONE: if(starts.size()+1!=expected.size()) {fail("undo");return false;}
            ++tally.undone;in.key(SDLK_y,KMOD_CTRL);phase=REDONE;return true;
        case REDONE: {
            if(!matches()) {fail("redo");return false;}
            ++tally.redone;const auto p=startPoint(expected.back());mode(in,Editor::MODE_ERASE);in.click(p.x,p.y);phase=ERASED;return true;
        }
        case ERASED: if(starts.size()+1!=expected.size()) {fail("Erase click left it");return false;}
            ++tally.erased;in.key(SDLK_z,KMOD_CTRL);phase=RESTORED;return true;
        case RESTORED: if(!matches()) {fail("undo of the erase");return false;}
            ++tally.restored;phase=PLACE;continue;
        case NINTH: {
            if(!check(matches() && e.modal==Editor::M_MESSAGE,label+": a ninth start is refused with a message (have "+std::to_string(starts.size())+")"))return false;
            ++tally.refused;in.key(SDLK_RETURN);
            // A region: the REGIONS canvas, Place mode, drag a box.
            regions(in);mode(in,Editor::MODE_PLACE);
            e.mapView.setZoom(0.5f);e.mapView.setOffset(0,0);
            region={"Region 1",30,40,49,52};
            const auto a=regionPoint(30,40),b=regionPoint(49,52);in.drag(a.x,a.y,b.x,b.y);phase=REGION;return true;
        }
        case REGION: {
            const auto& rs=e.scen.regions;
            if(!check(rs.size()==1 && rs[0].name==region.name && rs[0].x1==region.x1 && rs[0].z1==region.z1 && rs[0].x2==region.x2 && rs[0].z2==region.z2,
                      label+": region drawn by a Place drag"))return false;
            ++tally.kinds["regions"];in.key(SDLK_z,KMOD_CTRL);phase=REGION_UNDONE;return true;
        }
        case REGION_UNDONE: if(!check(e.scen.regions.empty(),label+": region undo"))return false;
            in.key(SDLK_y,KMOD_CTRL);phase=REGION_REDONE;return true;
        case REGION_REDONE: {
            if(!check(e.scen.regions.size()==1,label+": region redo"))return false;
            e.mapView.setZoom(0.5f);e.mapView.setOffset(0,0);
            const auto p=regionPoint(40,46);mode(in,Editor::MODE_ERASE);in.click(p.x,p.y);phase=REGION_ERASED;return true;
        }
        case REGION_ERASED: if(!check(e.scen.regions.empty(),label+": Erase click removes the region"))return false;
            in.key(SDLK_z,KMOD_CTRL);phase=REGION_RESTORED;return true;
        case REGION_RESTORED:
            if(!check(e.scen.regions.size()==1,label+": region restored"))return false;
            in.key(SDLK_ESCAPE);   // leave the REGIONS canvas
            saveAndReopen(e,in,label,root);phase=REOPENED;return true;
        case REOPENED:
            if(reopened(e,label)) {
                const auto& rs=e.scen.regions;
                if(!matches())check(false,label+": saved and reopened starts differ");else tally.reloaded+=int(expected.size());
                check(rs.size()==1 && rs[0].name==region.name && rs[0].x1==region.x1 && rs[0].z2==region.z2,label+": saved and reopened region differs");
                finished=true;
            }
            return false;
        }
    },300,label);
    std::error_code ec;fs::remove_all(root,ec);
    return finished;
}

int slotsWorkflow(const std::string& data,std::vector<std::string> worlds) {
    if(worlds.empty())worlds=kWorlds;
    for(const auto& world:worlds) {
        Tally tally;
        if(slotSession(data,world,tally))check(tally.placed==8 && tally.reloaded==8 && tally.refused==1,world+": eight start slots and a region");
        tally.print(world+" start slots");
    }
    if(!failures.empty())return 1;
    std::cout<<"PASS: start slots 1-8 and regions place by click, undo, redo, erase and survive save/reopen; a ninth start is refused\n";
    return 0;
}

// ---- rescan: the browser after its library is rescanned ------------------------

// A private install: links to the real install's root archives plus Maps/ with
// one downloaded map that ships its own feature file. Retail mounts every Maps/
// archive globally, so the FEATURES browser offers those features -- until a map
// opened from disk replaces the view and the library is rescanned without them.
std::optional<std::string> packInstall(const std::string& data,const std::string& root) {
    const auto install=fs::path(root)/"install";fs::create_directories(install/"Maps");
    for(const auto& entry:fs::directory_iterator(tak::u8path(data))) {
        if(!entry.is_regular_file())continue;   // root archives are all validInstall needs
        std::error_code ec;fs::create_symlink(fs::absolute(entry.path()),install/entry.path().filename(),ec);
        if(ec)fs::create_hard_link(entry.path(),install/entry.path().filename(),ec);
        if(ec)return std::nullopt;
    }
    cart::FeatureLibrary library;library.scan(tak::hpi::mountRetailRoot(tak::u8path(data)),"aramon");
    if(library.list().empty())return std::nullopt;
    const auto& art=library.list().front();   // borrowed sprite
    std::string tdf;
    for(int i=1;i<=3;++i)tdf+="[CartoTestStone"+std::to_string(i)+"]\r\n{\r\nworld=Aramon;\r\ncategory=rocks;\r\nfootprintx=2;\r\nfootprintz=2;\r\nfilename="+art.filename+";\r\nseqname="+art.seqname+";\r\n}\r\n";
    const auto packed=tak::hpi::pack({{"features/aramon/cartotest.tdf",{tdf.begin(),tdf.end()}}});
    std::ofstream(install/"Maps"/"Carto Test Pack.kmp",std::ios::binary).write(reinterpret_cast<const char*>(packed.data()),std::streamsize(packed.size()));
    return install.string();
}

int rescanWorkflow(const std::string& data) {
    const auto root=tempRoot("rescan");isolateProfile(root);
    const auto install=packInstall(data,root);
    if(!install) {std::cout<<"SKIP: cannot link the install's archives\n";return 77;}
    int stage=0;size_t listed=0,cell=0;const std::string label="rescan";
    auto expect=[&](bool value,const std::string& what){return check(value,label+": "+what);};
    drive({"cartographer","--new","8x8","--world","aramon","--data",*install,"--out",root},[&](Editor& e,const Input& in) {
        const auto& map=e.mapView.map();
        switch(stage++) {
        case 0:   // the downloaded map's features are listed; place one
            listed=e.features.list().size();
            if(!expect(e.features.byName("cartoteststone1")!=nullptr,"a Maps/ archive's feature is offered"))return false;
            tool(in,Editor::FEATURES);in.key(SDLK_f,KMOD_CTRL);in.text("cartoteststone3");in.key(SDLK_ESCAPE);return true;
        case 1: {
            const auto& list=e.features.list();
            const int index=int(std::find_if(list.begin(),list.end(),[](const auto& f){return f.name=="cartoteststone3";})-list.begin());
            const auto row=std::find(e.paletteItems.begin(),e.paletteItems.end(),index);
            const auto point=row==e.paletteItems.end()?std::nullopt:paletteRow(e,int(row-e.paletteItems.begin()));
            if(!expect(point.has_value(),"the search lists that feature"))return false;
            in.click(point->x,point->y);
            const auto p=focus(e,40*16+8.f,40*16+8.f);cell=40*size_t(map.width)+40;mode(in,Editor::MODE_PLACE);in.click(p.x,p.y);
            in.key(SDLK_f,KMOD_CTRL);in.key(SDLK_BACKSPACE);in.key(SDLK_ESCAPE);   // every feature listed again
            return true;
        }
        case 2:
            expect(map.features[cell]<map.featureNames.size() && map.featureNames[map.features[cell]]=="cartoteststone3","placed by click");
            expect(e.paletteItems.size()==listed,"the cleared search lists every feature");
            saveAndReopen(e,in,label,root);return true;
        case 3:   // opened from disk: the library is rescanned without the Maps/ archive
            if(!reopened(e,label))return false;
            expect(e.features.list().size()+3==listed,"the reopened map's browser no longer offers the archive's features");
            expect(map.features[cell]<map.featureNames.size() && map.featureNames[map.features[cell]]=="cartoteststone3","the placed name survives the round trip");
            // An undefined name stays selectable and deletable (drawn as a red marker).
            {const auto p=focus(e,40*16+8.f,40*16+8.f);mode(in,Editor::MODE_SELECT);in.click(p.x,p.y);in.key(SDLK_DELETE);}
            return true;
        case 4:
            expect(map.features[cell]==0xFFFF,"an undefined feature can be selected and deleted");
            return false;
        default: return false;
        }
    },120,label);
    std::error_code ec;fs::remove_all(root,ec);
    if(!failures.empty())return 1;
    std::cout<<"PASS: browser rebuilt after a library rescan (Maps/ archive features), undefined names stay selectable\n";
    return 0;
}

// ---- feature operations ------------------------------------------------------

// A frame script: a step runs each frame until it returns true; the input it
// pushed is handled during the next frame, where the next step checks it. Steps
// a running step adds run right after it, in the order added.
struct Script {
    using Step=std::function<bool(Editor&,const Input&)>;
    std::vector<Step> steps;size_t at=0,insertAt=SIZE_MAX;
    void then(Step step) {
        if(insertAt==SIZE_MAX)steps.push_back(std::move(step));
        else steps.insert(steps.begin()+ptrdiff_t(insertAt++),std::move(step));
    }
    bool frame(Editor& e,const Input& in) {
        if(at>=steps.size())return false;
        const auto step=steps[at];insertAt=at+1;
        const bool done=step(e,in);insertAt=SIZE_MAX;
        if(done)++at;
        return true;
    }
};

// The whole map at 1/8 (2 px a cell) or 1/16 scale, top-left at the origin: one
// view in which every click of a frame can land, whatever its cell.
SDL_Point overview(Editor& e,float zoom,int cx,int cz) {
    e.mapView.setZoom(zoom);e.mapView.setOffset(0,0);
    return cellScreen(e,cx*16+8.f,cz*16+8.f);
}
size_t cellOf(const tak::tnt::Map& map,int x,int z) {return size_t(z)*map.width+x;}
std::string nameAt(const tak::tnt::Map& map,size_t cell) {
    const auto v=cell<map.features.size()?map.features[cell]:0xFFFF;
    return v<0xFFFA && size_t(v)<map.featureNames.size()?map.featureNames[v]:std::string{};
}
// A held modifier for the next frame's events (the editor reads SDL_GetModState).
void holdShift(bool held) {SDL_SetModState(held?KMOD_LSHIFT:KMOD_NONE);}
void menu(const Input& in,int column,int row) {in.click(96+column*72+20,10);in.click(96+column*72+40,22+row*20+10);}

struct FeatureOps {
    std::string root,label;
    Script script;
    std::vector<cart::FeatureRef> refs;             // the world's browser list
    std::vector<std::pair<int,int>> slots;          // feature i's anchor cell
    std::map<size_t,std::string> placed;            // cell -> name (the session's expectation)
    BrowserPick pick;
    bool expect(bool ok,const std::string& what) {return check(ok,label+": "+what);}
    // The document's named features are exactly `placed`.
    bool matches(const Editor& e,const std::string& what) {
        const auto& map=e.mapView.map();size_t named=0;
        for(size_t i=0;i<map.features.size();++i)if(!nameAt(map,i).empty())++named;
        if(named!=placed.size())return expect(false,what+": "+std::to_string(named)+" features on the map, expected "+std::to_string(placed.size()));
        for(const auto& [cell,name]:placed)if(nameAt(map,cell)!=name)
            return expect(false,what+": cell "+std::to_string(cell%map.width)+","+std::to_string(cell/map.width)+" holds '"+nameAt(map,cell)+"', expected '"+name+"'");
        return true;
    }
    // Browser pick + Place click at cell (x,z): two steps.
    void place(int index,int x,int z) {
        script.then([this,index](Editor&,const Input& in) {pick.search(in,index,refs[size_t(index)].name);return true;});
        script.then([this,index,x,z](Editor& e,const Input& in) {
            const int clicked=pick.click(e,in);
            if(!clicked)return false;
            if(!expect(clicked>0,"browser lists '"+refs[size_t(index)].name+"'"))return true;
            const auto p=focus(e,x*16+8.f,z*16+8.f);mode(in,Editor::MODE_PLACE);in.click(p.x,p.y);
            placed[cellOf(e.mapView.map(),x,z)]=refs[size_t(index)].name;
            return true;
        });
    }
    void verify(const std::string& what) {script.then([this,what](Editor& e,const Input&) {matches(e,what);return true;});}
};

// Every feature of `world` on one 16x16-unit map, then every feature operation
// the editor has, on each of them and on all of them at once.
bool featureOpsSession(const std::string& data,const std::string& world) {
    FeatureOps ops;ops.root=tempRoot("featureops");ops.label=world+" feature ops";
    isolateProfile(ops.root);
    auto& s=ops.script;
    std::map<size_t,std::string> original;   // the populated map's features
    int boxX=0,boxZ=0;                       // the populated block (cells)
    std::shared_ptr<cart::SymmetryReport> report=std::make_shared<cart::SymmetryReport>();
    s.then([&](Editor& e,const Input& in) {
        ops.refs=e.features.list();
        const auto& map=e.mapView.map();
        if(!ops.expect(map.width==512 && !ops.refs.empty(),"a 16x16 map with features"))return true;
        ops.expect(std::all_of(map.features.begin(),map.features.end(),[](uint16_t v){return v==0xFFFF;}),"a new blank map has an empty feature plane (no road markers)");
        for(size_t i=0;i<ops.refs.size();++i)ops.slots.push_back({8+int(i%28)*8,8+int(i/28)*8});
        tool(in,Editor::FEATURES);
        // Populate: every feature by browser pick + click, then the map's corners
        // (the largest footprint reaches past the bottom-right edge).
        for(size_t i=0;i<ops.refs.size();++i)ops.place(int(i),ops.slots[i].first,ops.slots[i].second);
        size_t big=0;for(size_t i=0;i<ops.refs.size();++i)if(ops.refs[i].footX*ops.refs[i].footZ>ops.refs[big].footX*ops.refs[big].footZ)big=i;
        ops.place(0,0,map.height-1);ops.place(0,map.width-1,0);ops.place(int(big),map.width-1,map.height-1);ops.place(int(big),map.width-2,map.height-2);
        ops.verify("every feature placed by a browser pick and a click");
        s.then([&](Editor&,const Input&) {original=ops.placed;return true;});
        // One by one: select, drag a cell right, undo; every tenth also shift-clicks its neighbour.
        for(size_t i=0;i<ops.refs.size();++i) {
            s.then([&,i](Editor& e,const Input& in) {
                const auto [x,z]=ops.slots[i];const auto p=focus(e,x*16+8.f,z*16+8.f);
                mode(in,Editor::MODE_SELECT);in.drag(p.x,p.y,p.x+16,p.y);return true;
            });
            s.then([&,i](Editor& e,const Input& in) {
                const auto& map=e.mapView.map();const auto [x,z]=ops.slots[i];
                ops.expect(nameAt(map,cellOf(map,x,z)).empty() && nameAt(map,cellOf(map,x+1,z))==ops.refs[i].name &&
                           e.selectedFeatures.cells==std::set<int>{int(cellOf(map,x+1,z))},"'"+ops.refs[i].name+"' selected and dragged a cell right (at "+
                           nameAt(map,cellOf(map,x,z))+", right "+nameAt(map,cellOf(map,x+1,z))+", "+std::to_string(e.selectedFeatures.cells.size())+" selected, mode "+std::to_string(int(e.editMode))+")");
                in.key(SDLK_z,KMOD_CTRL);return true;
            });
            s.then([&,i](Editor& e,const Input& in) {
                ops.expect(nameAt(e.mapView.map(),cellOf(e.mapView.map(),ops.slots[i].first,ops.slots[i].second))==ops.refs[i].name,"undo returns '"+ops.refs[i].name+"'");
                if(i%10 || i+1>=ops.refs.size())return true;
                const auto [x,z]=ops.slots[i];const auto [x2,z2]=ops.slots[i+1];
                in.click(focus(e,x*16+8.f,z*16+8.f).x,focus(e,x*16+8.f,z*16+8.f).y);holdShift(true);
                const auto p=cellScreen(e,x2*16+8.f,z2*16+8.f);in.click(p.x,p.y);return true;
            });
            if(i%10==0 && i+1<ops.refs.size())s.then([&,i](Editor& e,const Input& in) {
                holdShift(false);const auto& map=e.mapView.map();
                ops.expect(e.selectedFeatures.cells==std::set<int>{int(cellOf(map,ops.slots[i].first,ops.slots[i].second)),int(cellOf(map,ops.slots[i+1].first,ops.slots[i+1].second))},
                           "shift-click adds '"+ops.refs[i+1].name+"' to the selection");
                const auto p=focus(e,ops.slots[i].first*16+72.f,ops.slots[i].second*16+72.f);in.click(p.x,p.y);   // an empty cell clears it
                return true;
            });
        }
        ops.verify("single moves undone");
        // All at once (1/8 view): box select, properties, group drag, undo/redo.
        boxX=8+28*8;boxZ=8+int((ops.refs.size()+27)/28)*8;
        auto boxSelect=[&](Editor& e,const Input& in) {
            const auto a=overview(e,.125f,2,2),b=overview(e,.125f,boxX,boxZ);mode(in,Editor::MODE_SELECT);in.drag(a.x,a.y,b.x,b.y);
        };
        auto selectionIsOriginal=[&](const Editor& e) {
            std::set<int> want;for(const auto& [cell,name]:original)if(int(cell%512)<boxX && int(cell/512)<boxZ)want.insert(int(cell));
            return e.selectedFeatures.cells==want;
        };
        s.then([=](Editor& e,const Input& in) {boxSelect(e,in);return true;});   // boxSelect holds only references to the session
        s.then([&,selectionIsOriginal](Editor& e,const Input& in) {
            ops.expect(selectionIsOriginal(e),"box select takes every feature in the box ("+std::to_string(e.selectedFeatures.cells.size())+")");
            in.key(SDLK_RETURN);return true;
        });
        s.then([&](Editor& e,const Input& in) {
            ops.expect(e.modal==Editor::M_MESSAGE && e.mTitle=="SELECTED FEATURES","Enter shows the selected features");
            in.key(SDLK_RETURN);
            const auto [x,z]=ops.slots[0];const auto a=overview(e,.125f,x,z),b=overview(e,.125f,x+2,z+1);in.drag(a.x,a.y,b.x,b.y);return true;
        });
        s.then([&](Editor& e,const Input& in) {
            std::map<size_t,std::string> moved;for(const auto& [cell,name]:original)moved[int(cell%512)<boxX && int(cell/512)<boxZ?cell+512+2:cell]=name;
            ops.placed=moved;ops.matches(e,"group drag moves every selected feature by (2,1)");
            in.key(SDLK_z,KMOD_CTRL);return true;
        });
        s.then([&](Editor& e,const Input& in) {ops.placed=original;ops.matches(e,"group drag undone");in.key(SDLK_y,KMOD_CTRL);return true;});
        s.then([&](Editor& e,const Input& in) {
            std::map<size_t,std::string> moved;for(const auto& [cell,name]:original)moved[int(cell%512)<boxX && int(cell/512)<boxZ?cell+512+2:cell]=name;
            ops.placed=moved;ops.matches(e,"group drag redone");in.key(SDLK_z,KMOD_CTRL);return true;
        });
        // Clipboard: copy + paste (right half), duplicate (lower right), cut + paste back, delete.
        s.then([&,boxSelect](Editor& e,const Input& in) {ops.placed=original;ops.matches(e,"undone again");boxSelect(e,in);return true;});
        s.then([&](Editor& e,const Input& in) {
            in.key(SDLK_c,KMOD_CTRL);const auto p=overview(e,.125f,ops.slots[0].first+256,ops.slots[0].second);in.pointer(p.x,p.y);in.key(SDLK_v,KMOD_CTRL);return true;
        });
        auto withCopy=[&](int dx,int dz) {
            auto out=original;for(const auto& [cell,name]:original)if(int(cell%512)<boxX && int(cell/512)<boxZ)out[cell+size_t(dz)*512+dx]=name;
            return out;
        };
        s.then([&,withCopy](Editor& e,const Input& in) {
            ops.placed=withCopy(256,0);ops.matches(e,"Ctrl+C / Ctrl+V pastes the group at the pointer");
            ops.expect(e.selectedFeatures.cells.size()==size_t(std::count_if(original.begin(),original.end(),[&](const auto& f){return int(f.first%512)<boxX && int(f.first/512)<boxZ;})),"the pasted group is selected");
            in.key(SDLK_z,KMOD_CTRL);return true;
        });
        s.then([&,boxSelect](Editor& e,const Input& in) {ops.placed=original;ops.matches(e,"paste undone");boxSelect(e,in);return true;});
        s.then([&](Editor& e,const Input& in) {
            const auto p=overview(e,.125f,ops.slots[0].first+256,ops.slots[0].second+200);in.pointer(p.x,p.y);in.key(SDLK_d,KMOD_CTRL);return true;
        });
        s.then([&,withCopy](Editor& e,const Input& in) {ops.placed=withCopy(256,200);ops.matches(e,"Ctrl+D duplicates at the pointer");in.key(SDLK_z,KMOD_CTRL);return true;});
        s.then([&,boxSelect](Editor& e,const Input& in) {ops.placed=original;ops.matches(e,"duplicate undone");boxSelect(e,in);return true;});
        s.then([&](Editor&,const Input& in) {in.key(SDLK_x,KMOD_CTRL);return true;});
        s.then([&](Editor& e,const Input& in) {
            ops.placed.clear();for(const auto& [cell,name]:original)if(!(int(cell%512)<boxX && int(cell/512)<boxZ))ops.placed[cell]=name;
            ops.matches(e,"Ctrl+X cuts the group");
            const auto p=overview(e,.125f,ops.slots[0].first,ops.slots[0].second);in.pointer(p.x,p.y);in.key(SDLK_v,KMOD_CTRL);return true;
        });
        s.then([&](Editor& e,const Input& in) {ops.placed=original;ops.matches(e,"Ctrl+V puts the cut group back");in.key(SDLK_DELETE);return true;});
        s.then([&](Editor& e,const Input& in) {
            ops.placed.clear();for(const auto& [cell,name]:original)if(!(int(cell%512)<boxX && int(cell/512)<boxZ))ops.placed[cell]=name;
            ops.matches(e,"Delete removes the selection");in.key(SDLK_z,KMOD_CTRL);return true;
        });
        // A stale selection (one member erased in Erase mode) still drags the rest.
        s.then([&](Editor& e,const Input& in) {
            ops.placed=original;ops.matches(e,"delete undone");
            const auto [x,z]=ops.slots[0];const auto [x2,z2]=ops.slots[1];
            const auto a=focus(e,x*16+8.f,z*16+8.f),b=cellScreen(e,x2*16+8.f,z2*16+8.f);
            mode(in,Editor::MODE_SELECT);in.click(a.x,a.y);holdShift(true);in.click(b.x,b.y);return true;
        });
        s.then([&](Editor& e,const Input& in) {
            holdShift(false);const auto [x,z]=ops.slots[0];const auto a=focus(e,x*16+8.f,z*16+8.f);
            mode(in,Editor::MODE_ERASE);in.click(a.x,a.y);return true;
        });
        s.then([&](Editor& e,const Input& in) {
            const auto [x2,z2]=ops.slots[1];const auto b=focus(e,x2*16+8.f,z2*16+8.f);
            mode(in,Editor::MODE_SELECT);in.drag(b.x,b.y,b.x,b.y+16);return true;
        });
        s.then([&](Editor& e,const Input& in) {
            const auto& map=e.mapView.map();const auto [x2,z2]=ops.slots[1];
            ops.expect(nameAt(map,cellOf(map,x2,z2+1))==ops.refs[1].name,"a selection that lost a member in Erase mode still drags");
            in.key(SDLK_z,KMOD_CTRL);in.key(SDLK_z,KMOD_CTRL);return true;
        });
        ops.script.then([&](Editor& e,const Input&) {ops.placed=original;ops.matches(e,"erase and drag undone");return true;});
        return true;
    });
    // Live symmetry (vertical): each feature placed in the lower left gets its
    // mirror copy; Erase removes both. Then the 4-way kinds on a few.
    s.then([&](Editor& e,const Input& in) {
        in.key(SDLK_m);
        const auto& map=e.mapView.map();const int width=map.width,height=map.height;
        for(size_t i=0;i<ops.refs.size();++i) {
            const int x=8+int(i%28)*8,z=300+int(i/28)*8;
            ops.place(int(i),x,z);
            s.then([&,i,x,z,width,height](Editor& e,const Input& in) {
                const auto& map=e.mapView.map();const auto& ref=ops.refs[i];
                const auto image=cart::mirrorFeature(x,z,ref.footX,ref.footZ,cart::Xf::FlipX,width,height);
                ops.expect(e.liveSymmetry && image && nameAt(map,cellOf(map,image->first,image->second))==ref.name,"live symmetry mirrors '"+ref.name+"'");
                const auto p=focus(e,x*16+8.f,z*16+8.f);mode(in,Editor::MODE_ERASE);in.click(p.x,p.y);
                ops.placed.erase(cellOf(map,x,z));return true;
            });
            ops.verify("live symmetry erases '"+ops.refs[i].name+"' and its mirror");
        }
        // Other kinds: Symmetry menu rows 2..8 choose the kind (the map is square).
        // Sources near the centre: no image of one reaches the populated block.
        for(int kind=1;kind<cart::kMirrorKinds;++kind)for(size_t i=0;i<std::min<size_t>(6,ops.refs.size());++i) {
            const int x=150+int(i)*12,z=200+kind*8;
            if(i==0)s.then([kind](Editor&,const Input& in) {menu(in,4,2+kind);return true;});
            ops.place(int(i),x,z);
            s.then([&,i,x,z,kind,width,height](Editor& e,const Input& in) {
                const auto& map=e.mapView.map();const auto& ref=ops.refs[i];
                for(const auto xf:cart::mirrorImages(cart::Mirror(kind)))
                    if(const auto image=cart::mirrorFeature(x,z,ref.footX,ref.footZ,xf,width,height))
                        ops.expect(nameAt(map,cellOf(map,image->first,image->second))==ref.name,std::string(cart::mirrorLabel(cart::Mirror(kind)))+" mirrors '"+ref.name+"'");
                const auto p=focus(e,x*16+8.f,z*16+8.f);mode(in,Editor::MODE_ERASE);in.click(p.x,p.y);
                ops.placed.erase(cellOf(map,x,z));return true;
            });
            ops.verify(std::string(cart::mirrorLabel(cart::Mirror(kind)))+" erases '"+ops.refs[i].name+"' and its images");
        }
        s.then([](Editor&,const Input& in) {menu(in,4,2);in.key(SDLK_m);return true;});   // vertical again, live off
        s.then([&](Editor& e,const Input& in) {
            ops.expect(!e.liveSymmetry,"live symmetry off");
            // Symmetrize (Shift+M): left half onto the right, terrain left alone.
            in.key(SDLK_m,KMOD_SHIFT);return true;
        });
        s.then([&](Editor& e,const Input& in) {
            if(!ops.expect(e.modal==Editor::M_SYMMETRIZE,"Shift+M opens Symmetrize"))return true;
            for(int i=0;i<2;++i) {in.key(SDLK_TAB);}in.key(SDLK_RIGHT);   // TERRAIN: LEAVE TERRAIN
            in.key(SDLK_RETURN);return true;
        });
        s.then([&](Editor& e,const Input& in) {
            if(!ops.expect(e.modal==Editor::M_CONFIRM,"Symmetrize asks to confirm ("+e.modalError+")"))return true;
            // The same request through the shared function, for the expected result.
            auto copy=e.mapView.map();auto starts=e.scenario.starts;auto scen=e.scen;auto units=e.units;
            cart::SymmetryRequest request;request.terrain=false;
            *report=cart::symmetrize(copy,starts,scen,units,request,e.featureFootprint(),nullptr);
            ops.placed.clear();for(size_t i=0;i<copy.features.size();++i)if(const auto name=nameAt(copy,i);!name.empty())ops.placed[i]=name;
            in.key(SDLK_RETURN);return true;
        });
        s.then([&](Editor& e,const Input& in) {
            ops.matches(e,"Symmetrize mirrors every feature of the left half");
            ops.expect(report->features>0,"Symmetrize reports feature copies ("+report->summary()+")");
            // Every source feature's mirror holds its name, unless it would leave the map.
            const auto& map=e.mapView.map();int mirrored=0;
            for(const auto& [cell,name]:original) {
                const int x=int(cell%map.width),z=int(cell/map.width);const auto* ref=e.features.byName(name);
                if(!ref || x>=map.width/2)continue;
                if(const auto image=cart::mirrorFeature(x,z,ref->footX,ref->footZ,cart::Xf::FlipX,map.width,map.height))
                    mirrored+=nameAt(map,cellOf(map,image->first,image->second))==name;
            }
            ops.expect(mirrored>0,"mirrored copies of the original features");
            in.key(SDLK_z,KMOD_CTRL);return true;
        });
        s.then([&](Editor& e,const Input&) {ops.placed=original;ops.matches(e,"Symmetrize undone in one step");return true;});
        return true;
    });
    // Terrain over features: a stamp with objects protected keeps them, an
    // unprotected one replaces them with the section's own; Clear Area; the
    // terrain clipboard carries them; Resize crops and grows.
    auto stampOver=std::make_shared<tak::tnt::Map>();
    s.then([&,stampOver](Editor& e,const Input& in) {
        int section=-1;
        for(size_t i=0;i<e.sections.list().size() && section<0;++i)if(const auto* m=e.sections.load(e.vfs,e.sections.list()[i].path))
            if(m->blocksX==16 && m->blocksY==16 && std::any_of(m->features.begin(),m->features.end(),[](uint16_t v){return v<0xFFFA;}))section=int(i);
        if(section<0)for(size_t i=0;i<e.sections.list().size() && section<0;++i)if(const auto* m=e.sections.load(e.vfs,e.sections.list()[i].path);m && m->blocksX==16)section=int(i);
        if(!ops.expect(section>=0,"a 16x16 section"))return true;
        tool(in,Editor::TERRAIN);ops.pick.search(in,section,e.sections.list()[size_t(section)].name);
        for(int protect=0;protect<2;++protect) {
            s.then([&,section,protect,stampOver](Editor& e,const Input& in) {
                if(protect==0) {
                    const int clicked=ops.pick.click(e,in);
                    if(!clicked)return false;
                    if(!ops.expect(clicked>0,"section listed"))return true;
                }
                if(protect)menu(in,1,3);   // Edit > Terrain brush: protect objects
                const auto* m=e.sections.load(e.vfs,e.sections.list()[size_t(section)].path);
                *stampOver=e.mapView.map();cart::StampLayers layers;layers.objects=protect==0;
                cart::stampSection(*stampOver,*m,0,0,layers);
                const auto p=overview(e,.125f,16,16);mode(in,Editor::MODE_PLACE);in.click(p.x,p.y);return true;
            });
            s.then([&,protect,stampOver](Editor& e,const Input& in) {
                ops.expect(planeDifference(e.mapView.map(),*stampOver).empty(),protect?"a protected stamp keeps the features under it":"an unprotected stamp replaces the features under it");
                if(protect) {
                    const auto& map=e.mapView.map();
                    for(const auto& [cell,name]:original)if(int(cell%map.width)<32 && int(cell/map.width)<32)
                        ops.expect(nameAt(map,cell)==name,"protected stamp keeps '"+name+"'");
                    menu(in,1,3);
                }
                in.key(SDLK_z,KMOD_CTRL);return true;
            });
            ops.verify(protect?"protected stamp undone":"unprotected stamp undone");
        }
        // Clear Area over the top-left block of features.
        s.then([&](Editor& e,const Input& in) {
            tool(in,Editor::FEATURES);in.key(SDLK_k);
            const auto a=overview(e,.125f,1,1),b=overview(e,.125f,100,40);in.drag(a.x,a.y,b.x,b.y);return true;
        });
        s.then([&](Editor& e,const Input& in) {
            if(!ops.expect(e.modal==Editor::M_CONFIRM && e.mTitle=="CLEAR AREA","Clear Area asks to confirm"))return true;
            in.key(SDLK_RETURN);return true;
        });
        s.then([&](Editor& e,const Input& in) {
            ops.placed.clear();
            for(const auto& [cell,name]:original) {const int x=int(cell%512),z=int(cell/512);if(!(x>=1 && x<=100 && z>=1 && z<=40))ops.placed[cell]=name;}
            ops.matches(e,"Clear Area removes the features in its box");in.key(SDLK_z,KMOD_CTRL);return true;
        });
        // Terrain clipboard: blocks 0..64 x 0..32 copied with their features, pasted at block (0,60).
        s.then([&](Editor& e,const Input& in) {
            ops.placed=original;ops.matches(e,"Clear Area undone");
            tool(in,Editor::TERRAIN);mode(in,Editor::MODE_SELECT);
            const auto a=overview(e,.125f,0,0),b=overview(e,.125f,127,63);in.drag(a.x,a.y,b.x,b.y);in.key(SDLK_c,KMOD_CTRL);in.key(SDLK_v,KMOD_CTRL);return true;
        });
        s.then([&](Editor& e,const Input& in) {
            if(!ops.expect(e.clipboardBrush() && e.terrainClip.map.blocksX==64 && e.terrainClip.map.blocksY==32,"Ctrl+C / Ctrl+V makes a 64x32-block clipboard brush"))return true;
            const auto p=overview(e,.125f,(0+32)*2,(60+16)*2);in.click(p.x,p.y);return true;   // the brush centres on the pointer
        });
        s.then([&](Editor& e,const Input& in) {
            auto want=original;for(const auto& [cell,name]:original)if(int(cell%512)<128 && int(cell/512)<64)want[cell+size_t(120)*512]=name;
            ops.placed=want;ops.matches(e,"the terrain clipboard pastes the features with the terrain");
            in.key(SDLK_z,KMOD_CTRL);in.key(SDLK_ESCAPE);return true;
        });
        // Resize: crop to 4x16 units (features beyond x=128 go), undo; grow to 20x20.
        s.then([&](Editor& e,const Input& in) {ops.placed=original;ops.matches(e,"clipboard paste undone");tool(in,Editor::FEATURES);in.key(SDLK_r);return true;});
        s.then([&](Editor& e,const Input& in) {
            if(!ops.expect(e.modal==Editor::M_RESIZE,"R opens Resize"))return true;
            in.text("4");in.key(SDLK_RETURN);return true;
        });
        s.then([&](Editor& e,const Input& in) {
            if(!ops.expect(e.modal==Editor::M_CONFIRM && e.mTitle=="CROP MAP","a crop asks to confirm"))return true;
            in.key(SDLK_RETURN);return true;
        });
        s.then([&](Editor& e,const Input& in) {
            ops.expect(e.mapView.map().width==128 && e.mapView.map().height==512,"cropped to 128x512 cells");
            ops.placed.clear();for(const auto& [cell,name]:original)if(int(cell%512)<128)ops.placed[cell/512*128+cell%512]=name;
            ops.matches(e,"the crop keeps the features inside and drops the rest");
            in.key(SDLK_z,KMOD_CTRL);return true;
        });
        s.then([&](Editor& e,const Input& in) {
            ops.placed=original;ops.matches(e,"crop undone");
            in.key(SDLK_r);in.text("20");in.key(SDLK_TAB);in.text("20");in.key(SDLK_RETURN);return true;
        });
        s.then([&](Editor& e,const Input& in) {
            ops.expect(e.mapView.map().width==640 && e.mapView.map().height==640,"grown to 640x640 cells");
            ops.placed.clear();for(const auto& [cell,name]:original)ops.placed[cell/512*640+cell%512]=name;
            ops.matches(e,"growing keeps every feature in its cell");
            in.key(SDLK_z,KMOD_CTRL);return true;
        });
        ops.script.then([&](Editor& e,const Input&) {ops.placed=original;ops.matches(e,"growth undone");return true;});
        return true;
    });
    // Analysis with features: the four terrain overlays and Check Map, frame
    // selection and hiding the layer under a selection.
    for(int row=7;row<=10;++row) {
        s.then([row](Editor&,const Input& in) {menu(in,2,row);return true;});
        s.then([&,row](Editor& e,const Input& in) {
            if(e.modal==Editor::M_ANALYZING)return false;
            ops.expect(e.overlayTexture!=nullptr,"View overlay "+std::to_string(row-6)+" built with features");
            if(e.modal!=Editor::M_NONE)in.key(SDLK_RETURN);
            return true;
        });
    }
    s.then([](Editor&,const Input& in) {menu(in,2,11);in.key(SDLK_c);return true;});
    s.then([&](Editor& e,const Input& in) {
        if(e.modal==Editor::M_CHECKING)return false;
        if(!ops.expect(e.modal==Editor::M_ISSUES,"Check Map reports"))return true;
        for(const auto& issue:e.mapIssues)if(issue.message.starts_with("Missing feature definition"))ops.expect(false,"Check Map: "+issue.message);
        in.key(SDLK_ESCAPE);
        const auto [x,z]=ops.slots[0];const auto p=focus(e,x*16+8.f,z*16+8.f);mode(in,Editor::MODE_SELECT);in.click(p.x,p.y);
        menu(in,2,4);return true;   // View > Frame selection
    });
    s.then([&](Editor& e,const Input& in) {
        ops.expect(e.modal==Editor::M_NONE && e.mapView.zoom()<=1,"Frame selection frames the selected feature");
        menu(in,2,14);in.key(SDLK_DELETE);menu(in,2,14);return true;   // hide features, Delete (ignored), show
    });
    s.then([&](Editor& e,const Input&) {ops.matches(e,"a hidden layer's selection is protected");return true;});
    // Test Map's snapshot: the package the game builds from it resolves every feature.
    s.then([&](Editor& e,const Input& in) {
        try {
            const auto path=fs::path(ops.root)/"Test snapshot.kmp";std::string error;
            if(!cart::writeDocumentBundle(path,cart::documentFiles(e.mapView.map(),e.scenario,e.scen,e.units,e.useOnly,{},"Test snapshot"),error))throw std::runtime_error(error);
            const auto package=tak::net::maps::importSnapshot(e.vfs,path);
            tak::hpi::Vfs view(&e.vfs);view.setMapFiles(package->files);
            const auto map=tak::tnt::Map::load(package->files->at(package->mapPath),package->mapPath);
            auto game=std::make_unique<tak::sim::World>();
            game->setTerrain(map.heights,map.width,map.height,map.seaLevel,&map.features);
            tak::sim::registerMapFeatures(*game,map,view,&e.unitRegistry);
            // Names only a downloaded map in Maps/ defines: the install alone does not resolve them either.
            tak::hpi::Vfs installed(&e.vfs,true);auto own=std::make_unique<tak::sim::World>();
            own->setTerrain(map.heights,map.width,map.height,map.seaLevel,&map.features);
            tak::sim::registerMapFeatures(*own,map,installed,&e.unitRegistry);
            const auto& types=game->mapPlacementTypes();const auto& installedTypes=own->mapPlacementTypes();int resolved=0,downloaded=0;
            for(size_t i=0;i<map.featureNames.size();++i) {
                if(i<types.size() && !types[i].name.empty()) {++resolved;continue;}
                if(i>=installedTypes.size() || installedTypes[i].name.empty()) {++downloaded;continue;}
                ops.expect(false,"Test Map snapshot cannot resolve '"+map.featureNames[i]+"'");
            }
            std::cout<<"note: "<<world<<" Test Map snapshot resolves "<<resolved<<" of "<<map.featureNames.size()<<" feature names";
            if(downloaded)std::cout<<"; "<<downloaded<<" are defined only by downloaded maps in Maps/, which a snapshot does not carry";
            std::cout<<'\n';
        } catch(const std::exception& error) {ops.expect(false,std::string("Test Map snapshot: ")+error.what());}
        saveAndReopen(e,in,ops.label,ops.root);return true;
    });
    // Save and reopen, then paste the whole set into a map of another world.
    s.then([&](Editor& e,const Input& in) {
        if(reopened(e,ops.label))ops.matches(e,"saved and reopened features");
        const auto a=overview(e,.0625f,0,0),b=overview(e,.0625f,511,511);tool(in,Editor::FEATURES);mode(in,Editor::MODE_SELECT);
        in.drag(a.x,a.y,b.x,b.y);in.key(SDLK_c,KMOD_CTRL);in.key(SDLK_n,KMOD_CTRL);return true;
    });
    s.then([&](Editor& e,const Input& in) {
        if(!ops.expect(e.modal==Editor::M_NEW && e.selectedFeatures.clipboard.size()==original.size(),"copied every feature ("+std::to_string(e.selectedFeatures.clipboard.size())+
                       " of "+std::to_string(original.size())+"), New Map open (modal "+std::to_string(int(e.modal))+")"))return true;
        for(int i=0;i<3;++i) {in.key(SDLK_TAB);in.key(SDLK_RIGHT);}   // width 16, height 16, the next world
        in.key(SDLK_RETURN);return true;
    });
    s.then([&](Editor& e,const Input& in) {
        if(!ops.expect(e.modal==Editor::M_NONE && e.world!=world,"a new map of another world ("+e.world+")"))return true;
        // The clipboard anchors its lowest cell -- the corner feature at (511,0) -- at the pointer.
        const auto p=overview(e,.0625f,511,0);in.pointer(p.x,p.y);in.key(SDLK_v,KMOD_CTRL);return true;
    });
    s.then([&](Editor& e,const Input& in) {
        capture(in,ops.label+" pasted into "+e.world);
        const auto& map=e.mapView.map();int foreign=0;
        if(!ops.expect(e.selectedFeatures.cells.size()==original.size(),"every feature pasted into the "+e.world+" map ("+std::to_string(e.selectedFeatures.cells.size())+")"))return true;
        for(const int cell:e.selectedFeatures.cells)foreign+=!e.features.byName(nameAt(map,size_t(cell)));
        ops.expect(foreign>0,"features of another world stay in the map (drawn as markers)");
        in.key(SDLK_DELETE);return true;
    });
    s.then([&](Editor& e,const Input&) {
        const auto& map=e.mapView.map();
        ops.expect(std::none_of(map.features.begin(),map.features.end(),[&](uint16_t v){return v<0xFFFA && v<map.featureNames.size();}),"pasted foreign features delete");
        return true;
    });
    const bool ran=drive({"cartographer","--new","16x16","--world",world,"--data",data,"--out",ops.root},[&](Editor& e,const Input& in){return s.frame(e,in);},900,ops.label);
    if(s.at<s.steps.size())check(false,ops.label+": script stopped at step "+std::to_string(s.at)+" of "+std::to_string(s.steps.size()));
    std::error_code ec;fs::remove_all(ops.root,ec);
    if(ran)std::cout<<ops.label<<": "<<ops.refs.size()<<" features through "<<s.steps.size()<<" steps\n";
    return ran;
}

// Very many features: drag-paint a dense field of one feature (one cell per
// motion event), then select it all, drag, delete, copy/paste, symmetrize,
// Check Map, and save/reopen.
bool manyFeaturesSession(const std::string& data,const std::string& world,int side) {
    const auto root=tempRoot("manyfeatures");isolateProfile(root);
    const std::string label=world+" "+std::to_string(side*side)+" features";
    Script s;BrowserPick pick;int feature=-1;std::string name;
    auto field=[&](int dx,int dz) {std::map<size_t,std::string> out;for(int z=0;z<side;++z)for(int x=0;x<side;++x)out[size_t(z+4+dz)*1024+x+4+dx]=name;return out;};
    auto matches=[&](const Editor& e,const std::map<size_t,std::string>& want,const std::string& what) {
        const auto& map=e.mapView.map();size_t named=0;bool ok=true;
        for(size_t i=0;i<map.features.size();++i)if(!nameAt(map,i).empty())++named;
        for(const auto& [cell,n]:want)if(nameAt(map,cell)!=n) {ok=false;break;}
        return check(ok && named==want.size(),label+": "+what+" ("+std::to_string(named)+" on the map, "+std::to_string(want.size())+" expected)");
    };
    s.then([&](Editor& e,const Input& in) {
        const auto& refs=e.features.list();
        for(size_t i=0;i<refs.size() && feature<0;++i)if(refs[i].footX==1 && refs[i].footZ==1)feature=int(i);
        if(feature<0)feature=0;
        name=refs[size_t(feature)].name;tool(in,Editor::FEATURES);pick.search(in,feature,name);return true;
    });
    s.then([&](Editor& e,const Input& in) {
        const int clicked=pick.click(e,in);if(!clicked)return false;
        if(!check(clicked>0,label+": feature listed"))return true;
        mode(in,Editor::MODE_PLACE);const auto p=overview(e,.125f,4,4);in.button(SDL_MOUSEBUTTONDOWN,p.x,p.y,SDL_BUTTON_LEFT,1);return true;
    });
    for(int band=0;band<side;band+=20)s.then([&,band](Editor& e,const Input& in) {   // ~20 rows of motion events a frame
        for(int z=band;z<std::min(side,band+20);++z)for(int x=0;x<side;++x) {const auto p=overview(e,.125f,x+4,z+4);in.motion(p.x,p.y,SDL_BUTTON_LMASK);}
        if(band+20>=side) {const auto p=overview(e,.125f,side+3,side+3);in.button(SDL_MOUSEBUTTONUP,p.x,p.y,SDL_BUTTON_LEFT,1);}
        return true;
    });
    s.then([&](Editor& e,const Input& in) {
        matches(e,field(0,0),"drag-painting fills every cell");
        const auto a=overview(e,.125f,2,2),b=overview(e,.125f,side+6,side+6);mode(in,Editor::MODE_SELECT);in.drag(a.x,a.y,b.x,b.y);return true;
    });
    s.then([&](Editor& e,const Input& in) {
        check(int(e.selectedFeatures.cells.size())==side*side,label+": box select takes them all");
        const auto a=overview(e,.125f,4,4),b=overview(e,.125f,5,5);in.drag(a.x,a.y,b.x,b.y);return true;
    });
    s.then([&](Editor& e,const Input& in) {matches(e,field(1,1),"the whole field drags by (1,1)");in.key(SDLK_z,KMOD_CTRL);return true;});
    s.then([&](Editor& e,const Input& in) {
        matches(e,field(0,0),"field drag undone");
        const auto a=overview(e,.125f,2,2),b=overview(e,.125f,side+6,side+6);in.drag(a.x,a.y,b.x,b.y);
        in.key(SDLK_c,KMOD_CTRL);const auto p=overview(e,.125f,side+20,4);in.pointer(p.x,p.y);in.key(SDLK_v,KMOD_CTRL);return true;
    });
    s.then([&](Editor& e,const Input& in) {
        auto want=field(0,0);for(const auto& [cell,n]:field(side+16,0))want[cell]=n;
        matches(e,want,"copy/paste of the whole field");in.key(SDLK_DELETE);return true;
    });
    s.then([&](Editor& e,const Input& in) {matches(e,field(0,0),"Delete removes the pasted field");in.key(SDLK_m,KMOD_SHIFT);return true;});
    s.then([&](Editor& e,const Input& in) {
        if(!check(e.modal==Editor::M_SYMMETRIZE,label+": Symmetrize opens"))return true;
        for(int i=0;i<2;++i) {in.key(SDLK_TAB);}in.key(SDLK_RIGHT);in.key(SDLK_RETURN);in.key(SDLK_RETURN);return true;
    });
    s.then([&](Editor& e,const Input& in) {
        auto want=field(0,0);for(const auto& [cell,n]:field(0,0))want[cell/1024*1024+(1023-cell%1024)]=n;
        matches(e,want,"Symmetrize mirrors the field");in.key(SDLK_z,KMOD_CTRL);in.key(SDLK_c);return true;
    });
    s.then([&](Editor& e,const Input& in) {
        if(e.modal==Editor::M_CHECKING)return false;
        check(e.modal==Editor::M_ISSUES,label+": Check Map finishes");in.key(SDLK_ESCAPE);
        saveAndReopen(e,in,label,root);return true;
    });
    s.then([&](Editor& e,const Input&) {if(reopened(e,label))matches(e,field(0,0),"saved and reopened field");return true;});
    const auto began=std::chrono::steady_clock::now();
    const bool ran=drive({"cartographer","--new","32x32","--world",world,"--data",data,"--out",root},[&](Editor& e,const Input& in){return s.frame(e,in);},900,label);
    if(s.at<s.steps.size())check(false,label+": script stopped at step "+std::to_string(s.at));
    std::error_code ec;fs::remove_all(root,ec);
    std::cout<<label<<": "<<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-began).count()<<" ms\n";
    return ran;
}

// Random scatter: a generated map's trees, rocks and mana, all selected,
// deleted, restored, symmetrized, checked, saved and reopened.
bool generatedFeaturesSession(const std::string& data,const std::string& world) {
    const auto root=tempRoot("generated");isolateProfile(root);
    const std::string label=world+" generated features";
    Script s;std::map<size_t,std::string> original;
    auto matches=[&](const Editor& e,const std::string& what) {
        const auto& map=e.mapView.map();std::map<size_t,std::string> now;
        for(size_t i=0;i<map.features.size();++i)if(const auto n=nameAt(map,i);!n.empty())now[i]=n;
        return check(now==original,label+": "+what+" ("+std::to_string(now.size())+" of "+std::to_string(original.size())+")");
    };
    s.then([&](Editor& e,const Input& in) {
        const auto& map=e.mapView.map();for(size_t i=0;i<map.features.size();++i)if(const auto n=nameAt(map,i);!n.empty())original[i]=n;
        check(!original.empty(),label+": the generator scatters features");
        tool(in,Editor::FEATURES);mode(in,Editor::MODE_SELECT);
        // Start the box on a cell without a feature.
        int x0=0;while(x0<16 && !nameAt(map,size_t(x0)).empty())++x0;
        const auto a=overview(e,.0625f,x0,0),b=overview(e,.0625f,map.width-1,map.height-1);in.drag(a.x,a.y,b.x,b.y);return true;
    });
    s.then([&](Editor& e,const Input& in) {
        check(e.selectedFeatures.cells.size()>=original.size()-16,label+": box select takes the scatter ("+std::to_string(e.selectedFeatures.cells.size())+")");
        in.key(SDLK_DELETE);return true;
    });
    s.then([&](Editor& e,const Input& in) {
        const auto& map=e.mapView.map();size_t left=0;for(size_t i=0;i<map.features.size();++i)left+=!nameAt(map,i).empty();
        check(left+e.selectedFeatures.cells.size()<=original.size() && left<=16,label+": Delete removes the scatter");
        in.key(SDLK_z,KMOD_CTRL);return true;
    });
    s.then([&](Editor& e,const Input& in) {matches(e,"undo restores the scatter");in.key(SDLK_m,KMOD_SHIFT);return true;});
    s.then([&](Editor& e,const Input& in) {
        if(!check(e.modal==Editor::M_SYMMETRIZE,label+": Symmetrize opens"))return true;
        in.key(SDLK_RETURN);in.key(SDLK_RETURN);return true;   // mirror section twins too
    });
    s.then([&](Editor& e,const Input& in) {check(e.modal==Editor::M_NONE || e.modal==Editor::M_MESSAGE,label+": Symmetrize applied");in.key(SDLK_RETURN);in.key(SDLK_z,KMOD_CTRL);in.key(SDLK_c);return true;});
    s.then([&](Editor& e,const Input& in) {
        if(e.modal==Editor::M_CHECKING)return false;
        matches(e,"Symmetrize undone");check(e.modal==Editor::M_ISSUES,label+": Check Map finishes");in.key(SDLK_ESCAPE);
        saveAndReopen(e,in,label,root);return true;
    });
    s.then([&](Editor& e,const Input&) {if(reopened(e,label))matches(e,"saved and reopened scatter");return true;});
    const bool ran=drive({"cartographer","--new","16x16","--world",world,"--random","--data",data,"--out",root},[&](Editor& e,const Input& in){return s.frame(e,in);},600,label);
    if(s.at<s.steps.size())check(false,label+": script stopped at step "+std::to_string(s.at));
    std::error_code ec;fs::remove_all(root,ec);
    return ran;
}

int featureOpsWorkflow(const std::string& data,std::vector<std::string> worlds) {
    if(worlds.empty())worlds=kWorlds;
    for(const auto& world:worlds) {
        featureOpsSession(data,world);
        generatedFeaturesSession(data,world);
        manyFeaturesSession(data,world,world==worlds.front()?240:100);
    }
    if(!failures.empty())return 1;
    std::cout<<"PASS: feature operations (select, drag, clipboard, delete, symmetry, terrain, Clear Area, resize, overlays, Check Map, Test Map snapshot, save/reopen, cross-world paste, generated scatter, dense fields) on "<<worlds.size()<<" worlds\n";
    return 0;
}

// ---- blocked cells: the BLOCKED tool -----------------------------------------

// The terrain file inside a saved .kmp.
std::optional<tak::tnt::Map> savedTerrain(const fs::path& kmp) {
    try {
        tak::hpi::Archive archive(kmp);
        for(const auto& entry:archive.entries())
            if(!entry.isDirectory && lowerText(entry.path).ends_with(".tnt"))return tak::tnt::Map::load(archive.read(entry),entry.path);
    } catch(const std::exception&) {}
    return std::nullopt;
}

// A flat Aramon map from the New Map dialog with one feature, then the BLOCKED
// tool through the real input path: the overlay, a Place click, a 4x4 brush from
// its browser row dragged across the feature in two motion events, a Shift+drag
// rectangle, an Erase stroke, undo and redo of each, save/reopen (minimaps kept),
// live symmetry, [ and ], and B / View > Show blocked cells.
int blockedWorkflow(const std::string& data) {
    const auto root=tempRoot("blocked");isolateProfile(root);
    const std::string label="blocked cells";
    Script s;BrowserPick pick;
    tak::tnt::Map expected,beforePaint,reopenedMap;   // the expectation; the save made before painting; the reopened document
    std::vector<std::vector<uint16_t>> states;        // the plane before painting and after each painting step
    size_t featureCell=0;std::string featureName;int player=0;
    auto expect=[&](bool ok,const std::string& what) {return check(ok,label+": "+what);};
    auto same=[&](const Editor& e,const std::string& what) {
        const auto d=planeDifference(e.mapView.map(),expected);
        return expect(d.empty(),what+(d.empty()?"":": "+d));
    };
    auto kmp=[&](const Editor& e) {return fs::path(root)/(e.mapName+".kmp");};
    const int brushes=int(cart::kBlockedBrushSizes.size());
    s.then([&](Editor& e,const Input& in) {   // a bare launch: no document (past the first-run welcome); Ctrl+N
        if(e.modal==Editor::M_MESSAGE) {in.key(SDLK_RETURN);return false;}
        expect(e.modal==Editor::M_NONE && e.noDocument,"a bare launch opens with no document");
        in.key(SDLK_n,KMOD_CTRL);return true;
    });
    s.then([&](Editor& e,const Input& in) {   // New Map: CREATE a flat Aramon map
        if(!expect(e.modal==Editor::M_NEW,"New Map dialog opens"))return true;
        const auto& worlds=e.kWorldOpts;
        const int from=int(std::find(worlds.begin(),worlds.end(),e.mf[3])-worlds.begin());
        const int to=int(std::find(worlds.begin(),worlds.end(),std::string("aramon"))-worlds.begin());
        for(int i=0;i<3;++i)in.key(SDLK_TAB);
        for(int i=0;i<(to-from+int(worlds.size()))%int(worlds.size());++i)in.key(SDLK_RIGHT);
        in.key(SDLK_RETURN);return true;
    });
    s.then([&](Editor& e,const Input& in) {   // one feature, by a browser pick and a click
        if(!expect(e.modal==Editor::M_NONE && e.world=="aramon" && !e.features.list().empty(),"a new flat Aramon map with features"))return true;
        const auto& plane=e.mapView.map().features;
        expect(std::all_of(plane.begin(),plane.end(),[](uint16_t v){return v==0xFFFF;}),"a new flat map has no blocked cells");
        featureName=e.features.list().front().name;tool(in,Editor::FEATURES);pick.search(in,0,featureName);return true;
    });
    s.then([&](Editor& e,const Input& in) {
        const int clicked=pick.click(e,in);
        if(!clicked)return false;
        if(!expect(clicked>0,"the features browser lists '"+featureName+"'"))return true;
        const auto p=overview(e,1,20,20);mode(in,Editor::MODE_PLACE);in.click(p.x,p.y);
        featureCell=cellOf(e.mapView.map(),20,20);
        // The feature search stays set: BLOCKED's brush list must ignore it (checked below).
        return true;
    });
    s.then([&](Editor& e,const Input& in) {   // save before painting: its minimaps are the reference
        expect(nameAt(e.mapView.map(),featureCell)==featureName,"the feature is placed");
        in.key(SDLK_s,KMOD_CTRL);in.key(SDLK_RETURN);return true;
    });
    s.then([&](Editor& e,const Input& in) {
        const auto saved=savedTerrain(kmp(e));
        if(!expect(saved && !saved->minimap.empty() && !saved->overview.empty(),"the map saves before painting"))return true;
        beforePaint=*saved;expected=e.mapView.map();states={expected.features};
        tool(in,Editor::BLOCKED);mode(in,Editor::MODE_PLACE);return true;
    });
    s.then([&](Editor& e,const Input& in) {   // the overlay is on with the tool; a Place click
        expect(e.tool==Editor::BLOCKED && e.editMode==Editor::MODE_PLACE,"the BLOCKED tool in Place mode");
        expect(!e.preferences.showBlocked && e.blockedShown() && e.blockedTexture!=nullptr,"the blocked-cell overlay is on while the tool is chosen");
        expect(int(e.paletteItems.size())==brushes && e.blockedBrush==0,"the browser lists the brush sizes, 1 x 1 chosen");
        const auto p=overview(e,1,5,15);in.click(p.x,p.y);cart::applyBlockers(expected,5,15,5,15,cart::BlockOp::Paint);
        return true;
    });
    s.then([&](Editor& e,const Input& in) {
        same(e,"a Place click blocks the cell under the pointer");states.push_back(expected.features);
        expect(e.mapView.map().features[cellOf(e.mapView.map(),5,15)]==0xFFFC && e.blockedCount==1,"the cell holds the blocker marker, and is counted");
        const auto row=paletteRow(e,2);
        if(!expect(row.has_value(),"the 4 x 4 brush row is listed"))return true;
        in.click(row->x,row->y);return true;
    });
    s.then([&](Editor& e,const Input& in) {   // twenty cells in two motion events, across the feature
        expect(e.blockedBrush==2,"a browser row chooses the 4 x 4 brush");
        const auto a=overview(e,1,10,20),b=overview(e,1,30,20);in.drag(a.x,a.y,b.x,b.y);
        cart::applyBlockerLine(expected,10,20,30,20,4,cart::BlockOp::Paint);return true;
    });
    s.then([&](Editor& e,const Input& in) {
        same(e,"a 4 x 4 drag paints a gap-free band");states.push_back(expected.features);
        expect(nameAt(e.mapView.map(),featureCell)==featureName,"painting leaves the feature in its cell");
        const auto a=overview(e,1,5,26),b=overview(e,1,15,30);holdShift(true);in.drag(a.x,a.y,b.x,b.y);
        cart::applyBlockers(expected,5,26,15,30,cart::BlockOp::Paint);return true;
    });
    s.then([&](Editor& e,const Input& in) {
        holdShift(false);same(e,"Shift+drag fills the rectangle");states.push_back(expected.features);
        const auto a=overview(e,1,12,20),b=overview(e,1,24,20);mode(in,Editor::MODE_ERASE);in.drag(a.x,a.y,b.x,b.y);
        cart::applyBlockerLine(expected,12,20,24,20,4,cart::BlockOp::Erase);return true;
    });
    s.then([&](Editor& e,const Input& in) {
        same(e,"an Erase drag clears blocked cells only");states.push_back(expected.features);
        expect(nameAt(e.mapView.map(),featureCell)==featureName,"erasing leaves the feature in its cell");
        in.key(SDLK_z,KMOD_CTRL);return true;
    });
    for(int step=3;step>=0;--step)s.then([&,step](Editor& e,const Input& in) {   // back through states 3..0, then forward again
        expect(e.mapView.map().features==states[size_t(step)],"undo restores the blocked cells before step "+std::to_string(step+1));
        if(step)in.key(SDLK_z,KMOD_CTRL);else in.key(SDLK_y,KMOD_CTRL);
        return true;
    });
    for(int step=1;step<=4;++step)s.then([&,step](Editor& e,const Input& in) {
        expect(e.mapView.map().features==states[size_t(step)],"redo repeats painting step "+std::to_string(step));
        if(step<4)in.key(SDLK_y,KMOD_CTRL);else saveAndReopen(e,in,label,root);
        return true;
    });
    s.then([&](Editor& e,const Input& in) {   // the round trip keeps the plane; blocked cells leave the minimaps alone
        if(!reopened(e,label))return true;
        same(e,"saved and reopened blocked cells");
        const auto& map=e.mapView.map();
        expect(map.minimap==beforePaint.minimap && map.overview==beforePaint.overview,"the saved minimaps match the save made before painting");
        expect(!e.edited && e.tool==Editor::BLOCKED,"a reopened map is unedited");
        reopenedMap=map;player=e.currentPlayer;
        for(int i=0;i<brushes+1;++i)in.key(SDLK_RIGHTBRACKET);
        return true;
    });
    s.then([&](Editor& e,const Input& in) {   // [ and ]: brush sizes, clamped, not the placement player
        expect(e.blockedBrush==brushes-1 && e.currentPlayer==player,"] steps up to the largest brush and leaves the player");
        for(int i=0;i<brushes+1;++i)in.key(SDLK_LEFTBRACKET);
        return true;
    });
    s.then([&](Editor& e,const Input& in) {   // live symmetry: a click blocks the mirror cell too
        expect(e.blockedBrush==0 && e.currentPlayer==player,"[ steps down to 1 x 1 and leaves the player");
        in.key(SDLK_m);mode(in,Editor::MODE_PLACE);
        const auto p=overview(e,1,3,33);in.click(p.x,p.y);in.key(SDLK_m);
        const int width=e.mapView.map().width;
        cart::applyBlockers(expected,3,33,3,33,cart::BlockOp::Paint);cart::applyBlockers(expected,width-1-3,33,width-1-3,33,cart::BlockOp::Paint);
        return true;
    });
    s.then([&](Editor& e,const Input& in) {   // a blocked edit changes no tiles: the next save keeps the loaded minimaps
        same(e,"live symmetry blocks the mirror cell");
        expect(!e.liveSymmetry && !e.edited,"blocked cells do not mark the terrain edited");
        in.key(SDLK_s,KMOD_CTRL);in.key(SDLK_RETURN);return true;
    });
    s.then([&](Editor& e,const Input& in) {
        const auto saved=savedTerrain(kmp(e));
        if(expect(saved.has_value(),"the painted map saves")) {
            expect(saved->features==e.mapView.map().features,"the saved plane holds the new blocked cells");
            expect(saved->minimap==reopenedMap.minimap && saved->overview==reopenedMap.overview,"the minimaps are saved unchanged");
        }
        in.key(SDLK_b);return true;
    });
    s.then([&](Editor& e,const Input& in) {   // B: blocked cells with every tool, remembered
        expect(e.preferences.showBlocked,"B shows blocked cells with every tool");
        tool(in,Editor::TERRAIN);return true;
    });
    s.then([&](Editor& e,const Input& in) {
        expect(e.tool==Editor::TERRAIN && e.blockedShown(),"the overlay stays with the Terrain tool");
        std::ifstream file(fs::path(root)/"TAKengine"/"Cartographer"/"preferences.txt");const std::string text{std::istreambuf_iterator<char>(file),{}};
        expect(text.find("blocked 1")!=std::string::npos,"the setting is saved with the editor preferences");
        in.key(SDLK_b);return true;
    });
    s.then([&](Editor& e,const Input& in) {
        expect(!e.preferences.showBlocked && !e.blockedShown(),"B again hides it outside the BLOCKED tool");
        menu(in,2,19);return true;
    });
    s.then([&](Editor& e,const Input&) {
        expect(e.preferences.showBlocked && e.blockedShown(),"View > Show blocked cells shows it");
        return true;
    });
    const bool ran=drive({"cartographer","--data",data,"--out",root},[&](Editor& e,const Input& in){return s.frame(e,in);},300,label);
    if(s.at<s.steps.size())check(false,label+": script stopped at step "+std::to_string(s.at)+" of "+std::to_string(s.steps.size()));
    std::error_code ec;fs::remove_all(root,ec);
    if(!ran || !failures.empty())return 1;
    std::cout<<"PASS: the BLOCKED tool paints, erases, fills rectangles, undoes, mirrors and survives save/reopen with the minimaps unchanged\n";
    return 0;
}

// ---- smoke: every browser tab, tool, mode, menu row and shortcut ---------------

// Waits for background jobs, then closes whatever a step left open (a message
// with Return, anything else with Escape). Returns true once nothing is open.
bool settle(Editor& e,const Input& in) {
    if(e.modal==Editor::M_ANALYZING || e.modal==Editor::M_CHECKING || e.modal==Editor::M_MODEL_LOADING || e.modal==Editor::M_GENERATING)return false;
    if(e.menuOpen>=0 || e.sectionMenuOpen || e.paletteFocus) {in.key(SDLK_ESCAPE);return false;}
    if(e.modal==Editor::M_MESSAGE) {in.key(SDLK_RETURN);return false;}
    if(e.modal!=Editor::M_NONE || e.scriptOpen || e.aiOpen || e.useOnlyOpen || e.regionsOpen || e.regionCanvas || e.clearArm) {
        if(e.clearArm)in.key(SDLK_k);else in.key(SDLK_ESCAPE);
        return false;
    }
    return true;
}

bool smokeSession(const std::string& data,const std::string& label,std::vector<std::string> args) {
    const auto root=tempRoot("smoke");isolateProfile(root);
    args.insert(args.begin(),"cartographer");args.insert(args.end(),{"--data",data,"--out",root});
    Script s;int actions=0;
    // One action a frame, then settle; `act` may push any input.
    auto action=[&s,&actions](std::function<void(Editor&,const Input&)> act) {
        s.then([act,&actions](Editor& e,const Input& in) {act(e,in);++actions;return true;});
        s.then(settle);
    };
    auto canvas=[](const Editor& e,int dx=0,int dy=0) {return SDL_Point{Editor::kPaletteW+e.canvasW/3+dx,Editor::kMenuH+e.canvasH/2+dy};};
    auto rect=[](const SDL_Rect& r) {return SDL_Point{r.x+r.w/2,r.y+r.h/2};};
    s.then(settle);
    s.then([&,canvas,rect,action](Editor& e,const Input&) {   // built here: menu rows and group counts come from the editor
        // Every tool with every mode: click, drag, right-drag pan, wheel zoom, undo.
        for(int t=0;t<Editor::kToolCount;++t)for(int m=0;m<4;++m)action([=](Editor& e,const Input& in) {
            tool(in,Editor::Tool(t));mode(in,Editor::EditMode(m));
            const auto p=canvas(e);in.click(p.x,p.y);in.drag(p.x,p.y,p.x+40,p.y+24);
            in.button(SDL_MOUSEBUTTONDOWN,p.x,p.y,SDL_BUTTON_RIGHT,1);in.motion(p.x-30,p.y-20,SDL_BUTTON_RMASK);in.button(SDL_MOUSEBUTTONUP,p.x-30,p.y-20,SDL_BUTTON_RIGHT,1);
            in.pointer(p.x,p.y);SDL_Event wheel{};wheel.type=SDL_MOUSEWHEEL;wheel.wheel.y=1;SDL_PushEvent(&wheel);wheel.wheel.y=-1;SDL_PushEvent(&wheel);
            in.key(SDLK_z,KMOD_CTRL);
        });
        action([=](Editor& e,const Input& in) {   // the REGIONS canvas: draw, select, move
            regions(in);mode(in,Editor::MODE_PLACE);const auto p=canvas(e);in.drag(p.x,p.y,p.x+50,p.y+30);
            mode(in,Editor::MODE_SELECT);in.drag(p.x+20,p.y+10,p.x+60,p.y+40);
        });
        // Every browser: search, each category (or terrain group) and faction, scroll, rows.
        for(int t=0;t<Editor::kToolCount;++t) {
            action([=](Editor&,const Input& in) {tool(in,Editor::Tool(t));in.click(60,Editor::kMenuH+12);in.text("a");in.key(SDLK_BACKSPACE);in.key(SDLK_ESCAPE);});
            if(t==Editor::TERRAIN)for(int g=0;g<int(e.sectionCatalog.groups().size()) || (g==0 && e.sectionCatalog.groups().empty());++g) {
                s.then([](Editor&,const Input& in) {tool(in,Editor::TERRAIN);in.click(60,Editor::kMenuH+40);return true;});
                s.then([=,&actions](Editor& e,const Input& in) {
                    bool hit=false;
                    if(e.sectionMenuOpen)for(int y=Editor::kMenuH+55;y<e.h-Editor::kStatusH && !hit;y+=2)if(e.sectionMenuRowAt(40,y)==g) {in.click(40,y);hit=true;}
                    if(!hit)in.key(SDLK_ESCAPE);   // scrolled out of the list
                    ++actions;return true;
                });
                s.then(settle);
            }
            else for(int c=0;c<=40;++c)s.then([=,&actions](Editor& e,const Input& in) {
                if(c<=int(e.paletteCategories.size())) {tool(in,Editor::Tool(t));in.click(60,Editor::kMenuH+40);++actions;}
                return true;
            });
            if(t==Editor::UNITS)for(int f=0;f<6;++f)action([](Editor&,const Input& in) {in.click(60,Editor::kMenuH+62);});
            action([=](Editor&,const Input& in) {
                tool(in,Editor::Tool(t));in.pointer(Editor::kPaletteW/2,Editor::kPaletteTop+60);
                for(int i=0;i<60;++i) {SDL_Event wheel{};wheel.type=SDL_MOUSEWHEEL;wheel.wheel.y=-2;SDL_PushEvent(&wheel);}
            });
            action([](Editor& e,const Input& in) {
                for(int row=0;row<4;++row)if(const auto p=paletteRow(e,e.paletteRowAt(10,Editor::kPaletteTop+4)+row))in.click(p->x,p->y);
                in.pointer(Editor::kPaletteW/2,Editor::kPaletteTop+60);
                for(int i=0;i<80;++i) {SDL_Event wheel{};wheel.type=SDL_MOUSEWHEEL;wheel.wheel.y=2;SDL_PushEvent(&wheel);}
            });
            action([](Editor& e,const Input& in) {for(int row=0;row<6;++row)if(const auto p=paletteRow(e,row))in.click(p->x,p->y);});
        }
        // Every menu row but File > Test Map, New/Open AI mod and Exit (Test Map starts the
        // game; an AI mod leaves the map, cartographer_aimod_ui covers it).
        for(int m=0;m<int(e.menuRows.size());++m)for(int r=0;r<int(e.menuRows[size_t(m)].size());++r) {
            if(m==0 && r>=5)continue;
            action([=](Editor&,const Input& in) {menu(in,m,r);});
        }
        // Shortcuts.
        for(const auto& [key,mod]:std::vector<std::pair<SDL_Keycode,Uint16>>{
                {SDLK_g,0},{SDLK_g,0},{SDLK_TAB,0},{SDLK_TAB,0},{SDLK_TAB,0},{SDLK_TAB,0},{SDLK_1,0},{SDLK_2,0},{SDLK_3,0},{SDLK_4,0},{SDLK_5,0},
                {SDLK_LEFTBRACKET,0},{SDLK_RIGHTBRACKET,0},{SDLK_k,0},{SDLK_p,0},{SDLK_r,0},{SDLK_u,0},{SDLK_c,0},{SDLK_t,0},{SDLK_m,0},{SDLK_m,0},
                {SDLK_m,KMOD_SHIFT},{SDLK_a,0},{SDLK_F6,0},{SDLK_l,KMOD_CTRL},{SDLK_l,KMOD_CTRL},{SDLK_f,KMOD_CTRL},{SDLK_z,KMOD_CTRL},{SDLK_y,KMOD_CTRL},
                {SDLK_o,KMOD_CTRL},{SDLK_n,0},{SDLK_s,KMOD_CTRL|KMOD_SHIFT},{SDLK_b,KMOD_CTRL},{SDLK_s,KMOD_CTRL}})
            action([key=key,mod=mod](Editor&,const Input& in) {in.key(key,mod);});
        // The scripting overlay's buttons (pickers they open take a row or close).
        s.then([](Editor&,const Input& in) {in.key(SDLK_t);return true;});
        for(int b=0;b<12;++b) {
            s.then([=,&actions](Editor& e,const Input& in) {
                if(!e.scriptOpen) {in.key(SDLK_t);return true;}
                const SDL_Rect buttons[]={e.rAddRule,e.rAddCond,e.rPickList,e.rAddAct,e.rPickList,e.rNameRule,e.rNextP,e.rPrevP,e.rDelCond,e.rDelAct,e.rDelRule,e.rTemplates};
                const auto p=rect(buttons[b]);if(buttons[b].w>0)in.click(p.x,p.y);++actions;return true;
            });
            s.then([](Editor& e,const Input& in) {if(e.modal!=Editor::M_NONE || (e.pickOpen && e.pickTemplate))in.key(SDLK_ESCAPE);return true;});
        }
        s.then(settle);
        // The AI Profiles window's controls (confirmations and Start from close again).
        s.then([](Editor&,const Input& in) {in.key(SDLK_a);return true;});
        // The game-mod dialogs and the group-rule word picker open and close with Escape: nothing is written into the install.
        for(int b=0;b<25;++b) {
            s.then([=,&actions](Editor& e,const Input& in) {
                if(!e.aiOpen) {in.key(SDLK_a);return true;}
                const SDL_Rect buttons[]={e.aiTabRects[1],e.aiSideBox,e.aiGroupBox,e.aiSetOnlyBox,e.aiUnbuildableBox,e.aiPresetRects[0],e.aiPresetRects[1],
                    e.aiViewButton,e.aiViewButton,e.aiCheckButton,e.aiCheckButton,e.aiPackButton,e.aiModSaveButton,e.aiModOpenButton,e.aiTabRects[0],e.aiOwnRadio,
                    e.aiStartButton,e.aiGroupsButton,e.aiAddGroupButton,e.aiGroupsButton,e.aiPresetRects[2],e.aiSearchBox,e.aiStandardRadio,e.aiList,e.aiDoneButton};
                const auto p=rect(buttons[b]);if(buttons[b].w>0)in.click(p.x,p.y);++actions;return true;
            });
            s.then([](Editor& e,const Input& in) {if(e.modal!=Editor::M_NONE || e.aiStartOpen || e.aiModSaveOpen || e.aiModOpenOpen || e.aiWordPick!=-2)in.key(SDLK_ESCAPE);return true;});
        }
        s.then(settle);
        // The regions list and Use Only.
        s.then([](Editor&,const Input& in) {menu(in,3,6);return true;});
        for(int b=0;b<5;++b)s.then([=,&actions](Editor& e,const Input& in) {
            if(e.modal==Editor::M_REGION) {in.key(SDLK_RETURN);return true;}
            if(!e.regionsOpen) {menu(in,3,6);return true;}
            const SDL_Rect buttons[]={e.regionNew,e.regionEdit,e.regionList,e.regionDelete,e.regionOnMap};
            const auto p=rect(buttons[b]);if(buttons[b].w>0)in.click(p.x,b==2?buttons[b].y+5:p.y);++actions;return true;
        });
        s.then(settle);
        s.then([](Editor&,const Input& in) {in.key(SDLK_u);return true;});
        s.then([&,rect](Editor& e,const Input& in) {
            if(e.useOnlyOpen) {in.click(rect(e.uoList).x,e.uoList.y+3);in.click(rect(e.uoClear).x,rect(e.uoClear).y);in.click(rect(e.uoDone).x,rect(e.uoDone).y);}
            ++actions;return true;
        });
        s.then(settle);
        // File > Exit; a dirty map asks, DISCARD ends the editor.
        s.then([](Editor&,const Input& in) {menu(in,0,8);return true;});
        s.then([](Editor& e,const Input& in) {   // the quit request arrives a frame after the menu pushes it
            if(e.modal!=Editor::M_QUITSAVE)return !e.running;
            in.click(e.mQuit.x+e.mQuit.w/2,e.mQuit.y+e.mQuit.h/2);return true;
        });
        s.then([](Editor& e,const Input&) {return !e.running;});   // the editor stops on its own
        return true;
    });
    size_t lastStep=SIZE_MAX;int stalled=0;
    const bool ran=drive(args,[&](Editor& e,const Input& in) {
        if(!std::isfinite(e.mapView.offX()) || !std::isfinite(e.mapView.offY()) || !std::isfinite(e.mapView.zoom()))check(false,label+": view went non-finite");
        stalled=s.at==lastStep?stalled+1:0;lastStep=s.at;
        if(stalled>400) {   // a dialog or overlay that does not close
            check(false,label+": stuck at step "+std::to_string(s.at)+" (modal "+std::to_string(int(e.modal))+", menu "+std::to_string(e.menuOpen)+
                  ", script "+std::to_string(e.scriptOpen)+"/"+std::to_string(e.pickOpen)+", useonly "+std::to_string(e.useOnlyOpen)+", regions "+std::to_string(e.regionsOpen)+
                  "/"+std::to_string(e.regionCanvas)+", clear "+std::to_string(e.clearArm)+", focus "+std::to_string(e.paletteFocus)+", groups "+std::to_string(e.sectionMenuOpen)+")");
            return false;
        }
        if(!s.frame(e,in))return false;
        return e.running || s.at+1<s.steps.size();
    },600,label);
    check(s.at+1>=s.steps.size(),label+": smoke stopped at step "+std::to_string(s.at)+" of "+std::to_string(s.steps.size()));
    std::error_code ec;fs::remove_all(root,ec);
    if(ran)std::cout<<"ok: "<<label<<" ("<<actions<<" actions)\n";
    return ran;
}

int smokeWorkflow(const std::string& data,std::vector<std::string> worlds) {
    if(worlds.empty())worlds=kWorlds;
    const auto shipped=mapsByWorld(data);
    for(const auto& world:worlds) {
        smokeSession(data,world+" blank map smoke",{"--new","8x8","--world",world});
        if(const auto found=shipped.find(world);found!=shipped.end())smokeSession(data,world+" map '"+found->second+"' smoke",{found->second});
    }
    if(!failures.empty())return 1;
    std::cout<<"PASS: every tool, mode, browser tab, menu row and shortcut on blank and shipped maps of "<<worlds.size()<<" worlds\n";
    return 0;
}

} // namespace

int main(int argc,char** argv) {
    if(argc<3)return 2;
    const std::string data=argv[1],mode=argv[2];
    if(data.empty() || !tak::hpi::validInstall(tak::u8path(data),nullptr)) {std::cout<<"SKIP: no game data at '"<<data<<"'\n";return 77;}
    std::vector<std::string> rest(argv+3,argv+argc);
    if(mode=="starts")return startsWorkflow(data,rest);
    if(mode=="terrain")return terrainWorkflow(data,rest);
    if(mode=="features")return featuresWorkflow(data,rest);
    if(mode=="units")return unitsWorkflow(data,rest);
    if(mode=="slots")return slotsWorkflow(data,rest);
    if(mode=="rescan")return rescanWorkflow(data);
    if(mode=="feature-ops")return featureOpsWorkflow(data,rest);
    if(mode=="blocked")return blockedWorkflow(data);
    if(mode=="smoke")return smokeWorkflow(data,rest);
    return 2;
}
