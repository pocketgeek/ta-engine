// Document services shared by the headless and interactive paths (save,
// history snapshots, preferences, minimap invalidation) and the editor's
// document commands: open, undo/redo restore, recovery autosave, Test Map.

#include "cartographer/editorstate.h"
#include "cartographer/dialog.h"
#include "cartographer/document.h"
#include "cartographer/generator.h"
#include "cartographer/savejob.h"
#include "cartographer/scenarioinfo.h"
#include "util/scenariotrace.h"
#include "util/virtualpath.h"

#include <chrono>
#include <fstream>
#include "util/u8path.h"

Uint64 cart::Session::recoveryClock() const {return hooks.recoveryClock?hooks.recoveryClock():SDL_GetTicks64();}

void cart::Session::persistPreferences() {
    if(!(SDL_GetWindowFlags(win)&SDL_WINDOW_FULLSCREEN))SDL_GetWindowSize(win,&preferences.width,&preferences.height);   // remember the windowed size
    std::string error;if(!cart::saveEditorPreferences(preferencesFolder,preferences,error))std::fprintf(stderr,"editor preferences: %s\n",error.c_str());
}

void cart::Session::invalidateMinimap() {minimapCancel->store(true,std::memory_order_relaxed);++minimapRevision;minimapPreviewPending=true;minimapDue=SDL_GetTicks64()+250;}

cart::HistoryState cart::Session::historySnapshot() {return cart::historyState(mapViewPtr->map(),scenario,scen,units,useOnly,aiFiles,mapName);}

void cart::Session::commitHistory() {
    if(resetHistory) {history.reset(historySnapshot(),false);resetHistory=false;}
    else if(historyPending) history.commit(historySnapshot());
    historyPending=false;dirty=history.dirty();
}

bool cart::Session::saveDocument(const std::string& destination,bool bundle) {
    MapView& mapView=*mapViewPtr;
    try {
        commitHistory();
        cart::SaveSnapshot snapshot;
        snapshot.map=mapView.map();snapshot.metadata=scenario;snapshot.scenario=scen;
        snapshot.units=units;snapshot.useOnly=useOnly;snapshot.aiFiles=aiFiles;snapshot.vfs=&vfs;
        snapshot.installMaps=cart::installMapsFolder(tak::u8path(dataRoot));
        snapshot.destination=tak::u8path(destination);
        snapshot.bundle=bundle;snapshot.rebuildMinimaps=edited;
        const auto path=snapshot.destination;
        auto progress=std::make_shared<cart::SaveProgress>();
        auto job=cart::saveInBackground(std::move(snapshot),progress);
        const std::string title=SDL_GetWindowTitle(win);
        std::vector<SDL_Event> deferred;
        if(interactive) {
            SDL_SetWindowTitle(win,(title+" - SAVING").c_str());
            while(job.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready) {
                int width,height;SDL_GetRendererOutputSize(ren,&width,&height);
                const int w=int(width/renderScale()),h=int(height/renderScale());
                const SDL_Rect cancel{w/2-70,h/2+34,140,24};
                SDL_Event event;
                while(SDL_PollEvent(&event)) {
                    const bool escape=event.type==SDL_KEYDOWN && event.key.keysym.sym==SDLK_ESCAPE;
                    const bool click=event.type==SDL_MOUSEBUTTONDOWN && event.button.button==SDL_BUTTON_LEFT &&
                        cart::pointIn(int(event.button.x/kUIScale),int(event.button.y/kUIScale),cancel);
                    if(escape || click || event.type==SDL_QUIT)progress->cancel=true;
                    // Preserve events already queued behind Save (including a
                    // close request), but let SDL keep painting/resizing now.
                    if(!escape && !click && event.type!=SDL_MOUSEMOTION && event.type!=SDL_WINDOWEVENT && deferred.size()<4096)
                        deferred.push_back(event);
                }
                SDL_RenderSetClipRect(ren,nullptr);
                SDL_SetRenderDrawColor(ren,30,34,44,255);SDL_RenderClear(ren);
                const auto panel=cart::drawPanel(ren,w,h,520,190,"SAVING MAP SNAPSHOT");
                const char* labels[]={"PREPARING SNAPSHOT","GENERATING MINIMAPS","SERIALIZING MAP","PACKING / WRITING FILES","FINISHING"};
                cart::drawText(ren,progress->cancel?"CANCELLING...":labels[progress->phase.load()],panel.x,panel.y+10,1,230,235,245);
                cart::drawText(ren,"The previous save stays intact until publication.",panel.x,panel.y+34,1,190,200,215);
                cart::drawButton(ren,cancel.x,cancel.y,cancel.w,cancel.h,"CANCEL (ESC)",false);
                SDL_RenderPresent(ren);SDL_Delay(8);
            }
            SDL_SetWindowTitle(win,title.c_str());
            for(auto& event:deferred)SDL_PushEvent(&event);
        }
        const auto result=job.get();saveError=result.error;
        if(result.ok) {
            history.markSaved();dirty=false;
            preferences.remember(pathText(std::filesystem::absolute(path)));
            documentRequest=pathText(std::filesystem::absolute(path));   // a restart reopens the saved file
            if(interactive)persistPreferences();
        }
        return result.ok;
    } catch(const std::exception& e) {saveError=e.what();return false;}
}
bool cart::Session::saveMap(const std::string& path) {return saveDocument(path,false);}
bool cart::Session::saveBundle(const std::string& path) {return saveDocument(path,true);}

bool cart::Editor::openDocument(const std::string& request) {
    try {
        auto nextVfs=tak::hpi::mountRetailRoot(tak::u8path(dataRoot));
        std::string path,chosen=request;
        cart::AiFiles nextAiFiles;
        const auto disk=tak::u8path(request);
        if(std::filesystem::is_regular_file(disk)) {
            auto files=std::make_shared<tak::hpi::Vfs::Files>();
            if(lowerText(pathText(disk.extension()))==".kmp") {
                tak::hpi::Archive archive(disk);
                nextAiFiles=cart::aiFilesFromArchive(archive);   // root ai/ members, all of them
                for(const auto& entry:archive.entries()) if(!entry.isDirectory && entry.path.starts_with("kmap/")) {
                    (*files)[tak::hpi::MountSet::key(entry.path)]=archive.read(entry);
                    if(tak::vpath::extension(entry.path)==".tnt")path=tak::hpi::MountSet::key(entry.path);
                }
            } else if(lowerText(pathText(disk.extension()))==".tnt") {
                const auto base=pathText(disk.stem());path="kmap/"+base+".tnt";
                // A sibling ai/ folder, except the install's Maps/ai (read for every map).
                const auto folder=disk.parent_path();
                if(!cart::insideFolder(folder,cart::installMapsFolder(tak::u8path(dataRoot))))nextAiFiles=cart::aiFilesFromFolder(folder);
                for(const char* ext:{".tnt",".ota",".crt",".tdf",".recipe",".editor"}) {
                    const auto input=disk.parent_path()/tak::u8path(base+ext);std::ifstream stream(input,std::ios::binary);
                    if(stream)(*files)[tak::hpi::MountSet::key("kmap/"+base+ext)]={std::istreambuf_iterator<char>(stream),{}};
                }
            }
            if(path.empty())throw std::runtime_error("Choose a KMP bundle or TNT map");
            nextVfs.setMapFiles(files);chosen=tak::vpath::stem(path);
        } else if(tak::vpath::extension(request)==".tnt" && nextVfs.has(request)) {path=request;chosen=tak::vpath::stem(request);}
        else path=tak::hpi::findMap(nextVfs,request);
        if(path.empty())throw std::runtime_error("Map not found: "+request+". Enter an installed map name or a full KMP / TNT path.");
        if(!std::filesystem::is_regular_file(disk))nextAiFiles=cart::aiFilesFromMapArchive(nextVfs,path);   // its own archive's
        (void)tak::tnt::Map::load(nextVfs.read(path),path);
        const auto stem=path.substr(0,path.rfind('.'));
        tak::tnt::Scenario nextMetadata;
        if(nextVfs.has(stem+".ota")) {const auto data=nextVfs.read(stem+".ota");nextMetadata=tak::tnt::Scenario::parse(std::string(data.begin(),data.end()));}
        nextMetadata.generatorRecipe=cart::mapGeneratorRecipe(nextVfs,path,nextMetadata.generatorRecipe);
        auto nextScenario=cart::loadScenario(nextVfs,stem+".crt");
        auto nextUnits=cart::toPlaced(nextScenario,&unitRegistry);
        std::set<std::string> nextUseOnly;
        if(!nextMetadata.useOnlyUnits.empty())for(const auto& type:cart::loadUseOnly(nextVfs,stem+".tdf"))nextUseOnly.insert(type);
        minimapCancel->store(true,std::memory_order_relaxed);if(minimapJob.valid())minimapJob.wait();
        thumbs.reset();unitPortraits.reset();features.quiesce();
        mapView.quiesce();vfs=std::move(nextVfs);mapView.reload(vfs,path);mapView.setOffset(0,0);minimapSource=nullptr;invalidateMinimap();
        scenario=std::move(nextMetadata);scen=std::move(nextScenario);units=std::move(nextUnits);useOnly=std::move(nextUseOnly);
        aiFiles=std::move(nextAiFiles);
        mapName=chosen;mapPath=path;world=scenario.kingdom.empty()?"aramon":scenario.kingdom;
        sections.scan(vfs,world);features.scan(vfs,world);
        for(auto& [key,t]:featTex)if(t)SDL_DestroyTexture(t);
        featTex.clear();
        selectedUnits.indices.clear();selectedUnits.dragOrigins.clear();selectedFeatures.clear();
        regionDrag.cancel();regionSelected=-1;
        selected=sections.list().empty()?-1:0;selectedFeat=features.list().empty()?-1:0;paletteScroll=0;
        edited=false;overlayInvalidated=true;historyPending=resetHistory=false;history.reset(historySnapshot(),true);dirty=false;noDocument=false;
        editRule=nullptr;editUnit=draggingUnit=draggingStart=-1;scrGroup=-1;
        if(std::filesystem::is_regular_file(disk)) {outDir=pathText(std::filesystem::absolute(disk).parent_path());mapName=pathText(disk.stem());}
        preferences.remember(request);persistPreferences();
        documentRequest=std::filesystem::is_regular_file(disk)?pathText(std::filesystem::absolute(disk)):request;
        modal=M_NONE;SDL_StopTextInput();return true;
    } catch(const std::exception& error) {openMessage("OPEN FAILED",error.what());return false;}
}

std::filesystem::path cart::Editor::gameClient() const {
    if(hooks.gameClient)return tak::u8path(hooks.gameClient());
    std::unique_ptr<char,decltype(&SDL_free)> base(SDL_GetBasePath(),SDL_free);
    if(!base)throw std::runtime_error("Cannot locate the game executable");
    return cart::Playtest::clientPath(tak::u8path(base.get()));
}

cart::DocumentFiles cart::Session::installedMapFiles(const std::string& path,const std::string& name) const {
    const auto stem=path.substr(0,path.rfind('.'));
    const auto map=tak::tnt::Map::load(vfs.read(path),path);
    tak::tnt::Scenario metadata;
    try {const auto b=vfs.read(stem+".ota");metadata=tak::tnt::Scenario::parse(std::string(b.begin(),b.end()));}
    catch(const std::exception&) {}   // no .ota: defaults, as the editor opens it
    metadata.generatorRecipe=cart::mapGeneratorRecipe(vfs,path,metadata.generatorRecipe);
    const auto scenarioFile=cart::loadScenario(vfs,stem+".crt");
    std::set<std::string> allowed;
    if(!metadata.useOnlyUnits.empty())for(auto& type:cart::loadUseOnly(vfs,stem+".tdf"))allowed.insert(type);
    return cart::documentFiles(map,metadata,scenarioFile,cart::toPlaced(scenarioFile,&unitRegistry),allowed,
                               cart::aiFilesFromMapArchive(vfs,path),name);
}

void cart::Editor::testMap() {
    if(noMap())return;   // no map to test (an AI mod tests through TEST IN GAME)
    if(playtest.running()) {openMessage("TEST MAP", "A test is already running. Close its game window before starting another.");return;}
    try {
        const auto client=gameClient();
        // A distinct map name keeps earlier cached playtests from shadowing edits.
        const auto name="Test "+std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        auto snapshot=mapView.map();
        if(edited) {
            mapView.quiesce();
            cart::generateMinimaps(snapshot,mapView.compositor(),cart::loadWorldPalette(vfs,world));
        }
        auto files=cart::documentFiles(snapshot,scenario,scen,units,useOnly,aiFiles,name);
        if(preferences.tracePlaytest)for(auto& file:files.members)if(file.path.ends_with(".ota")) {
            const std::string marker="\n[TAKPlaytest]\n{\nauthoredscenario=1;\ntrace=1;\n}\n";
            file.data.insert(file.data.end(),marker.begin(),marker.end());
        }
        std::string error;
        if(!playtest.start(client,tak::u8path(dataRoot),files,error))throw std::runtime_error(error);
        playtestAiMod=false;
        const auto log=preferences.tracePlaytest?" Trigger log: "+pathText(tak::scenarioTracePath(preferencesFolder,playtest.snapshot())):std::string{};
        openMessage("TEST MAP", "Opened a private game lobby from a snapshot. Seat the players used by your scenario, then start. Close the game to return here. Your document and save destination are unchanged."+log+
            cart::alwaysEnabledGameNote(tak::u8path(dataRoot)));
    } catch(const std::exception& e) {openMessage("TEST MAP FAILED",e.what());}
}

void cart::Editor::collectRecovery() {
    if(recoveryJob.valid()) {
        const auto error=recoveryJob.get();
        recoveryStatus=error.empty()?"Recovery saved":"Recovery failed: "+error;
    }
}

void cart::Editor::clearRecovery() {
    if(!recoveryFilesPresent && !recoveryJob.valid())return;
    collectRecovery();
    for(auto* session:{recoverySession.get(),recoveredSession.get()}) if(session) {
        std::string error;
        if(!session->discard(error)) {recoveryStatus=error;return;}
    }
    recoveredSession.reset();
    recoveredFrom.clear();recoveryFilesPresent=false;
}

// Every minute while dirty, snapshot the document into the recovery file on a
// worker; a clean document drops its recovery files.
void cart::Editor::autosaveRecovery() {
    if(interactive && recoverySession) {
        if(recoveryJob.valid() && recoveryJob.wait_for(std::chrono::seconds(0))==std::future_status::ready)collectRecovery();
        if(!dirty)clearRecovery();
        else if(!recoveryJob.valid() && recoveryClock()>=nextRecovery && !historyPending) {
            nextRecovery=recoveryClock()+60000;
            try {
                auto files=cart::documentFiles(mapView.map(),scenario,scen,units,useOnly,aiFiles,mapName);
                files.members.push_back(cart::recoveryInfo(mapName,tak::u8path(outDir)));
                recoveryFilesPresent=true;
                recoveryJob=std::async(std::launch::async,[files=std::move(files),path=recoverySession->path()]() {
                    std::string error;cart::writeDocumentBundle(path,files,error);return error;
                });
            } catch(const std::exception& error) {recoveryStatus=error.what();}
        }
    }
}

void cart::Editor::restoreHistory(const cart::HistoryState* state) {
    if(!state)return;
    overlayInvalidated=true;
    mapView.quiesce();
    auto map=tak::tnt::Map::load(state->terrain,"undo history");
    map.seaLevel=state->seaLevel;map.stockTerrain=state->stockTerrain;
    mapView.editMap()=std::move(map);mapView.tilesEdited();minimapSource=nullptr;invalidateMinimap();
    scenario=tak::tnt::Scenario::parse(state->metadata);
    scen=tak::crt::parse(state->scenario);cart::applyScenarioInfo(scen,state->editorMetadata);units=cart::toPlaced(scen,&unitRegistry);
    regionDrag.cancel();regionSelected=-1;
    useOnly=state->useOnly;aiFiles=state->aiFiles;mapName=state->name;dirty=history.dirty();edited=true;
    selectedUnits.indices.clear();selectedUnits.dragOrigins.clear();selectedFeatures.clear();
    editRule=nullptr;draggingUnit=draggingStart=-1;scrGroup=-1;
    const auto nextWorld=scenario.kingdom.empty()?"aramon":scenario.kingdom;
    if(nextWorld!=world) {
        world=nextWorld;sections.scan(vfs,world);features.scan(vfs,world);
        for(auto& [key,texture]:featTex)if(texture)SDL_DestroyTexture(texture);
        featTex.clear();selected=sections.list().empty()?-1:0;selectedFeat=features.list().empty()?-1:0;paletteScroll=0;
    }
}
