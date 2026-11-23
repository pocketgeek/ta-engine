#include <fstream>
#include "cartographer/editor.h"
#include "cartographer/editorstate.h"
#include "ai/builtinprofile.h"
#include "client/modelview.h"
#include "sim/matchsetup.h"
#include "cartographer/document.h"
#include "cartographer/ruleedit.h"
#include "cartographer/newmap.h"
#include "cartographer/thumbnails.h"
#include "cartographer/sections.h"
#include "cartographer/features.h"
#include "cartographer/symmetry.h"
#include "cartographer/sectiongroups.h"
#include "cartographer/preferences.h"
#include "cartographer/scrolling.h"
#include "terrain/terrain.h"
#include "cartographer/textedit.h"
#include "hpi/hpi.h"
#include "tnt/ota.h"
#include <algorithm>
#include <chrono>
#include <functional>
#include <filesystem>
#include "crt/crt.h"
#include "tnt/mapgen.h"
#include "cartographer/font5x7.h"
#include "util/png.h"
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>
#include "util/u8path.h"

// MinGW's create_symlink always fails with ENOSYS: fall back to a hard link, then a copy.
static void linkOrCopy(const std::filesystem::path& target, const std::filesystem::path& link) {
    std::error_code ec;
    std::filesystem::create_symlink(target, link, ec);
    if (ec) std::filesystem::create_hard_link(target, link, ec);
    if (ec) std::filesystem::copy_file(target, link, std::filesystem::copy_options::overwrite_existing, ec);
}

// Toolbar button centres (editorstate.h layout).
static int toolX(int tool) {return cart::Editor::kToolX+tool*cart::Editor::kToolW+30;}
static int modeX(int mode) {return cart::Editor::kModeX+mode*cart::Editor::kModeW+30;}
static void key(SDL_Keycode k,Uint16 mod=0) {
    SDL_Event e{};e.type=SDL_KEYDOWN;e.key.keysym.sym=k;e.key.keysym.mod=mod;SDL_PushEvent(&e);
}
static void text(const std::string& value) {
    // SDL text events hold short UTF-8 segments; these test strings are ASCII.
    for(size_t i=0;i<value.size();i+=20) {
        SDL_Event e{};e.type=SDL_TEXTINPUT;const auto part=value.substr(i,20);SDL_strlcpy(e.text.text,part.c_str(),sizeof(e.text.text));SDL_PushEvent(&e);
    }
}
// Point every editor profile lookup at <root>: XDG_* isolates SDL_GetPrefPath on Linux only, so the editor's
// own TAK_CARTOGRAPHER_PROFILE override keeps Windows/macOS runs off the user's real profile.
static void isolateProfile(const char* root) {
    SDL_setenv("XDG_CONFIG_HOME",root,1);SDL_setenv("XDG_DATA_HOME",root,1);
    const auto profile=std::filesystem::path(root)/"TAKengine"/"Cartographer";
    std::error_code ec;std::filesystem::create_directories(profile,ec);
    SDL_setenv("TAK_CARTOGRAPHER_PROFILE",profile.string().c_str(),1);
}
static int objectsWorkflow(const char* data) {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-editor-objects-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);isolateProfile(root.string().c_str());
    const auto vfs=tak::hpi::mountRetailRoot(data);const auto path=tak::hpi::findMap(vfs,"Ulasem Arena");
    const auto map=tak::tnt::Map::load(vfs.read(path),path);tak::tnt::Scenario metadata;metadata.kingdom="zhon";
    std::vector<cart::PlacedUnit> units{{"ARAARCH",0,168,168},{"ARAARCH",1,808,808}};
    units[0].name="East watch";units[1].name="West watch";std::string error;
    if(!cart::writeDocumentBundle(root/"Objects.kmp",cart::documentFiles(map,metadata,{},units,{},{},"Objects"),error))throw std::runtime_error(error);
    std::vector<std::string> args={"cartographer","Ulasem Arena","--data",data,"--out",root.string()};std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    bool failed=false;int lastFrame=0;
    const int result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer* renderer,int frame) {
        lastFrame=frame;const std::string title=SDL_GetWindowTitle(window);const bool dirty=title.ends_with(" *");
        auto openList=[&] {
            float sx,sy;SDL_RenderGetScale(renderer,&sx,&sy);
            for(const auto point:{SDL_Point{330,10},SDL_Point{330,232}}) {SDL_Event e{};e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;e.button.x=int(point.x*sx);e.button.y=int(point.y*sy);SDL_PushEvent(&e);e.type=SDL_MOUSEBUTTONUP;SDL_PushEvent(&e);}
        };
        if(frame<=8 || frame==10 || frame>=12)failed|=dirty;
        switch(frame) {
        case 0:key(SDLK_o,KMOD_CTRL);break;
        case 1:text((root/"Objects.kmp").string());key(SDLK_RETURN);break;
        case 2:openList();break;
        case 3:failed|=title.find("[Placed units]")==std::string::npos;text("no match");key(SDLK_RETURN);break;
        case 4:failed|=title.find("[Placed units]")==std::string::npos;key(SDLK_a,KMOD_CTRL);text("West");key(SDLK_RETURN);break;
        case 5:failed|=title!="Cartographer -- Objects";key(SDLK_RETURN);break;
        case 6:key(SDLK_ESCAPE);openList();break;
        case 7:text("PLAYER 2");key(SDLK_RETURN,KMOD_SHIFT);break;
        case 8:key(SDLK_TAB);key(SDLK_a,KMOD_CTRL);text("75");key(SDLK_RETURN);break;
        case 9:failed|=!dirty;key(SDLK_z,KMOD_CTRL);break;
        case 10:key(SDLK_y,KMOD_CTRL);break;
        case 11:failed|=!dirty;key(SDLK_s,KMOD_CTRL);break;
        case 12:key(SDLK_RETURN);break;
        case 13:{SDL_Event q{};q.type=SDL_QUIT;SDL_PushEvent(&q);break;}
        default:if(frame>15)std::_Exit(3);
        }
    });
    bool found=false;{
        tak::hpi::Archive archive(root/"Objects.kmp");
        for(const auto& entry:archive.entries())if(entry.path.ends_with(".crt")) {
            const auto saved=cart::toPlaced(tak::crt::parse(archive.read(entry)));
            if(saved.size()!=2 || saved[0].health!=100 || saved[1].health!=75 || saved[1].name!="West watch")throw std::runtime_error("object list edited the wrong unit");
            found=true;
        }
    }
    fs::remove_all(root);if(result || failed || !found || lastFrame<13 || lastFrame>14)return 1;
    std::cout<<"PASS: placed-unit search, empty match, name/owner locate, properties, undo/redo and save\n";return 0;
}
static int modelWorkflow(const char* data) {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-editor-model-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    isolateProfile(root.string().c_str());
    const auto vfs=tak::hpi::mountRetailRoot(data);
    for(const char* id:tak::sim::kMonarchs) {
        const auto asset=ModelView::loadAsset(vfs,id);
        if(asset.model.root.vertices.empty() && asset.model.root.children.empty())throw std::runtime_error("empty monarch preview");
        if(asset.textures.empty())throw std::runtime_error("untextured monarch preview");
    }
    bool rejected=false;std::atomic_bool cancelled=true;
    try {ModelView::loadAsset(vfs,"araarch",&cancelled);}catch(const std::exception&){rejected=true;}
    if(!rejected)throw std::runtime_error("cancelled model load completed");
    rejected=false;try {ModelView::loadAsset(vfs,"no-such-model");}catch(const std::exception&){rejected=true;}
    if(!rejected)throw std::runtime_error("missing model accepted");
    std::vector<std::string> args={"cartographer","Ulasem Arena","--data",data,"--out",root.string()};
    std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    int stage=0,frames=0;bool failed=false;const auto started=std::chrono::steady_clock::now();
    const int result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer* renderer,int) {
        const std::string title=SDL_GetWindowTitle(window);failed|=title.ends_with(" *");
        if(std::chrono::steady_clock::now()-started>std::chrono::seconds(30)) {std::cerr<<"model workflow timed out at "<<stage<<'\n';std::_Exit(3);}
        if(stage==0) {key(SDLK_TAB);key(SDLK_TAB);key(SDLK_F6);key(SDLK_ESCAPE);++stage;}
        else if(stage==1 && title=="Cartographer -- Ulasem Arena") {key(SDLK_F6);++stage;}
        else if(stage==2 && title.find("[Unit model]")!=std::string::npos) {if(++frames<10)return;
            int w,h;SDL_GetRendererOutputSize(renderer,&w,&h);
            if(const char* path=SDL_getenv("TAK_EDITOR_TEST_CAPTURE")) {
                std::vector<uint8_t> pixels(size_t(w)*h*4);if(SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,pixels.data(),w*4)==0)tak::png::write(path,w,h,pixels);
            }
            SDL_Event rotate{};rotate.type=SDL_MOUSEMOTION;rotate.motion.x=w/2;rotate.motion.y=h/2;
            rotate.motion.state=SDL_BUTTON_LMASK;rotate.motion.xrel=50;rotate.motion.yrel=20;SDL_PushEvent(&rotate);
            SDL_Event zoom{};zoom.type=SDL_MOUSEWHEEL;zoom.wheel.y=1;SDL_PushEvent(&zoom);++stage;
        } else if(stage==3) {key(SDLK_ESCAPE);++stage;}
        else if(stage==4) {failed|=title!="Cartographer -- Ulasem Arena";SDL_Event q{};q.type=SDL_QUIT;SDL_PushEvent(&q);++stage;}
        SDL_Delay(1);
    });
    fs::remove_all(root);
    if(result || failed || stage!=5)return 1;
    std::cout<<"PASS: five faction model assets, preview/cancel/rotate/zoom/close, unchanged document\n";return 0;
}
static int generationWorkflow(const char* data) {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-editor-generation-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    isolateProfile(root.string().c_str());
    std::vector<std::string> args={"cartographer","Ulasem Arena","--data",data,"--out",root.string()};
    std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    std::vector<uint8_t> originalTerrain;
    auto readTerrain=[&] {tak::hpi::Archive archive(root/"Generated preview.kmp");for(const auto& entry:archive.entries())if(entry.path.ends_with(".tnt"))return archive.read(entry);return std::vector<uint8_t>{};};
    auto readMetadata=[&] {
        tak::hpi::Archive archive(root/"Generated preview.kmp");
        for(const auto& entry:archive.entries())if(entry.path.ends_with(".ota")) {const auto bytes=archive.read(entry);return tak::tnt::Scenario::parse(std::string(bytes.begin(),bytes.end()));}
        return tak::tnt::Scenario{};
    };
    auto useSidecarRecipe=[&] {
        std::vector<tak::hpi::PackFile> files;
        {
            tak::hpi::Archive archive(root/"Generated preview.kmp");
            for(const auto& entry:archive.entries())if(!entry.isDirectory) {
                auto bytes=archive.read(entry);
                if(entry.path.ends_with(".ota")) {
                    auto metadata=tak::tnt::Scenario::parse(std::string(bytes.begin(),bytes.end()));
                    files.push_back({entry.path.substr(0,entry.path.size()-4)+".recipe",{metadata.generatorRecipe.begin(),metadata.generatorRecipe.end()}});
                    metadata.generatorRecipe.clear();const auto ota=metadata.write();bytes={ota.begin(),ota.end()};
                }
                files.push_back({entry.path,std::move(bytes)});
            }
        }
        std::string error;
        if(!cart::writeDocumentFiles(root,{{"Generated preview.kmp",tak::hpi::pack(files)}},error))throw std::runtime_error(error);
    };
    int stage=0;bool correctedSize=false;std::string failure;const auto begun=std::chrono::steady_clock::now();
    const auto result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer* renderer,int) {
        const std::string title=SDL_GetWindowTitle(window);
        auto check=[&](bool ok,const char* why){if(!ok && failure.empty())failure=why;};
        const auto elapsed=std::chrono::steady_clock::now()-begun;
        if(elapsed>std::chrono::seconds(50)) {failure="generation workflow timed out";key(SDLK_ESCAPE);SDL_Event quit{};quit.type=SDL_QUIT;SDL_PushEvent(&quit);return;}
        auto randomButton=[&] {
            float sx,sy;SDL_RenderGetScale(renderer,&sx,&sy);int w,h;SDL_GetRendererOutputSize(renderer,&w,&h);
            SDL_Event e{};e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;
            e.button.x=int((w/sx/2-36)*sx);e.button.y=int((h/sy/2+93)*sy);SDL_PushEvent(&e);
            e.type=SDL_MOUSEBUTTONUP;SDL_PushEvent(&e);
        };
        auto click=[&](int x,int y) {
            float sx,sy;SDL_RenderGetScale(renderer,&sx,&sy);SDL_Event e{};e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;
            e.button.x=int(x*sx);e.button.y=int(y*sy);SDL_PushEvent(&e);e.type=SDL_MOUSEBUTTONUP;SDL_PushEvent(&e);
        };
        auto recipeMenu=[&] {click(320,10);click(330,192);};
        switch(stage) {
        case 0: key(SDLK_n,KMOD_CTRL);++stage;break;
        case 1: text("bad/name");randomButton();key(SDLK_a,KMOD_CTRL);text("Generated preview");randomButton();++stage;break;
        case 2: text("123456");key(SDLK_TAB);text("8");key(SDLK_RETURN);++stage;break;
        case 3:
            check(title.find("Ulasem Arena")!=std::string::npos && !title.ends_with(" *"),"generation changed original before acceptance");
            if(title.find("[Generated preview]")!=std::string::npos) {key(SDLK_ESCAPE);++stage;}break;
        case 4: check(title=="Cartographer -- Ulasem Arena","discard preserves original document");key(SDLK_n,KMOD_CTRL);++stage;break;
        case 5: text("Generated preview");randomButton();++stage;break;
        case 6: text("123456");key(SDLK_TAB);text("8");key(SDLK_RETURN);key(SDLK_ESCAPE);++stage;break;
        case 7:
            check(title.find("[Generated preview]")==std::string::npos,"cancelled generation must not present preview");
            if(title.find("[Generating]")==std::string::npos) {
                check(title=="Cartographer -- Ulasem Arena","cancelled generation preserves original");key(SDLK_n,KMOD_CTRL);++stage;
            }break;
        case 8: text("Generated preview");randomButton();++stage;break;
        case 9: text("123456");key(SDLK_TAB);text("8");key(SDLK_RETURN);++stage;break;
        case 10: if(title.find("[Generated preview]")!=std::string::npos) {key(SDLK_RETURN);++stage;}break;
        case 11: check(title=="Cartographer -- Generated preview *","accept adopts unsaved generated map");key(SDLK_s,KMOD_CTRL);++stage;break;
        case 12: {
            check(!title.ends_with(" *"),"generated map saved");originalTerrain=readTerrain();
            const auto metadata=readMetadata();   // the recipe rides in its own key; the description starts empty
            check(tak::mapgen::isGeneratedMapId(metadata.generatorRecipe) && metadata.missionDescription.empty(),"a new random map's description is not empty");
            key(SDLK_RETURN);key(SDLK_p);++stage;break;
        }
        case 13: key(SDLK_TAB);text("Authored description");key(SDLK_RETURN);key(SDLK_s,KMOD_CTRL);++stage;break;
        case 14: key(SDLK_RETURN);key(SDLK_n,KMOD_CTRL);++stage;break; // change in-memory generator seed
        case 15: useSidecarRecipe();key(SDLK_ESCAPE);key(SDLK_o,KMOD_CTRL);++stage;break;
        case 16: text((root/"Generated preview.kmp").string());key(SDLK_RETURN);++stage;break;
        case 17: recipeMenu();++stage;break;
        case 18: key(SDLK_RETURN);++stage;break;
        case 19: if(title.find("[Generated preview]")!=std::string::npos) {key(SDLK_RETURN);++stage;}break;
        case 20: key(SDLK_s,KMOD_CTRL);++stage;break;
        case 21: check(readTerrain()==originalTerrain,"restored recipe reproduces saved terrain exactly");key(SDLK_RETURN);recipeMenu();++stage;break;
        case 22: key(SDLK_RETURN);++stage;break;
        case 23: if(title.find("[Generated preview]")!=std::string::npos) {
            float sx,sy;SDL_RenderGetScale(renderer,&sx,&sy);int w,h;SDL_GetRendererOutputSize(renderer,&w,&h);
            click(int(w/sx/2-166),int(h/sy/2+212));++stage;
        }break;
        case 24: text("789012");for(int i=0;i<8;++i)key(SDLK_TAB);text("16");key(SDLK_TAB);text("0");key(SDLK_RETURN);++stage;break;
        case 25:
            if(!correctedSize) {
                if(const char* path=SDL_getenv("TAK_EDITOR_TEST_CAPTURE")) {
                    int w,h;SDL_GetRendererOutputSize(renderer,&w,&h);std::vector<uint8_t> pixels(size_t(w)*h*4);
                    if(SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,pixels.data(),w*4)==0)tak::png::write(path,w,h,pixels);
                }
                check(title=="Cartographer -- Generated preview","invalid generator dimensions leave the map untouched");
                key(SDLK_a,KMOD_CTRL);text("12");key(SDLK_RETURN);correctedSize=true;
            } else if(title.find("[Generated preview]")!=std::string::npos) {key(SDLK_RETURN);++stage;}
            break;
        case 26: check(title.ends_with(" *"),"regeneration is an unsaved edit");key(SDLK_z,KMOD_CTRL);++stage;break;
        case 27: check(!title.ends_with(" *"),"undo regeneration restores saved revision");key(SDLK_y,KMOD_CTRL);++stage;break;
        case 28: check(title.ends_with(" *"),"redo regeneration restores new seed");key(SDLK_s,KMOD_CTRL);++stage;break;
        case 29: key(SDLK_RETURN);key(SDLK_z,KMOD_CTRL);++stage;break;
        case 30: key(SDLK_s,KMOD_CTRL);++stage;break;
        case 31: key(SDLK_RETURN);++stage;break;
        case 32: {SDL_Event quit{};quit.type=SDL_QUIT;SDL_PushEvent(&quit);++stage;break;}
        }
        SDL_Delay(1);
    });
    try {
        if(result || !failure.empty())throw std::runtime_error(failure.empty()?"editor failed":failure);
        if(stage!=33)throw std::runtime_error("generation workflow exited early");
        tak::hpi::Archive archive(root/"Generated preview.kmp");bool found=false;
        for(const auto& entry:archive.entries())if(entry.path.ends_with(".ota")) {
            const auto bytes=archive.read(entry);const auto metadata=tak::tnt::Scenario::parse(std::string(bytes.begin(),bytes.end()));
            if(metadata.starts.size()!=8 || metadata.missionDescription!="Authored description")throw std::runtime_error("generated metadata missing");
            const auto params=tak::mapgen::decodeMapId(metadata.generatorRecipe);
            if(params.seed!=123456 || params.players!=8)throw std::runtime_error("generated recipe differs from chosen options");
            found=true;
        }
        if(!found)throw std::runtime_error("generated metadata absent");
        if(readTerrain()!=originalTerrain)throw std::runtime_error("undo regeneration did not preserve saved terrain");
        tak::hpi::Archive backup(root/"Generated preview.kmp.bak");bool variant=false;
        for(const auto& entry:backup.entries())if(entry.path.ends_with(".ota")) {
            const auto bytes=backup.read(entry);const auto metadata=tak::tnt::Scenario::parse(std::string(bytes.begin(),bytes.end()));
            const auto params=tak::mapgen::decodeMapId(metadata.generatorRecipe);
            variant=params.seed==789012 && params.widthCells==16*32 && params.heightCells==12*32;
        }
        if(!variant)throw std::runtime_error("preview Settings did not regenerate with the edited seed");
        fs::remove_all(root);std::cout<<"PASS: background generation, preview/discard, accept and saved recipe\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<"; files: "<<root<<'\n';return 1;}
}
static int overlayWorkflow(const char* data) {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-editor-overlay-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    isolateProfile(root.string().c_str());
    std::vector<std::string> args={"cartographer","Ulasem Arena","--data",data,"--out",root.string()};
    std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    int stage=0;bool failed=false;auto began=std::chrono::steady_clock::now();
    const int result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer* renderer,int) {
        auto click=[&](int x,int y) {
            float sx,sy;SDL_RenderGetScale(renderer,&sx,&sy);
            SDL_Event e{};e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;e.button.x=int(x*sx);e.button.y=int(y*sy);SDL_PushEvent(&e);
            e.type=SDL_MOUSEBUTTONUP;SDL_PushEvent(&e);
        };
        const std::string title=SDL_GetWindowTitle(window);
        if(std::chrono::steady_clock::now()-began>std::chrono::seconds(30)) {failed=true;key(SDLK_ESCAPE);SDL_Event q{};q.type=SDL_QUIT;SDL_PushEvent(&q);return;}
        if(stage==0) {click(250,10);click(250,212);++stage;}
        else if(stage==1 && title.find("[Terrain overlay]")!=std::string::npos) {failed|=title.ends_with(" *");click(250,10);click(250,252);++stage;}
        else if(stage==2) {failed|=title!="Cartographer -- Ulasem Arena";SDL_Event q{};q.type=SDL_QUIT;SDL_PushEvent(&q);++stage;}
        SDL_Delay(1);
    });
    fs::remove_all(root);
    if(result || failed || stage!=3) {std::cerr<<"overlay menu workflow failed\n";return 1;}
    std::cout<<"PASS: background water overlay, unchanged document and hide control\n";return 0;
}
// SCENARIO > AI Profiles (A) end to end through real input: a TAK ENGINE AI row edited
// in its cell (ai/takengine.txt, previewed on Normal and then Hard), the map's own
// DEFAULT profile chosen and started empty, a preset applied
// after its confirmation, undo/redo after closing, and Save As writing the files at
// the .kmp root with aiprofile= in the .ota. The map also carries ai/n.txt, which no
// profile reads: it is not shown, never written, and saved byte for byte.
// TAK_EDITOR_TEST_CAPTURE=<prefix> also writes <prefix>-level.png and
// <prefix>-default.png of the window.
static int aiProfileWorkflow(const char* data) {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-editor-aiprofile-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);isolateProfile(root.string().c_str());
    const auto vfs=tak::hpi::mountRetailRoot(data);const auto path=tak::hpi::findMap(vfs,"Ulasem Arena");
    const auto map=tak::tnt::Map::load(vfs.read(path),path);tak::tnt::Scenario metadata;metadata.kingdom="zhon";
    const std::string unrelated="weight araarch 9\n";   // ai/n.txt: a file no profile of this map reads
    cart::AiFiles fixture;fixture["ai/n.txt"].assign(unrelated.begin(),unrelated.end());
    std::string error;
    if(!cart::writeDocumentBundle(root/"AiTest.kmp",cart::documentFiles(map,metadata,{},{},{},fixture,"AiTest"),error))throw std::runtime_error(error);
    std::vector<std::string> args={"cartographer","Ulasem Arena","--data",data,"--out",root.string()};
    std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    cart::Editor* editor=nullptr;cart::EditorHooks hooks;hooks.attached=[&](cart::Editor& e){editor=&e;};
    std::string failure;int lastFrame=0;
    auto check=[&](bool value,const std::string& message) {if(!value && failure.empty())failure=message;};
    auto file=[&](const std::string& key) {
        const auto it=editor->aiFiles.find(key);return it==editor->aiFiles.end()?std::string():std::string(it->second.begin(),it->second.end());
    };
    const int result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer* renderer,int frame) {
        lastFrame=frame;const std::string title=SDL_GetWindowTitle(window);const bool dirty=title.ends_with(" *");
        float sx=1,sy=1;SDL_RenderGetScale(renderer,&sx,&sy);
        auto click=[&](const SDL_Rect& r) {
            if(r.w<=0) {check(false,"frame "+std::to_string(frame)+": target not drawn");return;}
            SDL_Event e{};e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;e.button.clicks=1;
            e.button.x=int((r.x+r.w/2)*sx);e.button.y=int((r.y+r.h/2)*sy);SDL_PushEvent(&e);e.type=SDL_MOUSEBUTTONUP;SDL_PushEvent(&e);
        };
        auto capture=[&](const char* suffix) {
            const char* prefix=SDL_getenv("TAK_EDITOR_TEST_CAPTURE");if(!prefix)return;
            int pw=0,ph=0;SDL_GetRendererOutputSize(renderer,&pw,&ph);std::vector<uint8_t> rgba(size_t(pw)*ph*4);
            if(SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,rgba.data(),pw*4)==0)tak::png::write(std::string(prefix)+suffix,pw,ph,rgba);
        };
        auto cell=[&](const std::string& key,int column) {
            for(const auto& c:editor->aiCells)if(c.key==key && c.column==column)return c.rect;
            return SDL_Rect{};
        };
        switch(frame) {
        case 0:key(SDLK_o,KMOD_CTRL);break;
        case 1:text((root/"AiTest.kmp").string());key(SDLK_RETURN);break;
        case 2:check(!dirty && file("ai/n.txt")==unrelated,"opening the fixture marked it dirty or changed ai/n.txt");key(SDLK_a);break;
        case 3:check(editor->aiOpen && title.find("[AI]")!=std::string::npos,"A did not open the AI Profiles window");
            check(editor->aiTabKey()!="ai/n.txt" && editor->aiKeyFor(0)!="ai/n.txt" && editor->aiKeyFor(1)!="ai/n.txt","ai/n.txt is shown as a profile");
            click(editor->aiTabRects[1]);break;
        case 4:check(editor->aiTab==1 && editor->aiTabKey()=="ai/takengine.txt" && editor->aiTabLevel()==tak::ai::Difficulty::Normal,
                     "the TAK ENGINE AI tab was not selected, previewing Normal");click(editor->aiSearchBox);break;
        case 5:text("araarch");break;
        case 6:click(cell("araarch",0));break;
        case 7:check(editor->aiFocus==cart::Editor::AI_CELL,"the weight cell did not take focus");text("0");key(SDLK_RETURN);break;
        case 8:check(file("ai/takengine.txt").find("\nweight araarch 0\n")!=std::string::npos,"the archer weight 0 was not written to ai/takengine.txt");
            check(file("ai/takengine.txt").starts_with(cart::kAiProfileMagic) && file("ai/takengine.txt").find("TAK ENGINE AI (all six levels) profile for map")!=std::string::npos,
                  "ai/takengine.txt is not in the canonical form with its header");
            check(editor->aiFiles.size()==2 && file("ai/n.txt")==unrelated,"a level edit wrote more than ai/takengine.txt");
            capture("-level.png");click(editor->aiPreviewBox);break;
        case 9:{
            check(editor->aiTabLevel()==tak::ai::Difficulty::Hard && editor->aiTabView().preview==tak::ai::Difficulty::Hard,"PREVIEW LEVEL did not step to Hard");
            // The shown values are the engine's: Hard's own table with the file on top.
            const auto want=tak::ai::levelProfileFrom(tak::ai::Difficulty::Hard,&editor->aiFiles.at("ai/takengine.txt"));
            const auto& view=editor->aiTabView();
            check(view.level.weight==want.weight && view.level.limit==want.limit && view.builtin.weight==tak::ai::builtinProfile(tak::ai::Difficulty::Hard).weight,
                  "the Hard preview is not levelProfileFrom(Hard, ai/takengine.txt)");
            check(view.level.weight.at("araarch")==0,"the file's row is not applied on Hard");
            click(editor->aiTabRects[0]);break;
        }
        case 10:check(editor->aiTab==0,"the DEFAULT tab was not selected");click(editor->aiOwnRadio);break;
        case 11:check(editor->aiStartOpen,"choosing an own profile did not ask where to start");click(editor->aiStartEmpty);break;
        case 12:check(editor->scenario.aiProfile=="aitest","own profile name was not proposed from the map name");
            check(!file("ai/aitest.txt").empty() && file("ai/aitest.txt").find("\nlimit ")==std::string::npos,"Empty did not write ai/aitest.txt");
            click(editor->aiSearchBox);key(SDLK_a,KMOD_CTRL);key(SDLK_BACKSPACE);click(editor->aiPresetRects[0]);break;
        case 13:check(editor->aiSearch.empty(),"the search field did not clear");check(editor->modal==cart::Editor::M_CONFIRM,"No flyers did not ask first");key(SDLK_RETURN);break;
        case 14:check(file("ai/aitest.txt").find("\nlimit ")!=std::string::npos,"No flyers set no rows");
            capture("-default.png");click(editor->aiDoneButton);break;
        case 15:check(!editor->aiOpen && dirty,"DONE did not close the window, or the edits were lost");key(SDLK_z,KMOD_CTRL);break;
        case 16:check(file("ai/aitest.txt").find("\nlimit ")==std::string::npos && !file("ai/aitest.txt").empty(),"undo did not take back only the preset");key(SDLK_y,KMOD_CTRL);break;
        case 17:check(file("ai/aitest.txt").find("\nlimit ")!=std::string::npos && editor->scenario.aiProfile=="aitest","redo did not restore the preset");key(SDLK_s,KMOD_CTRL|KMOD_SHIFT);break;
        case 18:text("AiSaved");key(SDLK_RETURN);break;
        case 19:check(!dirty,"Save As left the map dirty");key(SDLK_RETURN);break;
        case 20:{SDL_Event quit{};quit.type=SDL_QUIT;SDL_PushEvent(&quit);break;}
        default:if(frame>23) {failure="AI profile workflow did not exit";std::_Exit(3);}
        }
    },hooks);
    if(failure.empty()) {
        std::set<std::string> entries;std::string ota,kept;
        tak::hpi::Archive archive(root/"AiSaved.kmp");
        for(const auto& entry:archive.entries()) {
            entries.insert(tak::hpi::MountSet::key(entry.path));
            if(entry.path.ends_with(".ota")) {const auto bytes=archive.read(entry);ota.assign(bytes.begin(),bytes.end());}
            if(tak::hpi::MountSet::key(entry.path)=="ai/n.txt") {const auto bytes=archive.read(entry);kept.assign(bytes.begin(),bytes.end());}
        }
        check(kept==unrelated,"the saved .kmp does not carry ai/n.txt byte for byte");
        check(entries.count("ai/takengine.txt") && entries.count("ai/aitest.txt"),"the saved .kmp lacks root ai/takengine.txt or ai/aitest.txt");
        for(const auto& entry:entries)check(!(entry.starts_with("kmap/") && entry.find("/ai/")!=std::string::npos),"an AI file was written under kmap/");
        check(ota.find("aiprofile=aitest;")!=std::string::npos,"the saved .ota does not say aiprofile=aitest");
    }
    fs::remove_all(root);
    if(result || !failure.empty() || lastFrame>23) {std::cerr<<failure<<'\n';return 1;}
    std::cout<<"PASS: AI Profiles window: TAK ENGINE AI cell and Hard preview, own DEFAULT profile, preset, undo/redo, Save As with root ai/ files and aiprofile=\n";return 0;
}
// The AI window's game-mod half, against a SCRATCH data root (the install's entries
// linked in, a fresh real mods/ folder): Save as game mod into an existing pack in
// case-variant folders (its own spelling reused, the different file replaced after the
// compare, the old copy backed up in the profile folder, never in the pack), then
// Open from game mod: an edit that is the pack's and not the map's, SAVE TO PACK
// creating a file without asking, an emptied draft offered for deletion unticked,
// and the ticked delete confirmed and backed up. TAK_EDITOR_TEST_CAPTURE=<prefix> also
// writes <prefix>-modsave.png and <prefix>-pack.png.
static int aiModWorkflow(const char* data) {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-editor-aimod-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);isolateProfile(root.string().c_str());
    const auto install=root/"data";fs::create_directory(install);
    for(const auto& entry:fs::directory_iterator(data))
        if(tak::hpi::MountSet::key(entry.path().filename().string())!="mods")linkOrCopy(fs::absolute(entry.path()),install/entry.path().filename());
    const auto ai=install/"Mods"/"Game"/"Harder"/"AI";fs::create_directories(ai);
    auto put=[](const fs::path& p,const std::string& text) {std::ofstream(p,std::ios::binary)<<text;};
    auto slurp=[](const fs::path& p) {std::ifstream f(p,std::ios::binary);return std::string(std::istreambuf_iterator<char>(f),{});};
    put(ai/"TakEngine.TXT","weight araarch 7\n");put(ai/"notes.txt","keep\n");
    put(ai/"n.txt","weight araarch 9\n");   // another DEFAULT profile of the pack: never touched
    const auto backups=root/"TAKengine"/"Cartographer"/"mod-backups"/"Harder";
    auto backedUp=[&](const std::string& name) {   // every backup copy of `name`, oldest first
        std::vector<std::string> out;std::error_code ec;std::vector<fs::path> stamps;
        for(const auto& d:fs::directory_iterator(backups,ec))stamps.push_back(d.path());
        std::sort(stamps.begin(),stamps.end());
        for(const auto& d:stamps)if(fs::exists(d/"ai"/name))out.push_back(slurp(d/"ai"/name));
        return out;
    };
    auto listing=[](const fs::path& dir) {std::set<std::string> out;std::error_code ec;for(const auto& e:fs::directory_iterator(dir,ec))out.insert(e.path().filename().string());return out;};
    std::vector<std::string> args={"cartographer","Ulasem Arena","--data",install.string(),"--out",root.string()};
    std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    cart::Editor* editor=nullptr;cart::EditorHooks hooks;hooks.attached=[&](cart::Editor& e){editor=&e;};
    std::string failure;int lastFrame=0;
    auto check=[&](bool value,const std::string& message) {if(!value && failure.empty())failure="frame "+std::to_string(lastFrame)+": "+message;};
    auto mapFile=[&](const std::string& key) {const auto it=editor->aiFiles.find(key);return it==editor->aiFiles.end()?std::string():std::string(it->second.begin(),it->second.end());};
    std::string own,ownBytes;   // the map's own DEFAULT profile, before it is saved into the pack
    auto packFile=[&](const std::string& key) {
        if(!editor->aiMod)return std::string();
        const auto it=editor->aiMod->files.find(key);return it==editor->aiMod->files.end()?std::string():std::string(it->second.begin(),it->second.end());
    };
    const int result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer* renderer,int frame) {
        lastFrame=frame;const std::string title=SDL_GetWindowTitle(window);
        float sx=1,sy=1;SDL_RenderGetScale(renderer,&sx,&sy);
        auto press=[&](const SDL_Rect& r,Uint8 button) {
            if(r.w<=0) {check(false,"target not drawn");return;}
            SDL_Event e{};e.type=SDL_MOUSEBUTTONDOWN;e.button.button=button;e.button.clicks=1;
            e.button.x=int((r.x+r.w/2)*sx);e.button.y=int((r.y+r.h/2)*sy);SDL_PushEvent(&e);e.type=SDL_MOUSEBUTTONUP;SDL_PushEvent(&e);
        };
        auto click=[&](const SDL_Rect& r) {press(r,SDL_BUTTON_LEFT);};
        auto capture=[&](const char* suffix) {
            const char* prefix=SDL_getenv("TAK_EDITOR_TEST_CAPTURE");if(!prefix)return;
            int pw=0,ph=0;SDL_GetRendererOutputSize(renderer,&pw,&ph);std::vector<uint8_t> rgba(size_t(pw)*ph*4);
            if(SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,rgba.data(),pw*4)==0)tak::png::write(std::string(prefix)+suffix,pw,ph,rgba);
        };
        auto cell=[&](const std::string& key,int column) {
            for(const auto& c:editor->aiCells)if(c.key==key && c.column==column)return c.rect;
            return SDL_Rect{};
        };
        auto change=[&](const std::string& key)->const cart::ModProfileChange* {
            for(const auto& c:editor->aiModPlan.changes)if(c.key==key)return &c;
            return nullptr;
        };
        using A=cart::ModProfileChange::Action;
        switch(frame) {
        case 0:key(SDLK_a);break;
        case 1:check(editor->aiOpen,"A did not open the AI Profiles window");click(editor->aiTabRects[1]);break;
        case 2:click(editor->aiSearchBox);break;
        case 3:text("araarch");break;
        case 4:click(cell("araarch",0));break;
        case 5:text("0");key(SDLK_RETURN);break;
        case 6:check(mapFile("ai/takengine.txt").find("\nweight araarch 0\n")!=std::string::npos,"the weight was not written to the map's ai/takengine.txt");
            click(editor->aiModSaveButton);break;
        case 7:check(editor->aiModSaveOpen,"SAVE AS GAME MOD did not open its dialog");
            check(editor->aiModRows.size()==1 && editor->aiModRows[0].target=="ai/takengine.txt" && editor->aiModRows[0].tick,"the dialog does not offer ai/takengine.txt, ticked");
            click(editor->aiModPackBox);break;
        case 8:key(SDLK_a,KMOD_CTRL);key(SDLK_BACKSPACE);text("harder");break;
        case 9:{
            const auto* n=change("ai/takengine.txt");
            check(editor->aiModPlanError.empty(),"plan error: "+editor->aiModPlanError);
            check(editor->aiModPlan.pack=="Harder" && editor->aiModPlan.folder==ai,"the existing Mods/Game/Harder/AI was not reused in its own case");
            check(n && n->action==A::Replace && n->path==ai/"TakEngine.TXT","the pack's different TakEngine.TXT is not planned as a replace");
            check(!change("ai/n.txt"),"the pack's ai/n.txt is planned");
            capture("-modsave.png");click(editor->aiModWriteButton);break;
        }
        case 10:check(editor->modal==cart::Editor::M_CONFIRM && editor->mTitle=="REPLACE PACK FILES","a different pack file was replaced without the compare");key(SDLK_RETURN);break;
        case 11:check(editor->modal==cart::Editor::M_MESSAGE && editor->mTitle=="SAVED","no after-save notice");
            check(slurp(ai/"TakEngine.TXT")==mapFile("ai/takengine.txt"),"TakEngine.TXT does not hold the map's ai/takengine.txt");
            check(listing(ai)==std::set<std::string>{"TakEngine.TXT","n.txt","notes.txt"},"the pack's ai/ gained or lost files");
            check(slurp(ai/"notes.txt")=="keep\n" && slurp(ai/"n.txt")=="weight araarch 9\n","another pack file was touched");
            check(backedUp("TakEngine.TXT")==std::vector<std::string>{"weight araarch 7\n"},"the replaced file was not backed up in the profile folder");
            check(listing(install/"Mods"/"Game")==std::set<std::string>{"Harder"} && listing(install/"Mods"/"Game"/"Harder")==std::set<std::string>{"AI"},"staging or backups were left beside the pack");
            key(SDLK_RETURN);break;
        case 12:click(editor->aiModOpenButton);break;
        case 13:{
            check(editor->aiModOpenOpen,"OPEN FROM GAME MOD did not list the packs");
            SDL_Rect row{};for(const auto& [r,pack]:editor->aiModPackRows)if(pack=="Harder")row=r;
            click(row);break;
        }
        case 14:check(editor->aiMod && editor->aiMod->pack=="Harder" && packFile("ai/takengine.txt")==mapFile("ai/takengine.txt"),"the pack's files were not opened");
            check(packFile("ai/n.txt")=="weight araarch 9\n","the pack's ai/n.txt is not kept");
            check(title.find("[AI]")!=std::string::npos,"the window closed");click(editor->aiTabRects[1]);break;
        case 15:capture("-pack.png");click(cell("araarch",0));break;
        case 16:text("5");key(SDLK_RETURN);break;
        case 17:check(packFile("ai/takengine.txt").find("\nweight araarch 5\n")!=std::string::npos,"the edit did not go to the pack's set");
            check(mapFile("ai/takengine.txt").find("\nweight araarch 0\n")!=std::string::npos,"a pack edit leaked into the map");
            check(editor->aiModDirty(),"the pack set is not marked unsaved");
            click(editor->aiModSaveButton);break;
        case 18:{
            bool eTicked=false,nTicked=false;
            for(const auto& row:editor->aiModRows) {if(row.target=="ai/takengine.txt")eTicked=row.tick;if(row.target=="ai/n.txt")nTicked=row.tick;}
            check(editor->aiModSaveOpen && eTicked && !nTicked,"SAVE TO PACK should tick only the changed file");
            check(editor->aiModPackBox.w==0,"the pack name is editable when saving back to the pack");
            click(editor->aiModWriteButton);break;
        }
        case 19:check(editor->modal==cart::Editor::M_CONFIRM && editor->mTitle=="REPLACE PACK FILES","replacing the pack's file did not show the compare");
            key(SDLK_RETURN);break;
        case 20:check(editor->modal==cart::Editor::M_MESSAGE && editor->mTitle=="SAVED","saving back to the pack failed");
            check(slurp(ai/"TakEngine.TXT")==packFile("ai/takengine.txt") && !editor->aiModDirty(),"TakEngine.TXT was not replaced, or the set stays unsaved");
            key(SDLK_RETURN);break;
        case 21:press(cell("araarch",0),SDL_BUTTON_RIGHT);break;   // the file's only row: the draft is now empty
        case 22:check(packFile("ai/takengine.txt").empty() && editor->aiModDirty(),"clearing the row did not empty ai/takengine.txt");click(editor->aiModSaveButton);break;
        case 23:check(editor->aiModSaveOpen && editor->aiModDeleteBoxes.size()==1 && editor->aiModDeleteTicked.empty(),"the emptied draft is not offered for deletion, unticked");
            click(editor->aiModDeleteBoxes.empty()?SDL_Rect{}:editor->aiModDeleteBoxes[0].first);break;
        case 24:check(editor->aiModDeleteTicked.count("ai/takengine.txt")==1,"the delete box did not tick");click(editor->aiModWriteButton);break;
        case 25:check(editor->modal==cart::Editor::M_CONFIRM,"deleting a pack file did not ask");key(SDLK_RETURN);break;
        case 26:check(editor->modal==cart::Editor::M_MESSAGE && !fs::exists(ai/"TakEngine.TXT") && slurp(ai/"n.txt")=="weight araarch 9\n","TakEngine.TXT was not deleted, or n.txt was touched");
            check(backedUp("TakEngine.TXT").size()==3 && backedUp("TakEngine.TXT")[2].find("weight araarch 5")!=std::string::npos,"the deleted file was not backed up");
            check(!editor->aiModDirty(),"the set stays unsaved after the delete");
            key(SDLK_RETURN);break;
        case 27:click(editor->aiModOpenButton);break;   // CLOSE PACK: nothing unsaved, so no question
        case 28:check(!editor->aiMod && editor->modal==cart::Editor::M_NONE,"CLOSE PACK did not return to the map's profiles");
            check(mapFile("ai/takengine.txt").find("\nweight araarch 0\n")!=std::string::npos && editor->aiFiles.size()==1,"the map's files changed");
            click(editor->aiTabRects[0]);break;
        case 29:click(editor->aiOwnRadio);break;
        // The map's own DEFAULT profile saved into the pack as ai/default.txt: its header
        // names that file and the pack; the map's file is left as it was.
        case 30:check(editor->aiStartOpen,"no Start from");click(editor->aiStartEmpty);break;
        case 31:own=cart::aiProfileFileKey(editor->scenario.aiProfile);ownBytes=mapFile(own);
            check(own!="ai/default.txt" && ownBytes.find("(ai/default.txt)")==std::string::npos,"no own profile file: "+own);
            click(editor->aiModSaveButton);break;
        case 32:{
            const cart::Editor::AiModRow* def=nullptr;for(const auto& row:editor->aiModRows)if(row.isDefault)def=&row;
            check(editor->aiModSaveOpen && def && def->source==own && def->target=="ai/default.txt" && !def->tick,"the DEFAULT row is not offered as ai/default.txt, unticked");
            click(def?def->box:SDL_Rect{});break;
        }
        case 33:{
            const auto* d=change("ai/default.txt");
            check(editor->aiModPlan.pack=="Harder" && d && d->action==A::Create,"ai/default.txt is not planned as a new file in Harder");
            click(editor->aiModWriteButton);break;
        }
        case 34:{
            check(editor->modal==cart::Editor::M_MESSAGE && editor->mTitle=="SAVED","saving ai/default.txt failed");
            const auto saved=slurp(ai/"default.txt");
            auto afterHeader=[](const std::string& t) {const auto a=t.find('\n');const auto b=a==std::string::npos?a:t.find('\n',a+1);return b==std::string::npos?std::string():t.substr(b+1);};
            const auto line2=saved.substr(saved.find('\n')+1,saved.find('\n',saved.find('\n')+1)-saved.find('\n')-1);
            check(line2=="// "+cart::aiModDefaultHeader("Harder",editor->mapName,own),"the pack's default.txt header does not name it: "+line2);
            check(afterHeader(saved)==afterHeader(ownBytes) && saved.substr(0,saved.find('\n'))==ownBytes.substr(0,ownBytes.find('\n')),"more than the header changed");
            check(mapFile(own)==ownBytes,"the map's own profile changed");
            key(SDLK_RETURN);break;
        }
        case 35:click(editor->aiDoneButton);break;
        case 36:{SDL_Event quit{};quit.type=SDL_QUIT;SDL_PushEvent(&quit);break;}
        case 37:if(editor->modal==cart::Editor::M_QUITSAVE)click(editor->mQuit);break;
        default:if(frame>41) {failure+=" (AI game-mod workflow did not exit)";std::_Exit(3);}
        }
    },hooks);
    fs::remove_all(root);
    if(result || !failure.empty() || lastFrame>41) {std::cerr<<failure<<'\n';return 1;}
    std::cout<<"PASS: AI Profiles game mod: save into an existing case-variant pack (compare, backup outside the pack), open from the pack, save back, emptied draft deleted, DEFAULT as default.txt names its file\n";return 0;
}
// TEST IN GAME's stand-in for the game: `cartographer_ui_test --data <root> --play-map
// <snapshot> --game-mod <pack>` (the workflow below names this executable as the client)
// writes its arguments, and what the snapshot holds, to $TAK_AIMOD_TEST_RECORD.
static int recordTestClient(int argc,char** argv) {
    const char* record=SDL_getenv("TAK_AIMOD_TEST_RECORD");
    if(!record)return 2;
    std::string out;
    for(int i=1;i<argc;++i)out+=std::string(argv[i])+"\n";
    try {
        tak::hpi::Archive snapshot(tak::u8path(argc>4?argv[4]:""));
        int maps=0;for(const auto& entry:snapshot.entries())maps+=entry.path.ends_with(".tnt");
        out+="snapshot maps="+std::to_string(maps)+"\n";
    } catch(const std::exception& e) {out+=std::string("snapshot error=")+e.what()+"\n";}
    std::ofstream(tak::u8path(std::string(record)+".tmp"),std::ios::binary)<<out;
    std::error_code ec;std::filesystem::rename(tak::u8path(std::string(record)+".tmp"),tak::u8path(record),ec);
    return ec?3:0;
}
// File > New AI mod / Open AI mod: an AI mod made and edited without a map, through real
// input, against a SCRATCH data root. A bare launch opens no document; File > New map makes
// one: a new map is no work to lose, but a start placed on it is, so Ctrl+Shift+N asks (Cancel
// keeps the map; OK leaves no document in its place), then asks for the pack name (an existing
// pack's is refused). The window opens with the pack as the only document: the map is not
// drawn and map menus are off.
// One TAK ENGINE AI cell, the DEFAULT AI's ai/default.txt (Start from Empty, one chance)
// and a named profile ai/islands.txt (NEW NAME...) are edited; SAVE TO PACK creates
// mods/game/Harder AI/ai/ with exactly those three files. CLOSE returns to the empty editor;
// Ctrl+Shift+O lists the packs and reopens it with the same values. TEST IN GAME on
// Ulasem Arena starts the client (this executable, see recordTestClient) with
// --play-map <test copy> --game-mod "Harder AI"; with an unsaved edit it asks, saves (the
// replace compare and backup) and then shows the map picker. Quitting with an unsaved
// edit asks first and leaves the pack as saved. TAK_EDITOR_TEST_CAPTURE=<prefix> also
// writes <prefix>-aimod.png (the TAK ENGINE AI tab) and <prefix>-aimod-default.png.
static int aiModNoMapWorkflow(const char* data,const char* self) {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-editor-aimodnomap-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);isolateProfile(root.string().c_str());
    const auto install=root/"data";fs::create_directory(install);
    for(const auto& entry:fs::directory_iterator(data))
        if(tak::hpi::MountSet::key(entry.path().filename().string())!="mods")linkOrCopy(fs::absolute(entry.path()),install/entry.path().filename());
    fs::create_directories(install/"mods"/"game"/"Other"/"ai");   // another pack: listed, never touched
    std::ofstream(install/"mods"/"game"/"Other"/"ai"/"takengine.txt",std::ios::binary)<<"weight araarch 2\n";
    const auto record=root/"client-args.txt";
    SDL_setenv("TAK_AIMOD_TEST_RECORD",record.string().c_str(),1);
    const auto pack=install/"mods"/"game"/"Harder AI"/"ai";
    auto slurp=[](const fs::path& p) {std::ifstream f(p,std::ios::binary);return std::string(std::istreambuf_iterator<char>(f),{});};
    auto listing=[](const fs::path& dir) {std::set<std::string> out;std::error_code ec;for(const auto& e:fs::directory_iterator(dir,ec))out.insert(e.path().filename().string());return out;};
    std::vector<std::string> args={"cartographer","--data",install.string(),"--out",root.string()};
    std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    cart::Editor* editor=nullptr;cart::EditorHooks hooks;hooks.attached=[&](cart::Editor& e){editor=&e;};
    hooks.gameClient=[&] {return fs::absolute(self).string();};
    std::string failure,unit,saved;int lastFrame=0,stage=0,waited=0;
    auto check=[&](bool value,const std::string& message) {if(!value && failure.empty())failure="stage "+std::to_string(stage)+": "+message;};
    auto packFile=[&](const std::string& key) {
        if(!editor->aiMod)return std::string();
        const auto it=editor->aiMod->files.find(key);return it==editor->aiMod->files.end()?std::string():std::string(it->second.begin(),it->second.end());
    };
    auto chance=[&](const std::string& key) {
        const auto& view=editor->aiTabView();const auto it=editor->aiCatalogue->byName.find(key);
        return it==editor->aiCatalogue->byName.end()?-1:view.classic.pref[it->second];
    };
    auto weight=[&](const std::string& key) {
        const auto& view=editor->aiTabView();const auto it=view.level.weight.find(key);return it==view.level.weight.end()?-1:it->second;
    };
    const int result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer* renderer,int frame) {
        lastFrame=frame;const std::string title=SDL_GetWindowTitle(window);
        if(!failure.empty() || frame>400) {if(frame>400)failure+=" (did not finish)";std::cerr<<"FAIL: "<<failure<<'\n';std::_Exit(3);}
        float sx=1,sy=1;SDL_RenderGetScale(renderer,&sx,&sy);
        auto click=[&](const SDL_Rect& r) {
            if(r.w<=0) {check(false,"target not drawn");return;}
            SDL_Event e{};e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;e.button.clicks=1;
            e.button.x=int((r.x+r.w/2)*sx);e.button.y=int((r.y+r.h/2)*sy);SDL_PushEvent(&e);e.type=SDL_MOUSEBUTTONUP;SDL_PushEvent(&e);
        };
        auto capture=[&](const char* suffix) {
            const char* prefix=SDL_getenv("TAK_EDITOR_TEST_CAPTURE");if(!prefix)return;
            int pw=0,ph=0;SDL_GetRendererOutputSize(renderer,&pw,&ph);std::vector<uint8_t> rgba(size_t(pw)*ph*4);
            if(SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,rgba.data(),pw*4)==0)tak::png::write(std::string(prefix)+suffix,pw,ph,rgba);
        };
        auto cell=[&](const std::string& key,int column) {
            for(const auto& c:editor->aiCells)if(c.key==key && c.column==column)return c.rect;
            return SDL_Rect{};
        };
        auto find=[&](const std::string& what) {click(editor->aiSearchBox);key(SDLK_a,KMOD_CTRL);key(SDLK_BACKSPACE);text(what);};
        using M=cart::Editor::Modal;
        switch(stage) {
        case 0:check(editor->modal==M::M_MESSAGE && editor->noDocument,"a bare launch did not open empty (with the first-run welcome)");key(SDLK_RETURN);break;
        case 1:check(editor->modal==M::M_NONE && editor->noDocument,"the welcome did not close onto the empty editor");key(SDLK_n,KMOD_CTRL);stage=89;break;
        case 90:check(editor->modal==M::M_NEW,"Ctrl+N did not open New Map");key(SDLK_RETURN);break;
        case 91:check(editor->modal==M::M_NONE && !editor->noDocument && !editor->mapHasUnsavedWork(),"a new map nothing was done to counts as work");
            click({cart::Editor::kToolX+3*cart::Editor::kToolW,25,cart::Editor::kToolW-4,16});   // STARTS
            stage=99;break;
        // A map with unsaved work: the command asks before it is put away (Cancel keeps it).
        case 100:click({cart::Editor::kPaletteW+200,cart::Editor::kMenuH+150,4,4});break;
        case 101:check(editor->scenario.starts.size()==1 && editor->mapHasUnsavedWork(),"no start placed");key(SDLK_n,KMOD_CTRL|KMOD_SHIFT);break;
        case 102:check(editor->modal==M::M_CONFIRM && editor->mTitle=="NEW AI MOD","the map's unsaved work was not asked about");key(SDLK_ESCAPE);break;
        case 103:check(editor->modal==M::M_NONE && !editor->aiModOnly && editor->scenario.starts.size()==1,"Cancel did not keep the map");
            key(SDLK_n,KMOD_CTRL|KMOD_SHIFT);break;
        case 104:check(editor->modal==M::M_CONFIRM,"no question the second time");key(SDLK_RETURN);stage=1;break;   // on to 2
        case 2:check(editor->modal==M::M_NEWAIMOD && editor->mf[0]=="my-ai","Ctrl+Shift+N did not ask for the pack name, or asked about the map");
            key(SDLK_a,KMOD_CTRL);key(SDLK_BACKSPACE);text("Other");key(SDLK_RETURN);break;
        case 3:check(editor->modal==M::M_NEWAIMOD && editor->modalError.find("already exists")!=std::string::npos,"an existing pack name was accepted for a new AI mod");
            key(SDLK_a,KMOD_CTRL);key(SDLK_BACKSPACE);text("Harder AI");key(SDLK_RETURN);break;
        case 4:
            check(editor->modal==M::M_NONE && editor->aiModOnly && editor->aiOpen && editor->aiMod && editor->aiMod->pack=="Harder AI" && editor->aiMod->files.empty(),
                  "the window did not open on the new pack alone");
            check(title=="Cartographer -- AI mod Harder AI" && editor->aiTab==1,"the title or the first tab is not the AI mod's");
            check(!editor->menuRowEnabled(3,0) && !editor->menuRowEnabled(0,2) && !editor->menuRowEnabled(0,5) && editor->menuRowEnabled(0,6) && editor->menuRowEnabled(0,8),
                  "map menus are not disabled (or the AI mod commands are)");
            check(!fs::exists(install/"mods"/"game"/"Harder AI"),"the pack was written before SAVE");
            check(editor->scenario.starts.empty() && !editor->dirty && editor->noDocument,"the discarded map was kept, or something is left to save");
            find("araarch");break;
        case 5:click(cell("araarch",0));break;
        case 6:text("0");key(SDLK_RETURN);break;
        case 7:
            check(packFile("ai/takengine.txt").find("\nweight araarch 0\n")!=std::string::npos && weight("araarch")==0,"the weight did not reach the pack's ai/takengine.txt");
            check(editor->aiFiles.empty() && !editor->dirty,"the edit touched the map");
            check(editor->aiModDirty() && title.ends_with(" *"),"the pack is not marked unsaved");
            capture("-aimod.png");click(editor->aiTabRects[0]);break;
        case 8:check(editor->aiTabKey()=="ai/default.txt" && editor->aiOwnRadio.w==0 && editor->aiStandardRadio.w==0,"the DEFAULT tab shows the map's profile choice");
            for(const auto& row:editor->aiDefaultRoster)if(row.type && row.reachable && !row.lockedForDefault && !row.variant && row.key!="araarch") {unit=row.key;break;}
            check(!unit.empty(),"no DEFAULT unit to set");
            click(editor->aiStartButton);break;
        case 9:check(editor->aiStartOpen,"no Start from");click(editor->aiStartEmpty);break;
        case 10:check(!packFile("ai/default.txt").empty(),"Start from Empty wrote no ai/default.txt");find(unit);break;
        case 11:click(cell(unit,0));break;
        case 12:text("40");key(SDLK_RETURN);break;
        case 13:check(chance(unit)==40,"the DEFAULT chance was not set to 40 %");capture("-aimod-default.png");click(editor->aiModNewNameButton);break;
        case 14:check(editor->aiFocus==cart::Editor::AI_NAME,"NEW NAME did not open its field");text("islands");key(SDLK_RETURN);break;
        case 15:check(editor->aiTabKey()=="ai/islands.txt","NEW NAME did not switch the DEFAULT tab to ai/islands.txt");click(editor->aiStartButton);break;
        case 16:click(editor->aiStartEmpty);break;
        case 17:check(!packFile("ai/islands.txt").empty() && packFile("ai/islands.txt")!=packFile("ai/default.txt"),"ai/islands.txt was not started");
            click(editor->aiModSaveButton);break;
        case 18:{
            check(editor->aiModSaveOpen && editor->aiModRows.size()==3,"SAVE TO PACK does not offer the three files");
            for(const auto& row:editor->aiModRows)check(row.tick,"a new file is not ticked: "+row.target);
            check(editor->aiModPlanError.empty() && editor->aiModPlan.create.size()>=2,"the new pack's folders are not planned: "+editor->aiModPlanError);
            click(editor->aiModWriteButton);break;
        }
        case 19:
            check(editor->modal==M::M_MESSAGE && editor->mTitle=="SAVED","the new pack was not saved");
            check(listing(pack)==std::set<std::string>{"default.txt","islands.txt","takengine.txt"},"the pack's ai/ does not hold exactly the three files");
            check(slurp(pack/"takengine.txt")==packFile("ai/takengine.txt") && slurp(pack/"default.txt")==packFile("ai/default.txt") &&
                  slurp(pack/"islands.txt")==packFile("ai/islands.txt"),"the files on disk differ from the window's");
            check(!editor->aiModDirty() && slurp(install/"mods"/"game"/"Other"/"ai"/"takengine.txt")=="weight araarch 2\n","unsaved after the save, or another pack touched");
            saved=packFile("ai/takengine.txt");
            key(SDLK_RETURN);break;
        case 20:click(editor->aiDoneButton);break;
        case 21:check(!editor->aiModOnly && !editor->aiOpen && !editor->aiMod && editor->modal==M::M_NONE && editor->noDocument && !editor->dirty && title=="Cartographer",
                      "CLOSE did not end the AI mod (back to the empty editor)");
            key(SDLK_o,KMOD_CTRL|KMOD_SHIFT);break;
        case 22:break;
        case 23:{
            check(editor->aiModOpenOpen && editor->aiModPickWorkspace && editor->modal==M::M_NONE,"Ctrl+Shift+O did not list the packs");
            SDL_Rect row{};for(const auto& [r,name]:editor->aiModPackRows)if(name=="Harder AI")row=r;
            click(row);break;
        }
        case 24:check(editor->aiModOnly && editor->aiMod && editor->aiMod->pack=="Harder AI" && editor->aiMod->files.size()==3 && !editor->aiModDirty(),"Open AI mod did not open the saved pack");
            check(editor->aiTab==1 && weight("araarch")==0 && packFile("ai/takengine.txt")==saved,"the reopened TAK ENGINE AI value differs");
            click(editor->aiTabRects[0]);break;
        case 25:check(editor->aiTabKey()=="ai/default.txt" && chance(unit)==40,"the reopened DEFAULT chance differs");
            click(editor->aiModDefaultButton);break;
        case 26:check(editor->aiTabKey()=="ai/islands.txt","the file button does not step to the named profile");
            click(editor->aiTestButton);break;
        case 27:check(editor->modal==M::M_OPENPATH && editor->openForTest && !editor->openCatalog.empty(),"TEST IN GAME did not open the map picker");
            text("Ulasem Arena");key(SDLK_RETURN);break;
        case 28:check(editor->modal==M::M_MESSAGE && editor->mTitle=="TEST IN GAME","the test did not start: "+editor->mTitle+" "+(editor->mMsg.empty()?"":editor->mMsg[0]));
            key(SDLK_RETURN);break;
        case 29:
            if(!fs::exists(record) || editor->playtest.running()) {if(++waited>300)check(false,"the client never ran");return;}
            {
                const auto got=slurp(record);
                check(got.starts_with("--data\n"+install.string()+"\n--play-map\n") && got.find("\n--game-mod\nHarder AI\nsnapshot maps=1\n")!=std::string::npos,
                      "the client did not get --play-map <test copy> --game-mod \"Harder AI\": "+got);
                fs::remove(record);
            }
            // An unsaved edit: TEST IN GAME asks, saves (replacing, so the compare), then picks.
            click(editor->aiTabRects[1]);break;
        case 30:find("araarch");break;
        case 31:click(cell("araarch",0));break;
        case 32:text("3");key(SDLK_RETURN);break;
        case 33:check(editor->aiModDirty(),"the edit is not unsaved");click(editor->aiTestButton);break;
        case 34:check(editor->modal==M::M_CONFIRM && editor->mTitle=="TEST IN GAME","an unsaved pack was tested without asking");key(SDLK_RETURN);break;
        case 35:check(editor->aiModSaveOpen,"the save dialog did not open");click(editor->aiModWriteButton);break;
        case 36:check(editor->modal==M::M_CONFIRM && editor->mTitle=="REPLACE PACK FILES","replacing the pack's file did not show the compare");key(SDLK_RETURN);break;
        case 37:check(editor->modal==M::M_OPENPATH && editor->openForTest && !editor->aiModDirty() && slurp(pack/"takengine.txt").find("\nweight araarch 3\n")!=std::string::npos,
                      "after the save the map picker did not follow, or the pack was not saved");
            key(SDLK_ESCAPE);break;
        // Quitting with an unsaved edit asks first; the pack stays as saved.
        case 38:saved=slurp(pack/"takengine.txt");click(cell("araarch",0));break;
        case 39:text("8");key(SDLK_RETURN);break;
        case 40:{check(editor->aiModDirty(),"the last edit is not unsaved");SDL_Event quit{};quit.type=SDL_QUIT;SDL_PushEvent(&quit);break;}
        case 41:check(editor->modal==M::M_CONFIRM && editor->mTitle=="CLOSE PACK" && editor->running,"quitting did not ask about the pack");key(SDLK_RETURN);break;
        default:if(++waited>60)check(false,"the editor did not quit");break;
        }
        ++stage;
    },hooks);
    const bool unchanged=slurp(pack/"takengine.txt")==saved;
    fs::remove_all(root);
    if(result || !failure.empty() || !unchanged) {std::cerr<<failure<<(unchanged?"":" (the quit wrote the unsaved edit)")<<'\n';return 1;}
    std::cout<<"PASS: AI mod without a map: New AI mod (no map shown, map menus off), TAK ENGINE + DEFAULT + named profile saved into a new pack, reopened by Open AI mod, TEST IN GAME --game-mod (asks and saves first), quit asks\n";return 0;
}
// OPEN FROM GAME MOD with more packs than its 14 rows, against a SCRATCH data root
// with 22 synthetic game packs (an ai/takengine.txt each): the wheel, a track click, a thumb
// drag and End all reach the last pack, the selection stays in view, Enter opens it.
// TAK_EDITOR_TEST_CAPTURE=<prefix> also writes <prefix>-packs.png (scrolled to the end).
static int aiPackListWorkflow(const char* data) {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-editor-aipacks-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);isolateProfile(root.string().c_str());
    const auto install=root/"data";fs::create_directory(install);
    for(const auto& entry:fs::directory_iterator(data))
        if(tak::hpi::MountSet::key(entry.path().filename().string())!="mods")linkOrCopy(fs::absolute(entry.path()),install/entry.path().filename());
    constexpr int kPacks=22;
    for(int i=0;i<kPacks;++i) {
        char name[16];std::snprintf(name,sizeof name,"Pack%02d",i);
        const auto ai=install/"mods"/"game"/name/"ai";fs::create_directories(ai);
        std::ofstream(ai/"takengine.txt",std::ios::binary)<<"weight araarch "<<i<<"\n";
    }
    std::vector<std::string> args={"cartographer","Ulasem Arena","--data",install.string(),"--out",root.string()};
    std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    cart::Editor* editor=nullptr;cart::EditorHooks hooks;hooks.attached=[&](cart::Editor& e){editor=&e;};
    std::string failure,last;int lastFrame=0;
    auto check=[&](bool value,const std::string& message) {if(!value && failure.empty())failure="frame "+std::to_string(lastFrame)+": "+message;};
    auto shown=[&](const std::string& pack) {
        for(const auto& [r,p]:editor->aiModPackRows)if(p==pack)
            return r.y>=editor->aiModPackList.y && r.y+r.h<=editor->aiModPackList.y+editor->aiModPackList.h;
        return false;
    };
    const int result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window*,SDL_Renderer* renderer,int frame) {
        lastFrame=frame;
        float sx=1,sy=1;SDL_RenderGetScale(renderer,&sx,&sy);
        auto mouse=[&](Uint32 type,int x,int y) {
            SDL_Event e{};e.type=type;
            if(type==SDL_MOUSEMOTION) {e.motion.x=int(x*sx);e.motion.y=int(y*sy);e.motion.state=SDL_BUTTON_LMASK;}
            else {e.button.button=SDL_BUTTON_LEFT;e.button.clicks=1;e.button.x=int(x*sx);e.button.y=int(y*sy);}
            SDL_PushEvent(&e);
        };
        auto click=[&](const SDL_Rect& r) {
            if(r.w<=0) {check(false,"target not drawn");return;}
            mouse(SDL_MOUSEBUTTONDOWN,r.x+r.w/2,r.y+r.h/2);mouse(SDL_MOUSEBUTTONUP,r.x+r.w/2,r.y+r.h/2);
        };
        auto capture=[&](const char* suffix) {
            const char* prefix=SDL_getenv("TAK_EDITOR_TEST_CAPTURE");if(!prefix)return;
            int pw=0,ph=0;SDL_GetRendererOutputSize(renderer,&pw,&ph);std::vector<uint8_t> rgba(size_t(pw)*ph*4);
            if(SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,rgba.data(),pw*4)==0)tak::png::write(std::string(prefix)+suffix,pw,ph,rgba);
        };
        const auto bar=editor?editor->aiModPackBar():cart::ScrollBar{};
        switch(frame) {
        case 0:key(SDLK_a);break;
        case 1:check(editor->aiOpen && int(editor->aiPacks.size())==kPacks,"the window did not list the 22 scratch packs");
            if(!editor->aiPacks.empty())last=editor->aiPacks.back();
            click(editor->aiModOpenButton);break;
        case 2:check(editor->aiModOpenOpen && bar.active() && shown(editor->aiPacks.front()) && !shown(last),"the list does not start at the top with a scrollbar");
            check(int(editor->aiModPackRows.size())<=cart::Editor::kAiPackRowsShown+1,"rows outside the list are hit targets");
            for(int i=0;i<10;++i) {SDL_Event e{};e.type=SDL_MOUSEWHEEL;e.wheel.y=-1;SDL_PushEvent(&e);}
            break;
        case 3:check(editor->aiModPackScroll==bar.maxScroll() && shown(last),"the wheel does not reach the last pack");
            capture("-packs.png");key(SDLK_HOME);break;
        case 4:check(editor->aiModPackScroll==0 && editor->aiModPackSel==0,"Home did not go back to the first pack");
            mouse(SDL_MOUSEBUTTONDOWN,bar.x+bar.w/2,bar.y+bar.h-2);mouse(SDL_MOUSEBUTTONUP,bar.x+bar.w/2,bar.y+bar.h-2);break;
        case 5:check(editor->aiModPackScroll==bar.maxScroll() && shown(last),"a track click did not page down");
            key(SDLK_HOME);break;
        case 6:{   // drag the thumb from the top to the bottom of the track
            const int top=bar.thumbTop(0)+bar.thumbHeight()/2;
            mouse(SDL_MOUSEBUTTONDOWN,bar.x+bar.w/2,top);mouse(SDL_MOUSEMOTION,bar.x+bar.w/2,top+bar.h);mouse(SDL_MOUSEBUTTONUP,bar.x+bar.w/2,top+bar.h);
            break;
        }
        case 7:check(editor->aiModPackScroll==bar.maxScroll() && shown(last) && editor->aiModOpenOpen,"dragging the thumb did not reach the last pack");
            key(SDLK_HOME);for(int i=0;i<16;++i)key(SDLK_DOWN);break;
        case 8:check(editor->aiModPackSel==16 && shown(editor->aiPacks[16]),"Down does not keep the selected pack in view");
            key(SDLK_END);break;
        case 9:check(editor->aiModPackSel==kPacks-1 && shown(last),"End does not show the last pack");
            key(SDLK_PAGEUP);break;
        case 10:check(editor->aiModPackSel==kPacks-1-(cart::Editor::kAiPackRowsShown-1) && shown(editor->aiPacks[size_t(editor->aiModPackSel)]),"PgUp lost the selection");
            key(SDLK_END);key(SDLK_RETURN);break;
        case 11:check(!editor->aiModOpenOpen && editor->aiMod && editor->aiMod->pack==last,"Enter did not open the last pack");
            click(editor->aiDoneButton);break;
        case 12:{SDL_Event quit{};quit.type=SDL_QUIT;SDL_PushEvent(&quit);break;}
        case 13:if(editor->modal==cart::Editor::M_QUITSAVE)click(editor->mQuit);break;
        default:if(frame>18) {failure+=" (AI pack list workflow did not exit)";std::_Exit(3);}
        }
    },hooks);
    fs::remove_all(root);
    if(result || !failure.empty() || lastFrame>18) {std::cerr<<failure<<'\n';return 1;}
    std::cout<<"PASS: AI Profiles OPEN FROM GAME MOD: 22 packs, wheel, track, thumb drag and keys reach the last; Enter opens it\n";return 0;
}
// The AI window's phase-5 extras, against a SCRATCH data root whose game mod "Extra"
// adds one synthetic unit: the pack roster (PREVIEW WITH GAME MOD and OPEN FROM GAME
// MOD list the pack's units; back to the base install without it), the DEFAULT group
// rules (a word from this install's catalogue picked, a unit set individually shown
// as skipped, the value edited; a second rule moved down and back up, one undo step
// each, to the same bytes), Ctrl+Z / Ctrl+Y inside the window down to where it
// opened, and a limit in play previewed on Hard (Hard raises 3 to 4). TAK_EDITOR_TEST_CAPTURE=
// <prefix> also writes <prefix>-groups.png and <prefix>-hard.png.
static int aiExtrasWorkflow(const char* data) {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-editor-aiextras-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);isolateProfile(root.string().c_str());
    const auto install=root/"data";fs::create_directory(install);
    for(const auto& entry:fs::directory_iterator(data))
        if(tak::hpi::MountSet::key(entry.path().filename().string())!="mods")linkOrCopy(fs::absolute(entry.path()),install/entry.path().filename());
    const auto pack=install/"mods"/"game"/"Extra";fs::create_directories(pack/"units");
    // A synthetic unit: no retail text.
    std::ofstream(pack/"units"/"zzpack.fbi",std::ios::binary)<<"[UNITINFO]\n{\n\tUnitName=zzpack;\n\tName=Pack Test Unit;\n\tSide=ARAMON;\n\tCategory=ARA LAND;\n\tMaxVelocity=1;\n}\n";
    std::vector<std::string> args={"cartographer","Ulasem Arena","--data",install.string(),"--out",root.string()};
    std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    cart::Editor* editor=nullptr;cart::EditorHooks hooks;hooks.attached=[&](cart::Editor& e){editor=&e;};
    std::string failure;int lastFrame=0;
    auto check=[&](bool value,const std::string& message) {if(!value && failure.empty())failure="frame "+std::to_string(lastFrame)+": "+message;};
    auto file=[&](const std::string& key) {const auto it=editor->aiFiles.find(key);return it==editor->aiFiles.end()?std::string():std::string(it->second.begin(),it->second.end());};
    auto listed=[&](const std::vector<cart::Editor::AiRosterRow>& roster,const std::string& key)->const cart::Editor::AiRosterRow* {
        for(const auto& row:roster)if(row.key==key)return &row;
        return nullptr;
    };
    std::string word,unit,unitId,own;   // the group word, a unit it covers (DEFAULT key, UnitType id)
    std::string word2,ordered;std::vector<uint16_t> covered;   // the move down / up check
    const int result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window*,SDL_Renderer* renderer,int frame) {
        lastFrame=frame;
        float sx=1,sy=1;SDL_RenderGetScale(renderer,&sx,&sy);
        auto click=[&](const SDL_Rect& r) {
            if(r.w<=0) {check(false,"target not drawn");return;}
            SDL_Event e{};e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;e.button.clicks=1;
            e.button.x=int((r.x+r.w/2)*sx);e.button.y=int((r.y+r.h/2)*sy);SDL_PushEvent(&e);e.type=SDL_MOUSEBUTTONUP;SDL_PushEvent(&e);
        };
        auto capture=[&](const char* suffix) {
            const char* prefix=SDL_getenv("TAK_EDITOR_TEST_CAPTURE");if(!prefix)return;
            int pw=0,ph=0;SDL_GetRendererOutputSize(renderer,&pw,&ph);std::vector<uint8_t> rgba(size_t(pw)*ph*4);
            if(SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,rgba.data(),pw*4)==0)tak::png::write(std::string(prefix)+suffix,pw,ph,rgba);
        };
        auto cell=[&](const std::string& key,int column) {
            for(const auto& c:editor->aiCells)if(c.key==key && c.column==column)return c.rect;
            return SDL_Rect{};
        };
        auto groups=[&]() {return editor->aiTabView().draft.groups;};
        switch(frame) {
        case 0:   // a map edit from before the window: Ctrl+Z inside it must not reach this
            editor->useOnly.insert("araarch");editor->dirty=editor->historyPending=true;key(SDLK_a);break;
        case 1:check(editor->aiOpen && editor->aiRosterPack.empty(),"the window did not open on the base install's units");
            check(!listed(editor->aiLevelRoster,"zzpack"),"a pack unit is listed without the pack");
            check(editor->aiPacks==std::vector<std::string>{"Extra"},"the scratch pack is not found");
            click(editor->aiPackButton);break;
        case 2:{
            const auto* level=listed(editor->aiLevelRoster,"zzpack");const auto* def=listed(editor->aiDefaultRoster,"zzpack");
            check(editor->aiRosterPack=="Extra" && level && level->fromPack && def && def->fromPack,"PREVIEW WITH GAME MOD does not list the pack's unit");
            check(!listed(editor->aiLevelRoster,"araarch") || !listed(editor->aiLevelRoster,"araarch")->fromPack,"a base unit is marked as the pack's");
            click(editor->aiPackButton);break;
        }
        case 3:check(editor->aiRosterPack.empty() && !listed(editor->aiLevelRoster,"zzpack"),"the base roster did not come back");
            click(editor->aiTabRects[0]);click(editor->aiOwnRadio);break;
        case 4:check(editor->aiStartOpen,"no Start from");click(editor->aiStartEmpty);break;
        case 5:own=cart::aiProfileFileKey(editor->scenario.aiProfile);
            check(!file(own).empty(),"no own profile file");click(editor->aiGroupsButton);break;
        case 6:{
            check(editor->aiView==cart::Editor::AiView::Groups && editor->aiGroupRows.empty(),"GROUP RULES did not open on an empty list");
            // A real category word of this install covering at least two buildable units.
            const auto& c=*editor->aiCatalogue;
            for(const auto& w:cart::aiGroupWords(c)) {
                if(w=="all")continue;
                std::vector<std::string> reach;
                for(const auto i:c.categories.at(w))if(const auto* row=listed(editor->aiDefaultRoster,c.names[i]);row && row->reachable && !row->lockedForDefault && row->type)reach.push_back(row->key);
                if(reach.size()>=2) {word=w;unit=reach[0];break;}
            }
            check(!word.empty(),"no category word covers two buildable units");
            if(!word.empty())unitId=listed(editor->aiDefaultRoster,unit)->type->id;
            click(editor->aiAddGroupButton);break;
        }
        case 7:check(editor->aiWordPick==-1,"ADD GROUP RULE did not open the word picker");capture("-words.png");
            check(editor->aiWordRows.size()==cart::aiGroupWords(*editor->aiCatalogue).size(),"the picker does not list exactly this install's category words");
            text(word);break;
        case 8:{
            SDL_Rect row{};for(const auto& [r,w]:editor->aiWordRows)if(w==word)row=r;
            click(row);break;
        }
        case 9:check(editor->aiWordPick==-2 && groups().size()==1 && groups()[0].word==word && groups()[0].weight,"the word did not add a weight rule");
            check(file(own).find("\nweight "+word+" "+cart::classicChanceText(50)+"\n")!=std::string::npos,"the group rule was not written at 50 %");
            click(editor->aiGroupsButton);break;   // back to the table
        case 10:click(editor->aiSearchBox);break;
        case 11:text(unit);break;
        case 12:click(cell(unit,0));break;
        case 13:text("80");key(SDLK_RETURN);break;
        case 14:check(file(own).find("\nweight "+unit+" "+cart::classicChanceText(80)+"\n")!=std::string::npos,"the unit row was not written");
            click(editor->aiGroupsButton);break;
        case 15:{
            const auto& view=editor->aiTabView();const auto& c=*editor->aiCatalogue;
            const auto units=cart::aiGroupRuleUnits(view.draft,0,c);
            int skipped=0,halved=0;
            for(const auto& u:units) {
                if(c.names[u.index]==unit)check(u.skipped && view.classic.pref[u.index]==80,"the unit set individually is not skipped, or not at 80 %");
                else if(!u.skipped && u.times==1 && view.classic.pref[u.index]==50)++halved;
                skipped+=u.skipped;
            }
            check(skipped==1 && halved>=1,"the rule's covered units are wrong");
            check(editor->aiGroupRows.size()==1 && editor->aiGroupUnits.w>0,"the rule row or its unit list is not drawn");
            capture("-groups.png");click(editor->aiGroupRows.empty()?SDL_Rect{}:editor->aiGroupRows[0].value);break;
        }
        case 16:check(editor->aiFocus==cart::Editor::AI_GROUP,"the value did not take focus");
            key(SDLK_a,KMOD_CTRL);key(SDLK_BACKSPACE);text("25");key(SDLK_RETURN);break;
        case 17:check(groups().size()==1 && groups()[0].value==cart::classicChanceText(25),"the value edit was not written");key(SDLK_z,KMOD_CTRL);break;
        case 18:check(editor->aiOpen && groups().size()==1 && groups()[0].value==cart::classicChanceText(50),"Ctrl+Z in the window did not take back the value");
            key(SDLK_y,KMOD_CTRL);break;
        case 19:check(groups().size()==1 && groups()[0].value==cart::classicChanceText(25),"Ctrl+Y did not restore the value");
            for(int i=0;i<5;++i)key(SDLK_z,KMOD_CTRL);   // four steps, then the floor
            break;
        case 20:check(editor->aiOpen && cart::standardAiProfile(editor->scenario.aiProfile) && editor->aiFiles.empty(),"undo did not walk back to the window's start");
            check(editor->aiNote.starts_with("Nothing to undo in this window"),"undo went past where the window opened: "+editor->aiNote);
            check(editor->useOnly.count("araarch")==1,"undo in the window took back a map edit from before it opened");
            for(int i=0;i<4;++i)key(SDLK_y,KMOD_CTRL);
            break;
        case 21:check(!cart::standardAiProfile(editor->scenario.aiProfile) && file(own).find("\nweight "+word+" "+cart::classicChanceText(25)+"\n")!=std::string::npos,
                      "redo did not bring every step back");
            click(editor->aiGroupRows.empty()?SDL_Rect{}:editor->aiGroupRows[0].keyword);break;
        case 22:check(groups().size()==1 && !groups()[0].weight && file(own).find("\nlimit "+word+" 2\n")!=std::string::npos,"WEIGHT did not turn into a limit rule");
            click(editor->aiAddGroupButton);break;
        case 23:{   // a second rule, then MOVE DOWN / MOVE UP: one undo step each, back to the same bytes
            SDL_Rect row{};
            for(const auto& [r,w]:editor->aiWordRows)if(w!=word && w!="all") {row=r;word2=w;break;}
            click(row);break;
        }
        case 24:check(groups().size()==2 && groups()[1].word==word2 && editor->aiGroupSelected==1,"the second rule was not added last and selected");
            check(editor->aiGroupRows.size()==2 && editor->aiGroupRows[0].up.w==0 && editor->aiGroupRows[0].down.w>0 &&
                  editor->aiGroupRows[1].up.w>0 && editor->aiGroupRows[1].down.w==0,"move up / move down are not offered exactly where a rule can move");
            ordered=file(own);covered.clear();
            for(const auto& u:cart::aiGroupRuleUnits(editor->aiTabView().draft,0,*editor->aiCatalogue))covered.push_back(u.index);
            click(editor->aiGroupRows.empty()?SDL_Rect{}:editor->aiGroupRows[0].down);break;
        case 25:{
            check(groups().size()==2 && groups()[0].word==word2 && groups()[1].word==word && file(own)!=ordered,"MOVE DOWN did not swap the rules in the file");
            check(editor->aiGroupSelected==1 && editor->aiGroupUnitScroll==0,"the moved rule is not still selected");
            std::vector<uint16_t> now;
            for(const auto& u:cart::aiGroupRuleUnits(editor->aiTabView().draft,size_t(std::max(0,editor->aiGroupSelected)),*editor->aiCatalogue))now.push_back(u.index);
            check(now==covered,"the covered-units list does not follow the moved rule");
            key(SDLK_z,KMOD_CTRL);break;
        }
        case 26:check(file(own)==ordered,"one Ctrl+Z did not take back MOVE DOWN");key(SDLK_y,KMOD_CTRL);break;
        case 27:check(groups().size()==2 && groups()[0].word==word2,"Ctrl+Y did not redo MOVE DOWN");
            click(editor->aiGroupRows.size()<2?SDL_Rect{}:editor->aiGroupRows[1].up);break;
        case 28:check(file(own)==ordered && editor->aiGroupSelected==0,"MOVE DOWN then MOVE UP did not give back the same bytes");
            click(editor->aiTabRects[1]);click(editor->aiPreviewBox);break;   // TAK ENGINE AI, previewing Hard
        case 29:check(editor->aiTab==1 && editor->aiTabLevel()==tak::ai::Difficulty::Hard,"the TAK ENGINE AI tab is not previewing Hard");
            click(editor->aiSearchBox);key(SDLK_a,KMOD_CTRL);key(SDLK_BACKSPACE);text(unitId);break;
        case 30:click(cell(unitId,1));break;
        case 31:text("3");key(SDLK_RETURN);break;
        case 32:{
            check(file("ai/takengine.txt").find("\nlimit "+unitId+" 3\n")!=std::string::npos,"the limit was not written to ai/takengine.txt");
            const auto* row=listed(editor->aiLevelRoster,unitId);
            const auto why=row?editor->aiRowExplanation(*row):std::string();
            check(why.find("In play on Hard: 4 in play")!=std::string::npos,"Hard's limit in play is not shown: "+why);
            capture("-hard.png");click(editor->aiModOpenButton);break;
        }
        case 33:{
            SDL_Rect row{};for(const auto& [r,p]:editor->aiModPackRows)if(p=="Extra")row=r;
            click(row);break;
        }
        case 34:check(editor->aiMod && editor->aiRosterPack=="Extra" && listed(editor->aiLevelRoster,"zzpack"),"OPEN FROM GAME MOD does not list the pack's units");
            key(SDLK_z,KMOD_CTRL);break;
        case 35:check(editor->aiNote.find("no undo")!=std::string::npos,"Ctrl+Z on a pack's files did not say they have no undo");
            click(editor->aiModOpenButton);break;   // CLOSE PACK
        case 36:check(!editor->aiMod && editor->aiRosterPack.empty() && !listed(editor->aiLevelRoster,"zzpack"),"closing the pack did not go back to the base units");
            click(editor->aiPackButton);break;   // preview Extra, then save the map's files into it
        case 37:click(editor->aiModSaveButton);break;
        case 38:check(editor->aiModSaveOpen,"SAVE AS GAME MOD did not open");click(editor->aiModPackBox);break;
        case 39:key(SDLK_a,KMOD_CTRL);key(SDLK_BACKSPACE);text("extra");break;
        case 40:check(editor->aiRosterLists(editor->aiModPlan.pack) && editor->aiModPlan.pack=="Extra","the dialog still warns that the pack's units are not listed");
            key(SDLK_ESCAPE);key(SDLK_ESCAPE);break;
        case 41:check(!editor->aiModSaveOpen && !fs::exists(pack/"ai"),"Escape wrote into the pack");click(editor->aiDoneButton);break;
        case 42:{SDL_Event quit{};quit.type=SDL_QUIT;SDL_PushEvent(&quit);break;}
        case 43:if(editor->modal==cart::Editor::M_QUITSAVE)click(editor->mQuit);break;
        default:if(frame>48) {failure+=" (AI extras workflow did not exit)";std::_Exit(3);}
        }
    },hooks);
    fs::remove_all(root);
    if(result || !failure.empty() || lastFrame>48) {std::cerr<<failure<<'\n';return 1;}
    std::cout<<"PASS: AI Profiles extras: pack roster, group rules with the lock shown and move down / up, undo inside the window, Hard's limit in play\n";return 0;
}
static int resizeSaveWorkflow(const char* data) {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-editor-resize-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);isolateProfile(root.string().c_str());
    auto vfs=tak::hpi::mountRetailRoot(data);const auto path=tak::hpi::findMap(vfs,"Ulasem Arena");
    auto map=tak::tnt::Map::load(vfs.read(path),path);tak::tnt::Scenario metadata;metadata.kingdom="zhon";
    tak::crt::Scenario scenario;scenario.regions.push_back({"Edge region",map.width-10,map.height-10,map.width-5,map.height-5});
    const auto width=std::to_string(map.width/32),height=std::to_string(map.height/32);
    const auto wider=std::to_string(map.width/32+1),higher=std::to_string(map.height/32+1);
    std::string error;
    const auto files=cart::documentFiles(map,metadata,scenario,{}, {},{},"Resize");
    if(!cart::writeDocumentBundle(root/"Resize.kmp",files,error) || !cart::writeDocumentBundle(root/"Existing.kmp",files,error))throw std::runtime_error(error);
    auto read=[&](const fs::path& file) {std::ifstream in(file,std::ios::binary);return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),{});};
    const auto existing=read(root/"Existing.kmp");
    std::vector<std::string> args={"cartographer","Ulasem Arena","--data",data,"--out",root.string()};
    std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    std::string failure;int lastFrame=0;
    auto check=[&](bool value,const char* message) {if(!value && failure.empty())failure=message;};
    const int result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer*,int frame) {
        lastFrame=frame;const bool dirty=std::string(SDL_GetWindowTitle(window)).ends_with(" *");
        switch(frame) {
        case 0:key(SDLK_o,KMOD_CTRL);break;
        case 1:text((root/"Resize.kmp").string());key(SDLK_RETURN);break;
        case 2:key(SDLK_r);break;
        case 3:text("0");key(SDLK_RETURN);break;
        case 4:check(!dirty,"invalid resize changed map");key(SDLK_ESCAPE);key(SDLK_r);break;
        case 5:text(std::to_string(map.width/32-1));key(SDLK_RETURN);break;
        case 6:check(!dirty,"resize cropped a region");key(SDLK_ESCAPE);key(SDLK_r);break;
        case 7:key(SDLK_RETURN);break;
        case 8:check(!dirty,"same-size resize marked map dirty");key(SDLK_r);break;
        case 9:text(wider);key(SDLK_TAB);text(higher);key(SDLK_RETURN);break;
        case 10:check(dirty,"expansion did not apply");key(SDLK_z,KMOD_CTRL);break;
        case 11:check(!dirty,"expansion undo did not restore saved revision");key(SDLK_y,KMOD_CTRL);key(SDLK_r);break;
        case 12:text(width);key(SDLK_TAB);text(height);key(SDLK_RETURN);break;
        case 13:key(SDLK_ESCAPE);key(SDLK_z,KMOD_CTRL);break;
        case 14:check(!dirty,"cancelled crop consumed undo");key(SDLK_y,KMOD_CTRL);key(SDLK_r);break;
        case 15:text(width);key(SDLK_TAB);text(height);key(SDLK_RETURN);break;
        case 16:key(SDLK_RETURN);break;
        case 17:key(SDLK_z,KMOD_CTRL);key(SDLK_s,KMOD_CTRL|KMOD_SHIFT);break;
        case 18:text("Existing");key(SDLK_RETURN);break;
        case 19:check(read(root/"Existing.kmp")==existing,"Save As replaced file before confirmation");key(SDLK_ESCAPE);break;
        case 20:check(std::string(SDL_GetWindowTitle(window)).find("Resize")!=std::string::npos,"cancelled overwrite renamed map");key(SDLK_s,KMOD_CTRL|KMOD_SHIFT);break;
        case 21:text("Existing");key(SDLK_RETURN);break;
        case 22:key(SDLK_RETURN);break;
        case 23:check(!dirty,"confirmed Save As remains dirty");key(SDLK_RETURN);break;
        case 24:{SDL_Event quit{};quit.type=SDL_QUIT;SDL_PushEvent(&quit);break;}
        default:if(frame>27) {failure="resize/save workflow did not exit";std::_Exit(3);}
        }
    });
    check(read(root/"Existing.kmp.bak")==existing,"overwrite did not retain previous bytes");
    {
        tak::hpi::Archive archive(root/"Existing.kmp");bool found=false;
        for(const auto& entry:archive.entries())if(entry.path.ends_with(".tnt")) {
            const auto saved=tak::tnt::Map::load(archive.read(entry),entry.path);found=saved.width==map.width+32 && saved.height==map.height+32;
            for(int y=0;y<map.height;++y)for(int x=0;x<map.width;++x)
                check(saved.heights[y*saved.width+x]==map.heights[y*map.width+x] && saved.features[y*saved.width+x]==map.features[y*map.width+x],"resize/undo changed retained terrain or features");
        }
        check(found,"crop undo did not restore expanded dimensions");
    }
    fs::remove_all(root);
    if(result || !failure.empty() || lastFrame>27) {std::cerr<<failure<<'\n';return 1;}
    std::cout<<"PASS: resize validation, crop confirmation, undo/redo and overwrite cancellation/backup\n";return 0;
}
static int templateWorkflow(const char* data) {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-editor-templates-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);isolateProfile(root.string().c_str());
    auto vfs=tak::hpi::mountRetailRoot(data);const auto path=tak::hpi::findMap(vfs,"Ulasem Arena");
    auto map=tak::tnt::Map::load(vfs.read(path),path);tak::tnt::Scenario metadata;metadata.kingdom="zhon";
    tak::crt::Scenario scenario;scenario.players.resize(9);scenario.regions.push_back({"Bridge",10,10,20,20});
    std::string error;
    if(!cart::writeDocumentBundle(root/"Templates.kmp",cart::documentFiles(map,metadata,scenario,{}, {},{},"Templates"),error))throw std::runtime_error(error);
    std::vector<std::string> args={"cartographer","Ulasem Arena","--data",data,"--out",root.string()};
    std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    bool failed=false;int lastFrame=0;
    const int result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer* renderer,int frame) {
        lastFrame=frame;float sx,sy;SDL_RenderGetScale(renderer,&sx,&sy);int pw,ph;SDL_GetRendererOutputSize(renderer,&pw,&ph);
        const int w=int(pw/sx),h=int(ph/sy);
        auto click=[&](int x,int y) {SDL_Event e{};e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;e.button.x=int(x*sx);e.button.y=int(y*sy);SDL_PushEvent(&e);e.type=SDL_MOUSEBUTTONUP;SDL_PushEvent(&e);};
        const bool dirty=std::string(SDL_GetWindowTitle(window)).ends_with(" *");
        if(frame<=11 || frame==13 || frame>=15)failed|=dirty;
        switch(frame) {
        case 0:key(SDLK_o,KMOD_CTRL);break;
        case 1:text((root/"Templates.kmp").string());key(SDLK_RETURN);break;
        case 2:key(SDLK_t);break;
        case 3:click(w/2-80,h/2-225);break;
        case 4:text("nothing matches this");key(SDLK_RETURN);break;
        case 5:key(SDLK_ESCAPE);break;
        case 6:click(w/2-80,h/2-225);break;
        case 7:text("reinforce");break;
        case 8:
            if(const char* capture=SDL_getenv("TAK_EDITOR_TEST_CAPTURE")) {std::vector<uint8_t> rgba(size_t(pw)*ph*4);if(SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,rgba.data(),pw*4)==0)tak::png::write(capture,pw,ph,rgba);}
            key(SDLK_ESCAPE);break;
        case 9:click(w/2-80,h/2-225);break;
        case 10:text("ReAcH");break;
        case 11:key(SDLK_RETURN);break;
        case 12:failed|=!dirty;key(SDLK_ESCAPE);key(SDLK_z,KMOD_CTRL);break;
        case 13:key(SDLK_y,KMOD_CTRL);break;
        case 14:failed|=!dirty;key(SDLK_s,KMOD_CTRL);break;
        case 15:key(SDLK_RETURN);key(SDLK_o,KMOD_CTRL);break;
        case 16:text((root/"Templates.kmp").string());key(SDLK_RETURN);break;
        case 17:key(SDLK_t);break;
        case 18:key(SDLK_RETURN);break;
        case 19:key(SDLK_a,KMOD_CTRL);key(SDLK_c,KMOD_CTRL);break;
        case 20:{char* clipboard=SDL_GetClipboardText();failed|=!clipboard || std::string(clipboard)!="Reach a region";SDL_free(clipboard);key(SDLK_ESCAPE);key(SDLK_ESCAPE);SDL_Event quit{};quit.type=SDL_QUIT;SDL_PushEvent(&quit);break;}
        default:if(frame>24) {failed=true;key(SDLK_ESCAPE);key(SDLK_RETURN);}
        }
    });
    tak::hpi::Archive archive(root/"Templates.kmp");bool found=false;
    for(const auto& entry:archive.entries())if(entry.path.ends_with(".crt")) {
        const auto saved=tak::crt::parse(archive.read(entry));
        if(!saved.players.empty() && saved.players[0].size()==1) {
            const auto& group=saved.players[0][0];
            found=group.conditions.size()==1 && group.conditions[0].opcode==15 &&
                group.conditions[0].slot[0]=="0" && group.conditions[0].slot[1]=="Any Unit" &&
                group.conditions[0].slot[2]=="Bridge" && group.actions.size()==2 && group.actions[0].opcode==5 && group.actions[1].opcode==14;
        }
    }
    fs::remove_all(root);
    if(result || failed || !found || lastFrame>24) {std::cerr<<"template workflow failed\n";return 1;}
    std::cout<<"PASS: template search, empty result, cancel, insert, undo/redo and named save/reopen\n";return 0;
}
static int ruleTextWorkflow(const char* data) {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-editor-rules-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);isolateProfile(root.string().c_str());
    auto vfs=tak::hpi::mountRetailRoot(data);const auto path=tak::hpi::findMap(vfs,"Ulasem Arena");
    auto map=tak::tnt::Map::load(vfs.read(path),path);tak::tnt::Scenario metadata;metadata.kingdom="zhon";
    tak::crt::Scenario scenario;scenario.players.resize(9);scenario.players[0].resize(1);
    auto& group=scenario.players[0][0];group.conditions.push_back({1,{"12345"}});
    for(int i=0;i<12;++i)group.actions.push_back({25,{"Player 1","Message "+std::to_string(i)+" "+std::string(45,'x'),"progress",std::string(63,'y')}});
    const auto expected=cart::ruleDetails(group,cart::RuleColumn::Group,0);
    std::string error;
    if(!cart::writeDocumentBundle(root/"Rules.kmp",cart::documentFiles(map,metadata,scenario,{}, {},{},"Rules"),error))throw std::runtime_error(error);
    std::vector<std::string> args={"cartographer","Ulasem Arena","--data",data,"--out",root.string()};
    std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    bool failed=false;int lastFrame=0;std::vector<uint8_t> before;
    const int result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer* renderer,int frame) {
        lastFrame=frame;float sx,sy;SDL_RenderGetScale(renderer,&sx,&sy);int pw,ph;SDL_GetRendererOutputSize(renderer,&pw,&ph);
        const int w=int(pw/sx),h=int(ph/sy);
        auto click=[&](int x,int y) {SDL_Event e{};e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;e.button.x=int(x*sx);e.button.y=int(y*sy);SDL_PushEvent(&e);e.type=SDL_MOUSEBUTTONUP;SDL_PushEvent(&e);};
        auto pixels=[&]() {SDL_Rect r{int((w/2-376)*sx),int((h/2+133)*sy),int(752*sx),int(78*sy)};std::vector<uint8_t> out(size_t(r.w)*r.h*4);if(SDL_RenderReadPixels(renderer,&r,SDL_PIXELFORMAT_RGBA32,out.data(),r.w*4))failed=true;return out;};
        const bool dirty=std::string(SDL_GetWindowTitle(window)).ends_with(" *");
        if(frame<=11 || (frame>=14 && frame<=16) || frame>=20)failed|=dirty;
        switch(frame) {
        case 0: key(SDLK_o,KMOD_CTRL);break;
        case 1: text((root/"Rules.kmp").string());key(SDLK_RETURN);break;
        case 2: key(SDLK_t);break;
        case 3: click(w/2-350,h/2-166);break; // Select the rule group.
        case 4: before=pixels();click(w/2+338,h/2+124);break;
        case 5: {char* clipboard=SDL_GetClipboardText();failed|=!clipboard || expected!=clipboard;SDL_free(clipboard);key(SDLK_PAGEDOWN);break;}
        case 6: failed|=before==pixels();key(SDLK_PAGEUP);break;
        case 7: {
            failed|=before!=pixels();
            if(const char* capture=SDL_getenv("TAK_EDITOR_TEST_CAPTURE")) {std::vector<uint8_t> rgba(size_t(pw)*ph*4);if(SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,rgba.data(),pw*4)==0)tak::png::write(capture,pw,ph,rgba);}
            break;
        }
        case 8: click(w/2+180,h/2-166);key(SDLK_RETURN);break;
        case 9: key(SDLK_TAB);key(SDLK_TAB);text("temporary_flag");key(SDLK_DOWN,KMOD_ALT);break;
        case 10: click(w/2,h/2+28);key(SDLK_RETURN);break;
        case 11: key(SDLK_RETURN);break;
        case 12: key(SDLK_TAB);key(SDLK_TAB);text("new_flag");key(SDLK_RETURN);break;
        case 13: failed|=!dirty;key(SDLK_ESCAPE);key(SDLK_s,KMOD_CTRL);break;
        case 14: key(SDLK_RETURN);break;
        case 15: key(SDLK_t);key(SDLK_RETURN);break;
        case 16: text("Bridge defense");key(SDLK_RETURN);break;
        case 17: key(SDLK_ESCAPE);key(SDLK_z,KMOD_CTRL);break;
        case 18: failed|=dirty;key(SDLK_y,KMOD_CTRL);break;
        case 19: failed|=!dirty;key(SDLK_s,KMOD_CTRL);break;
        case 20: key(SDLK_RETURN);key(SDLK_o,KMOD_CTRL);break;
        case 21: text((root/"Rules.kmp").string());key(SDLK_RETURN);break;
        case 22: key(SDLK_t);key(SDLK_RETURN);break;
        case 23: key(SDLK_a,KMOD_CTRL);key(SDLK_c,KMOD_CTRL);break;
        case 24: {char* clipboard=SDL_GetClipboardText();failed|=!clipboard || std::string(clipboard)!="Bridge defense";SDL_free(clipboard);key(SDLK_ESCAPE);key(SDLK_ESCAPE);break;}
        case 25: {SDL_Event quit{};quit.type=SDL_QUIT;SDL_PushEvent(&quit);break;}
        default: if(frame>29) {failed=true;key(SDLK_ESCAPE);key(SDLK_RETURN);}
        }
    });
    {
        tak::hpi::Archive archive(root/"Rules.kmp");bool found=false;
        for(const auto& entry:archive.entries())if(entry.path.ends_with(".crt")) {
            const auto saved=tak::crt::parse(archive.read(entry));
            found=!saved.players.empty() && !saved.players[0].empty() && !saved.players[0][0].actions.empty() && saved.players[0][0].actions[0].slot[2]=="new_flag";
        }
        failed|=!found;
    }
    fs::remove_all(root);
    if(result || failed || lastFrame>29) {std::cerr<<"rule text workflow failed\n";return 1;}
    std::cout<<"PASS: full rule text clipboard, page navigation, flag suggestions and new flag persistence\n";return 0;
}
static int regionWorkflow(const char* data) {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-editor-regions-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    isolateProfile(root.string().c_str());
    std::vector<std::string> args={"cartographer","Ulasem Arena","--data",data,"--out",root.string()};
    std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    bool failed=false;int lastFrame=0;
    const int result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer* renderer,int frame) {
        lastFrame=frame;float sx,sy;SDL_RenderGetScale(renderer,&sx,&sy);
        auto mouse=[&](Uint32 type,int x,int y) {
            SDL_Event e{};e.type=type;
            if(type==SDL_MOUSEMOTION) {e.motion.state=SDL_BUTTON_LMASK;e.motion.x=int(x*sx);e.motion.y=int(y*sy);}
            else {e.button.button=SDL_BUTTON_LEFT;e.button.x=int(x*sx);e.button.y=int(y*sy);}
            SDL_PushEvent(&e);
        };
        auto click=[&](int x,int y) {mouse(SDL_MOUSEBUTTONDOWN,x,y);mouse(SDL_MOUSEBUTTONUP,x,y);};
        auto drag=[&](int x,int y,int toX,int toY) {mouse(SDL_MOUSEBUTTONDOWN,x,y);mouse(SDL_MOUSEMOTION,toX,toY);mouse(SDL_MOUSEBUTTONUP,toX,toY);};
        const bool dirty=std::string(SDL_GetWindowTitle(window)).ends_with(" *");
        switch(frame) {
        case 0: key(SDLK_1);click(cart::Editor::kRegionsX+cart::Editor::kRegionsW/2,32);break;
        case 1: drag(361,205,521,365);break; // cells 10,10 through 20,20
        case 2: failed|=!dirty;drag(441,285,473,317);break; // move two cells
        case 3: drag(569,413,601,429);break; // resize right/bottom by two/one cells
        case 4: click(601,429);key(SDLK_z,KMOD_CTRL);break; // no-motion click must not consume undo
        case 5: key(SDLK_y,KMOD_CTRL);click(480,330);key(SDLK_RETURN);break;
        case 6: text("Canvas region");key(SDLK_RETURN);break;
        case 7: mouse(SDL_MOUSEBUTTONDOWN,393,333);mouse(SDL_MOUSEMOTION,280,333);key(SDLK_ESCAPE);mouse(SDL_MOUSEBUTTONUP,280,333);break;
        case 8: key(SDLK_s,KMOD_CTRL);break;
        case 9: failed|=dirty;key(SDLK_RETURN);key(SDLK_o,KMOD_CTRL);break;
        case 10: text((root/"Ulasem Arena.kmp").string());key(SDLK_RETURN);break;
        case 11: failed|=dirty;key(SDLK_s,KMOD_CTRL);break; // serialize the reopened document too
        case 12: failed|=dirty;key(SDLK_RETURN);break;
        case 13: {SDL_Event quit{};quit.type=SDL_QUIT;SDL_PushEvent(&quit);break;}
        default: if(frame>20) {failed=true;key(SDLK_ESCAPE);key(SDLK_RETURN);}
        }
    });
    try {
        if(result || failed || lastFrame>20)throw std::runtime_error("region canvas UI workflow failed");
        tak::hpi::Archive archive(root/"Ulasem Arena.kmp");bool found=false;
        for(const auto& entry:archive.entries())if(entry.path.ends_with(".crt")) {
            const auto scenario=tak::crt::parse(archive.read(entry));
            for(const auto& r:scenario.regions)if(r.name=="Canvas region") {
                if(r.x1!=12 || r.z1!=12 || r.x2!=24 || r.z2!=23)throw std::runtime_error("region bounds differ after draw/move/resize/cancel/undo/reopen");
                found=true;
            }
        }
        if(!found)throw std::runtime_error("canvas region not saved");
        fs::remove_all(root);std::cout<<"PASS: region canvas drawing, movement, handles, cancellation, undo/redo and reopen\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<"; files: "<<root<<'\n';return 1;}
}
static int fontWorkflow() {
    if(SDL_Init(SDL_INIT_VIDEO)!=0)return 2;
    constexpr int W=256,H=64;
    auto* surface=SDL_CreateRGBSurfaceWithFormat(0,W,H,32,SDL_PIXELFORMAT_RGBA32);
    auto* renderer=surface?SDL_CreateSoftwareRenderer(surface):nullptr;
    if(!renderer) {if(surface)SDL_FreeSurface(surface);SDL_Quit();return 2;}
    std::string failure;auto check=[&](bool value,const std::string& message){if(!value && failure.empty())failure=message;};
    {
        cart::EditorFont font(renderer);
        auto pixels=[&](const std::string& text,int scale=2) {
            SDL_SetRenderDrawColor(renderer,0,0,0,255);SDL_RenderClear(renderer);
            cart::drawText(renderer,text,20,20,scale,255,255,255);SDL_RenderPresent(renderer);
            std::vector<uint8_t> rgba(W*H*4);SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,rgba.data(),W*4);return rgba;
        };
        // Horizontal ink extent [first,last] over all rows (-1 when blank).
        auto extent=[&](const std::vector<uint8_t>& rgba) {
            int first=-1,last=-1;for(int x=0;x<W;++x)for(int y=0;y<H;++y)if(rgba[(size_t(y)*W+x)*4]>0) {if(first<0)first=x;last=x;}
            return std::pair{first,last};
        };
        const auto blank=pixels(""),missing=pixels("\xef\xbf\xbd"),latin=pixels("é"),greek=pixels("Ω"),cyrillic=pixels("Ж");
        check(!(latin==missing || greek==missing || cyrillic==missing || pixels("a")==pixels("A") || latin==pixels("e")),"Latin/Greek/Cyrillic glyphs distinct");
        bool antialiased=false;for(size_t i=0;i<latin.size();i+=4)antialiased|=latin[i]>0 && latin[i]<255;
        check(antialiased && cart::textWidth("éΩЖ",1)==18,"antialiasing and 6 px narrow advance");
        // Every added range draws real ink that differs from the replacement box and from its neighbours.
        const std::vector<std::pair<const char*,std::string>> covered{
            {"Vietnamese","Ệ"},{"Armenian","Ա"},{"Armenian lower","ա"},{"Georgian","ს"},{"Georgian Mtavruli","Ა"},{"arrow","↔"},
            {"symbol","★"},{"math","∞"},{"box drawing","┌"},{"braille","⣿"},{"currency","₹"},{"halfwidth katakana","ｱ"},
            {"Hiragana","あ"},{"Katakana","カ"},{"Hangul","한"},{"Han simplified","图"},{"Han traditional","圖"},{"fullwidth Latin","Ａ"},{"CJK punctuation","、"}};
        const auto narrowBox=pixels("\u0905"),wideBox=pixels("\U00020000");
        std::vector<std::vector<uint8_t>> seen;
        for(const auto& [name,glyph]:covered) {
            const auto image=pixels(glyph);
            check(image!=blank,std::string(name)+" draws nothing");
            check(image!=narrowBox && image!=wideBox && image!=missing,std::string(name)+" shows a replacement glyph");
            for(const auto& other:seen)check(image!=other,std::string(name)+" duplicates another glyph");
            seen.push_back(image);
        }
        // Noto Sans Mono draws these on a two-column advance (or overshoots); they must be fitted, i.e. centred
        // in their one column like "H", not drawn at their own metrics with the right half cut off.
        const auto [hx0,hx1]=extent(pixels("H"));
        for(const char* glyph:{"⇒","⇔","⅓","◆","▶","℃","※"}) {
            const auto [g0,g1]=extent(pixels(glyph));
            check(g0>=0 && std::abs((g0+g1)-(hx0+hx1))<=3 && g1-g0<=14,std::string(glyph)+" is clipped or off-centre in its column");
        }
        // Replacement boxes: valid-but-unmapped code points are visible, one or two columns wide; they
        // differ from the U+FFFD that invalid UTF-8 produces.
        const auto [n0,n1]=extent(narrowBox);const auto [w0,w1]=extent(wideBox);const auto [h0,h1]=extent(pixels("日"));
        check(narrowBox!=blank && narrowBox!=missing,"narrow replacement box is visible");
        check(n0>=0 && n1-n0+1>=8 && n1-n0+1<=12,"narrow box spans about one column");
        check(w0>=0 && w1-w0+1>=20 && w1-w0+1<=24 && w1-w0>n1-n0+8,"wide replacement box spans about two columns");
        check(extent(pixels("\U0001F600"))==std::pair{w0,w1},"unmapped emoji is a two column box");
        check(h0>=20 && h1<=20+24,"wide glyph stays inside its two columns");
        check(extent(pixels("日日"))==std::pair{h0,h1+24},"wide advance is two columns");
        check(extent(pixels("a日"))==std::pair{extent(pixels("a")).first,h1+12},"mixed advance");
        // Layout metrics: a wide glyph is two columns everywhere.
        check(cart::textWidth("日本",1)==24 && cart::textColumns("a日Ж")==4 && cart::glyphCells(0x3042)==2 && cart::glyphCells(0x41)==1 && cart::glyphCells(0xff21)==2 && cart::glyphCells(0xff71)==1,"glyphCells/textWidth");
        check(cart::clipColumns("日本語",5)=="日本" && cart::clipColumns("a日",2)=="a" && cart::clipColumns("abc",9)=="abc" && cart::clipColumns("日",1).empty(),"clipColumns");
        // Caret, wrap and selection geometry (textedit.h) follow columns.
        namespace te=cart;
        const std::string jp="日本語日本語";
        auto rows=te::TextEdit::lines(jp,6);
        check(rows.size()==2 && rows[0].end==9 && rows[1].begin==9,"wide wrap at three glyphs per six columns");
        rows=te::TextEdit::lines("ab日本",3);
        check(rows.size()==3 && rows[0].end==2 && rows[1].end==5 && rows[2].begin==5,"a wide glyph that does not fit moves to the next row");
        check(te::TextEdit::lines("日本",1).size()==2,"one column still makes progress");
        check(te::TextEdit::lines("abcdef",3).size()==2,"narrow wrap unchanged");
        te::TextEdit edit;const std::string pair="日本";const auto one=te::TextEdit::lines(pair,0)[0];
        check(te::TextEdit::offsetAt(pair,one,0)==0 && te::TextEdit::offsetAt(pair,one,1)==3 && te::TextEdit::offsetAt(pair,one,2)==3 && te::TextEdit::offsetAt(pair,one,3)==6 && te::TextEdit::offsetAt(pair,one,99)==6,"click column maps left/right halves of a wide glyph");
        check(te::TextEdit::columnOf(pair,one,3)==2 && te::TextEdit::columnOf(pair,one,6)==4,"caret column");
        std::string multi="日本\nabcd";edit.caret=3;edit.anchor=3;
        SDL_Event down{};down.type=SDL_KEYDOWN;down.key.keysym.sym=SDLK_DOWN;
        edit.input(down,multi,false,true,20);
        check(edit.caret==multi.size()-2,"Down keeps the caret in the same text column across wide glyphs");
        SDL_Event up{};up.type=SDL_KEYDOWN;up.key.keysym.sym=SDLK_UP;
        edit.input(up,multi,false,true,20);
        check(edit.caret==3,"Up returns to the same column");
        edit.click(pair,0,0,3);check(edit.caret==6,"click right half of last wide glyph");
        SDL_Event left{};left.type=SDL_KEYDOWN;left.key.keysym.sym=SDLK_LEFT;
        std::string copy=pair;edit.input(left,copy);check(edit.caret==3,"Left steps one wide glyph");
    }
    SDL_DestroyRenderer(renderer);SDL_FreeSurface(surface);SDL_Quit();
    if(!failure.empty()) {std::cerr<<"FAIL: "<<failure<<'\n';return 1;}
    std::cout<<"PASS: embedded Unicode glyph ranges (Latin to CJK), replacement boxes, double-width layout and caret metrics\n";return 0;
}
// Developer aid (not a ctest): `cartographer_ui_test fontsheet out.png` renders sample lines of every
// script the embedded font covers, plus the replacement boxes, for eyeballing generator changes.
static int fontSheet(const char* out) {
    if(SDL_Init(SDL_INIT_VIDEO)!=0)return 2;
    const int width=1100,height=520;
    auto* surface=SDL_CreateRGBSurfaceWithFormat(0,width,height,32,SDL_PIXELFORMAT_RGBA32);
    auto* renderer=surface?SDL_CreateSoftwareRenderer(surface):nullptr;
    if(!renderer)return 2;
    {
        cart::EditorFont font(renderer);
        SDL_SetRenderDrawColor(renderer,24,26,32,255);SDL_RenderClear(renderer);
        const char* lines[]={"Latin: The quick brown fox 0123456789 \u00c9\u00f1\u00fc\u00df \u1ea0\u1ec6\u1ef2 (Vietnamese Vi\u1ec7t Nam)",
            "Greek/Cyrillic: \u0391\u03b2\u03b3\u03b4 \u0416\u0434\u044f \u041f\u0440\u0438\u0432\u0435\u0442",
            "Armenian: \u0540\u0561\u0575\u0561\u057d\u057f\u0561\u0576 \u0531\u0562\u0563\u0564",
            "Georgian: \u10e1\u10d0\u10e5\u10d0\u10e0\u10d7\u10d5\u10d4\u10da\u10dd \u1c91\u1c92",
            "Symbols: \u2190\u2191\u2192\u2193\u2194 \u21d2 \u2200\u2203\u2208 \u221e \u2264\u2265 \u2500\u2502\u250c\u2510 \u2588\u2593 \u25cf\u25a0\u25b2 \u2605\u2606 \u2713\u2717 \u20ac\u00a3\u00a5\u20b9 \u2800\u28ff \u21d4\u2153\u25b6\u25c6\u2103\u203b\u1ea8",
            "Japanese: \u30de\u30c3\u30d7\u306e\u540d\u524d \u3072\u3089\u304c\u306a \u30ab\u30bf\u30ab\u30ca \uff71\uff72\uff73 \uff21\uff22\uff23\uff11\uff12",
            "Chinese: \u5730\u56fe\u540d\u79f0 \u81ea\u5b9a\u4e49\u5730\u56fe \u5730\u5716\u540d\u7a31 \u9f8d\u738b\u6230\u722d",
            "Korean: \ud55c\uad6d\uc5b4 \uc9c0\ub3c4 \uc774\ub984 \uac00\ub098\ub2e4",
            "Missing: [\u0905][\u05d0][\u0e01][\U0001f600][\U00020000] valid-but-absent, [\xff] invalid byte"};
        int y=10;
        for(const char* line:lines) {cart::drawText(renderer,line,10,y,1,235,235,240);cart::drawText(renderer,line,10,y+20,2,200,225,255);y+=54;}
        SDL_RenderPresent(renderer);
        std::vector<uint8_t> rgba(size_t(width)*height*4);
        SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,rgba.data(),width*4);tak::png::write(out,width,height,rgba);
    }
    SDL_DestroyRenderer(renderer);SDL_FreeSurface(surface);SDL_Quit();return 0;
}
static int thumbnailWorkflow(const char* data) {
    if(SDL_Init(SDL_INIT_VIDEO)!=0)return 2;
    int result=0;
    {
        auto assets=tak::hpi::mountRetailRoot(data);cart::SectionLibrary sections;sections.scan(assets,"zhon");
        if(sections.list().size()<5) {SDL_Quit();return 2;}
        auto* surface=SDL_CreateRGBSurfaceWithFormat(0,512,512,32,SDL_PIXELFORMAT_RGBA32);
        auto* renderer=surface?SDL_CreateSoftwareRenderer(surface):nullptr;
        if(!renderer) {if(surface)SDL_FreeSurface(surface);SDL_Quit();return 2;}
        {
            cart::Thumbnails thumbnails(renderer,assets,2);
            auto awaitTexture=[&](const std::string& path) {
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
                SDL_Texture* texture=nullptr;
                while(!(texture=thumbnails.get(path))) {
                    if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("thumbnail worker timeout");
                    SDL_Delay(1);
                }
                return texture;
            };
            try {
                const auto& path=sections.list().front().path;
                const auto* source=sections.load(assets,path);tak::terrain::Compositor compositor(assets);
                const auto reference=compositor.renderMap(*source);
                auto* texture=awaitTexture(path);int w=0,h=0;SDL_QueryTexture(texture,nullptr,nullptr,&w,&h);
                if(w!=reference.width || h!=reference.height)throw std::runtime_error("thumbnail lost source resolution");
                if(w>512 || h>512)throw std::runtime_error("thumbnail fixture exceeds test surface");
                SDL_Rect dst{0,0,w,h};SDL_RenderCopy(renderer,texture,nullptr,&dst);SDL_RenderPresent(renderer);
                std::vector<uint8_t> rgba(size_t(w)*h*4);
                if(SDL_RenderReadPixels(renderer,&dst,SDL_PIXELFORMAT_RGBA32,rgba.data(),w*4)!=0 || rgba!=reference.rgba)
                    throw std::runtime_error("asynchronous thumbnail differs from source compositor");
                for(int i=1;i<4;++i)awaitTexture(sections.list()[i].path);
                if(thumbnails.entries()>2)throw std::runtime_error("thumbnail cache exceeds entry budget");
                thumbnails.get(sections.list()[4].path);thumbnails.reset();
                if(thumbnails.loading() || thumbnails.entries())throw std::runtime_error("thumbnail reset retained work");
                awaitTexture(path);
                const std::string portraitPath="anims/buildpic/araarch.jpg";
                const auto portrait=tak::jpeg::load(assets.read(portraitPath));
                auto* icon=awaitTexture(portraitPath);SDL_QueryTexture(icon,nullptr,nullptr,&w,&h);
                if(w!=portrait.width || h!=portrait.height)throw std::runtime_error("unit portrait aspect/resolution changed");
                dst={0,0,w,h};SDL_RenderCopy(renderer,icon,nullptr,&dst);SDL_RenderPresent(renderer);
                rgba.resize(size_t(w)*h*4);
                if(SDL_RenderReadPixels(renderer,&dst,SDL_PIXELFORMAT_RGBA32,rgba.data(),w*4)!=0 || rgba!=portrait.rgba)
                    throw std::runtime_error("unit portrait colors differ from its retail JPEG");
            } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';result=1;}
        }
        SDL_DestroyRenderer(renderer);SDL_FreeSurface(surface);
    }
    SDL_Quit();if(!result)std::cout<<"PASS: background thumbnail raster, cache eviction and reset with work pending\n";return result;
}
static int minimapWorkflow(const char* data,const char* mapName="Ulasem Arena") {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-editor-minimap-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);isolateProfile(root.string().c_str());
    auto vfs=tak::hpi::mountRetailRoot(data);const auto path=tak::hpi::findMap(vfs,mapName);
    const auto original=tak::tnt::Map::load(vfs.read(path),path);tak::terrain::Compositor compositor(vfs);
    auto cancelled=std::make_shared<std::atomic_bool>(true);
    const auto otaBytes=vfs.read(path.substr(0,path.size()-4)+".ota");
    auto world=tak::tnt::Scenario::parse(std::string(otaBytes.begin(),otaBytes.end())).kingdom;
    std::transform(world.begin(),world.end(),world.begin(),[](unsigned char c){return char(std::tolower(c));});
    const auto palette=cart::loadWorldPalette(vfs,world);
    if(!cart::minimapPreview(original,compositor,palette,cancelled).rgba.empty())throw std::runtime_error("cancelled minimap produced pixels");
    const auto originalPreview=cart::minimapPreview(original,compositor,palette).rgba;
    std::vector<std::string> args={"cartographer",mapName,"--data",data,"--out",root.string()};
    std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    cart::EditorHooks hooks;int updates=0;std::vector<uint8_t> pixels,paintedPixels;
    hooks.minimapUpdated=[&](const auto& rgba) {++updates;pixels=rgba;};
    int stage=0;bool failed=false;const auto began=std::chrono::steady_clock::now();
    const int result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer* renderer,int) {
        if(std::chrono::steady_clock::now()-began>std::chrono::seconds(30))std::_Exit(3);
        const bool dirty=std::string(SDL_GetWindowTitle(window)).ends_with(" *");
        auto click=[&](int x,int y) {
            float sx,sy;SDL_RenderGetScale(renderer,&sx,&sy);SDL_Event e{};e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;e.button.x=int(x*sx);e.button.y=int(y*sy);SDL_PushEvent(&e);
            e.type=SDL_MOUSEBUTTONUP;SDL_PushEvent(&e);
        };
        switch(stage) {
        case 0:key(SDLK_1);++stage;break;
        case 1:click(500,300);++stage;break;
        case 2:if(updates==1) {failed|=!dirty;paintedPixels=pixels;key(SDLK_z,KMOD_CTRL);++stage;}break;
        case 3:if(updates==2) {failed|=dirty || pixels!=originalPreview;key(SDLK_y,KMOD_CTRL);++stage;}break;
        case 4:if(updates==3) {failed|=!dirty || pixels!=paintedPixels;key(SDLK_s,KMOD_CTRL);++stage;}break;
        case 5:failed|=dirty;key(SDLK_RETURN);++stage;break;
        case 6:{SDL_Event q{};q.type=SDL_QUIT;SDL_PushEvent(&q);++stage;break;}
        }
        SDL_Delay(1);
    },hooks);
    try {
        if(result || failed || stage!=7)throw std::runtime_error("live minimap workflow failed");
        tak::hpi::Archive archive(root/(std::string(mapName)+".kmp"));bool found=false;
        for(const auto& entry:archive.entries())if(entry.path.ends_with(".tnt")) {
            const auto saved=tak::tnt::Map::load(archive.read(entry),entry.path);
            std::vector<uint8_t> expected(saved.minimap.size()*4);
            for(size_t i=0;i<saved.minimap.size();++i)std::copy(palette.rgba[saved.minimap[i]],palette.rgba[saved.minimap[i]]+4,expected.begin()+i*4);
            if(expected!=paintedPixels)throw std::runtime_error("background preview differs from saved minimap");
            found=true;
        }
        if(!found)throw std::runtime_error("saved minimap missing");
        fs::remove_all(root);std::cout<<"PASS: live background minimap, undo/redo, unchanged dirty state and saved image parity\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<"; files: "<<root<<'\n';return 1;}
}
static int featureWorkflow(const char* data) {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-editor-features-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    isolateProfile(root.string().c_str());
    auto vfs=tak::hpi::mountRetailRoot(data);const auto path=tak::hpi::findMap(vfs,"Ulasem Arena");
    auto map=tak::tnt::Map::load(vfs.read(path),path);
    if(map.featureNames.empty())return 2;
    map.features.assign(map.features.size(),0xFFFF);map.features[10*map.width+10]=0;map.features[10*map.width+12]=0;
    tak::tnt::Scenario metadata;metadata.kingdom="zhon";
    std::string error;
    if(!cart::writeDocumentBundle(root/"Features.kmp",cart::documentFiles(map,metadata,{}, {},{},{},"Features"),error))throw std::runtime_error(error);
    std::vector<std::string> args={"cartographer","Ulasem Arena","--data",data,"--out",root.string()};
    std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    bool failed=false;int lastFrame=0;
    const int result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer* renderer,int frame) {
        lastFrame=frame;float sx,sy;SDL_RenderGetScale(renderer,&sx,&sy);
        auto mouse=[&](Uint32 type,int x,int y) {
            SDL_Event e{};e.type=type;
            if(type==SDL_MOUSEMOTION) {e.motion.state=SDL_BUTTON_LMASK;e.motion.x=int(x*sx);e.motion.y=int(y*sy);}
            else {e.button.button=SDL_BUTTON_LEFT;e.button.x=int(x*sx);e.button.y=int(y*sy);}
            SDL_PushEvent(&e);
        };
        auto click=[&](int x,int y) {mouse(SDL_MOUSEBUTTONDOWN,x,y);mouse(SDL_MOUSEBUTTONUP,x,y);};
        auto drag=[&](int x,int y,int tx,int ty) {mouse(SDL_MOUSEBUTTONDOWN,x,y);mouse(SDL_MOUSEMOTION,tx,ty);mouse(SDL_MOUSEBUTTONUP,tx,ty);};
        const bool dirty=std::string(SDL_GetWindowTitle(window)).ends_with(" *");
        switch(frame) {
        case 0:key(SDLK_o,KMOD_CTRL);break;
        case 1:text((root/"Features.kmp").string());key(SDLK_RETURN);break;
        case 2:key(SDLK_1);click(toolX(cart::Editor::FEATURES),32);click(modeX(cart::Editor::MODE_SELECT),32);break;
        case 3:drag(345,189,425,237);break; // select both features
        case 4:click(250,10);click(250,312);break; // hide selected features
        case 5:failed|=dirty;key(SDLK_DELETE);key(SDLK_x,KMOD_CTRL);break; // hidden selection cannot be deleted/cut
        case 6:failed|=dirty;click(toolX(cart::Editor::FEATURES),32);break; // selecting the feature tool reveals it again
        case 7:drag(361,205,393,253);break; // move two columns, three rows
        case 8:failed|=!dirty;key(SDLK_z,KMOD_CTRL);break;
        case 9:failed|=dirty;key(SDLK_y,KMOD_CTRL);break;
        case 10:failed|=!dirty;drag(377,237,457,285);break;
        case 11:key(SDLK_DELETE);break;
        case 12:key(SDLK_z,KMOD_CTRL);break;
        case 13:key(SDLK_s,KMOD_CTRL);break;
        case 14:failed|=dirty;key(SDLK_RETURN);key(SDLK_o,KMOD_CTRL);break;
        case 15:text((root/"Features.kmp").string());key(SDLK_RETURN);break;
        case 16:failed|=dirty;key(SDLK_s,KMOD_CTRL);break;
        case 17:key(SDLK_RETURN);break;
        case 18:{SDL_Event q{};q.type=SDL_QUIT;SDL_PushEvent(&q);break;}
        }
    });
    try {
        if(result || failed || lastFrame<18 || lastFrame>19)throw std::runtime_error("feature UI workflow failed at frame "+std::to_string(lastFrame));
        tak::hpi::Archive archive(root/"Features.kmp");bool found=false;
        for(const auto& entry:archive.entries())if(entry.path.ends_with(".tnt")) {
            const auto saved=tak::tnt::Map::load(archive.read(entry),entry.path);
            auto expected=map.features;expected[10*map.width+10]=expected[10*map.width+12]=0xFFFF;
            expected[13*map.width+12]=expected[13*map.width+14]=0;
            if(saved.features!=expected)throw std::runtime_error("feature group positions not preserved");
            found=true;
        }
        if(!found)throw std::runtime_error("saved feature terrain missing");
        fs::remove_all(root);std::cout<<"PASS: feature box selection, layer hiding, protected hidden selection, group drag, delete, undo/redo, save and reopen\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<"; files: "<<root<<'\n';return 1;}
}
static int validationWorkflow(const char* data,const char* mapName="Ulasem Arena") {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-editor-validation-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    isolateProfile(root.string().c_str());
    auto vfs=tak::hpi::mountRetailRoot(data);const auto path=tak::hpi::findMap(vfs,mapName);
    auto map=tak::tnt::Map::load(vfs.read(path),path);
    std::cout<<"Validation fixture: "<<mapName<<" ("<<map.width/32<<" x "<<map.height/32<<")\n";
    map.features[size_t(map.height/2)*map.width+map.width/2]=uint16_t(map.featureNames.size());
    map.featureNames.push_back("MissingEditorTestFeature");
    tak::tnt::Scenario metadata;metadata.kingdom="aramon";metadata.starts={{1,10,10},{2,20,20}};
    std::string error;
    if(!cart::writeDocumentBundle(root/"Validation.kmp",cart::documentFiles(map,metadata,{}, {},{},{},"Validation"),error))throw std::runtime_error(error);
    std::vector<std::string> args={"cartographer","Ulasem Arena","--data",data,"--out",root.string()};
    std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    int stage=0;bool failed=false;auto began=std::chrono::steady_clock::now();
    const int result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer* renderer,int) {
        auto click=[&](int x,int y) {
            float sx,sy;SDL_RenderGetScale(renderer,&sx,&sy);
            SDL_Event e{};e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;e.button.x=int(x*sx);e.button.y=int(y*sy);SDL_PushEvent(&e);
            e.type=SDL_MOUSEBUTTONUP;SDL_PushEvent(&e);
        };
        const std::string title=SDL_GetWindowTitle(window);failed|=title.ends_with(" *");
        if(std::chrono::steady_clock::now()-began>std::chrono::seconds(40)) {failed=true;key(SDLK_ESCAPE);SDL_Event q{};q.type=SDL_QUIT;SDL_PushEvent(&q);return;}
        switch(stage) {
        case 0: key(SDLK_o,KMOD_CTRL);++stage;break;
        case 1: text((root/"Validation.kmp").string());key(SDLK_RETURN);++stage;break;
        case 2: key(SDLK_c);key(SDLK_ESCAPE);++stage;break;
        case 3: if(title.find("[Checking map]")==std::string::npos) {
            failed|=title.find("[Validation results]")!=std::string::npos;key(SDLK_c);++stage;
        } break;
        case 4: if(title.find("[Validation results]")!=std::string::npos) {
            float sx,sy;SDL_RenderGetScale(renderer,&sx,&sy);int w,h;SDL_GetRendererOutputSize(renderer,&w,&h);
            click(int(w/sx/2),int(h/sy/2-150));++stage;
        } break;
        case 5: failed|=title!="Cartographer -- Validation";click(320,10);click(330,172);++stage;break;
        case 6: failed|=title.find("[Validation results]")==std::string::npos;key(SDLK_ESCAPE);++stage;break;
        case 7: {SDL_Event q{};q.type=SDL_QUIT;SDL_PushEvent(&q);++stage;break;}
        }
        SDL_Delay(1);
    });
    fs::remove_all(root);
    if(result || failed || stage!=8) {std::cerr<<"map validation workflow failed\n";return 1;}
    std::cout<<"PASS: background map validation, cancel, located result click and results reopening\n";return 0;
}
static int recoveryWorkflow(const char* data,const char* folder,const std::string& phase) {
    namespace fs=std::filesystem;const fs::path root=folder;
    fs::create_directories(root);
    isolateProfile(folder);
    const bool restore=phase=="restore",probe=phase=="probe";
    const auto destination=root/"original-destination";fs::create_directories(destination);
    std::vector<std::string> args={"cartographer"};if(!restore && !probe)args.push_back("Ulasem Arena");
    args.insert(args.end(),{"--data",data});
    if(!restore)args.insert(args.end(),{"--out",destination.string()});
    std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    uint64_t now=0;cart::EditorHooks hooks;hooks.recoveryClock=[&]{return now;};int prompts=0;hooks.recoveryChoice=[&] {++prompts;return probe?0:1;};
    const auto begun=std::chrono::steady_clock::now();std::string failure;
    const int result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer*,int frame) {
        if(std::chrono::steady_clock::now()-begun>std::chrono::seconds(20))std::_Exit(3);
        const bool dirty=std::string(SDL_GetWindowTitle(window)).ends_with(" *");
        if(probe) {if(frame==0) {SDL_Event q{};q.type=SDL_QUIT;SDL_PushEvent(&q);}else key(SDLK_RETURN);}
        else if(!restore) {
            if(frame==0)key(SDLK_p);
            if(frame==1) {text("Recovered unsaved map");key(SDLK_RETURN);}
            if(frame==2)now=61000;
            if(frame>2)for(const auto& entry:fs::recursive_directory_iterator(root)) {
                if(!entry.is_regular_file() || !entry.path().filename().string().starts_with("recovery-") || entry.path().extension()!=".kmp")continue;
                tak::hpi::Archive archive(entry.path());
                for(const auto& member:archive.entries())if(member.path.ends_with(".ota")) {
                    const auto bytes=archive.read(member);
                    const auto metadata=tak::tnt::Scenario::parse(std::string(bytes.begin(),bytes.end()));
                    if(metadata.missionName=="Recovered unsaved map") {
                        if(phase=="write")std::_Exit(0); // deliberately bypass editor cleanup
                        std::ofstream(root/"writer-ready")<<"ready";
                    }
                }
            }
        } else {
            if(frame==0) {if(!dirty)failure="recovered document is not marked unsaved";key(SDLK_s,KMOD_CTRL);}
            if(frame==1) {if(dirty)failure="recovered map could not be saved";key(SDLK_RETURN);}
            if(frame==2) {SDL_Event q{};q.type=SDL_QUIT;SDL_PushEvent(&q);}
        }
        SDL_Delay(1);
    },hooks);
    if(result || !failure.empty()) {std::cerr<<failure<<'\n';return 1;}
    if(probe && prompts)return 5;
    if(restore) {
        if(prompts!=1)return 6;
        tak::hpi::Archive archive(destination/"Ulasem Arena.kmp");bool found=false;
        for(const auto& member:archive.entries())if(member.path.ends_with(".ota")) {
            const auto bytes=archive.read(member);const auto metadata=tak::tnt::Scenario::parse(std::string(bytes.begin(),bytes.end()));
            found=metadata.missionName=="Recovered unsaved map";
        }
        if(!found)return 2;
        for(const auto& entry:fs::recursive_directory_iterator(root))if(entry.path().filename().string().starts_with("recovery-"))return 4;
    }
    return 0;
}
// Section browser: open the group list, pick a folder, select its first piece,
// click it again for the next variant, stamp and save. A second session must
// reopen the remembered group (its first thumbnail is that folder's piece).
static int sectionsWorkflow(const char* data) {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-editor-sections-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);isolateProfile(root.string().c_str());
    auto vfs=tak::hpi::mountRetailRoot(data);cart::SectionLibrary sections;sections.scan(vfs,"zhon");
    cart::SectionCatalog catalog;catalog.build(sections.list());
    const std::string groupId="folder:coast cliffs sandy";int groupRow=-1;
    for(size_t i=0;i<catalog.groups().size();++i)if(catalog.groups()[i].id==groupId)groupRow=int(i);
    if(groupRow<0 || groupRow>8) {std::cerr<<"group row "<<groupRow<<'\n';return 2;}
    const auto& piece=catalog.pieces()[size_t(catalog.groups()[size_t(groupRow)].pieces.front())];
    const auto& firstOfAll=catalog.pieces()[size_t(catalog.groups().front().pieces.front())];
    if(piece.variants.size()<2 || firstOfAll.folder==piece.folder)return 2;
    // Block positions where the saved map carries exactly this prefab's tile art
    // (keys/cols/rows) and heights. Cliff sets share heights, so art decides.
    auto stampedAt=[&](int section) {
        const auto* prefab=sections.load(vfs,sections.list()[size_t(section)].path);
        if(!prefab || prefab->blocksX!=16 || prefab->blocksY!=16)return -1;
        int found=0;tak::hpi::Archive archive(root/"Ulasem Arena.kmp");
        for(const auto& entry:archive.entries())if(entry.path.ends_with(".tnt")) {
            const auto map=tak::tnt::Map::load(archive.read(entry),entry.path);
            for(int by=0;by+16<=map.blocksY;++by)for(int bx=0;bx+16<=map.blocksX;++bx) {
                bool same=true;
                for(int y=0;y<16 && same;++y)for(int x=0;x<16 && same;++x) {
                    const size_t m=size_t(by+y)*map.blocksX+bx+x,p=size_t(y)*16+x;
                    same=map.tileKeys[m]==prefab->tileKeys[p] && map.tileCols[m]==prefab->tileCols[p] && map.tileRows[m]==prefab->tileRows[p];
                }
                for(int y=0;y<32 && same;++y)same=std::equal(prefab->heights.begin()+y*32,prefab->heights.begin()+y*32+32,map.heights.begin()+size_t(by*2+y)*map.width+bx*2);
                found+=same;
            }
        }
        return found;
    };
    std::vector<std::string> args={"cartographer","Ulasem Arena","--data",data,"--out",root.string()};
    std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    std::string failure;auto check=[&](bool value,const char* message){if(!value && failure.empty())failure=message;};
    auto session=[&](std::vector<std::function<void(SDL_Renderer*,bool)>> steps) {
        int last=0;
        const int result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer* renderer,int frame) {
            last=frame;const bool dirty=std::string(SDL_GetWindowTitle(window)).ends_with(" *");
            if(frame<int(steps.size()))steps[size_t(frame)](renderer,dirty);
            else if(frame==int(steps.size())) {SDL_Event q{};q.type=SDL_QUIT;SDL_PushEvent(&q);}
            else if(frame>int(steps.size())+3) {check(false,"editor did not exit");key(SDLK_ESCAPE);key(SDLK_RETURN);}
        });
        check(result==0 && last>=int(steps.size()),"section browser session failed");
    };
    auto click=[](SDL_Renderer* renderer,int x,int y) {
        float sx,sy;SDL_RenderGetScale(renderer,&sx,&sy);
        SDL_Event e{};e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;e.button.x=int(x*sx);e.button.y=int(y*sy);SDL_PushEvent(&e);
        e.type=SDL_MOUSEBUTTONUP;SDL_PushEvent(&e);
    };
    // A press on a list row over the canvas that wobbles before release: the
    // drag belongs to the list, not to the terrain brush underneath.
    auto pressDrag=[](SDL_Renderer* renderer,int x,int y) {
        float sx,sy;SDL_RenderGetScale(renderer,&sx,&sy);
        SDL_Event e{};e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;e.button.x=int(x*sx);e.button.y=int(y*sy);SDL_PushEvent(&e);
        SDL_Event m{};m.type=SDL_MOUSEMOTION;m.motion.state=SDL_BUTTON_LMASK;m.motion.x=int((x+3)*sx);m.motion.y=int((y+1)*sy);m.motion.xrel=int(3*sx);m.motion.yrel=int(sy);SDL_PushEvent(&m);
        e.type=SDL_MOUSEBUTTONUP;e.button.x=m.motion.x;e.button.y=m.motion.y;SDL_PushEvent(&e);
    };
    const int groupButtonY=44+40,firstCellX=48,firstCellY=44+76+4+44,menuRowY=44+53+2+groupRow*16+8;
    session({
        [&](SDL_Renderer* r,bool){click(r,100,groupButtonY);},             // open the group list
        [&](SDL_Renderer* r,bool){pressDrag(r,260,menuRowY);},             // Coast Cliffs Sandy, over the canvas
        [&](SDL_Renderer* r,bool){click(r,firstCellX,firstCellY);},       // select its first piece
        [&](SDL_Renderer* r,bool){click(r,firstCellX,firstCellY);},       // again: next variant
        [&](SDL_Renderer* r,bool dirty){check(!dirty,"browser clicks must not edit");click(r,560,400);},
        [&](SDL_Renderer*,bool dirty){check(dirty,"stamp marks the map dirty");key(SDLK_s,KMOD_CTRL);},
        [&](SDL_Renderer*,bool dirty){check(!dirty,"stamped map saved");key(SDLK_RETURN);},
    });
    if(failure.empty()) {
        check(stampedAt(piece.variants[1])>0,"second click stamps the piece's second variant");
        check(stampedAt(piece.variants[0])==0,"first variant was not left selected");
        const auto preferences=cart::loadEditorPreferences(root/"TAKengine"/"Cartographer");
        const auto remembered=preferences.sectionGroups.find("zhon");   // Ulasem says "Zhon"; new maps "zhon"
        check(remembered!=preferences.sectionGroups.end() && remembered->second==groupId,"chosen group remembered for the world");
    }
    if(failure.empty()) session({
        // The menu bar's File list closes the group list (it used to stay open
        // on top of File), so the click on its "All sections" row lands nowhere.
        [&](SDL_Renderer* r,bool){click(r,100,groupButtonY);},
        [&](SDL_Renderer* r,bool){click(r,110,10);},
        [&](SDL_Renderer*,bool){key(SDLK_ESCAPE);},
        [&](SDL_Renderer* r,bool){click(r,100,44+53+2+8);},
        [&](SDL_Renderer* r,bool){click(r,firstCellX,firstCellY);},       // first thumbnail of the reopened group
        [&](SDL_Renderer* r,bool){click(r,195,firstCellY);},              // right margin: no cell (it picked the next row's)
        [&](SDL_Renderer* r,bool){click(r,560,400);},
        [&](SDL_Renderer*,bool dirty){check(dirty,"second stamp marks the map dirty");key(SDLK_s,KMOD_CTRL);},
        [&](SDL_Renderer*,bool){key(SDLK_RETURN);},
    });
    if(failure.empty())check(stampedAt(piece.variants[0])>0 && stampedAt(firstOfAll.variants[0])==0,"reopened editor restores the remembered group");
    // Search looks past the remembered folder: another folder's variant, found by
    // its raw name, is the first thumbnail and is what stamps.
    const auto& wanted=sections.list()[size_t(firstOfAll.variants[1])].name;
    if(failure.empty()) session({
        [&](SDL_Renderer*,bool){key(SDLK_f,KMOD_CTRL);text(wanted);},
        [&](SDL_Renderer*,bool){key(SDLK_RETURN);},
        [&](SDL_Renderer* r,bool){click(r,firstCellX,firstCellY);},
        [&](SDL_Renderer* r,bool){click(r,560,400);},
        [&](SDL_Renderer*,bool dirty){check(dirty,"searched stamp marks the map dirty");key(SDLK_s,KMOD_CTRL);},
        [&](SDL_Renderer*,bool){key(SDLK_RETURN);},
    });
    if(failure.empty())check(stampedAt(firstOfAll.variants[1])>0,"search finds a variant outside the remembered group");
    if(!failure.empty()) {std::cerr<<"FAIL: "<<failure<<"; files: "<<root<<'\n';return 1;}
    fs::remove_all(root);
    std::cout<<"PASS: section group list, folder selection, variant cycling, stamp, remembered group and search across groups\n";return 0;
}

// The command-line paths end through Session::Exit before any UI exists: usage
// and missing maps/sections fail, --save/--stamp/--bundle write and exit, and
// --shot renders one frame of the real editor to a PNG.
static int headlessWorkflow(const char* data) {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-editor-headless-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);isolateProfile(root.string().c_str());
    auto run=[](std::vector<std::string> args) {
        args.insert(args.begin(),"cartographer");std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
        return cart::runEditor(int(raw.size()),raw.data());
    };
    auto written=[](const fs::path& path) {return fs::exists(path) && fs::file_size(path)>0;};
    const auto vfs=tak::hpi::mountRetailRoot(data);cart::SectionLibrary sections;sections.scan(vfs,"zhon");
    if(sections.list().empty()) {std::cerr<<"no zhon sections\n";return 1;}
    const auto tnt=root/"Saved.tnt",stamped=root/"Stamped.tnt",kmp=root/"Saved.kmp",png=root/"shot.png";
    std::string failure;auto check=[&](bool value,const char* message){if(!value && failure.empty())failure=message;};
    check(run({"Ulasem Arena","--save",tnt.string()})==2,"no data root prints usage and exits 2");
    check(run({"No Such Map Anywhere","--data",data,"--save",tnt.string()})==1 && !fs::exists(tnt),"a missing map exits 1 without writing");
    check(run({"Ulasem Arena","--data",data,"--save",tnt.string()})==0 && written(tnt),"--save writes the map and exits 0");
    check(run({"Ulasem Arena","--data",data,"--stamp","no such section","0","0","--save",stamped.string()})==1 && !fs::exists(stamped),"an unknown --stamp section exits 1");
    check(run({"Ulasem Arena","--data",data,"--stamp",sections.list().front().name,"0","0","--save",stamped.string()})==0 && written(stamped),"--stamp edits, saves and exits 0");
    check(run({"Ulasem Arena","--data",data,"--bundle",kmp.string()})==0 && written(kmp),"--bundle writes the KMP and exits 0");
    check(run({"Ulasem Arena","--data",data,"--shot",png.string()})==0 && written(png),"--shot writes one frame and exits 0");
    // --shot waits for the async palette art: the first frame draws empty boxes (workers have not
    // started), so the captured (last) frame must differ from it and carry real texture detail.
    auto shotCell=[&](const char* shotTool,SDL_Rect logical,const char* name) {
        int frames=0;std::vector<uint8_t> first,last;
        auto read=[&](SDL_Renderer* renderer) {
            float sx=1,sy=1;SDL_RenderGetScale(renderer,&sx,&sy);
            SDL_Rect physical{int(logical.x*sx),int(logical.y*sy),int(logical.w*sx),int(logical.h*sy)};
            std::vector<uint8_t> rgba(size_t(physical.w)*physical.h*4);
            SDL_RenderReadPixels(renderer,&physical,SDL_PIXELFORMAT_RGBA32,rgba.data(),physical.w*4);return rgba;
        };
        std::vector<std::string> args{"cartographer","Ulasem Arena","--data",data,"--shot",png.string(),"--shot-tool",shotTool};std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
        const int code=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window*,SDL_Renderer* renderer,int frame) {
            frames=frame+1;last=read(renderer);if(!frame)first=last;
        });
        auto distinct=[](const std::vector<uint8_t>& rgba) {std::set<uint32_t> colours;for(size_t i=0;i+3<rgba.size();i+=4)colours.insert(rgba[i]|rgba[i+1]<<8|rgba[i+2]<<16);return colours.size();};
        check(code==0,"--shot-tool exits 0");
        check(frames>1,(std::string(name)+": --shot captured before the palette art could load").c_str());
        check(!first.empty() && distinct(first)<=3,(std::string(name)+": test premise: first frame has empty boxes").c_str());
        check(distinct(last)>40,(std::string(name)+": captured frame still shows empty palette boxes").c_str());
    };
    shotCell("terrain",{8,144,56,64},"terrain thumbnails");   // clear of the variant badge (top right of the cell)
    shotCell("units",{6,123,28,20},"unit portraits");
    fs::remove_all(root);
    if(!failure.empty()) {std::cerr<<"FAIL: "<<failure<<'\n';return 1;}
    std::cout<<"PASS: headless exit codes, --save/--stamp/--bundle output and --shot capture with loaded palette art\n";return 0;
}

// A saved KMP read back: terrain, metadata (starts) and placed units.
struct SavedMap {tak::tnt::Map map;tak::tnt::Scenario metadata;std::vector<cart::PlacedUnit> units;};
static SavedMap readSaved(const std::filesystem::path& path,const tak::sim::TypeRegistry* registry=nullptr) {
    SavedMap saved;tak::hpi::Archive archive(path);
    for(const auto& entry:archive.entries()) {
        if(entry.path.ends_with(".tnt"))saved.map=tak::tnt::Map::load(archive.read(entry),entry.path);
        if(entry.path.ends_with(".ota")) {const auto bytes=archive.read(entry);saved.metadata=tak::tnt::Scenario::parse(std::string(bytes.begin(),bytes.end()));}
        if(entry.path.ends_with(".crt"))saved.units=cart::toPlaced(tak::crt::parse(archive.read(entry)),registry);
    }
    return saved;
}
// Symmetry > Symmetrize through its dialog and confirmation, one undo step,
// redo and save (checked against an independent symmetrize of the fixture),
// then live symmetry mirroring a unit placement as one undo step.
static int symmetryWorkflow(const char* data) {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-editor-symmetry-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);isolateProfile(root.string().c_str());
    const auto vfs=tak::hpi::mountRetailRoot(data);const auto path=tak::hpi::findMap(vfs,"Ulasem Arena");
    const auto map=tak::tnt::Map::load(vfs.read(path),path);
    tak::tnt::Scenario metadata;metadata.kingdom="zhon";metadata.starts={{1,40,40}};
    tak::sim::TypeRegistry registry;tak::sim::setupRegistry(registry,vfs,false);
    cart::PlacedUnit guard;guard.type="ARAARCH";guard.player=0;guard.angle=90;guard.name="Left guard";
    if(const auto* type=registry.find("araarch")) {guard.footX=type->footX;guard.footZ=type->footZ;}
    guard.x=100*16+guard.footX*8.f;guard.z=50*16+guard.footZ*8.f;
    std::string error;
    if(!cart::writeDocumentBundle(root/"Symmetry.kmp",cart::documentFiles(map,metadata,{},{guard},{},{},"Symmetry"),error))throw std::runtime_error(error);
    // The expected result, computed outside the editor.
    cart::SectionLibrary sections;sections.scan(vfs,"zhon");cart::FeatureLibrary features;features.scan(vfs,"zhon");
    cart::SectionMatcher matcher;matcher.build(sections.list(),[&](size_t i){return sections.load(vfs,sections.list()[i].path);});
    auto expected=map;auto expectedStarts=metadata.starts;tak::crt::Scenario expectedScenario;std::vector<cart::PlacedUnit> expectedUnits{guard};
    cart::SymmetryRequest request;request.kind=cart::Mirror::Vertical;request.source=0;
    const auto report=cart::symmetrize(expected,expectedStarts,expectedScenario,expectedUnits,request,
        [&](const std::string& name){const auto* ref=features.byName(name);return ref?std::pair{ref->footX,ref->footZ}:std::pair{1,1};},&matcher);
    if(report.blocks<40000 || expectedUnits.size()!=2)throw std::runtime_error("fixture symmetrize did not mirror the map");
    std::vector<std::string> args={"cartographer","Ulasem Arena","--data",data,"--out",root.string()};
    std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    std::string failure;int lastFrame=0;
    auto fail=[&](const std::string& why){if(failure.empty())failure=why;};
    const int result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer* renderer,int frame) {
        lastFrame=frame;float sx,sy;SDL_RenderGetScale(renderer,&sx,&sy);
        auto click=[&](int x,int y) {
            SDL_Event e{};e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;e.button.x=int(x*sx);e.button.y=int(y*sy);SDL_PushEvent(&e);
            e.type=SDL_MOUSEBUTTONUP;SDL_PushEvent(&e);
        };
        const bool dirty=std::string(SDL_GetWindowTitle(window)).ends_with(" *");
        const auto saved=root/"Symmetry.kmp";
        switch(frame) {
        case 0:key(SDLK_o,KMOD_CTRL);break;
        case 1:text(saved.string());key(SDLK_RETURN);break;
        case 2:key(SDLK_m,KMOD_SHIFT);break;            // Symmetrize dialog: vertical axis, left half
        case 3:if(dirty)fail("dialog edited the map");key(SDLK_RETURN);break;   // dry run -> confirmation
        case 4:if(dirty)fail("confirmation edited the map");key(SDLK_RETURN);break;
        case 5:if(!dirty)fail("symmetrize did not edit");key(SDLK_RETURN);break;   // unmirrored-terrain report
        case 6:key(SDLK_z,KMOD_CTRL);break;
        case 7:if(dirty)fail("one undo did not restore the map");key(SDLK_y,KMOD_CTRL);break;
        case 8:if(!dirty)fail("redo lost the symmetrize");key(SDLK_s,KMOD_CTRL);break;
        case 9:{
            key(SDLK_RETURN);const auto file=readSaved(saved,&registry);
            if(file.map.tileKeys!=expected.tileKeys || file.map.heights!=expected.heights)fail("saved terrain differs from the expected mirror");
            if(file.map.features.size()!=expected.features.size())fail("saved feature plane size changed");
            else for(size_t i=0;i<expected.features.size();++i) {
                const auto name=[](const tak::tnt::Map& m,size_t c){const auto v=m.features[c];return v<0xFFFA && v<m.featureNames.size()?m.featureNames[v]:std::to_string(v);};
                if(name(file.map,i)!=name(expected,i)) {fail("saved features differ from the expected mirror");break;}
            }
            if(file.metadata.starts.size()!=2 || file.metadata.starts[1].number!=2 || file.metadata.starts[1].xpos!=600)fail("mirrored start missing");
            if(file.units.size()!=2 || file.units[1].x!=expectedUnits[1].x || file.units[1].z!=expectedUnits[1].z ||
               file.units[1].angle!=270 || file.units[1].player!=1 || !file.units[1].name.empty() || file.units[0].name!="Left guard")fail("mirrored unit wrong");
            key(SDLK_m);key(SDLK_TAB);key(SDLK_TAB);key(SDLK_1);break;   // live symmetry, Units tool, 100%
        }
        case 10:click(200+5*16+8,44+3*16+8);break;      // place a unit at cell (5,3)
        case 11:if(!dirty)fail("live placement did not edit");key(SDLK_s,KMOD_CTRL);break;
        case 12:{
            key(SDLK_RETURN);const auto file=readSaved(saved,&registry);
            if(file.units.size()!=4)fail("live symmetry did not add the mirror");
            else if(std::abs(file.units[2].x+file.units[3].x-640*16)>.01f || file.units[2].z!=file.units[3].z)fail("live mirror is not symmetric");
            key(SDLK_z,KMOD_CTRL);key(SDLK_s,KMOD_CTRL);break;
        }
        case 13:{
            key(SDLK_RETURN);
            if(readSaved(saved).units.size()!=2)fail("one undo did not remove the placement and its mirror");
            SDL_Event q{};q.type=SDL_QUIT;SDL_PushEvent(&q);break;
        }
        default:if(frame>20)std::_Exit(3);
        }
    });
    if(result || !failure.empty() || lastFrame<13) {std::cerr<<"FAIL: "<<failure<<" (frame "<<lastFrame<<"); files: "<<root<<'\n';return 1;}
    fs::remove_all(root);
    std::cout<<"PASS: Symmetrize dialog/confirm, one-step undo/redo, saved mirror of terrain/features/starts/units, live mirrored placement\n";return 0;
}
// Terrain tool + Select drags a block rectangle; Ctrl+C copies, Ctrl+V makes
// the clipboard the brush, a click pastes (one undo step), the saved map has
// the copy, and the clipboard survives File > Open of a map of another world.
static int terrainClipboardWorkflow(const char* data) {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-editor-terrainclip-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);isolateProfile(root.string().c_str());
    const auto vfs=tak::hpi::mountRetailRoot(data);const auto path=tak::hpi::findMap(vfs,"Ulasem Arena");
    const auto map=tak::tnt::Map::load(vfs.read(path),path);
    tak::tnt::Scenario metadata;metadata.kingdom="zhon";std::string error;
    if(!cart::writeDocumentBundle(root/"Clip.kmp",cart::documentFiles(map,metadata,{},{},{},{},"Clip"),error))throw std::runtime_error(error);
    // Another world's map to paste into after File > Open.
    std::string other;
    for(const auto& [name,mapPath]:tak::hpi::listMaps(vfs)) {
        const auto ota=mapPath.substr(0,mapPath.rfind('.'))+".ota";if(!vfs.has(ota))continue;
        const auto bytes=vfs.read(ota);const auto scenario=tak::tnt::Scenario::parse(std::string(bytes.begin(),bytes.end()));
        auto kingdom=scenario.kingdom;std::transform(kingdom.begin(),kingdom.end(),kingdom.begin(),[](unsigned char c){return char(std::tolower(c));});
        if(kingdom=="aramon" && cart::validDocumentName(name)) {const auto m=tak::tnt::Map::load(vfs.read(mapPath),mapPath);if(m.blocksX>=16 && m.blocksY>=12) {other=name;break;}}
    }
    if(other.empty()) {std::cerr<<"no aramon map\n";return 1;}
    auto block=[&](const tak::tnt::Map& m,int x,int z){const size_t i=size_t(z)*m.blocksX+x;return std::tuple{m.tileKeys[i],m.tileCols[i],m.tileRows[i]};};
    std::vector<std::string> args={"cartographer","Ulasem Arena","--data",data,"--out",root.string()};
    std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    std::string failure;int lastFrame=0;
    auto fail=[&](const std::string& why){if(failure.empty())failure=why;};
    auto pastedFrom=[&](const tak::tnt::Map& saved,int ox,int oz) {
        for(int z=0;z<2;++z)for(int x=0;x<3;++x)if(block(saved,ox+x,oz+z)!=block(map,1+x,1+z))return false;
        for(int z=0;z<4;++z)for(int x=0;x<6;++x)if(saved.heights[size_t(oz*2+z)*saved.width+ox*2+x]!=map.heights[size_t(2+z)*map.width+2+x])return false;
        return true;
    };
    const int result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer* renderer,int frame) {
        lastFrame=frame;float sx,sy;SDL_RenderGetScale(renderer,&sx,&sy);
        auto mouse=[&](Uint32 type,int x,int y) {
            SDL_Event e{};e.type=type;
            if(type==SDL_MOUSEMOTION) {e.motion.state=SDL_BUTTON_LMASK;e.motion.x=int(x*sx);e.motion.y=int(y*sy);}
            else {e.button.button=SDL_BUTTON_LEFT;e.button.x=int(x*sx);e.button.y=int(y*sy);}
            SDL_PushEvent(&e);
        };
        auto click=[&](int x,int y) {mouse(SDL_MOUSEBUTTONDOWN,x,y);mouse(SDL_MOUSEBUTTONUP,x,y);};
        const std::string title=SDL_GetWindowTitle(window);const bool dirty=title.ends_with(" *");
        switch(frame) {
        case 0:key(SDLK_o,KMOD_CTRL);break;
        case 1:text((root/"Clip.kmp").string());key(SDLK_RETURN);break;
        case 2:key(SDLK_1);click(modeX(cart::Editor::MODE_SELECT),32);break;          // 100% zoom, Select mode (Terrain tool)
        case 3:mouse(SDL_MOUSEBUTTONDOWN,200+32+5,44+32+5);mouse(SDL_MOUSEMOTION,200+96+5,44+64+5);mouse(SDL_MOUSEBUTTONUP,200+96+5,44+64+5);break;   // blocks 1..3 x 1..2
        case 4:if(dirty)fail("selecting terrain edited the map");key(SDLK_c,KMOD_CTRL);key(SDLK_v,KMOD_CTRL);break;
        case 5:if(dirty || title.find("[Checking map]")!=std::string::npos)fail("copy edited the map or ran Check Map");click(200+10*32+16,44+5*32+16);break;   // paste centred on block (10,5)
        case 6:if(!dirty)fail("paste did not edit");key(SDLK_z,KMOD_CTRL);break;
        case 7:if(dirty)fail("one undo did not remove the paste");key(SDLK_y,KMOD_CTRL);break;
        case 8:if(!dirty)fail("redo lost the paste");key(SDLK_s,KMOD_CTRL);break;
        case 9:key(SDLK_RETURN);if(!pastedFrom(readSaved(root/"Clip.kmp").map,9,4))fail("saved map lacks the pasted tiles/heights");
            key(SDLK_o,KMOD_CTRL);break;
        case 10:text(other);key(SDLK_RETURN);break;       // another world: the clipboard survives
        case 11:if(title!="Cartographer -- "+other)fail("could not open "+other);key(SDLK_v,KMOD_CTRL);click(200+6*32+16,44+4*32+16);break;
        case 12:if(dirty)fail("cross-world paste skipped its warning");key(SDLK_RETURN);break;   // accept the warning
        case 13:if(!dirty)fail("accepted cross-world paste did not edit");key(SDLK_s,KMOD_CTRL);break;
        case 14:{
            key(SDLK_RETURN);
            if(!pastedFrom(readSaved(root/(other+".kmp")).map,5,3))fail("clipboard did not survive File > Open");
            SDL_Event q{};q.type=SDL_QUIT;SDL_PushEvent(&q);break;
        }
        default:if(frame>20)std::_Exit(3);
        }
    });
    if(result || !failure.empty() || lastFrame<14) {std::cerr<<"FAIL: "<<failure<<" (frame "<<lastFrame<<"); files: "<<root<<'\n';return 1;}
    fs::remove_all(root);
    std::cout<<"PASS: terrain selection, copy, clipboard brush paste, one-step undo/redo, save, cross-world warning after File > Open\n";return 0;
}
// Logical-pixel input for the frame hook: events carry physical window pixels.
struct Pointer {
    SDL_Window* window;float sx,sy;
    void send(Uint32 type,int x,int y,Uint32 state=SDL_BUTTON_LMASK) const {
        SDL_Event e{};e.type=type;
        if(type==SDL_MOUSEMOTION) {e.motion.state=state;e.motion.x=int(x*sx);e.motion.y=int(y*sy);}
        else {e.button.button=SDL_BUTTON_LEFT;e.button.x=int(x*sx);e.button.y=int(y*sy);e.button.clicks=1;}
        SDL_PushEvent(&e);
    }
    void click(int x,int y) const {send(SDL_MOUSEBUTTONDOWN,x,y);send(SDL_MOUSEBUTTONUP,x,y);}
    void warp(int x,int y) const {SDL_WarpMouseInWindow(window,int(x*sx),int(y*sy));}   // what SDL_GetMouseState reports
    void wheel(int y) const {SDL_Event e{};e.type=SDL_MOUSEWHEEL;e.wheel.y=y;SDL_PushEvent(&e);}
};
static void savePng(SDL_Renderer* renderer,const std::filesystem::path& path) {
    int w,h;SDL_GetRendererOutputSize(renderer,&w,&h);std::vector<uint8_t> rgba(size_t(w)*h*4);
    if(SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,rgba.data(),w*4)==0)tak::png::write(path.string(),w,h,rgba);
}

// The left browser scrolls in every tool: the wheel, a thumb drag (which must not
// paint the canvas it ends over), track paging, and clicks on scrolled rows/cells.
// TAK_CARTO_SHOT_DIR=<dir> also writes each tool's scrolled browser as a PNG.
static int paletteScrollWorkflow(const char* data) {
    namespace fs=std::filesystem;
    constexpr int kPaletteW=200,kPaletteTop=120,kStatusH=22,kCellH=106;   // editorstate.h layout
    const auto root=fs::temp_directory_path()/("tak-editor-scroll-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);isolateProfile(root.string().c_str());
    const auto vfs=tak::hpi::mountRetailRoot(data);const auto path=tak::hpi::findMap(vfs,"Ulasem Arena");
    const auto map=tak::tnt::Map::load(vfs.read(path),path);
    tak::tnt::Scenario metadata;metadata.kingdom="zhon";
    for(int i=0;i<30;++i)metadata.starts.push_back({i+1,40+(i%6)*60,40+(i/6)*60});   // 30 rows overflow the STARTS list
    std::string error;
    if(!cart::writeDocumentBundle(root/"Scroll.kmp",cart::documentFiles(map,metadata,{},{},{},{},"Scroll"),error))throw std::runtime_error(error);
    std::vector<std::string> args={"cartographer","Ulasem Arena","--data",data,"--out",root.string()};
    std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    const char* shots=std::getenv("TAK_CARTO_SHOT_DIR");
    cart::EditorHooks hooks;cart::EditorHooks::FrameState state;hooks.frameState=[&](const auto& s) {state=s;};
    std::string failure;int lastFrame=0,before=-1,expected=-1,toolScroll=0,held=0;float expectX=0;Uint64 holdUntil=0;
    auto check=[&](bool value,const std::string& message) {if(!value && failure.empty())failure=message+" (frame "+std::to_string(lastFrame)+", tool "+std::to_string(state.tool)+", scroll "+std::to_string(state.paletteScroll)+"/"+std::to_string(state.paletteMaxScroll)+")";};
    const char* toolNames[]={"terrain","features","units","starts"};
    const int result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer* renderer,int frame) {
        lastFrame=frame;float sx,sy;SDL_RenderGetScale(renderer,&sx,&sy);const Pointer pointer{window,sx,sy};
        int ow,oh;SDL_GetRendererOutputSize(renderer,&ow,&oh);const int lw=int(ow/sx),lh=int(oh/sy);
        cart::ScrollBar bar;bar.x=kPaletteW-10;bar.w=8;bar.y=kPaletteTop;bar.h=lh-kStatusH-kPaletteTop;bar.view=bar.h;bar.content=state.paletteMaxScroll+bar.h;
        const bool dirty=std::string(SDL_GetWindowTitle(window)).ends_with(" *");
        if(frame==0) {key(SDLK_o,KMOD_CTRL);return;}
        if(frame==1) {text((root/"Scroll.kmp").string());key(SDLK_RETURN);return;}
        const int tool=(frame-2-held)/12,step=(frame-2-held)%12;
        if(tool>=4) {
            if(frame==2+4*12+held) {SDL_Event q{};q.type=SDL_QUIT;SDL_PushEvent(&q);}
            else if(frame>2+4*12+held+3) {check(false,"editor did not exit");std::cerr<<"palette scroll: "<<failure<<'\n';std::_Exit(3);}
            return;
        }
        if(step>0 && step<11)check(!dirty,std::string("browser scrolling edited the map in ")+toolNames[tool]);
        switch(step) {
        case 0:pointer.click(toolX(tool),32);pointer.warp(100,lh/2);break;
        case 1:
            check(state.tool==tool && state.paletteScroll==0,"a new tool's list starts at the top");
            check(state.paletteMaxScroll>0,std::string("the ")+toolNames[tool]+" list overflows the browser");
            pointer.wheel(-3);break;
        case 2:
            check(state.paletteScroll==std::min(120,state.paletteMaxScroll),"the wheel scrolls the browser");
            pointer.send(SDL_MOUSEBUTTONDOWN,bar.x+4,bar.thumbTop(state.paletteScroll)+bar.thumbHeight()/2);
            pointer.send(SDL_MOUSEMOTION,lw/2,lh-60);pointer.send(SDL_MOUSEMOTION,lw/2,lh+40);   // dragged over the canvas and past the window
            pointer.send(SDL_MOUSEBUTTONUP,lw/2,lh+40);break;
        case 3:
            check(state.paletteScroll==state.paletteMaxScroll,"dragging the thumb down reaches the end of the list");
            pointer.click(bar.x+4,bar.y+4);break;   // the track above the thumb pages up
        case 4:
            check(state.paletteScroll==std::max(0,state.paletteMaxScroll-bar.view),"a track click pages one viewport");
            toolScroll=state.paletteScroll;pointer.wheel(-1);break;
        case 5: {
            check(state.paletteScroll==std::min(toolScroll+40,state.paletteMaxScroll),"the wheel scrolls from a paged position");
            const int y=kPaletteTop+230;before=state.selection;
            if(tool==1)expected=(230-4+state.paletteScroll)/kCellH*2+1;
            if(tool==2)expected=(230+state.paletteScroll)/26;
            if(tool==3) {
                const auto& start=metadata.starts[size_t((230+state.paletteScroll)/26)];
                expectX=start.xpos*16-(lw-kPaletteW)/(2*state.zoom);
            }
            pointer.click(tool==1?kPaletteW-90:60,y);break;   // FEATURES: the second column
        }
        case 6:
            if(tool==0)check(state.selection!=before && state.selection>=0,"a scrolled terrain cell selects its section");
            if(tool==1 || tool==2)check(state.selection==expected,"a scrolled row selects the item drawn under the pointer");
            if(tool==3)check(std::abs(state.viewX-expectX)<1,"a scrolled start row centres the view on that start");
            holdUntil=SDL_GetTicks64()+4000;break;
        case 7:pointer.warp(lw/2,lh/2);   // off the thumb so it draws idle
            if(shots && SDL_GetTicks64()<holdUntil)++held;   // thumbnails and portraits decode one per frame
            break;
        case 8:if(shots)savePng(renderer,fs::path(shots)/(std::string("palette-")+toolNames[tool]+".png"));
            pointer.warp(100,lh/2);pointer.wheel(1000);break;
        case 9:check(state.paletteScroll==0,"scrolling up clamps at the top");break;
        }
    },hooks);
    fs::remove_all(root);
    if(result || !failure.empty()) {std::cerr<<"palette scroll: "<<failure<<'\n';return 1;}
    std::cout<<"PASS: browser scrollbar (wheel, thumb drag, track paging, clamping, scrolled hit-testing) in terrain, features, units and starts\n";return 0;
}

static std::string readText(const std::filesystem::path& path) {std::ifstream in(path);return {std::istreambuf_iterator<char>(in),{}};}

// View > Preferences: borderless fullscreen (live, persisted, restored on startup,
// pointer mapping intact at the desktop size, windowed size kept) and edge scrolling
// (dwell, direction, map-edge stop, nothing over the browser but the screen edge,
// off by preference).
static int displayWorkflow(const char* data) {
    namespace fs=std::filesystem;
    constexpr int kPaletteW=200;
    const auto root=fs::temp_directory_path()/("tak-editor-display-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);isolateProfile(root.string().c_str());
    const auto profile=root/"TAKengine"/"Cartographer";
    const auto vfs=tak::hpi::mountRetailRoot(data);const auto path=tak::hpi::findMap(vfs,"Ulasem Arena");
    const auto map=tak::tnt::Map::load(vfs.read(path),path);
    std::vector<std::string> args={"cartographer","Ulasem Arena","--data",data,"--out",root.string()};
    std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    const char* shots=std::getenv("TAK_CARTO_SHOT_DIR");
    cart::EditorHooks hooks;cart::EditorHooks::FrameState state;hooks.frameState=[&](const auto& s) {state=s;};
    std::string failure;int lastFrame=0,phase=0,windowedW=0,windowedH=0;Uint64 since=0;float mark=0,markZ=0;
    auto check=[&](bool value,const std::string& message) {if(!value && failure.empty())failure=message+" (phase "+std::to_string(phase)+", frame "+std::to_string(lastFrame)+")";};
    auto fullscreen=[](SDL_Window* window) {return (SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN_DESKTOP)==SDL_WINDOW_FULLSCREEN_DESKTOP;};
    auto quit=[] {SDL_Event q{};q.type=SDL_QUIT;SDL_PushEvent(&q);};
    const int result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer* renderer,int frame) {
        lastFrame=frame;float sx,sy;SDL_RenderGetScale(renderer,&sx,&sy);const Pointer pointer{window,sx,sy};
        int ow,oh;SDL_GetRendererOutputSize(renderer,&ow,&oh);const int lw=int(ow/sx),lh=int(oh/sy);
        const Uint64 now=SDL_GetTicks64();auto next=[&] {++phase;since=now;};
        if(phase>0 && now-since>8000) {check(false,"workflow stalled");quit();phase=99;return;}
        switch(phase) {
        case 0:
            check(!fullscreen(window),"the editor starts windowed by default");SDL_GetWindowSize(window,&windowedW,&windowedH);
            pointer.click(96+2*72+20,10);pointer.click(300,22+18*20+10);next();break;   // View > Preferences
        case 1:
            check(state.modal==23,"View > Preferences opens the preferences dialog");
            if(shots)savePng(renderer,fs::path(shots)/"preferences-dialog.png");
            key(SDLK_RIGHT);key(SDLK_TAB);key(SDLK_RIGHT);next();break;   // BORDERLESS FULLSCREEN, edge scrolling ON
        case 2:
            if(shots)savePng(renderer,fs::path(shots)/"preferences-dialog-changed.png");
            key(SDLK_RETURN);next();break;
        case 3: {
            SDL_Rect desktop{};SDL_GetDisplayBounds(SDL_GetWindowDisplayIndex(window),&desktop);
            check(fullscreen(window),"OK switches to borderless fullscreen at once");
            check(ow==desktop.w && oh==desktop.h,"fullscreen covers the desktop ("+std::to_string(ow)+"x"+std::to_string(oh)+" vs "+std::to_string(desktop.w)+"x"+std::to_string(desktop.h)+")");
            const auto saved=readText(profile/"preferences.txt");
            check(saved.find("fullscreen 1\n")!=std::string::npos && saved.find("edgescroll 1\n")!=std::string::npos,"preferences persist at once");
            check(saved.find("width "+std::to_string(windowedW)+"\n")!=std::string::npos,"the windowed size is what is remembered");
            next();break;   // this frame was laid out before the switch; input from now on is at the new scale
        }
        case 4:pointer.click(96+3*72+20,10);pointer.click(330,22+10*20+10);next();break;   // Scenario > Placed units, in fullscreen coordinates
        case 5:
            check(std::string(SDL_GetWindowTitle(window)).find("[Placed units]")!=std::string::npos,"menu clicks map correctly at the fullscreen scale");
            if(shots)savePng(renderer,fs::path(shots)/"fullscreen.png");
            key(SDLK_ESCAPE);key(SDLK_1);pointer.warp(lw-4,lh/2);next();break;   // rest just inside the right canvas edge
        case 6:mark=state.viewX;markZ=state.viewZ;next();break;
        case 7:check(state.viewX==mark,"edge scrolling waits for a short dwell");next();break;
        case 8:
            if(state.viewX>mark+100) {check(state.viewZ==markZ,"the right edge scrolls only horizontally");pointer.warp(kPaletteW-20,lh/2);next();}
            break;
        case 9:mark=state.viewX;next();break;
        case 10:
            check(state.viewX==mark,"the browser beside the canvas does not edge-scroll");
            if(now-since>400) {pointer.click(96+3*72+20,10);pointer.click(330,22+10*20+10);pointer.warp(lw-4,lh/2);next();}   // Placed units, pointer at the edge
            break;
        case 11:mark=state.viewX;check(state.modal!=0,"Placed units is open");next();break;
        case 12:
            check(state.viewX==mark,"an open dialog stops edge scrolling");
            if(now-since>400) {key(SDLK_ESCAPE);key(SDLK_5);next();}   // 12.5%: reach the map's right edge quickly
            break;
        case 13: {
            const float limit=map.width*16.f-(lw-kPaletteW)/state.zoom;
            check(state.viewX<=std::max(mark,limit)+0.5f,"edge scrolling stops at the map edge");
            if(now-since>2500) {check(std::abs(state.viewX-limit)<0.5f,"edge scrolling reaches the map edge ("+std::to_string(state.viewX)+" vs "+std::to_string(limit)+")");
                SDL_WarpMouseInWindow(window,0,oh/2);next();}   // the screen's left edge: the browser's outermost column
            break;
        }
        case 14:mark=state.viewX;markZ=state.viewZ;next();break;
        case 15:
            if(state.viewX<mark-100) {check(state.viewZ==markZ,"the left screen edge scrolls only horizontally");
                pointer.warp(lw/2,lh/2);pointer.click(96+2*72+20,10);pointer.click(300,22+18*20+10);next();}
            break;
        case 16:key(SDLK_TAB);key(SDLK_LEFT);key(SDLK_RETURN);next();break;   // edge scrolling OFF
        case 17:check(fullscreen(window) && state.modal==0,"turning edge scrolling off keeps fullscreen");key(SDLK_1);pointer.warp(lw-4,lh/2);next();break;
        case 18:mark=state.viewX;next();break;
        case 19:check(state.viewX==mark,"edge scrolling off leaves the view alone");if(now-since>500) {key(SDLK_F11);next();}break;
        case 20: {
            int ww,wh;SDL_GetWindowSize(window,&ww,&wh);
            check(!fullscreen(window) && ww==windowedW && wh==windowedH,"F11 returns to the remembered window size");
            check(readText(profile/"preferences.txt").find("fullscreen 0\n")!=std::string::npos,"F11 persists windowed mode");
            key(SDLK_F11);next();break;
        }
        case 21:check(fullscreen(window),"F11 toggles fullscreen back on");quit();next();break;
        }
    },hooks);
    // A restart restores fullscreen; the windowed size stays the remembered one.
    bool restored=false;int restartFrame=0;
    const int second=failure.empty()?cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer*,int frame) {
        restartFrame=frame;if(frame==0)restored=fullscreen(window);
        if(frame==1)quit();
        if(frame>10)std::_Exit(3);
    },hooks):0;
    if(failure.empty()) {
        check(restored,"fullscreen is restored on startup");
        check(readText(profile/"preferences.txt").find("width "+std::to_string(windowedW)+"\nheight "+std::to_string(windowedH)+"\n")!=std::string::npos,"a fullscreen session keeps the windowed size");
    }
    fs::remove_all(root);
    if(result || second || phase<22 || !failure.empty()) {std::cerr<<"display: "<<(failure.empty()?"editor exited early":failure)<<'\n';return 1;}
    std::cout<<"PASS: preferences dialog, borderless fullscreen (live, persisted, restored, pointer mapping, windowed size), edge scrolling (dwell, edges, browser, screen edge, map limit, off)\n";return 0;
}

// View > Preferences > GAME DATA FOLDER, the setting shared with the game: an invalid
// pick keeps the folder, a cancel changes nothing, a valid pick is written to the game's
// settings file at once and offers RESTART NOW, which relaunches with this document;
// the restarted editor mounts the new folder; unsaved work is asked about first (CANCEL
// keeps the editor, DISCARD restarts); a --data launch greys the row. The game's file is
// the profile's copy (TAK_CARTOGRAPHER_PROFILE), never the player's.
static int dataFolderWorkflow(const char* data) {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-editor-datafolder-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);isolateProfile(root.string().c_str());
    const auto gameSettings=root/"TAKengine"/"Cartographer"/"game-settings.ini";
    const auto text=[](const fs::path& p) {const auto t=p.u8string();return std::string(t.begin(),t.end());};
    // A second spelling of the install that does not compare equal to the first: a
    // directory symlink, else the root archives hard-linked into a folder of their own.
    const auto alt=root/"alt-install";std::error_code ec;
    fs::create_directory_symlink(fs::absolute(data),alt,ec);
    if(ec) {
        ec.clear();fs::create_directory(alt,ec);
        for(const auto& entry:fs::directory_iterator(data))if(entry.is_regular_file() && entry.path().extension().string().size()==4)fs::create_hard_link(entry.path(),alt/entry.path().filename(),ec);
    }
    fs::create_directory(root/"not-an-install");
    const std::string original=data,other=text(alt),bad=text(root/"not-an-install");
    {std::ofstream seed(gameSettings);seed<<"dataDir = "<<original<<"\n";}
    const char* shots=std::getenv("TAK_CARTO_SHOT_DIR");
    std::string failure;int phase=0,lastFrame=0;Uint64 since=0;
    auto check=[&](bool value,const std::string& message) {if(!value && failure.empty())failure=message+" (phase "+std::to_string(phase)+", frame "+std::to_string(lastFrame)+")";};
    auto savedDir=[&] {const auto t=readText(gameSettings);const auto at=t.find("dataDir = ");return at==std::string::npos?std::string():t.substr(at+10,t.find('\n',at)-at-10);};
    std::vector<std::string> picks;size_t picked=0;
    std::vector<std::vector<std::string>> relaunches;
    cart::Editor* editor=nullptr;
    cart::EditorHooks hooks;
    hooks.attached=[&](cart::Editor& e) {editor=&e;};
    hooks.pickFolder=[&](const std::string&,const std::string&) {return picked<picks.size()?picks[picked++]:std::string();};
    hooks.relaunch=[&](const std::vector<std::string>& args) {relaunches.push_back(args);};
    auto quit=[] {SDL_Event q{};q.type=SDL_QUIT;SDL_PushEvent(&q);};
    auto run=[&](std::vector<std::string> args,const std::function<void(SDL_Window*,SDL_Renderer*,const Pointer&,Uint64)>& step) {
        phase=0;since=SDL_GetTicks64();std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
        return cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer* renderer,int frame) {
            lastFrame=frame;float sx,sy;SDL_RenderGetScale(renderer,&sx,&sy);const Uint64 now=SDL_GetTicks64();
            if(now-since>15000) {check(false,"workflow stalled");quit();phase=99;return;}
            step(window,renderer,Pointer{window,sx,sy},now);
        },hooks);
    };
    auto next=[&] {++phase;since=SDL_GetTicks64();};
    auto center=[](const SDL_Rect& r) {return SDL_Point{r.x+r.w/2,r.y+r.h/2};};
    auto openPreferences=[](const Pointer& pointer) {pointer.click(96+2*72+20,10);pointer.click(300,22+18*20+10);};

    // 1. Saved folder in use: invalid, cancel, then a valid pick and RESTART NOW (clean document).
    picks={bad,"",other};
    const std::vector<std::string> args={"cartographer","Ulasem Arena","--out",root.string()};
    int result=run(args,[&](SDL_Window*,SDL_Renderer* renderer,const Pointer& pointer,Uint64) {
        switch(phase) {
        case 0:check(editor && editor->dataRoot==original,"the saved shared folder is the one mounted");openPreferences(pointer);next();break;
        case 1:
            check(editor->modal==cart::Editor::M_PREFERENCES && editor->mFolderChange.w>0,"Preferences shows an enabled CHANGE button");
            if(shots)savePng(renderer,fs::path(shots)/"preferences-datafolder.png");
            {const auto c=center(editor->mFolderChange);pointer.click(c.x,c.y);}next();break;
        // Each pick: wait for the worker, then check on the NEXT frame, which collects
        // the outcome before its events and draws it.
        case 2:case 4:case 6:if(!editor->folderPick.busy())next();break;
        case 3:
            check(editor->folderNoteError && editor->folderNote.find("missing")!=std::string::npos,"an invalid pick says what is missing ("+editor->folderNote+")");
            check(savedDir()==original && editor->folderSaved==original,"an invalid pick keeps the shared setting");
            check(editor->mFolderRestart.w==0,"an invalid pick offers no restart");
            if(shots)savePng(renderer,fs::path(shots)/"preferences-datafolder-invalid.png");
            {const auto c=center(editor->mFolderChange);pointer.click(c.x,c.y);}next();break;
        case 5:
            check(!editor->folderNoteError && savedDir()==original && editor->mFolderRestart.w==0,"a cancelled pick changes nothing");
            {const auto c=center(editor->mFolderChange);pointer.click(c.x,c.y);}next();break;
        case 7:
            check(savedDir()==other && editor->folderSaved==other,"a valid pick is written to the game's settings at once ("+savedDir()+")");
            check(readText(gameSettings).find("dataManifest = "+tak::hpi::rootManifest(alt))!=std::string::npos,"a valid pick saves its manifest");
            check(editor->mFolderRestart.w>0 && editor->dataRoot==original,"a valid pick offers RESTART NOW and the open install stays mounted");
            if(shots)savePng(renderer,fs::path(shots)/"preferences-datafolder-restart.png");
            {const auto c=center(editor->mFolderRestart);pointer.click(c.x,c.y);}next();break;
        case 8:if(!editor->running)break;check(false,"RESTART NOW on a clean document leaves the editor");quit();next();break;   // the frame that handled it still finishes
        }
    });
    check(result==0,"the first editor exits cleanly");
    check(relaunches.size()==1 && relaunches.back()==std::vector<std::string>{"--out",root.string(),"Ulasem Arena"},
          "RESTART NOW relaunches with the same output folder and this map");

    // 2. The restarted editor mounts the new folder. Unsaved work: RESTART NOW asks; CANCEL
    // stays; RESTART NOW + DISCARD restarts.
    picks={original};picked=0;
    if(failure.empty())result=run(args,[&](SDL_Window*,SDL_Renderer* renderer,const Pointer& pointer,Uint64) {
        switch(phase) {
        case 0:
            check(editor->dataRoot==other && editor->dataOrigin==tak::DataFolderOrigin::Saved,"the restarted editor mounts the folder chosen ("+editor->dataRoot+")");
            editor->dirty=true;   // unsaved work, as any edit would leave
            openPreferences(pointer);next();break;
        case 1:check(editor->mFolderRestart.w==0,"nothing to restart for while the setting is the folder in use");
            {const auto c=center(editor->mFolderChange);pointer.click(c.x,c.y);}next();break;
        case 2:if(!editor->folderPick.busy())next();break;
        case 3:next();break;   // collected and drawn
        case 4:check(editor->mFolderRestart.w>0,"changing back offers RESTART NOW");{const auto c=center(editor->mFolderRestart);pointer.click(c.x,c.y);}next();break;
        case 5:
            check(editor->modal==cart::Editor::M_QUITSAVE && editor->restartAfterQuit,"unsaved work is asked about before a restart");
            if(shots)savePng(renderer,fs::path(shots)/"preferences-datafolder-unsaved.png");
            {const auto c=center(editor->mCancel);pointer.click(c.x,c.y);}next();break;
        case 6:check(editor->modal==cart::Editor::M_NONE && !editor->restartAfterQuit && editor->running,"CANCEL keeps editing");
            openPreferences(pointer);next();break;
        case 7:{const auto c=center(editor->mFolderRestart);pointer.click(c.x,c.y);}next();break;
        case 8:check(editor->modal==cart::Editor::M_QUITSAVE,"asked again");{const auto c=center(editor->mQuit);pointer.click(c.x,c.y);}next();break;
        case 9:if(!editor->running)break;check(false,"DISCARD restarts");quit();next();break;
        }
    });
    check(result==0 && relaunches.size()==2 && savedDir()==original,"DISCARD restarts after asking");

    // 3. --data overrides the setting: the row is greyed and says so, and never picks.
    picks={bad};picked=0;
    const std::vector<std::string> cli={"cartographer","Ulasem Arena","--data",original,"--out",root.string()};
    if(failure.empty())result=run(cli,[&](SDL_Window*,SDL_Renderer* renderer,const Pointer& pointer,Uint64) {
        switch(phase) {
        case 0:openPreferences(pointer);next();break;
        case 1:{
            check(editor->mFolderChange.w==0 && editor->mFolderRestart.w==0,"a --data launch has no CHANGE button");
            if(shots)savePng(renderer,fs::path(shots)/"preferences-datafolder-locked.png");
            check(editor->dataOrigin==tak::DataFolderOrigin::CommandLine,"--data is the folder's origin");
            editor->clickDataFolder();next();break;   // the handler itself refuses, not just the missing button
        }
        case 2:check(!editor->folderPick.busy() && picked==0,"a locked row never opens the picker");key(SDLK_ESCAPE);quit();next();break;
        }
    });
    check(result==0 && relaunches.size()==2,"quitting a --data launch does not restart");

    // 4. What the restarted editor is handed. SAVE in the restart prompt relaunches with the
    // saved file, which the new editor opens from disk on the new install; a map the new
    // install lacks still opens the editor (OPEN FAILED over the blank map, not an exit); a
    // SAVE that fails cancels the pending restart, so a later Esc exit is only an exit.
    const auto saved=root/"Ulasem Arena.kmp";
    auto edit=[&] {editor->scenario.missionName+=" (edited)";editor->historyPending=editor->dirty=true;};   // a real, undoable change
    picks={other};picked=0;editor=nullptr;
    if(failure.empty())result=run(args,[&](SDL_Window*,SDL_Renderer*,const Pointer& pointer,Uint64) {
        switch(phase) {
        case 0:check(editor->dataRoot==original,"part 4 starts on the original folder");edit();openPreferences(pointer);next();break;
        case 1:{const auto c=center(editor->mFolderChange);pointer.click(c.x,c.y);}next();break;
        case 2:if(!editor->folderPick.busy())next();break;
        case 3:next();break;
        case 4:{const auto c=center(editor->mFolderRestart);pointer.click(c.x,c.y);}next();break;
        case 5:check(editor->modal==cart::Editor::M_QUITSAVE,"unsaved work is asked about");{const auto c=center(editor->mOK);pointer.click(c.x,c.y);}next();break;
        case 6:if(!editor->running)break;check(false,"SAVE restarts");quit();next();break;
        }
    });
    std::error_code same;
    check(result==0 && relaunches.size()==3 && relaunches.back().size()==3 && relaunches.back()[0]=="--out" &&
          fs::equivalent(tak::u8path(relaunches.back()[2]),saved,same),"SAVE relaunches with the saved file");
    std::vector<std::string> reopened={"cartographer"};
    if(relaunches.size()==3)reopened.insert(reopened.end(),relaunches.back().begin(),relaunches.back().end());
    editor=nullptr;
    if(failure.empty())result=run(reopened,[&](SDL_Window*,SDL_Renderer*,const Pointer&,Uint64) {
        if(phase!=0)return;
        check(editor && editor->dataRoot==other && editor->modal!=cart::Editor::M_MESSAGE && editor->mapName=="Ulasem Arena" &&
              !editor->dirty && fs::equivalent(tak::u8path(editor->documentRequest),saved,same),
              "the restarted editor reopens the saved file on the new folder ("+(editor?editor->mapName+", "+editor->mTitle:std::string())+")");
        if(editor)editor->dirty=false;   // leave without the save prompt, whatever was open
        quit();next();
    });
    check(result==0,"the reopened editor exits cleanly");
    editor=nullptr;
    if(failure.empty())result=run({"cartographer","No Such Map","--out",root.string()},[&](SDL_Window*,SDL_Renderer* renderer,const Pointer&,Uint64) {
        if(phase!=0)return;
        std::string message;if(editor)for(const auto& line:editor->mMsg)message+=line+" ";
        check(editor && editor->running && editor->modal==cart::Editor::M_MESSAGE && editor->mTitle=="OPEN FAILED" &&
              message.find("No Such Map")!=std::string::npos,"a map the new folder lacks opens the editor and names it ("+message+")");
        if(shots)savePng(renderer,fs::path(shots)/"preferences-datafolder-missing-map.png");
        if(editor)editor->dirty=false;
        quit();next();
    });
    check(result==0,"a missing map is not an exit code");
    const auto notFolder=root/"not-a-folder";{std::ofstream(notFolder)<<"x";}
    picks={original};picked=0;editor=nullptr;
    if(failure.empty())result=run({"cartographer","Ulasem Arena","--out",text(notFolder)},[&](SDL_Window*,SDL_Renderer*,const Pointer& pointer,Uint64) {
        switch(phase) {
        case 0:edit();openPreferences(pointer);next();break;
        case 1:{const auto c=center(editor->mFolderChange);pointer.click(c.x,c.y);}next();break;
        case 2:if(!editor->folderPick.busy())next();break;
        case 3:next();break;
        case 4:{const auto c=center(editor->mFolderRestart);pointer.click(c.x,c.y);}next();break;
        case 5:{const auto c=center(editor->mOK);pointer.click(c.x,c.y);}next();break;   // SAVE, into a file: fails
        case 6:
            check(editor->running && editor->modal==cart::Editor::M_MESSAGE && editor->mTitle=="SAVE FAILED" && !editor->restartAfterQuit,
                  "a failed save keeps the editor and cancels the restart");
            editor->modal=cart::Editor::M_NONE;key(SDLK_ESCAPE);next();break;   // then leave with Esc
        case 7:check(editor->modal==cart::Editor::M_QUITSAVE,"Esc asks about the unsaved work");next();break;
        case 8:{const auto c=center(editor->mQuit);pointer.click(c.x,c.y);}next();break;
        case 9:if(!editor->running)break;check(false,"DISCARD exits");quit();next();break;
        }
    });
    check(result==0 && relaunches.size()==3,"an exit after a failed restart save is not a restart");
    fs::remove_all(root,ec);
    if(!failure.empty()) {std::cerr<<"data folder: "<<failure<<'\n';return 1;}
    std::cout<<"PASS: preferences data folder (invalid kept, cancel, shared setting saved at once, RESTART NOW reopens the map, restarted editor mounts it, unsaved prompt CANCEL/DISCARD, --data greys it, SAVE reopens the saved file, a missing map opens anyway, a failed save cancels the restart)\n";return 0;
}

// A bare launch (no map named, no --new) opens an EMPTY editor through real input: the
// first-run welcome over no document, no New Map dialog, nothing to save, and only File's
// map-free commands enabled. Every map path is inert there -- canvas clicks and wheel,
// the toolbar, browser and minimap, map shortcuts, the greyed menu rows. File > New map
// (menu) and Ctrl+O open their dialogs without asking about unsaved changes; cancelling
// either stays empty. No recovery copy is written, and quitting asks nothing.
// TAK_EDITOR_TEST_CAPTURE=<prefix> also writes <prefix>-blank.png (the empty editor).
static int blankLaunchWorkflow(const char* data) {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-editor-blank-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);isolateProfile(root.string().c_str());
    std::vector<std::string> args={"cartographer","--data",data,"--out",root.string()};
    std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    cart::Editor* editor=nullptr;cart::EditorHooks hooks;hooks.attached=[&](cart::Editor& e){editor=&e;};
    uint64_t now=0;hooks.recoveryClock=[&]{return now;};
    std::string failure;int stage=0;cart::Editor::Tool toolBefore{};bool gridBefore=false;float zoomBefore=0;
    auto check=[&](bool value,const std::string& message) {if(!value && failure.empty())failure="stage "+std::to_string(stage)+": "+message;};
    const int result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer* renderer,int frame) {
        const std::string title=SDL_GetWindowTitle(window);
        if(!failure.empty() || frame>60) {if(frame>60)failure+=" (did not finish)";std::cerr<<"FAIL: "<<failure<<'\n';std::_Exit(3);}
        float sx=1,sy=1;SDL_RenderGetScale(renderer,&sx,&sy);
        auto press=[&](Uint32 type,int x,int y,Uint8 button=SDL_BUTTON_LEFT) {
            SDL_Event e{};e.type=type;e.button.button=button;e.button.clicks=1;e.button.x=int(x*sx);e.button.y=int(y*sy);SDL_PushEvent(&e);
        };
        auto click=[&](int x,int y) {press(SDL_MOUSEBUTTONDOWN,x,y);press(SDL_MOUSEBUTTONUP,x,y);};
        auto menuRow=[&](int menu,int row) {click(96+menu*72+20,10);click(96+menu*72+20,22+row*20+10);};
        auto empty=[&](const char* what) {
            check(editor->modal==cart::Editor::M_NONE && editor->noDocument && editor->noMap() && !editor->dirty && !editor->history.canUndo() &&
                  title=="Cartographer",std::string(what)+" (title \""+title+"\")");
        };
        using M=cart::Editor::Modal;
        switch(stage) {
        case 0:check(editor->modal==M::M_MESSAGE && editor->mTitle=="WELCOME TO CARTOGRAPHER","the first run's welcome is not up (or New Map is)");
            key(SDLK_RETURN);break;
        case 1:{
            empty("the welcome did not close onto an empty, clean editor");
            for(const auto [menu,row]:{std::pair{0,0},{0,1},{0,6},{0,7},{0,8},{2,12},{2,13},{2,18},{5,0},{5,1}})
                check(editor->menuRowEnabled(menu,row),"a map-free command is disabled: menu "+std::to_string(menu)+" row "+std::to_string(row));
            for(const auto [menu,row]:{std::pair{0,2},{0,3},{0,4},{0,5},{1,0},{1,1},{1,2},{2,0},{2,2},{2,7},{2,17},{3,0},{3,1},{3,3},{3,11},{4,0},{4,1}})
                check(!editor->menuRowEnabled(menu,row),"a map command is enabled: menu "+std::to_string(menu)+" row "+std::to_string(row));
            toolBefore=editor->tool;gridBefore=editor->showGrid;zoomBefore=editor->mapView.zoom();
            // Everything that edits, views or tests a map: none of it may do anything.
            const int cx=cart::Editor::kPaletteW+200,cy=cart::Editor::kMenuH+150;
            click(cart::Editor::kToolX+3*cart::Editor::kToolW+30,32);   // STARTS tool
            click(cart::Editor::kModeX+30,32);click(cart::Editor::kTestX+30,32);   // PLACE mode, TEST MAP
            click(cx,cy);press(SDL_MOUSEBUTTONDOWN,cx,cy,SDL_BUTTON_RIGHT);press(SDL_MOUSEBUTTONUP,cx+40,cy+30,SDL_BUTTON_RIGHT);
            click(40,200);click(editor->w-100,cart::Editor::kMenuH+40);   // the browser, the minimap
            {SDL_Event wheel{};wheel.type=SDL_MOUSEWHEEL;wheel.wheel.y=1;SDL_PushEvent(&wheel);}
            for(const SDL_Keycode k:{SDLK_TAB,SDLK_p,SDLK_r,SDLK_u,SDLK_c,SDLK_t,SDLK_a,SDLK_k,SDLK_g,SDLK_b,SDLK_m,SDLK_F5,SDLK_F6,SDLK_2,SDLK_LEFT,SDLK_DELETE})key(k);
            for(const SDL_Keycode k:{SDLK_s,SDLK_b,SDLK_z,SDLK_y,SDLK_l,SDLK_v,SDLK_d})key(k,KMOD_CTRL);
            key(SDLK_s,KMOD_CTRL|KMOD_SHIFT);key(SDLK_m,KMOD_SHIFT);
            menuRow(0,2);menuRow(0,5);menuRow(3,0);menuRow(2,2);   // greyed rows: Save, Test Map, Properties, Grid
            break;
        }
        case 2:
            empty("a map input acted on the empty editor");
            check(editor->tool==toolBefore && editor->showGrid==gridBefore && editor->mapView.zoom()==zoomBefore && !editor->clearArm && editor->scenario.starts.empty() &&
                  !editor->playtest.running() && !editor->aiOpen && !editor->useOnlyOpen && !editor->scriptOpen && !editor->regionsOpen && editor->menuOpen<0,
                  "a tool, view, dialog or test changed");
            check(!fs::exists(root/"Untitled.kmp") && !fs::exists(root/"Untitled.tnt"),"something was saved");
            if(const char* prefix=SDL_getenv("TAK_EDITOR_TEST_CAPTURE")) {
                int pw=0,ph=0;SDL_GetRendererOutputSize(renderer,&pw,&ph);std::vector<uint8_t> rgba(size_t(pw)*ph*4);
                if(SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,rgba.data(),pw*4)==0)tak::png::write(std::string(prefix)+"-blank.png",pw,ph,rgba);
            }
            menuRow(0,0);break;   // File > New map
        case 3:check(editor->modal==M::M_NEW,"File > New map did not open New Map (or asked about unsaved changes)");key(SDLK_ESCAPE);break;
        case 4:empty("cancelling New Map left the empty editor");key(SDLK_o,KMOD_CTRL);break;
        case 5:check(editor->modal==M::M_OPENPATH,"Ctrl+O did not open Open Map");key(SDLK_ESCAPE);break;
        case 6:empty("cancelling Open Map left the empty editor");key(SDLK_n,KMOD_CTRL);break;
        case 7:{
            check(editor->modal==M::M_NEW,"Ctrl+N did not open New Map");now=10*60000;   // well past the recovery minute
            key(SDLK_ESCAPE);   // held Esc: its repeats after New Map closes must not quit
            for(int i=0;i<3;++i) {SDL_Event e{};e.type=SDL_KEYDOWN;e.key.keysym.sym=SDLK_ESCAPE;e.key.repeat=1;SDL_PushEvent(&e);}
            break;
        }
        case 8:empty("cancelling Ctrl+N's New Map left the empty editor");break;   // a quit the repeats pushed lands this frame
        case 9:empty("held Esc's repeats changed the empty editor");check(editor->running,"held Esc's repeats quit the editor");check(!editor->recoveryFilesPresent && !editor->recoveryJob.valid(),"a recovery copy was written for no document");
            {SDL_Event quit{};quit.type=SDL_QUIT;SDL_PushEvent(&quit);}break;
        default:check(!editor->running,"quitting the empty editor asked something ("+std::to_string(int(editor->modal))+")");break;   // the quit's own frame
        }
        ++stage;
    },hooks);
    fs::remove_all(root);
    if(result || !failure.empty() || stage!=11) {std::cerr<<"FAIL: "<<(failure.empty()?"the editor did not quit (stage "+std::to_string(stage)+")":failure)<<'\n';return 1;}
    std::cout<<"PASS: bare launch opens empty (welcome, no New Map, clean, map commands off and inert), New/Open map cancel back to empty, no recovery, quit asks nothing\n";return 0;
}

// A map named at launch that cannot be opened: OPEN FAILED, then the same empty editor as a bare launch.
static int failedOpenWorkflow(const char* data) {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-editor-failopen-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);isolateProfile(root.string().c_str());
    std::vector<std::string> args={"cartographer","--data",data,"--out",root.string(),"No Such Map Here"};
    std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    cart::Editor* editor=nullptr;cart::EditorHooks hooks;hooks.attached=[&](cart::Editor& e){editor=&e;};
    std::string failure;int stage=0;
    auto check=[&](bool value,const std::string& message) {if(!value && failure.empty())failure="stage "+std::to_string(stage)+": "+message;};
    const int result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer*,int frame) {
        const std::string title=SDL_GetWindowTitle(window);
        if(!failure.empty() || frame>30) {if(frame>30)failure+=" (did not finish)";std::cerr<<"FAIL: "<<failure<<'\n';std::_Exit(3);}
        switch(stage) {
        case 0:check(editor->modal==cart::Editor::M_MESSAGE && editor->mTitle=="OPEN FAILED","a missing map did not say OPEN FAILED");key(SDLK_RETURN);break;
        case 1:
            check(editor->modal==cart::Editor::M_NONE && editor->noDocument && !editor->dirty && !editor->history.canUndo() && title=="Cartographer" &&
                  !editor->menuRowEnabled(0,2),"a failed launch-time open left an editable map (title \""+title+"\")");
            {SDL_Event quit{};quit.type=SDL_QUIT;SDL_PushEvent(&quit);}break;
        default:check(!editor->running,"quitting after a failed open asked something");break;
        }
        ++stage;
    },hooks);
    fs::remove_all(root);
    if(result || !failure.empty() || stage!=3) {std::cerr<<"FAIL: "<<(failure.empty()?"the editor did not quit (stage "+std::to_string(stage)+")":failure)<<'\n';return 1;}
    std::cout<<"PASS: a failed launch-time open ends on the empty editor, clean, quit asks nothing\n";return 0;
}

int main(int argc,char** argv) {
    if(argc>1 && std::string(argv[1])=="--data")return recordTestClient(argc,argv);   // TEST IN GAME's client
    if(argc==3 && std::string(argv[2])=="blank-launch")return blankLaunchWorkflow(argv[1]);
    if(argc==3 && std::string(argv[2])=="failed-open")return failedOpenWorkflow(argv[1]);
    if(argc==3 && std::string(argv[2])=="aimod-nomap")return aiModNoMapWorkflow(argv[1],argv[0]);
    if(argc==5 && std::string(argv[2])=="recovery")return recoveryWorkflow(argv[1],argv[3],argv[4]);
    if((argc==3 || argc==4) && std::string(argv[2])=="validation")return validationWorkflow(argv[1],argc==4?argv[3]:"Ulasem Arena");
    if(argc==2 && std::string(argv[1])=="font")return fontWorkflow();
    if(argc==3 && std::string(argv[1])=="fontsheet")return fontSheet(argv[2]);
    if(argc==3 && std::string(argv[2])=="objects")return objectsWorkflow(argv[1]);
    if(argc==3 && std::string(argv[2])=="model")return modelWorkflow(argv[1]);
    if(argc==3 && std::string(argv[2])=="thumbnails")return thumbnailWorkflow(argv[1]);
    if((argc==3 || argc==4) && std::string(argv[2])=="minimap")return minimapWorkflow(argv[1],argc==4?argv[3]:"Ulasem Arena");
    if(argc==3 && std::string(argv[2])=="features")return featureWorkflow(argv[1]);
    if(argc==3 && std::string(argv[2])=="resize-save")return resizeSaveWorkflow(argv[1]);
    if(argc==3 && std::string(argv[2])=="aiprofile")return aiProfileWorkflow(argv[1]);
    if(argc==3 && std::string(argv[2])=="aiprofile-mod")return aiModWorkflow(argv[1]);
    if(argc==3 && std::string(argv[2])=="aiprofile-extras")return aiExtrasWorkflow(argv[1]);
    if(argc==3 && std::string(argv[2])=="aiprofile-packs")return aiPackListWorkflow(argv[1]);
    if(argc==3 && std::string(argv[2])=="templates")return templateWorkflow(argv[1]);
    if(argc==3 && std::string(argv[2])=="rules")return ruleTextWorkflow(argv[1]);
    if(argc==3 && std::string(argv[2])=="regions")return regionWorkflow(argv[1]);
    if(argc==3 && std::string(argv[2])=="overlay")return overlayWorkflow(argv[1]);
    if(argc==3 && std::string(argv[2])=="generation")return generationWorkflow(argv[1]);
    if(argc==3 && std::string(argv[2])=="headless")return headlessWorkflow(argv[1]);
    if(argc==3 && std::string(argv[2])=="symmetry")return symmetryWorkflow(argv[1]);
    if(argc==3 && std::string(argv[2])=="terrain-clipboard")return terrainClipboardWorkflow(argv[1]);
    if(argc==3 && std::string(argv[2])=="sections")return sectionsWorkflow(argv[1]);
    if(argc==3 && std::string(argv[2])=="palette-scroll")return paletteScrollWorkflow(argv[1]);
    if(argc==3 && std::string(argv[2])=="display")return displayWorkflow(argv[1]);
    if(argc==3 && std::string(argv[2])=="datafolder")return dataFolderWorkflow(argv[1]);
    if(argc!=2)return 2;
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-editor-ui-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    isolateProfile(root.string().c_str());
    std::vector<std::string> args={"cartographer","Ulasem Arena","--data",argv[1],"--out",root.string()};
    std::vector<char*> raw;for(auto& arg:args)raw.push_back(arg.data());
    std::string failure;
    auto check=[&](bool value,const char* message){if(!value && failure.empty())failure=message;};
    const int result=cart::runEditor(int(raw.size()),raw.data(),[&](SDL_Window* window,SDL_Renderer* renderer,int frame) {
        const bool dirty=std::string(SDL_GetWindowTitle(window)).ends_with(" *");
        auto click=[&](int x,int y) {
            float sx,sy;SDL_RenderGetScale(renderer,&sx,&sy);
            SDL_Event e{};e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;
            e.button.x=int(x*sx);e.button.y=int(y*sy);SDL_PushEvent(&e);
            e.type=SDL_MOUSEBUTTONUP;SDL_PushEvent(&e);
        };
        switch(frame) {
        case 0: key(SDLK_p);break;
        case 1: text("Editor integration test");key(SDLK_TAB);text("Saved after undo and redo");key(SDLK_RETURN,KMOD_SHIFT);text("Second description line");key(SDLK_RETURN);break;
        case 2: check(dirty,"property editing marks document dirty");key(SDLK_z,KMOD_CTRL);break;
        case 3: check(!dirty,"undo restores saved revision");key(SDLK_y,KMOD_CTRL);break;
        case 4: check(dirty,"redo restores edited revision");key(SDLK_s,KMOD_CTRL);break;
        case 5: check(!dirty,"successful save clears dirty marker");key(SDLK_RETURN);key(SDLK_o,KMOD_CTRL);break;
        case 6: text((root/"Ulasem Arena.kmp").string());key(SDLK_RETURN);break;
        case 7: check(!dirty,"reopening saved map is clean");key(SDLK_p);break;
        case 8: text("Do not apply this edit");key(SDLK_ESCAPE);break;
        case 9: check(!dirty,"cancelled properties do not mutate document");key(SDLK_s,KMOD_CTRL|KMOD_SHIFT);break;
        case 10: text("Copied map");key(SDLK_RETURN);break;
        case 11: check(!dirty,"Save As marks renamed document saved");key(SDLK_RETURN);key(SDLK_o,KMOD_CTRL);break;
        case 12: text("Map which does not exist");key(SDLK_RETURN);break;
        case 13: check(std::string(SDL_GetWindowTitle(window)).find("Copied map")!=std::string::npos,"failed open preserves current document");key(SDLK_RETURN);break;
        case 14: click(320,10);click(330,152);break; // Scenario > Regions
        case 15: key(SDLK_n,KMOD_CTRL);break;
        case 16: text("UI test region");key(SDLK_TAB);text("10");key(SDLK_TAB);text("11");key(SDLK_TAB);text("20");key(SDLK_TAB);text("99999");key(SDLK_RETURN);key(SDLK_a,KMOD_CTRL);text("21");key(SDLK_RETURN);break;
        case 17: check(dirty,"region creation marks document dirty");key(SDLK_ESCAPE);key(SDLK_s,KMOD_CTRL);break;
        case 18: check(!dirty,"region save succeeds");key(SDLK_RETURN);break;
        case 19: key(SDLK_t);break;
        case 20: {
            float sx,sy;SDL_RenderGetScale(renderer,&sx,&sy);int w,h;SDL_GetRendererOutputSize(renderer,&w,&h);
            click(int(w/sx/2-354),int(h/sy/2+240));break;
        }
        case 21: key(SDLK_d,KMOD_CTRL);break;
        case 22: key(SDLK_ESCAPE);key(SDLK_s,KMOD_CTRL);break;
        case 23: check(!dirty,"duplicated script rules saved");key(SDLK_RETURN);break;
        case 24: key(SDLK_o,KMOD_CTRL);break;
        case 25: text("ulasem");break;
        case 26: {
            float sx,sy;SDL_RenderGetScale(renderer,&sx,&sy);int w,h;SDL_GetRendererOutputSize(renderer,&w,&h);
            click(int(w/sx/2),int(h/sy/2-130));break;
        }
        case 27: check(std::string(SDL_GetWindowTitle(window)).find("Ulasem Arena")!=std::string::npos,"searchable Open chooses recent map");
            key(SDLK_TAB);key(SDLK_TAB);click(500,300);key(SDLK_RETURN);break;
        case 28: text("9");key(SDLK_RETURN);key(SDLK_a,KMOD_CTRL);text("8");
            for(int i=0;i<6;++i)key(SDLK_TAB);
            text("UI neutral unit");key(SDLK_RETURN);break;
        case 29: check(dirty,"corrected unit properties apply");key(SDLK_s,KMOD_CTRL);break;
        case 30: check(!dirty,"unit properties saved");key(SDLK_RETURN);break;
        case 31:
            {SDL_Event quit{};quit.type=SDL_QUIT;SDL_PushEvent(&quit);break;}
        default: if(frame>34) {failure="editor did not exit";key(SDLK_ESCAPE);key(SDLK_RETURN);}
        }
    });
    try {
        if(result!=0)throw std::runtime_error("editor returned failure");
        if(!failure.empty())throw std::runtime_error(failure);
        for(const auto& file:{"Ulasem Arena.kmp","Copied map.kmp"}) {
            tak::hpi::Archive archive(root/file);bool found=false;
            for(const auto& entry:archive.entries())if(entry.path.ends_with(".ota")) {
                const auto bytes=archive.read(entry);const auto metadata=tak::tnt::Scenario::parse(std::string(bytes.begin(),bytes.end()));
                if(metadata.missionName!="Editor integration test" || metadata.missionDescription!="Saved after undo and redo\nSecond description line")throw std::runtime_error("saved metadata differs after UI workflow");
                found=true;
            }
            if(!found)throw std::runtime_error("saved bundle lacks metadata");
            if(std::string(file)=="Ulasem Arena.kmp") {
                bool neutral=false;
                for(const auto& entry:archive.entries())if(entry.path.ends_with(".crt"))
                    for(const auto& unit:cart::toPlaced(tak::crt::parse(archive.read(entry))))
                        if(unit.name=="UI neutral unit" && unit.player==8)neutral=true;
                if(!neutral)throw std::runtime_error("corrected owner and retained unit fields not saved");
            }
            if(std::string(file)=="Copied map.kmp") {
                bool regionFound=false;
                for(const auto& entry:archive.entries())if(entry.path.ends_with(".crt")) {
                    const auto scenario=tak::crt::parse(archive.read(entry));
                    if(scenario.players.empty() || scenario.players[0].size()!=2)throw std::runtime_error("script duplicate workflow did not save two rules");
                    for(const auto& r:scenario.regions)if(r.name=="UI test region" && r.x1==10 && r.z1==11 && r.x2==20 && r.z2==21)regionFound=true;
                }
                if(!regionFound)throw std::runtime_error("region UI workflow did not save expected CRT bounds");
            }
        }
        // UTF-8 editing must erase a character, not leave partial byte sequences.
        cart::TextEdit edit;std::string unicode="A\xc3\xa9\xe6\xb0\xb4";edit.focus(unicode,false);
        SDL_Event back{};back.type=SDL_KEYDOWN;back.key.keysym.sym=SDLK_BACKSPACE;
        edit.input(back,unicode);if(unicode!="A\xc3\xa9")throw std::runtime_error("UTF-8 deletion");
        edit.input(back,unicode);if(unicode!="A")throw std::runtime_error("UTF-8 deletion twice");
        std::string multiline="ab\xc3\xa9\nx\nabcdef";edit.focus(multiline,false);
        auto navigate=[&](SDL_Keycode key,Uint16 mod=0) {SDL_Event e{};e.type=SDL_KEYDOWN;e.key.keysym.sym=key;e.key.keysym.mod=mod;edit.input(e,multiline,false,true,20);};
        navigate(SDLK_HOME,KMOD_CTRL);navigate(SDLK_RIGHT);navigate(SDLK_RIGHT);
        navigate(SDLK_DOWN);if(edit.caret!=6)throw std::runtime_error("vertical movement clamps short line");
        navigate(SDLK_DOWN);if(edit.caret!=9)throw std::runtime_error("vertical movement retains desired column");
        navigate(SDLK_UP);navigate(SDLK_UP,KMOD_SHIFT);
        if(edit.caret!=2 || edit.anchor!=6)throw std::runtime_error("multiline shifted navigation preserves selection anchor");
        navigate(SDLK_END);if(edit.caret!=4)throw std::runtime_error("End targets current line");
        edit.click(multiline,20,2,3);if(edit.caret!=10)throw std::runtime_error("multiline mouse caret position");
        const auto wrapped=cart::TextEdit::lines("one two three",5);
        if(wrapped.size()!=3 || wrapped[0].end!=4 || wrapped[1].begin!=4)throw std::runtime_error("word wrapping");
        if(cart::textWidth("A\xc3\xa9\xe6\xb0\xb4",1)!=24)throw std::runtime_error("Unicode display width counts columns (a wide Han glyph is two)");
        fs::remove_all(root);std::cout<<"PASS: editor properties, undo/redo, save, reopen, Save As, cancel, failed open, UTF-8 editing\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<"; files: "<<root<<'\n';return 1;}
}
