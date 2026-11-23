#include "client/settings.h"
#include "net/crypto.h"
#include <SDL.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
int main() {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("ta-settings-"+ta::crypto::toHex(ta::crypto::randomVec(8)));
    try {
        fs::create_directories(root);
        SDL_setenv("XDG_DATA_HOME",root.string().c_str(),1);
        const auto path=fs::path(ta::settingsPath());
        if(path.string().find(root.string()+"/")!=0)throw std::runtime_error("settings not isolated");
        ta::Settings s;
        s.spRoom.gamePacks={"Sound Pack #1","Textures", "Accents-é"};
        s.mpRoom.gamePacks={"Net Rules"};
        s.cosmeticModPacks={"My local art"};
        using PathMode=ta::sim::PathfindingMode;
        // Stable identities: 1-3 belonged to the removed Flowfield,
        // Cooperative and Retail+ modes and are invalid.
        static_assert(uint8_t(PathMode::Retail)==0&&uint8_t(PathMode::Legion)==4);
        for(int value=0;value<256;++value)
            if(ta::sim::validPathfindingMode(uint8_t(value))!=(value==0||value==4))
                throw std::runtime_error("only Retail (0) and Legion (4) are valid pathfinder bytes");
        // Display order: Retail <-> Legion, in both directions.
        if(ta::sim::cyclePathfindingMode(PathMode::Retail,1)!=PathMode::Legion ||
           ta::sim::cyclePathfindingMode(PathMode::Retail,-1)!=PathMode::Legion ||
           ta::sim::cyclePathfindingMode(PathMode::Legion,1)!=PathMode::Retail ||
           ta::sim::cyclePathfindingMode(PathMode::Legion,-1)!=PathMode::Retail ||
           std::string(ta::sim::pathfindingModeName(PathMode::Retail))!="Retail" ||
           std::string(ta::sim::pathfindingModeName(PathMode::Legion))!="Legion")
            throw std::runtime_error("pathfinder choice order or display name missing");
        if(s.spRoom.pathfindingMode!=PathMode::Retail || s.mpRoom.pathfindingMode!=PathMode::Retail)
            throw std::runtime_error("new room preferences must default to Retail");
        s.spRoom.pathfindingMode=PathMode::Legion;
        s.uiScale=0.5f;s.scorecardScale=1.75f;
        if(!ta::saveSettings(s))throw std::runtime_error("save failed");
        const auto loaded=ta::loadSettings();
        if(loaded.spRoom.gamePacks!=s.spRoom.gamePacks || loaded.mpRoom.gamePacks!=s.mpRoom.gamePacks ||
           loaded.cosmeticModPacks!=s.cosmeticModPacks)
            throw std::runtime_error("mod selections did not survive save/load");
        {   // Game packs under each room set's key, cosmetic packs under mods.*, one line per pack.
            std::ifstream saved(path);std::string text((std::istreambuf_iterator<char>(saved)),{});
            if(text.find("spRoom.gamePack = ")==text.npos || text.find("mpRoom.gamePack = ")==text.npos ||
               text.find("mods.cosmeticPack = ")==text.npos || text.find("mods.gamePack")!=text.npos)
                throw std::runtime_error("mod selections not written under the spRoom/mpRoom.gamePack and mods.cosmeticPack keys");
        }
        {   // taclient --game-mod (Cartographer's TEST IN GAME): a launch-only selection never
            // replaces the player's saved ticks, until the player changes the ticks themselves.
            if(!ta::saveSettings(s))throw std::runtime_error("save failed");   // the player's ticks on disk
            // The test room is a single-player room: the selection is the spRoom packs.
            auto run=s;run.spRoom.gamePacks={"My AI"};
            ta::setLaunchGameModPacks(run.spRoom.gamePacks);
            run.uiScale=0.75f;
            if(!ta::saveSettings(run))throw std::runtime_error("save failed");
            auto back=ta::loadSettings();
            if(back.spRoom.gamePacks!=s.spRoom.gamePacks || back.uiScale!=0.75f)
                throw std::runtime_error("a launch-only game mod selection was saved over the player's ticks (or blocked the save)");
            if(back.mpRoom.gamePacks!=s.mpRoom.gamePacks)
                throw std::runtime_error("a launch-only game mod selection changed the multiplayer ticks");
            run.spRoom.gamePacks={"My AI","Textures"};   // the player ticks another pack in that room
            if(!ta::saveSettings(run) || ta::loadSettings().spRoom.gamePacks!=run.spRoom.gamePacks)
                throw std::runtime_error("the player's own new ticks were not saved");
            run.spRoom.gamePacks={"My AI"};   // the launch selection ended with that change
            if(!ta::saveSettings(run) || ta::loadSettings().spRoom.gamePacks!=run.spRoom.gamePacks)
                throw std::runtime_error("a launch selection outlived the player's own change");
            ta::setLaunchGameModPacks(std::nullopt);
            if(!ta::saveSettings(s))throw std::runtime_error("save failed");
            // An EMPTY launch selection (--game-mod naming an always-enabled pack: nothing
            // ticked for the run) still keeps the player's saved ticks.
            auto none=s;none.spRoom.gamePacks={};
            ta::setLaunchGameModPacks(none.spRoom.gamePacks);
            none.spRoom.map="Somewhere";
            if(!ta::saveSettings(none) || ta::loadSettings().spRoom.gamePacks!=s.spRoom.gamePacks)
                throw std::runtime_error("an empty launch-only selection was saved over the player's ticks");
            ta::setLaunchGameModPacks(std::nullopt);
            if(!ta::saveSettings(s))throw std::runtime_error("save failed");
        }
        if(loaded.uiScale!=0.5f || loaded.scorecardScale!=1.75f)
            throw std::runtime_error("independent scorecard scale did not persist");
        if(loaded.spRoom.pathfindingMode!=PathMode::Legion || loaded.mpRoom.pathfindingMode!=PathMode::Retail)
            throw std::runtime_error("Legion single-player preference did not persist on its own");
        auto retail=s;retail.spRoom.pathfindingMode=PathMode::Retail;retail.mpRoom.pathfindingMode=PathMode::Legion;
        if(retail==s || !ta::saveSettings(retail) || ta::loadSettings().spRoom.pathfindingMode!=PathMode::Retail ||
           ta::loadSettings().mpRoom.pathfindingMode!=PathMode::Legion)
            throw std::runtime_error("Retail and Legion room preferences did not persist independently");
        // Both sets read the pathfinder the same way.
        for(const char* set:{"spRoom","mpRoom"}) {
            auto mode=[&](const char* value) {
                std::ofstream(path)<<set<<".pathfindingMode = "<<value<<"\n";
                const auto back=ta::loadSettings();
                return std::string(set)=="spRoom" ? back.spRoom.pathfindingMode : back.mpRoom.pathfindingMode;
            };
            if(mode("4")!=PathMode::Legion)
                throw std::runtime_error(std::string(set)+": saved value 4 did not load as Legion");
            // 1, 2 and 3 name no pathfinding mode: they load as Retail.
            for(const char* removed:{"1","2","3"})
                if(mode(removed)!=PathMode::Retail)
                    throw std::runtime_error("a pathfinding preference that names no mode did not fall back to Retail");
            for(const char* value:{"", "0", "-1", "5", "255", "256", "04", "4 4", "flowfield", "cooperative", "legion", "1oops", "2oops", "4oops"})
                if(mode(value)!=PathMode::Retail)
                    throw std::runtime_error("unknown pathfinding preference must fall back to Retail");
        }
        auto modeChanged=s;modeChanged.spRoom.pathfindingMode=PathMode::Retail;
        if(modeChanged==s)throw std::runtime_error("pathfinding preference changes not detected");
        std::ofstream(path)<<"spRoom.crusades = true\nspRoom.doubleSight = true\nmpRoom.unitCap = 2000\n";
        if(ta::loadSettings().spRoom.pathfindingMode!=PathMode::Retail || ta::loadSettings().mpRoom.pathfindingMode!=PathMode::Retail)
            throw std::runtime_error("room preferences without a pathfinding row must load as Retail");
        // The old key is not read: no migration.
        std::ofstream(path)<<"gameCreate.pathfindingMode = 4\n";
        if(ta::loadSettings().spRoom.pathfindingMode!=PathMode::Retail || ta::loadSettings().mpRoom.pathfindingMode!=PathMode::Retail)
            throw std::runtime_error("gameCreate.pathfindingMode must not be migrated");
        if(!ta::saveSettings(modeChanged) || ta::loadSettings().spRoom.pathfindingMode!=PathMode::Retail)
            throw std::runtime_error("Retail room preference did not persist");
        // START LOCATIONS: 0 FIXED, 1 RANDOM, 2 BY SLOT; absent, out of range or not a
        // number reads as the nearest choice or FIXED (true/on are not numbers: FIXED).
        struct StartsCase { const char* text; int sp, mp; };
        for(const StartsCase& c:std::initializer_list<StartsCase>{
                {"spRoom.randomStarts = 0\n",0,0},{"spRoom.randomStarts = 1\n",1,0},
                {"spRoom.randomStarts = 2\n",2,0},{"spRoom.randomStarts = 7\n",2,0},
                {"mpRoom.randomStarts = 1\n",0,1},{"mpRoom.randomStarts = 2\nspRoom.randomStarts = 1\n",1,2},
                {"spRoom.randomStarts = true\n",0,0},{"mpRoom.randomStarts = on\n",0,0},
                {"spRoom.crusades = 1\n",0,0},{"gameCreate.randomStarts = 2\n",0,0}}) {
            std::ofstream(path)<<c.text;
            const auto back=ta::loadSettings();
            if(back.spRoom.randomStarts!=c.sp || back.mpRoom.randomStarts!=c.mp)
                throw std::runtime_error(std::string("start locations preference not read: ")+c.text);
        }
        for(int mode:{2,1,0}) {
            auto starts=s;starts.spRoom.randomStarts=mode;starts.mpRoom.randomStarts=(mode+1)%3;
            if(!ta::saveSettings(starts) || ta::loadSettings().spRoom.randomStarts!=mode ||
               ta::loadSettings().mpRoom.randomStarts!=(mode+1)%3)
                throw std::runtime_error("start locations preference did not persist: "+std::to_string(mode));
        }
        auto changed=s;changed.scorecardScale=1.0f;
        if(changed==s)throw std::runtime_error("scorecard scale changes not detected");
        // Texture Filtering persists all three levels under its own key.
        for(int level=0;level<3;++level) {
            auto filtered=s;filtered.textureFilter=level;
            if(!ta::saveSettings(filtered) || ta::loadSettings().textureFilter!=level)
                throw std::runtime_error("texture filtering level did not persist");
        }
        {
            std::ifstream saved(path);std::string text((std::istreambuf_iterator<char>(saved)),{});
            if(text.find("textureFilter = 2\n")==text.npos || text.find("bilinearFilter")!=text.npos)
                throw std::runtime_error("texture filtering is not written as textureFilter");
        }
        // GPU MEMORY LIMIT: every choice persists under its own key, a file without it loads
        // as the 1280 MiB default, which is saved as its number, and a hand-edited value is
        // snapped or falls back.
        for(int mib:ta::kGpuMemoryLimits) {
            auto limited=s;limited.gpuMemoryLimit=mib;
            if(!ta::saveSettings(limited) || ta::loadSettings().gpuMemoryLimit!=mib)
                throw std::runtime_error("GPU memory limit did not persist: "+std::to_string(mib));
        }
        {
            auto limited=s;limited.gpuMemoryLimit=-1;ta::saveSettings(limited);
            std::ifstream saved(path);std::string text((std::istreambuf_iterator<char>(saved)),{});
            if(text.find("\ngpuMemoryLimit = unlimited\n")==text.npos)throw std::runtime_error("Unlimited must be written as 'gpuMemoryLimit = unlimited'");
            limited.gpuMemoryLimit=0;ta::saveSettings(limited);
            std::ifstream again(path);std::string defText((std::istreambuf_iterator<char>(again)),{});
            if(defText.find("\ngpuMemoryLimit = 1280\n")==defText.npos)throw std::runtime_error("the default must be written as 'gpuMemoryLimit = 1280'");
        }
        std::ofstream(path)<<"vsync = 1\n";
        if(ta::loadSettings().gpuMemoryLimit!=1280)throw std::runtime_error("a file without the key must load GPU memory limit as 1280");
        struct GpuCase { const char* text; int expected; };
        for(const GpuCase& c:std::initializer_list<GpuCase>{
                {"gpuMemoryLimit = 2048\n",2048},{"gpuMemoryLimit =  UNLIMITED \n",-1},{"gpuMemoryLimit = 900\n",1024},
                {"gpuMemoryLimit = 64\n",512},{"gpuMemoryLimit = lots\n",1280},{"gpuMemoryLimit =\n",1280},
                {"gpuMemoryLimit = 0\n",1280}}) {
            std::ofstream(path)<<c.text;
            if(ta::loadSettings().gpuMemoryLimit!=c.expected)
                throw std::runtime_error(std::string("GPU memory limit value not normalised: ")+c.text);
        }
        std::ofstream(path)<<"uiScale = 2\n";
        if(ta::loadSettings().scorecardScale!=1.0f)throw std::runtime_error("scorecard inherits HUD scale");
        std::ofstream(path)<<"scorecardScale = 9\n";
        if(ta::loadSettings().scorecardScale!=2.0f)throw std::runtime_error("scorecard upper limit");
        std::ofstream(path)<<"scorecardScale = 0\n";
        if(ta::loadSettings().scorecardScale!=0.5f)throw std::runtime_error("scorecard lower limit");
        // Streaming SAVE A LOCAL COPY: off by default and in files written before it
        // existed; the choice survives a save and load (a restart) both ways.
        if(ta::Settings{}.streamLocalCopy)throw std::runtime_error("local copy must default to off");
        std::ofstream(path)<<"uiScale = 2\n";
        if(ta::loadSettings().streamLocalCopy)throw std::runtime_error("local copy must load as off when absent");
        {
            auto copy=s;copy.streamLocalCopy=true;
            if(!ta::saveSettings(copy) || !ta::loadSettings().streamLocalCopy)throw std::runtime_error("local copy on must persist");
            copy.streamLocalCopy=false;
            if(!ta::saveSettings(copy) || ta::loadSettings().streamLocalCopy)throw std::runtime_error("local copy off must persist");
            if(ta::serializeSettings(copy).find("streamLocalCopy = 0\n")==std::string::npos)
                throw std::runtime_error("local copy saved as an on/off key");
            if(ta::videoDir()!=ta::replayDir()+"videos"+ta::replayDir().back())
                throw std::runtime_error("local copies go in the user folder's videos folder");
        }
        // Renderer: AUTO ("") by default and when the key is absent (every file written
        // before the option existed); stored by SDL driver id, written as "auto" for AUTO.
        if(!ta::Settings{}.renderer.empty())throw std::runtime_error("renderer must default to Auto");
        std::ofstream(path)<<"vsync = 1\n";
        if(!ta::loadSettings().renderer.empty())throw std::runtime_error("pre-option file must load as Auto");
        for(const char* id:{"","opengl","opengles2","software","direct3d11","metal"}) {
            ta::Settings v;v.renderer=id;
            if(!ta::saveSettings(v) || ta::loadSettings().renderer!=id)
                throw std::runtime_error(std::string("renderer did not round trip: ")+id);
        }
        {
            ta::Settings v;
            if(!ta::saveSettings(v))throw std::runtime_error("save failed");
            std::ifstream in(path);std::string all((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
            if(all.find("\nrenderer = auto\n")==std::string::npos)throw std::runtime_error("Auto must be written as 'renderer = auto'");
        }
        struct RendererCase { const char* text; const char* expected; };
        for(const RendererCase& c:std::initializer_list<RendererCase>{
                {"renderer = auto\n",""},{"renderer = AUTO\n",""},{"renderer =\n",""},
                {"renderer = OpenGL\n","opengl"},{"renderer =  software  \n","software"},
                // a well-formed id this build lacks is kept; startup falls back for that launch
                {"renderer = vulkan\n","vulkan"},
                // junk never reaches SDL_SetHint
                {"renderer = open gl\n",""},{"renderer = ../../x\n",""}}) {
            std::ofstream(path)<<c.text;
            if(ta::loadSettings().renderer!=c.expected)
                throw std::runtime_error(std::string("renderer value not normalised: ")+c.text);
        }
        {
            ta::Settings a,b;b.renderer="software";
            if(a==b)throw std::runtime_error("renderer changes not detected");
            if(!ta::preferenceDefaults(b).renderer.empty())throw std::runtime_error("DEFAULTS must reset the renderer to Auto");
        }
        // Graphics defaults: Texture Filtering Everything, art filters on.
        auto graphicsDefaults=[](const ta::Settings& v) {
            if(v.textureFilter!=2 || !v.smoothArt || !v.videoDeblock)throw std::runtime_error("graphics defaults changed");
        };
        graphicsDefaults(ta::Settings{});
        if(!ta::Settings{}.treeSway)throw std::runtime_error("tree sway must default on");
        // Tactical Dots is a non-retail extra: on by default, on for a file written
        // before the key existed, and its own key round-trips both ways.
        if(!ta::Settings{}.tacticalDots)throw std::runtime_error("tactical dots must default on");
        if(ta::Settings{}.tacticalDotsZoom!=0)throw std::runtime_error("tactical dots zoom must default to 0%");
        std::ofstream(path)<<"treeSway = 0\nunitShadows = 1\n";
        if(!ta::loadSettings().tacticalDots)throw std::runtime_error("pre-dots settings file must load dots on");
        for(const char* text:{"tacticalDots = 1\n","tacticalDots = on\n","tacticalDots = true\n"}) {
            std::ofstream(path)<<text;
            if(!ta::loadSettings().tacticalDots)throw std::runtime_error(std::string("tactical dots not read: ")+text);
        }
        for(bool dots:{true,false}) {
            ta::Settings v;v.tacticalDots=dots;v.tacticalDotsZoom=dots?73:0;
            if(!ta::saveSettings(v) || !(ta::loadSettings()==v) || ta::loadSettings().tacticalDots!=dots)
                throw std::runtime_error("tactical dots failed round trip");
        }
        for(int level:{0,1,2}) {
            ta::Settings v;v.textureFilter=level;v.smoothArt=v.videoDeblock=true;v.treeSway=false;v.vsync=false;
            if(!ta::saveSettings(v) || !(ta::loadSettings()==v))
                throw std::runtime_error("graphics preferences failed round trip");
        }
        // A file without textureFilter loads the default, Everything. The keys it replaced
        // (bilinearFilter, bilinear, zoomSmoothing) are not read: unknown keys like any other,
        // never written back.
        for(const char* text:{"","bilinearFilter = 1\n","bilinear = 1\n","zoomSmoothing = smooth\n"}) {
            std::ofstream(path)<<text<<"smoothArt = 1\n";
            const auto loaded=ta::loadSettings();
            if(loaded.textureFilter!=2 || !loaded.smoothArt)throw std::runtime_error(std::string("texture filtering without its key: ")+text);
            if(*text && ta::serializeSettings(loaded).find(text)!=std::string::npos)
                throw std::runtime_error(std::string("an unknown key was written back: ")+text);
        }
        // Out of range clamps like every other number.
        std::ofstream(path)<<"textureFilter = 7\n";
        if(ta::loadSettings().textureFilter!=2)throw std::runtime_error("texture filtering not clamped high");
        std::ofstream(path)<<"textureFilter = -3\n";
        if(ta::loadSettings().textureFilter!=0)throw std::runtime_error("texture filtering not clamped low");
        s.textureFilter=2;s.smoothArt=s.videoDeblock=true;
        if(!ta::saveSettings(s))throw std::runtime_error("save failed");
        graphicsDefaults(ta::loadSettings());
        fs::remove_all(root);std::cout<<"PASS: game and cosmetic mod selections and other preferences persist\n";return 0;
    }catch(const std::exception& e){fs::remove_all(root);std::cerr<<e.what()<<'\n';return 1;}
}
