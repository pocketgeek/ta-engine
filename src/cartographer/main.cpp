#include "util/winargv.h"
// Cartographer -- a clean-room re-implementation of the retail TA:Kingdoms map
// editor (see docs/cartographer-port.md). Static-analysis RE of the shipped
// Cartographer.exe drives the behaviour; this shares the engine's rendering
// stack (hpi VFS, tnt loader, terrain compositor, MapView) so terrain looks
// byte-identical to the game.
//
// Document serialization and undo live in document/history; selection operations
// live in selection.h. This file starts the editor (Session: command line, data
// root, window, document load) and runs the frame loop; editorstate.h maps the
// other concerns to their files.

#include "net/netcompat.h"
#include <SDL.h>

#include "cartographer/editorstate.h"
#include "cartographer/dialog.h"
#include "cartographer/document.h"
#include "cartographer/generator.h"
#include "version.h"
#include "client/settings.h"
#include "client/waylandchoice.h"
#include "client/waylandmeasure.h"
#include "client/datafolder.h"
#include "client/dirpicker.h"
#include "cartographer/restart.h"
#include "util/relaunch.h"
#include "util/prefpath.h"
#include "util/appicon.h"
#include "util/virtualpath.h"
#include "terrain/terrain.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include "util/u8path.h"

float cart::kUIScale = 1.5f;
float cart::kBacking = 1.0f;

cart::FreshMap cart::buildFreshMap(const tak::hpi::Vfs& vfs, cart::SectionLibrary& sections,
                       tak::terrain::Compositor& comp, const std::string& world,
                       int wUnits, int hUnits, bool random, const tak::mapgen::Params* requested) {
    FreshMap out;
    if (random) {
        tak::mapgen::Params gp=requested?*requested:tak::mapgen::Params{};
        gp.mapType = tak::mapgen::Aramon;
        for (uint8_t i = 0; i < tak::mapgen::kMapTypes; ++i) if (world == tak::mapgen::worldKey(i)) gp.mapType = i;
        gp.widthCells = uint16_t(wUnits * 32);
        gp.heightCells = uint16_t(hUnits * 32);
        if(!requested) {gp.players=4;gp.seed=uint64_t(SDL_GetPerformanceCounter());}
        gp=tak::mapgen::sanitize(gp);out.recipe=tak::mapgen::encodeMapId(gp);
        auto res = tak::mapgen::generate(tak::mapgen::sanitize(gp), vfs);
        out.map = std::move(res.map);
        int n = 1;
        for (auto& [sx, sz] : res.starts) out.starts.push_back({n++, sx, sz});
    } else {
        out.map = cart::newBlankMap(vfs, sections, comp, world, wUnits, hUnits);
    }
    return out;
}

// TAK_CARTOGRAPHER_PROFILE names a test profile folder. Debug builds and the editor's test
// executables (TAK_CARTOGRAPHER_TEST, every build type) read it; a release editor reads no
// TAK_* variable, like a release takclient.
static const char* testProfile() {
#if !defined(NDEBUG) || defined(TAK_CARTOGRAPHER_TEST)
    const char* o=SDL_getenv("TAK_CARTOGRAPHER_PROFILE");
    return o && *o ? o : nullptr;
#else
    return nullptr;
#endif
}

// Preferences/recovery folder. A test profile (above) names it outright; otherwise the
// per-user folder, which follows XDG_DATA_HOME in tests on every platform (util/prefpath.h).
static std::filesystem::path profileFolder() {
    if(const char* o=testProfile()) return tak::u8path(o);
    const std::string path=tak::prefPath("Cartographer");
    return path.empty()?std::filesystem::path():tak::u8path(path);
}

namespace {
const char* const kUsage =
            "Cartographer -- TA:Kingdoms map editor\n"
            "usage: cartographer \"<map name>\" [--data <retail-install-dir>]\n"
            "         (--data is optional when run from inside a game folder)\n"
            "         [--out <dir>]        Ctrl+S / Ctrl+B save destination (default .)\n"
            "         [--new WxH --world <w> [--random]]  start a blank/random map\n"
            "                                 (or press N in-editor)\n"
            "         [--save <file.tnt>]  headless: save loose .tnt/.ota/.crt and exit\n"
            "         [--bundle <file.kmp>] headless: save a packed .kmp map and exit\n"
            "         --version, -v        print version and exit\n"
            "         --help, -h           print this help and exit\n"
            "in-editor: Tab tools, 1-5 zoom, G grid, N new, P/R/U/C/T/K scenario menu,\n"
            "           Ctrl+S save .kmp, Ctrl+Shift+S export loose files\n";
}

cart::Session::Session(int argc,char** argv,const FrameHook& frameHook,const EditorHooks& hooks)
    : frameHook(frameHook), hooks(hooks) {
    parseArguments(argc,argv);
    launchArgs.assign(argv+std::min(argc,1),argv+argc);
    // A test profile has its own copy of the game's settings file, so a test can
    // exercise the shared data folder without ever touching the player's real one.
    if(const char* profile=testProfile())
        tak::setSettingsPath(pathText(tak::u8path(profile)/"game-settings.ini"));
    // Data root, by the game's own rules (tak::chooseDataFolder): --data wins; then the
    // local directory if it holds a valid install (drop the editor into a game folder
    // and it just works); then the GAME's saved folder, which Preferences changes for
    // both programs; then ask.
    {
        const auto valid=[](const std::string& folder,std::string* why) {return tak::hpi::validInstall(tak::u8path(folder),why);};
        std::error_code ec;const auto here=std::filesystem::current_path(ec);
        const auto choice=tak::chooseDataFolder(dataRoot,ec?std::string():pathText(here),tak::loadSettings().dataDir,valid);
        dataOrigin=choice.origin;
        if(choice.origin!=tak::DataFolderOrigin::CommandLine)dataRoot=choice.folder;
        if(choice.origin==tak::DataFolderOrigin::LaunchFolder)std::fprintf(stderr,"data: using the local directory %s\n",dataRoot.c_str());
        const char* video=std::getenv("SDL_VIDEODRIVER");
        if(dataRoot.empty() && (!video || (std::string(video)!="dummy" && std::string(video)!="offscreen")))
            for(int tries=0;tries<6 && dataRoot.empty();++tries) {
                const auto picked=tak::pickDirectory(tak::kDataFolderPickerTitle,"");
                if(picked.empty())break;   // cancelled
                std::string why;
                if(valid(picked,&why)) {dataRoot=picked;tak::saveDataFolderSetting(picked,tak::hpi::rootManifest(tak::u8path(picked)));}
                else SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Not a game folder",tak::dataFolderInvalidMessage(why).c_str(),nullptr);
            }
    }
    if (dataRoot.empty()) {
        std::fprintf(stderr,"%s",kUsage);
        throw Exit{2};
    }

    if(!explicitOutput) outDir=pathText(tak::u8path(dataRoot)/"Maps");
    // A .kmp/.tnt FILE named at launch (how a restart reopens a saved document) opens
    // through File > Open once the editor is up; a bare name is an installed map.
    {
        std::error_code ec;
        if(!mapName.empty() && shotPath.empty() && exportPath.empty() && bundlePath.empty() && stampName.empty() &&
           std::filesystem::is_regular_file(tak::u8path(mapName),ec)) {pendingOpen=mapName;mapName.clear();}
    }

    SDL_SetMainReady();   // we own main() (SDL_MAIN_HANDLED); tell SDL not to hijack it
#if defined(__linux__)
    // NATIVE WAYLAND, the game's Display option (shared settings file): a Wayland window in
    // real pixels instead of X11 through XWayland, stretched at a desktop scale above 100%.
    bool appIdSet=false;
    if(shotPath.empty() && exportPath.empty() && bundlePath.empty() && stampName.empty())tak::wayland::startup().session=tak::wayland::probeSession();
    if(shotPath.empty() && exportPath.empty() && bundlePath.empty() && stampName.empty() &&
       tak::wayland::wantNative(tak::loadSettings().nativeWayland,tak::wayland::startup().session)) {
        tak::wayland::startup().requested=true;
        SDL_SetHint(SDL_HINT_VIDEODRIVER,"wayland,x11");   // X11 if Wayland cannot start
        if(!std::getenv("SDL_VIDEO_WAYLAND_WMCLASS")) {setenv("SDL_VIDEO_WAYLAND_WMCLASS","tak-cartographer",0);appIdSet=true;}
    }
#endif
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        throw Exit{1};
    }
#if defined(__linux__)
    if(appIdSet)unsetenv("SDL_VIDEO_WAYLAND_WMCLASS");   // read by SDL_Init; keep it out of TEST IN GAME
    if(tak::wayland::startup().requested) {
        const char* drv=SDL_GetCurrentVideoDriver();
        tak::wayland::startup().native=drv && std::string(drv)=="wayland";
        std::fprintf(stderr,"video: native wayland %s, driver %s\n",tak::wayland::startup().native?"on":"unavailable -- using X11",drv?drv:"?");
    }
#endif
    sdlLifetime.active=true;
    preferencesFolder=profileFolder();
    preferences=cart::loadEditorPreferences(preferencesFolder);
    SDL_Rect display{0,0,1600,1000};SDL_GetDisplayUsableBounds(0,&display);
    const int initialWidth=std::min(preferences.width?preferences.width:1600,std::max(800,int(display.w*.9f)));
    const int initialHeight=std::min(preferences.height?preferences.height:1000,std::max(600,int(display.h*.9f)));
    win = SDL_CreateWindow(
        ("Cartographer -- " + mapName).c_str(), SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, initialWidth, initialHeight, SDL_WINDOW_RESIZABLE
#if defined(__linux__)
        | (tak::wayland::startup().native ? SDL_WINDOW_ALLOW_HIGHDPI : 0)
#endif
        );
    windowOwner.reset(win);
    {   // Application icon: the compass-rose badge (src/util/appicon).
        std::vector<uint8_t> ic = tak::appicon::render(tak::appicon::Kind::Cartographer, 64);
        if (SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(
                ic.data(), 64, 64, 32, 64 * 4, SDL_PIXELFORMAT_RGBA32)) {
            SDL_SetWindowIcon(win, s);
            SDL_FreeSurface(s);
        }
    }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    rendererOwner.reset(ren);
    // Draw everything at kUIScale: the logical canvas stays 1280x800-ish while
    // the window is that many times larger, so the UI is magnified uniformly.
    if (ren) SDL_RenderSetScale(ren, float(kUIScale), float(kUIScale));
    if (!win || !ren) {
        std::fprintf(stderr, "window/renderer: %s\n", SDL_GetError());
        throw Exit{1};
    }
#if defined(__linux__)
    if(shotPath.empty() && exportPath.empty() && bundlePath.empty() && stampName.empty()) {
        // UI SCALE and the window size are in window points except on a real-pixel X11
        // window (KDE's default), so keep them the same physical size when that changes
        // (NATIVE WAYLAND switched): client/waylandchoice.h.
        namespace wlc=tak::wayland;
        const auto& wl=wlc::startup();
        const char* drv=SDL_GetCurrentVideoDriver();
        wlc::Observation now;now.mode=wlc::sessionMode(drv?drv:"",wl.native,wl.session);
        // A native window's scale only once the compositor has sent it (waylandmeasure.h).
        if(now.mode==wlc::kModeWayland) {now.scale=wlc::settledNativeScale(win,ren);now.logicalWidth=wlc::desktopWidth();}
        else if(now.mode==wlc::kModeX11)now.x11Width=wlc::desktopWidth();
        wlc::DisplayHistory h{preferences.displayMode,preferences.displayScale,preferences.displayX11Width,preferences.displayLogicalWidth};
        int kinds[2]={0,0};
        const float f=wlc::advanceDisplay(h,now,wlc::Units::CartographerPoints,wlc::fallbackX11Kind(wl.session),&kinds);
        if(now.mode!=wlc::kModeUnknown)
            std::fprintf(stderr,"dpi: X11 desktop %d, logical %d, scale %.2f: last window %s, this one %s\n",h.x11Width,h.logicalWidth,now.scale>0?now.scale:h.scale,
                         wlc::modeName(kinds[0]),wlc::modeName(kinds[1]));
        if(std::abs(f-1)>=.005f) {
            preferences.scalePercent=std::clamp(int(preferences.scalePercent*f+.5f),50,200);
            if(preferences.width)preferences.width=std::clamp(int(preferences.width*f+.5f),640,8192);
            if(preferences.height)preferences.height=std::clamp(int(preferences.height*f+.5f),480,8192);
            if(!preferences.fullscreen) {
                SDL_SetWindowSize(win,std::min(preferences.width?preferences.width:1600,std::max(800,int(display.w*.9f))),
                                  std::min(preferences.height?preferences.height:1000,std::max(600,int(display.h*.9f))));
                SDL_SetWindowPosition(win,SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED);   // X11 grows from the top-left
            }
            std::fprintf(stderr,"dpi: display %s -> %s: UI scale x%.3f -> %d%%\n",wlc::modeName(kinds[0]),wlc::modeName(kinds[1]),f,preferences.scalePercent);
        }
        if(std::abs(f-1)>=.005f || h.mode!=preferences.displayMode || h.scale!=preferences.displayScale ||
           h.x11Width!=preferences.displayX11Width || h.logicalWidth!=preferences.displayLogicalWidth) {
            preferences.displayMode=h.mode;preferences.displayScale=h.scale;
            preferences.displayX11Width=h.x11Width;preferences.displayLogicalWidth=h.logicalWidth;
            std::string error;if(!cart::saveEditorPreferences(preferencesFolder,preferences,error))std::fprintf(stderr,"editor preferences: %s\n",error.c_str());
        }
    }
#endif

    editorFont.emplace(ren);

    // The game's downloaded maps and Test Map copies (MapCache/) and saved generated
    // maps live in the game's per-user folder, beside its settings.ini; list them too.
    tak::hpi::setUserMapRoot(tak::u8path(tak::replayDir()));
    try {
        vfs = tak::hpi::mountRetailRoot(tak::u8path(dataRoot));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "mount %s: %s\n", dataRoot.c_str(), e.what());
        throw Exit{1};
    }
    interactive=shotPath.empty() && exportPath.empty() && bundlePath.empty() && stampName.empty();
    recoveryFolder=profileFolder();
    if(interactive && !recoveryFolder.empty()) {
        try {
            recoverySession=cart::RecoveryFile::create(recoveryFolder);
            if(mapName.empty() && pendingOpen.empty() && !newW)recoveredSession=cart::RecoveryFile::claimNewest(recoveryFolder);
        } catch(const std::exception& e) {recoveryStatus=std::string("Recovery unavailable: ")+e.what();}
        if(recoveredSession) {
            const SDL_MessageBoxButtonData buttons[]={{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT,1,"Recover"},{0,2,"Discard recovery"},{SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,0,"Cancel"}};
            SDL_MessageBoxData box{SDL_MESSAGEBOX_INFORMATION,win,"Recover map","An unsaved map recovery is available.",3,buttons,nullptr};
            int choice=0;if(hooks.recoveryChoice)choice=hooks.recoveryChoice();else SDL_ShowMessageBox(&box,&choice);
            if(choice==0)throw Exit{0};
            if(choice==2) {
                std::string error;
                if(!recoveredSession->discard(error)) {SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Recovery cleanup failed",error.c_str(),win);throw Exit{1};}
                recoveredSession.reset();
            }
            if(choice==1) {
                try {
                    auto snapshot=cart::readRecoverySnapshot(recoveredSession->path());
                    recoveredMapPath=snapshot.mapPath;mapName=tak::vpath::stem(snapshot.mapPath);
                    if(snapshot.destination) {mapName=snapshot.destination->name;if(!explicitOutput)outDir=snapshot.destination->directory;}
                    if(snapshot.fromBackup)recoveryStatus="Recovered previous snapshot; newest recovery was unreadable";
                    aiFiles=cart::liftAiFiles(*snapshot.files);   // root ai/ members back into the document
                    vfs.setMapFiles(std::move(snapshot.files));recoveredFrom=recoveredSession->path();
                } catch(const std::exception& e) {SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Recovery failed",e.what(),win);throw Exit{1};}
            }
        }
    }

    // Resolve the map name to its .tnt VFS path (Maps/<name>.tnt or a .kmp's
    // kmap/<name>.tnt), same resolution the game uses. An empty name (or --new
    // without --save) starts a fresh blank map instead -- mapPath stays empty so
    // there are no sibling scenario files to load. A bare interactive launch opens
    // no document at all: the blank map is only a hidden placeholder (noDocument).
    showWelcome=interactive && mapName.empty() && pendingOpen.empty() && !newW && !preferences.guideSeen;
    mapPath=recoveredMapPath;
    if(recoveredMapPath.empty())documentRequest=mapName;   // an installed map reopens by name
    if (!mapName.empty() && mapPath.empty()) {
        for (const auto& [name, path] : tak::hpi::listMaps(vfs)) {
            std::string lo = name;
            for (char& c : lo) c = char(std::tolower((unsigned char)c));
            std::string want = mapName;
            for (char& c : want) c = char(std::tolower((unsigned char)c));
            if (lo == want) { mapPath = path; break; }
        }
        if (mapPath.empty()) {
            std::fprintf(stderr, "map '%s' not found in %s\n", mapName.c_str(), dataRoot.c_str());
            // Headless runs fail. The editor itself opens and says why (File > Open's
            // OPEN FAILED over the blank map): a restart onto a new GAME DATA FOLDER that
            // lacks the open map, or a VFS path File > Open accepted, must not just vanish.
            if(!interactive) throw Exit{1};
            pendingOpen=mapName;mapName.clear();
        }
    }
    if (!mapName.empty()) {
        mapViewPtr = std::make_unique<MapView>(ren, vfs, mapPath);
        // An installed map by name: its own archive's ai/ files (recovery lifted its own).
        if (recoveredMapPath.empty()) aiFiles = cart::aiFilesFromMapArchive(vfs, mapPath);
        // Companion .ota (metadata + start positions), so a Save round-trips the
        // whole map. Missing/parse-fail = defaults.
        std::string otaPath = mapPath.substr(0, mapPath.rfind('.')) + ".ota";
        try {
            auto b = vfs.read(otaPath);
            scenario = tak::tnt::Scenario::parse(std::string(b.begin(), b.end()));
        } catch (const std::exception&) { /* no .ota: keep defaults */ }
        scenario.generatorRecipe=cart::mapGeneratorRecipe(vfs,mapPath,scenario.generatorRecipe);
    } else {
        int fw = newW > 0 ? newW : 8, fh = newH > 0 ? newH : 8;
        cart::SectionLibrary ns; ns.scan(vfs, newWorld);
        tak::terrain::Compositor nc(vfs);
        FreshMap fm = buildFreshMap(vfs, ns, nc, newWorld, fw, fh, randomTerrain);
        if (fm.map.width == 0) {
            std::fprintf(stderr, "new: could not build terrain for world '%s'\n", newWorld.c_str());
            throw Exit{1};
        }
        mapName = "Untitled";
        noDocument=interactive && !newW && pendingOpen.empty();   // File > New map / Open map makes the first one
        scenario.kingdom = newWorld; scenario.sizeW = fw; scenario.sizeH = fh;
        scenario.missionName = mapName;
        scenario.generatorRecipe=fm.recipe;
        scenario.starts = std::move(fm.starts);
        mapViewPtr = std::make_unique<MapView>(ren, vfs, std::move(fm.map));
        SDL_SetWindowTitle(win, noDocument?"Cartographer":"Cartographer -- Untitled");
    }
    MapView& mapView = *mapViewPtr;
    mapView.setBilinear(true);
    std::fprintf(stderr,
                 "cartographer: editing '%s' (%s), %dx%d blocks; scenario '%s' "
                 "kingdom=%s, %zu start positions\n",
                 mapName.c_str(), mapPath.c_str(), mapView.map().blocksX,
                 mapView.map().blocksY, scenario.missionName.c_str(),
                 scenario.kingdom.c_str(), scenario.starts.size());

    std::string crtPath = mapPath.substr(0, mapPath.rfind('.')) + ".crt";
    try {scen=cart::loadScenario(vfs,crtPath);}
    catch(const std::exception& error) {
        std::fprintf(stderr,"open scenario: %s\n",error.what());
        if(interactive)SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Cannot open map",error.what(),win);
        throw Exit{1};
    }
    units = cart::toPlaced(scen);

    std::string useOnlyTdf = mapPath.substr(0, mapPath.rfind('.')) + ".tdf";
    if (!scenario.useOnlyUnits.empty())
        for (auto& t : cart::loadUseOnly(vfs, useOnlyTdf)) useOnly.insert(t);
    dirty = (mapPath.empty() && !noDocument) || !recoveredFrom.empty();   // no document: nothing to lose
    history.reset(historySnapshot(),!dirty);

    world = scenario.kingdom.empty() ? "aramon" : scenario.kingdom;
    sections.scan(vfs, world);
    features.scan(vfs, world);
    std::fprintf(stderr, "cartographer: %zu placeable features for '%s'\n",
                 features.list().size(), world.c_str());

    // Unit-type list for the palette (units + scen loaded above, before saveMap).
    unitTypes = cart::unitTypeNames(vfs);
    tak::sim::setupRegistry(unitRegistry,vfs,false);
    tak::sim::setupRegistry(crusadesRegistry,vfs,true);
    // Offer only types the game spawns by that name: a scenario naming any other
    // stops the match ("Scenario unit type is missing"). Iron Plague's campaign
    // tarnecr2.fbi declares objectname TARNECRO, so the registry has no TARNECR2.
    std::erase_if(unitTypes,[&](const std::string& type){return !unitRegistry.find(lowerText(type));});
    units=cart::toPlaced(scen,&unitRegistry);
    std::fprintf(stderr, "cartographer: %zu placed units, %zu unit types\n",
                 units.size(), unitTypes.size());
    std::fprintf(stderr, "cartographer: %zu sections for world '%s'\n",
                 sections.list().size(), world.c_str());

    if(const auto code=runHeadless())throw Exit{*code};
}

cart::Editor::Editor(int argc,char** argv,const FrameHook& frameHook,const EditorHooks& hooks)
    : Session(argc,argv,frameHook,hooks) {
    for(uint8_t i=0;i<tak::mapgen::kLayouts;++i)generatorLayouts.push_back(tak::mapgen::layoutName(i));
    for(uint8_t i=0;i<tak::mapgen::kScatterLevels;++i)generatorScatter.push_back(tak::mapgen::scatterName(i));
    if(showWelcome) {
        openMessage("WELCOME TO CARTOGRAPHER",
            "Start with File > Open (Ctrl+O), or New (Ctrl+N). Pick a tool and search the left browser (Ctrl+F).\n"
            "Place adds objects; Select moves or edits them. Right-drag pans in any mode; the mouse wheel zooms. Click the minimap to travel.\n"
            "Ctrl+Z undoes edits. Ctrl+S saves a playable map bundle. Unsaved work gets recovery copies every minute.\n"
            "Before playing, use Scenario > Check map (C). Hover over toolbar controls for help; Help > Editor controls lists shortcuts.\n"
            "For fair multiplayer maps, the Symmetry menu mirrors your edits live (M) or copies one half onto the other (Shift+M).");
        preferences.guideSeen=true;persistPreferences();
    }
    nextRecovery=recoveryClock()+60000;
    if(interactive)applyDisplayMode();   // a remembered borderless fullscreen (shots keep their window size)
    prepareShot();
    // A bare launch (no map named, no --new size) opens with no document: the File
    // menu makes or opens a map, or an AI mod.
    // A failure says why (OPEN FAILED) over no document, as a bare launch would show.
    if(!pendingOpen.empty() && !openDocument(pendingOpen)) {noDocument=true;dirty=historyPending=false;history.reset(historySnapshot(),true);}
}

int cart::Editor::run() {
    int frameNumber=0;
    if(hooks.attached)hooks.attached(*this);
    while (running) {
        if(const auto result=playtest.poll();result && *result!=0)
            // Worded by what was tested and what is still open (an AI mod closed mid-test leaves no document).
            openMessage(playtestAiMod?"TEST IN GAME ENDED":"TEST MAP ENDED", "The game exited with an error ("+std::to_string(*result)+")."+
                        (aiModOnly?" Your AI mod is still open here.":noDocument?std::string():" Your map is still open here."));
        SDL_GetRendererOutputSize(ren, &w, &h);
        {int ww=0,wh=0;SDL_GetWindowSize(win,&ww,&wh);kBacking=tak::backingScale(ww,w);}
        kUIScale=std::max(.5f,std::min({preferences.scalePercent/100.f,float(w)/kBacking/900.0f,float(h)/kBacking/600.0f}));
        SDL_RenderSetScale(ren,renderScale(),renderScale());
        w /= renderScale(); h /= renderScale();   // physical -> logical (SDL_RenderSetScale)
        canvasH = h - kMenuH - kStatusH;
        canvasW = w - kPaletteW;
        const float miniScale=140.0f/std::max(mapView.map().width,mapView.map().height);
        miniRect={w-148,kMenuH+8,std::max(1,int(mapView.map().width*miniScale)),std::max(1,int(mapView.map().height*miniScale))};

        pollJobs();
        pollDataFolder();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            // The browser's rows index the current tool's list (sections, features,
            // unit types or starts). An earlier event may have switched the tool or
            // changed the list, so rebuild before anything reads a row.
            refreshPalette();
            if(swallowWhileBusy(e))continue;
            const float pointerDX=e.type==SDL_MOUSEMOTION?float(e.motion.xrel)/kUIScale:0;
            const float pointerDZ=e.type==SDL_MOUSEMOTION?float(e.motion.yrel)/kUIScale:0;
            if(e.type==SDL_RENDER_TARGETS_RESET || e.type==SDL_RENDER_DEVICE_RESET)
                mapView.invalidateRenderTargets();
            // Map input from physical window pixels into the logical draw space.
            if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
                e.button.x /= kUIScale; e.button.y /= kUIScale;
            } else if (e.type == SDL_MOUSEMOTION) {
                e.motion.x /= kUIScale; e.motion.y /= kUIScale;
                e.motion.xrel /= kUIScale; e.motion.yrel /= kUIScale;
            }
            if(handleRegionDrag(e))continue;
            if(handleGeneratedPreview(e))continue;
            if (e.type == SDL_QUIT) {
                restartAfterQuit=false;   // closing the window is an exit, whatever was pending
                // A game-mod pack edited in the AI window is not the map: ask about it on its own.
                if(aiModDirty()) {
                    if(modal==M_CONFIRM)continue;   // a question is already up
                    modal=M_NONE;   // any other dialog gives way: unsaved pack edits are asked about first
                    aiClosePack([] {SDL_Event quit{};quit.type=SDL_QUIT;SDL_PushEvent(&quit);});
                    continue;
                }
                if (dirty) {
                    // Close any transient overlay so the quit prompt gets input.
                    useOnlyOpen = false; scriptOpen = false; pickOpen = false;regionsOpen=false;closeAiProfiles();
                    modal = M_QUITSAVE; SDL_StopTextInput();
                } else running = false;
                continue;
            }
            // An AI mod without a map keeps the menu bar (its map commands disabled) while
            // the window has no dialog of its own up.
            const bool workspaceMenus=aiModOnly && aiOpen && !aiStartOpen && !aiModSaveOpen && !aiModOpenOpen && aiWordPick==-2;
            if(modal==M_NONE && !scriptOpen && (!aiOpen || workspaceMenus) && !useOnlyOpen && !regionsOpen && handleMenuInput(e))continue;
            if(modal==M_NONE && !scriptOpen && !aiOpen && !useOnlyOpen && !regionsOpen && !noMap() && (handleMinimapInput(e) || handlePaletteScrollbar(e) || handlePaletteFocus(e)))continue;
            if(regionsOpen && modal==M_NONE) {handleRegionsList(e);continue;}
            if (useOnlyOpen) {handleUseOnly(e);continue;}
            if (modal != M_NONE) {handleModal(e);continue;}
            if (scriptOpen) {handleScripting(e);continue;}
            if (aiOpen) {handleAiProfiles(e);continue;}
            if (aiModOpenOpen) {handleAiModDialogs(e);continue;}   // File > Open AI mod's pack list
            if (noMap()) {handleShortcuts(e);continue;}            // no document: File's keys only, no canvas
            // Toolbar, canvas keys, global shortcuts, then palette and canvas
            // pointer input; the first handler that takes the event wins.
            if(handleToolbarModes(e) || handleCanvasKeys(e) || handleShortcuts(e) ||
               handleToolbarTools(e) || handlePaletteClick(e) || handlePaletteWheel(e))continue;
            handleCanvasInput(e,pointerDX,pointerDZ);
        }

        // A confirmed New (discarding unsaved edits) opens the dialog now, after
        // the confirm's applyModal has closed itself.
        if (wantNew) { wantNew = false; openModal(M_NEW); }
        edgeScroll();
        refreshPalette();   // the drawn browser and status bar follow this frame's edits

        if(overlayInvalidated || historyPending || resetHistory) {overlayTexture.reset();overlayLegend.clear();overlayInvalidated=false;}
        if((historyPending || resetHistory) && !(SDL_GetMouseState(nullptr,nullptr)&(SDL_BUTTON_LMASK|SDL_BUTTON_RMASK)))
            commitHistory();
        const std::string title=noDocument && !aiModOnly?std::string("Cartographer"):aiModOnly?"Cartographer -- AI mod "+(aiMod?aiMod->pack:std::string())+(aiModDirty()?" *":""):"Cartographer -- "+mapName+(modal==M_OBJECTS?" [Placed units]":modal==M_MODEL_LOADING?" [Loading model]":modal==M_MODEL?" [Unit model]":modal==M_GENERATING?" [Generating]":modal==M_GENERATED?" [Generated preview]":modal==M_CHECKING?" [Checking map]":modal==M_ISSUES?" [Validation results]":modal==M_ANALYZING?" [Analyzing]":aiOpen?" [AI]":overlayTexture?" [Terrain overlay]":"")+(dirty?" *":"");
        SDL_SetWindowTitle(win,title.c_str());
        autosaveRecovery();
        refreshMinimap();updateLiveMinimap();
        if(!noMap())mapView.ensureChunks(canvasW, canvasH);

        SDL_SetRenderDrawColor(ren, 24, 26, 32, 255);
        SDL_RenderClear(ren);

        if(!noMap()) {   // no map (none open, or an AI mod's): a plain backdrop
            drawCanvas();
            drawMinimap();
            drawPalette();
        }
        drawChrome();
        if(!noMap()) {
            drawSectionMenu();
            drawClearArea();
            drawRegionsList();
            drawScripting();
        }
        drawAiProfiles();
        if(aiModOnly)drawMenuRows();     // over the AI mod's window
        if(!aiOpen)drawAiModDialogs();   // File > Open AI mod's pack list
        drawModal();
        drawUseOnly();
        drawHints();

        if(hooks.frameState)hooks.frameState({mapView.offX(),mapView.offY(),mapView.zoom(),int(tool),paletteScroll,paletteScrollBar().maxScroll(),
            tool==UNITS?selectedType:tool==FEATURES?selectedFeat:tool==TERRAIN?selected:tool==BLOCKED?blockedBrush:-1,int(modal)});
        if(frameHook)frameHook(win,ren,frameNumber++);
        SDL_RenderPresent(ren);
        captureShot();
    }

    minimapCancel->store(true,std::memory_order_relaxed);
    if(restartAfterQuit)restartArgs=cart::restartArguments(launchArgs,documentRequest);
    if(interactive) {clearRecovery();persistPreferences();}
    thumbs.reset();unitPortraits.reset();modelPreview.reset();
    for (auto& [k, t] : featTex) if (t) SDL_DestroyTexture(t);
    return 0;
}

int cart::runEditor(int argc, char** argv, const std::function<void(SDL_Window*,SDL_Renderer*,int)>& frameHook,const EditorHooks& hooks) {
    std::optional<std::vector<std::string>> restart;int code=0;
    try {
        Editor editor(argc,argv,frameHook,hooks);
        code=editor.run();restart=std::move(editor.restartArgs);
    } catch(const Session::Exit& exit) {return exit.code;}
    // Preferences changed the GAME DATA FOLDER and the player chose Restart: the editor
    // (window, renderer, SDL) is gone, so the new process owns everything afresh.
    if(restart) {
        if(hooks.relaunch) {hooks.relaunch(*restart);return code;}
        std::string why;
        if(!tak::relaunchSelf(*restart,why)) {
            std::fprintf(stderr,"restart failed: %s\n",why.c_str());
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Restart failed",("The new game data folder is saved. Start Cartographer again to use it.\n\n("+why+")").c_str(),nullptr);
            return 1;
        }
    }
    return code;
}

#ifndef TAK_CARTOGRAPHER_TEST
int main(int argc,char** argv) {
    // Informational flags answer before any editor, window or data-folder prompt exists.
    for(int i=1;i<argc;++i) {
        const std::string a=argv[i];
        if(a=="--version"||a=="-v") {std::printf("cartographer (TAK engine) %s (build %s)\n",tak::kVersion,tak::kBuildId);return 0;}
        if(a=="--help"||a=="-h") {std::fputs(kUsage,stdout);return 0;}
    }
    return cart::runEditor(argc,argv);
}
#ifdef _WIN32
int WINAPI WinMain(HINSTANCE,HINSTANCE,LPSTR,int) {return tak::utf8Main(main);}
#endif
#endif
