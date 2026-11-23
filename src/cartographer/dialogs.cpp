// Modal dialogs (New / Open / Save As / properties / resize / generator / rule
// parameters / messages), the regions, Use Only and placed-unit lists, the unit
// model preview, terrain overlays and Check Map, plus their background jobs.

#include "cartographer/editorstate.h"
#include "cartographer/dialog.h"
#include "cartographer/document.h"
#include "cartographer/generator.h"
#include "cartographer/triggers.h"
#include "client/settings.h"

#include <charconv>
#include <cmath>
#include <limits>
#include "util/u8path.h"

namespace {
std::string objectOwner(int player) {return player==8?std::string("Neutral"):"Player "+std::to_string(player+1);}
}

void cart::Editor::filterOpenMaps() {
    const auto query=lowerText(mf[0]);if(!openFilterDirty && query==openFilterPrevious)return;
    openFilterDirty=false;openFilterPrevious=query;openMatches.clear();openScroll=0;
    for(const auto& entry:openCatalog)if(query.empty() || lowerText(entry.first).find(query)!=std::string::npos || lowerText(entry.second).find(query)!=std::string::npos)openMatches.push_back(entry);
    openScroll=std::clamp(openScroll,0,std::max(0,int(openMatches.size())*20-openList.h));
}

// Pop a message box (word-wrapped to ~46 cols) — used by Check Map.
void cart::Editor::openMessage(const std::string& title, const std::string& text) {
    mMsg.clear();
    for(const auto& line:cart::TextEdit::lines(text,46))mMsg.push_back(text.substr(line.begin,line.end-line.begin));
    mTitle = title; modal = M_MESSAGE; mN = 0; mfocus = 0;
}

// A yes/no confirmation that runs `action` on OK (used by Clear Area).
void cart::Editor::openConfirm(const std::string& title, const std::string& text,
                               std::function<void()> action) {
    openMessage(title, text);
    modal = M_CONFIRM; confirmAction = std::move(action);
}

void cart::Editor::openModal(Modal m, int unitIdx) {
    mfocus = 0; editUnit = unitIdx;descriptionScroll=0;descriptionFollowCaret=true;modalError.clear();
    mDropOpen = -1;
    for (auto& editable : mfEditableChoice) editable=false;
    for (auto& c : mfChoices) c = nullptr;   // default: plain text fields
    if (m == M_SCENARIO) {
        mTitle = "SCENARIO PROPERTIES"; mN = 2;
        mLabel[0] = "SCENARIO NAME"; mf[0] = scenario.missionName;        mfNumeric[0] = false;
        mLabel[1] = "DESCRIPTION";   mf[1] = scenario.missionDescription; mfNumeric[1] = false;
    } else if(m==M_GENERATOR) {
        mTitle="RANDOM MAP SETTINGS";mN=15;
        const auto fields=cart::generatorFields(generatorParams);
        const char* labels[]={"SEED (REPRODUCIBLE)","PLAYERS (2-8)","LAYOUT","TREES (0-255)","ROCKS (0-255)","MANA (0-255)","WATER / LAVA PADS (0-255)","RELIEF (0-255)"};
        for(int i=0;i<8;++i) {mf[i]=fields[i];mLabel[i]=labels[i];mfNumeric[i]=i!=2;}
        mfChoices[2]=&generatorLayouts;
        mLabel[8]="WIDTH (1-64 UNITS)";mf[8]=generatorDraft[1];mfNumeric[8]=true;
        mLabel[9]="HEIGHT (1-64 UNITS)";mf[9]=generatorDraft[2];mfNumeric[9]=true;
        mLabel[10]="WORLD";mf[10]=generatorDraft[3];mfNumeric[10]=false;mfChoices[10]=&kWorldOpts;
        mLabel[11]="SCATTERED MANA (BY MAP AREA)";mf[11]=fields[8];mfNumeric[11]=false;mfChoices[11]=&generatorScatter;
        // v10 retail kits (Maze and Highlands raise their own high ground).
        mLabel[12]="PLATEAUS (0-255)";mf[12]=fields[9];mfNumeric[12]=true;
        mLabel[13]="MOUNTAINS (0-255)";mf[13]=fields[10];mfNumeric[13]=true;
        mLabel[14]="TOWNS (0-255)";mf[14]=fields[11];mfNumeric[14]=true;
    } else if(m==M_SYMMETRIZE) {
        setupSymmetrizeFields();
    } else if(m==M_PREFERENCES) {
        mTitle="PREFERENCES";mN=3;
        mLabel[0]="DISPLAY (F11 TOGGLES)";mf[0]=kDisplayOpts[preferences.fullscreen];mfChoices[0]=&kDisplayOpts;
        mLabel[1]="EDGE SCROLLING (POINTER AT THE MAP EDGE)";mf[1]=kOnOffOpts[preferences.edgeScroll];mfChoices[1]=&kOnOffOpts;
        mLabel[2]="UI SCALE (CAPPED TO FIT THE WINDOW)";mf[2]=std::to_string(preferences.scalePercent)+"%";mfChoices[2]=&kScaleOpts;
        if(std::find(kScaleOpts.begin(),kScaleOpts.end(),mf[2])==kScaleOpts.end())mf[2]="100%";
        for(int i=0;i<3;++i)mfNumeric[i]=false;
        // GAME DATA FOLDER below the fields: the shared setting as it is NOW (the game
        // may have changed it since this editor started).
        folderSaved=tak::loadSettings().dataDir;folderNote.clear();folderNoteError=false;
    } else if(m==M_REGION) {
        editRegion=unitIdx;
        tak::crt::Region region;
        if(unitIdx>=0 && unitIdx<int(scen.regions.size()))region=scen.regions[unitIdx];
        else {
            region.name="Region "+std::to_string(scen.regions.size()+1);
            region.x1=std::clamp(int(mapView.offX()/16),0,mapView.map().width-1);
            region.z1=std::clamp(int(mapView.offY()/16),0,mapView.map().height-1);
            region.x2=std::min(region.x1+15,mapView.map().width-1);
            region.z2=std::min(region.z1+15,mapView.map().height-1);
        }
        mTitle=unitIdx<0?"NEW REGION":"REGION PROPERTIES";mN=5;
        mLabel[0]="NAME (RENAMES UPDATE RULES)";mf[0]=region.name;mfNumeric[0]=false;
        mLabel[1]="LEFT CELL (INCLUSIVE)";mf[1]=std::to_string(region.x1);
        mLabel[2]="TOP CELL (INCLUSIVE)";mf[2]=std::to_string(region.z1);
        mLabel[3]="RIGHT CELL (INCLUSIVE)";mf[3]=std::to_string(region.x2);
        mLabel[4]="BOTTOM CELL (INCLUSIVE)";mf[4]=std::to_string(region.z2);
        for(int i=1;i<5;++i)mfNumeric[i]=true;
    } else if (m == M_SAVEAS) {
        mTitle="SAVE MAP AS";mN=2;
        mLabel[0]="MAP FILE NAME";mf[0]=mapName;mfNumeric[0]=false;
        mLabel[1]="FOLDER";mf[1]=outDir;mfNumeric[1]=false;
    } else if (m == M_OPENPATH) {
        mTitle="OPEN MAP";mN=1;openForTest=false;
        mLabel[0]="SEARCH MAPS OR ENTER KMP / TNT PATH";mf[0]="";mfNumeric[0]=false;
        openCatalog.clear();openScroll=0;openFilterDirty=true;
        for(const auto& recent:preferences.recent)openCatalog.emplace_back("Recent: "+recent,recent);
        for(const auto& [name,path]:tak::hpi::listMaps(vfs))openCatalog.emplace_back(name,path);
        filterOpenMaps();
    } else if(m==M_NEWAIMOD) {
        // A pack name the install does not have yet (Open AI mod edits an existing one).
        std::vector<std::string> packs;
        try {packs=tak::hpi::modPacks(tak::u8path(dataRoot),tak::hpi::ModKind::Game);} catch(const std::exception&) {}
        auto taken=[&](const std::string& name) {return std::any_of(packs.begin(),packs.end(),[&](const std::string& p){return lowerText(p)==lowerText(name);}) ||
                                                        !cart::modPackReservedError(tak::u8path(dataRoot),name).empty();};
        std::string name="my-ai";
        for(int n=2;taken(name);++n)name="my-ai-"+std::to_string(n);
        mTitle="NEW AI MOD";mN=1;
        mLabel[0]="GAME MOD PACK NAME (MODS/GAME/<NAME>)";mf[0]=name;mfNumeric[0]=false;
    } else if (m == M_RESIZE) {
        mTitle = "RESIZE MAP"; mN = 2;
        mLabel[0] = "WIDTH (UNITS)";  mf[0] = std::to_string(mapView.map().width / 32);  mfNumeric[0] = true;
        mLabel[1] = "HEIGHT (UNITS)"; mf[1] = std::to_string(mapView.map().height / 32); mfNumeric[1] = true;
    } else if (m == M_NEW) {
        // Name is typed; size + world are dropdowns. RANDOM is a separate
        // button (drawn in the render pass) that generates procedural terrain
        // for the chosen size/world and jumps straight to it.
        regeneratingCurrent=false;generatorParams.seed=uint64_t(SDL_GetPerformanceCounter());
        mTitle = "NEW MAP"; mN = 4;
        mLabel[0] = "MAP NAME";  mf[0] = "Untitled"; mfNumeric[0] = false;
        mLabel[1] = "WIDTH";     mf[1] = "8";  mfChoices[1] = &kSizeOpts;
        mLabel[2] = "HEIGHT";    mf[2] = "8";  mfChoices[2] = &kSizeOpts;
        std::string w0 = world;
        std::transform(w0.begin(), w0.end(), w0.begin(), [](unsigned char c){return char(std::tolower(c));});
        if (std::find(kWorldOpts.begin(), kWorldOpts.end(), w0) == kWorldOpts.end())
            w0 = "aramon";
        mLabel[3] = "WORLD";     mf[3] = w0;   mfChoices[3] = &kWorldOpts;
    } else if (m == M_UNIT && unitIdx >= 0 && unitIdx < int(units.size())) {
        const auto& u = units[size_t(unitIdx)];
        mTitle = selectedUnits.indices.size()>1?"SELECTED UNIT PROPERTIES":"UNIT PROPERTIES"; mN = 9;
        mLabel[0] = "PLAYER (0-7, 8=NEUTRAL)";  mf[0] = std::to_string(u.player);      mfNumeric[0] = true;
        mLabel[1] = "HEALTH %";      mf[1] = std::to_string(u.health);      mfNumeric[1] = true;
        mLabel[2] = "ARMOR %";       mf[2] = std::to_string(u.armor);       mfNumeric[2] = true;
        mLabel[3] = "WEAPON %";      mf[3] = std::to_string(u.weapon);      mfNumeric[3] = true;
        mLabel[4] = "VETERAN (0-9)"; mf[4] = std::to_string(u.veteran);     mfNumeric[4] = true;
        mLabel[5] = "ANGLE (DEG)";   mf[5] = std::to_string(int(u.angle));  mfNumeric[5] = true;
        mLabel[6] = selectedUnits.indices.size()>1?"NAME (SINGLE UNIT ONLY)":"DISPLAY NAME";mf[6]=u.name;mfNumeric[6]=false;
        mLabel[7] = "X (CELL)";mf[7]=std::to_string(int((u.x-u.footX*8)/16));mfNumeric[7]=true;
        mLabel[8] = "Z (CELL)";mf[8]=std::to_string(int((u.z-u.footZ*8)/16));mfNumeric[8]=true;
    } else {
        return;   // nothing to open
    }
    for(int i=0;i<mN;++i)mfOriginal[i]=mf[i];
    modal = m;fieldEditor.focus(mf[mfocus]);
    SDL_StartTextInput();
}

// Build a fresh map from the New Map dialog's fields and switch to it. `random`
// picks procedural terrain (the RANDOM button) vs a flat stamp (CREATE). On
// failure it pops a message and leaves the current map untouched; on success it
// closes the dialog. Shared by the CREATE and RANDOM buttons.
void cart::Editor::adoptFreshMap(FreshMap fm,const std::string& nm,const std::string& wld) {
    const auto previousName=scenario.missionName,previousDescription=scenario.missionDescription;
    sections.scan(vfs,wld);features.scan(vfs,wld);
    for(auto& [key,texture]:featTex)if(texture)SDL_DestroyTexture(texture);
    featTex.clear();paletteKey.clear();
    mapView.quiesce();
    mapView.editMap() = std::move(fm.map);minimapSource=nullptr;invalidateMinimap();
    mapView.tilesEdited();
    mapView.setOffset(0, 0);
    world = wld;
    scenario = tak::tnt::Scenario{};
    scenario.kingdom = wld; scenario.sizeW = mapView.map().width/32; scenario.sizeH = mapView.map().height/32;
    scenario.missionName = nm;
    scenario.generatorRecipe=fm.recipe;
    if(regeneratingCurrent) {scenario.missionName=previousName;scenario.missionDescription=previousDescription;}
    scenario.starts = std::move(fm.starts);
    if(!regeneratingCurrent)documentRequest.clear();   // a new, unsaved map: a restart cannot reopen it
    mapName = nm;
    selectedUnits.indices.clear();selectedUnits.dragOrigins.clear();selectedFeatures.clear();
    regionDrag.cancel();regionSelected=-1;
    units.clear(); scen = tak::crt::Scenario{}; useOnly.clear(); aiFiles.clear();
    resetHistory=!regeneratingCurrent;regeneratingCurrent=false;
    selected = sections.list().empty() ? -1 : 0;
    selectedFeat = features.list().empty() ? -1 : 0;
    paletteScroll = 0;
    edited = true;
    SDL_SetWindowTitle(win, ("Cartographer -- " + mapName).c_str());
    dirty = true; historyPending=true;noDocument=false;
    modal = M_NONE; SDL_StopTextInput();
}

void cart::Editor::applyNewMap(bool random) {
    const auto* fields=random?generatorDraft.data():mf;
    std::string nm=fields[0].empty()?"Untitled":fields[0],wld=fields[3];
    if(!cart::validDocumentName(nm)) {modalError="Choose a map name without path separators or reserved filename characters.";return;}
    int wu=std::clamp(std::atoi(fields[1].c_str()),1,64),hu=std::clamp(std::atoi(fields[2].c_str()),1,64);
    std::transform(wld.begin(),wld.end(),wld.begin(),[](unsigned char c){return char(std::tolower(c));});
    if(random) {
        auto params=generatorParams;params.widthCells=uint16_t(wu*32);params.heightCells=uint16_t(hu*32);
        for(uint8_t i=0;i<tak::mapgen::kMapTypes;++i)if(wld==tak::mapgen::worldKey(i))params.mapType=i;
        params=tak::mapgen::sanitize(params);
        wld=tak::mapgen::worldKey(params.mapType);
        generatorDraft[3]=wld;
        generatorParams=params;
        generatingName=nm;generatingWorld=wld;cancelGeneration=quitAfterGeneration=false;
        // The busy modal prevents VFS replacement until this read-only job ends.
        generatorJob=std::async(std::launch::async,[&,params,wld] {
            const auto recipe=tak::mapgen::encodeMapId(params);
            auto result=tak::mapgen::generate(params,vfs);FreshMap map;
            map.map=std::move(result.map);map.recipe=recipe;
            tak::terrain::Compositor compositor(vfs);
            cart::generateMinimaps(map.map,compositor,cart::loadWorldPalette(vfs,wld));
            int number=1;for(auto [x,z]:result.starts)map.starts.push_back({number++,x,z});
            if(params.mountainDensity&&tak::mapgen::kitAvailable(params.mapType,tak::mapgen::KitMountains)&&!result.mountains)
                map.note="No room for mountains on this map: try it larger or with fewer players.";
            return map;
        });
        modal=M_GENERATING;SDL_StopTextInput();return;
    }
    mapView.quiesce();
    try {
        cart::SectionLibrary freshSections;freshSections.scan(vfs,wld);
        auto map=buildFreshMap(vfs,freshSections,mapView.compositor(),wld,wu,hu,false);
        if(!map.map.width) {openMessage("NEW MAP","Could not build terrain for this world.");return;}
        adoptFreshMap(std::move(map),nm,wld);
    } catch(const std::exception& error) {openMessage("NEW MAP FAILED",error.what());}
}

void cart::Editor::openGeneratorRecipe() {
    std::string error;auto params=generatorParams;
    if(!cart::restoreGeneratorRecipe(scenario.generatorRecipe,params,error)) {openMessage("GENERATOR RECIPE",error);return;}
    generatorParams=params;
    regeneratingCurrent=true;
    generatorDraft={mapName,std::to_string(params.widthCells/32),std::to_string(params.heightCells/32),tak::mapgen::worldKey(params.mapType)};
    openModal(M_GENERATOR);
}

void cart::Editor::applyModal() {
    const auto applying=modal;modalError.clear();
    if(modal==M_GENERATOR) {
        cart::GeneratorFields fields;for(int i=0;i<8;++i)fields[i]=mf[i];fields[8]=mf[11];
        for(int i=9;i<12;++i)fields[size_t(i)]=mf[i+3];
        std::string error;
        auto nextParams=generatorParams;
        if(!cart::parseGeneratorFields(fields,nextParams,error)) {modalError=error;return;}
        for(int i=8;i<10;++i) {
            int units=0;const auto parsed=std::from_chars(mf[i].data(),mf[i].data()+mf[i].size(),units);
            if(parsed.ec!=std::errc{} || parsed.ptr!=mf[i].data()+mf[i].size() || units<1 || units>64) {modalError="Width and height must be whole numbers from 1 to 64. The preview shows the generator's adjusted size.";return;}
        }
        generatorParams=nextParams;
        generatorDraft[1]=mf[8];generatorDraft[2]=mf[9];generatorDraft[3]=mf[10];
        applyNewMap(true);return;
    } else if(modal==M_REGION) {
        int cells[4];
        for(int i=0;i<4;++i) {
            const auto& field=mf[i+1];const auto parsed=std::from_chars(field.data(),field.data()+field.size(),cells[i]);
            if(parsed.ec!=std::errc{} || parsed.ptr!=field.data()+field.size()) {modalError="Corners must be whole cell numbers.";return;}
        }
        std::string error;
        if(!cart::setRegion(scen,editRegion,{mf[0],cells[0],cells[1],cells[2],cells[3]},mapView.map().width,mapView.map().height,error)) {modalError=error;return;}
        regionSelected=editRegion<0?int(scen.regions.size())-1:editRegion;
        dirty=true;historyPending=true;
    } else if(modal==M_SAVEAS) {
        if(!cart::validDocumentName(mf[0])) {modalError="Choose a map name without path separators or reserved filename characters.";return;}
        const auto nextName=mf[0],nextDir=mf[1];
        const auto path=pathText(tak::u8path(nextDir)/tak::u8path(nextName+".kmp"));
        auto performSave=[&,path,nextName,nextDir] {
            if(!saveBundle(path)) {openMessage("SAVE FAILED",saveError);return;}
            mapName=nextName;outDir=nextDir;historyPending=true;commitHistory();history.markSaved();dirty=false;
            openMessage("SAVED",path);
        };
        if(std::filesystem::exists(tak::u8path(path)) && (nextName!=mapName || nextDir!=outDir)) {
            openConfirm("REPLACE MAP", "Replace the existing map? Its previous version will be retained as a .bak file.",performSave);return;
        }
        performSave();return;
    } else if(modal==M_OPENPATH && openForTest) {
        // TEST IN GAME: one of the install's maps, picked from the list or typed in full.
        std::string request=mf[0];
        while(!request.empty() && request.back()==' ')request.pop_back();
        std::string path;
        for(const auto& [name,mapPath]:openCatalog)if(mapPath==request || lowerText(name)==lowerText(request))path=mapPath;
        if(path.empty() && openMatches.size()==1)path=openMatches.front().second;
        if(path.empty()) {modalError="Pick one of the install's maps from the list.";return;}
        modal=M_NONE;openForTest=false;
        aiLaunchTest(path);
        return;
    } else if(modal==M_NEWAIMOD) {
        std::string name=mf[0];
        while(!name.empty() && name.back()==' ')name.pop_back();
        while(!name.empty() && name.front()==' ')name.erase(name.begin());
        if(const auto why=cart::modPackNameError(name);!why.empty()) {modalError=why;return;}
        if(const auto why=cart::modPackReservedError(tak::u8path(dataRoot),name);!why.empty()) {modalError=why;return;}
        std::vector<std::string> packs;
        try {packs=tak::hpi::modPacks(tak::u8path(dataRoot),tak::hpi::ModKind::Game);} catch(const std::exception&) {}
        for(const auto& pack:packs)if(lowerText(pack)==lowerText(name)) {
            modalError="mods/game/"+pack+" already exists: File > Open AI mod edits its AI files.";return;
        }
        modal=M_NONE;
        startAiModWorkspace(name,true);
        return;
    } else if(modal==M_OPENPATH) {
        const auto request=mf[0];
        if(dirty) {openConfirm("OPEN MAP","Discard unsaved changes and open this map?",[&,request] {openDocument(request);});return;}
        if(!openDocument(request))return;
    } else if (modal == M_SCENARIO) {
        scenario.missionName = mf[0];
        scenario.missionDescription = mf[1]; dirty = true; historyPending=true;
    } else if (modal == M_RESIZE) {
        int wu=0,hu=0;
        const auto widthResult=std::from_chars(mf[0].data(),mf[0].data()+mf[0].size(),wu);
        const auto heightResult=std::from_chars(mf[1].data(),mf[1].data()+mf[1].size(),hu);
        if(widthResult.ec!=std::errc{} || widthResult.ptr!=mf[0].data()+mf[0].size() ||
           heightResult.ec!=std::errc{} || heightResult.ptr!=mf[1].data()+mf[1].size() || wu<1 || wu>64 || hu<1 || hu>64) {
            modalError="Width and height must be whole numbers from 1 to 64.";return;
        }
        for(const auto& start:scenario.starts) if(start.xpos>=wu*32 || start.zpos>=hu*32) {
            modalError="Move or remove start positions outside the new map first.";return;
        }
        for(const auto& unit:units) if(unit.x>=wu*512 || unit.z>=hu*512) {
            modalError="Move or remove units outside the new map first.";return;
        }
        for(const auto& region:scen.regions) if(std::max(region.x1,region.x2)>=wu*32 || std::max(region.z1,region.z2)>=hu*32) {
            modalError="Resize or remove regions outside the new map first.";return;
        }
        if(wu*32==mapView.map().width && hu*32==mapView.map().height) {modal=M_NONE;SDL_StopTextInput();return;}
        auto performResize=[&,wu,hu] {
            try {
                mapView.quiesce();
                auto resized=mapView.map();
                const auto wld=scenario.kingdom.empty()?"aramon":scenario.kingdom;
                cart::resizeMap(resized,mapView.compositor(),cart::loadWorldPalette(vfs,wld),wu,hu);
                mapView.editMap()=std::move(resized);minimapSource=nullptr;
                selectedFeatures.clear();invalidateMinimap();scenario.sizeW=wu;scenario.sizeH=hu;
                mapView.tilesEdited();edited=dirty=historyPending=true;
                modal=M_NONE;SDL_StopTextInput();
            } catch(const std::exception& error) {openMessage("RESIZE FAILED",error.what());}
        };
        if(wu*32<mapView.map().width || hu*32<mapView.map().height) {
            openConfirm("CROP MAP","Terrain and features beyond the new right or bottom edge will be removed. Undo can restore them. Continue?",performResize);return;
        }
        performResize();return;
    } else if (modal == M_NEW) {
        applyNewMap(false);   // CREATE = flat stamp; closes on success
        return;               // (RANDOM is handled at its button click)
    } else if (modal == M_UNIT && editUnit >= 0 && editUnit < int(units.size())) {
        const auto& origin=units[size_t(editUnit)];
        auto targets=selectedUnits.indices;
        if(!targets.count(editUnit))targets={editUnit};
        std::array<int,9> values{};
        const std::array<int,9> minimum={0,0,0,0,0,std::numeric_limits<int>::min(),0,0,0};
        const std::array<int,9> maximum={8,100,1000,1000,9,std::numeric_limits<int>::max(),0,mapView.map().width-1,mapView.map().height-1};
        for(int i=0;i<9;++i)if(i!=6 && mf[i]!=mfOriginal[i]) {
            const auto parsed=std::from_chars(mf[i].data(),mf[i].data()+mf[i].size(),values[i]);
            if(parsed.ec!=std::errc{} || parsed.ptr!=mf[i].data()+mf[i].size() || values[i]<minimum[i] || values[i]>maximum[i]) {
                modalError=i==5?"ANGLE: enter whole degrees, for example 90 or -90.":
                    std::string(mLabel[i])+": enter a whole number from "+std::to_string(minimum[i])+" to "+std::to_string(maximum[i])+".";
                mfocus=i;fieldEditor.focus(mf[i]);return;
            }
        }
        const float dx=mf[7]==mfOriginal[7]?0:values[7]*16.0f+origin.footX*8-origin.x;
        const float dz=mf[8]==mfOriginal[8]?0:values[8]*16.0f+origin.footZ*8-origin.z;
        if(targets.size()==1 && mf[6]!=origin.name) {
            if(mf[6].size()>255) {modalError="Use a name shorter than 256 bytes.";return;}
        }
        for(int i:targets)if(i>=0 && i<int(units.size()) && (units[i].x+dx<0 || units[i].z+dz<0 || units[i].x+dx>=mapView.map().width*16 || units[i].z+dz>=mapView.map().height*16)) {
            modalError="The selection would extend outside the map.";return;
        }
        for(int i:targets)if(i>=0 && i<int(units.size())) {
            auto& u=units[size_t(i)];
            if(mf[0]!=mfOriginal[0])u.player=values[0];
            if(mf[1]!=mfOriginal[1])u.health=values[1];
            if(mf[2]!=mfOriginal[2])u.armor=values[2];
            if(mf[3]!=mfOriginal[3])u.weapon=values[3];
            if(mf[4]!=mfOriginal[4])u.veteran=values[4];
            if(mf[5]!=mfOriginal[5]) {int angle=values[5]%360;u.angle=float(angle<0?angle+360:angle);}
            if(targets.size()==1)u.name=mf[6];
            u.x+=dx;u.z+=dz;
        }
        unitsEdited=true;dirty=true;historyPending=true;
    } else if(modal==M_RULENAME && editRuleGroup) {
        if(mf[0].size()>255 || mf[0].find('\0')!=std::string::npos) {modalError="Use a rule name of at most 255 UTF-8 bytes.";return;}
        if(editRuleGroup->editorName!=mf[0]) {editRuleGroup->editorName=mf[0];dirty=historyPending=true;}
        editRuleGroup=nullptr;
    } else if (modal == M_RULE && editRule) {
        auto candidate=*editRule;
        for(int i=0;i<mN;++i)candidate.slot[i]=mf[i];
        for(int i=mN;i<5;++i)candidate.slot[i].clear();
        for(const auto& problem:cart::validateRuleOperands(editRuleAction,candidate,scen,unitRegistry))
            if(problem.severity==cart::MapIssue::Severity::Error) {modalError=problem.message;return;}
        bool changed=false;
        for(int i=0;i<5;++i)changed|=candidate.slot[i]!=editRule->slot[i];
        if(changed) {*editRule=std::move(candidate);dirty=true;historyPending=true;}
        editRule = nullptr;
    } else if (modal == M_CONFIRM) {
        // Taken out first: an action may ask its own question (a new confirmAction).
        auto action=std::move(confirmAction);confirmAction=nullptr;
        if(action)action();
    } else if (modal == M_SYMMETRIZE) {
        applySymmetrize();   // opens its confirmation, or reports why not
        if(!modalError.empty())return;
    } else if(modal==M_PREFERENCES) {
        preferences.fullscreen=mf[0]==kDisplayOpts[1];preferences.edgeScroll=mf[1]==kOnOffOpts[1];
        preferences.scalePercent=std::clamp(std::atoi(mf[2].c_str()),50,200);
        persistPreferences();   // before a fullscreen switch, so the windowed size is kept
        modal=M_NONE;SDL_StopTextInput();applyDisplayMode();return;
    }
    if(modal!=applying)return;
    modal = M_NONE; SDL_StopTextInput();
}

// Open the param editor for a condition/action rule (fields = its opcode's
// parameters, in slot order).
void cart::Editor::openRuleEditor(tak::crt::Rule* r, bool isAction) {
    modalError.clear();
    const auto& defs = isAction ? cart::actionDefs() : cart::conditionDefs();
    int op = std::clamp(r->opcode, 0, int(defs.size()) - 1);
    const auto& params = defs[size_t(op)].params;
    editRule = r;editRuleAction=isAction; mfocus = 0; mDropOpen = -1;
    mN = std::min(int(params.size()), kMaxFields);
    mTitle = (isAction ? "ACTION: " : "CONDITION: ") + cart::formatRule(isAction, *r);
    for (int i = 0; i < mN; ++i) {
        mLabel[i] = cart::paramLabel(params[size_t(i)]);
        mf[i] = r->slot[size_t(i)];
        const auto kind=params[size_t(i)];
        mfNumeric[i] = kind==cart::PKind::Value;
        mfChoices[i] = nullptr;mfEditableChoice[i]=kind==cart::PKind::Flag;
        auto& choices=ruleChoices[i];choices.clear();
        if(kind==cart::PKind::Flag && scrPlayer<int(scen.players.size()))choices=cart::ruleFlags(scen.players[scrPlayer]);
        if(kind==cart::PKind::Location) {choices.push_back("Anywhere");for(const auto& region:scen.regions)choices.push_back(region.name);}
        if(kind==cart::PKind::UnitType) {choices.push_back("Any Unit");choices.insert(choices.end(),unitTypes.begin(),unitTypes.end());}
        if(kind==cart::PKind::Player) {choices.push_back("All Players");for(int p=1;p<=8;++p)choices.push_back("Player "+std::to_string(p));}
        if(!choices.empty()) {
            if(std::find(choices.begin(),choices.end(),mf[i])==choices.end())choices.insert(choices.begin(),mf[i]);
            mfChoices[i]=&choices;
        }
    }
    modal = M_RULE;fieldEditor.focus(mf[0]);
    if (mN > 0) SDL_StartTextInput();
}

void cart::Editor::openRuleName(tak::crt::RuleGroup* group) {
    if(!group)return;
    editRuleGroup=group;modal=M_RULENAME;modalError.clear();mTitle="RULE NAME";mN=1;mfocus=0;mDropOpen=-1;
    mf[0]=group->editorName;mLabel[0]="NAME (BLANK USES RULE NUMBER)";mfNumeric[0]=false;mfChoices[0]=nullptr;mfEditableChoice[0]=false;
    fieldEditor.focus(mf[0]);SDL_StartTextInput();
}

void cart::Editor::filterObjects() {
    objectMatches.clear();const auto query=lowerText(objectSearch);
    for(size_t i=0;i<units.size();++i) {
        const auto& unit=units[i];const auto* type=unitRegistry.find(lowerText(unit.type));
        if(lowerText(unit.name+" "+unit.type+" "+(type?type->name:std::string{})+" "+objectOwner(unit.player)).find(query)!=std::string::npos)objectMatches.push_back(int(i));
    }
    objectScroll=objectRow=0;
}

void cart::Editor::openObjects() {objectSearch.clear();objectEditor.focus(objectSearch);filterObjects();modal=M_OBJECTS;SDL_StartTextInput();}

void cart::Editor::chooseObject(int row,bool edit) {
    if(row<0 || row>=int(objectMatches.size()))return;
    const int index=objectMatches[row];const auto& unit=units[index];
    selectedUnits.indices={index};selectedUnits.dragOrigins.clear();tool=UNITS;regionCanvas=false;editMode=MODE_SELECT;revealTool();
    int w,h;SDL_GetRendererOutputSize(ren,&w,&h);
    if(std::isfinite(unit.x) && std::isfinite(unit.z)) {
        const float x=std::clamp(unit.x,0.f,mapView.map().width*16.f),z=std::clamp(unit.z,0.f,mapView.map().height*16.f);
        mapView.setOffset(x-(w/renderScale()-kPaletteW)/(2*mapView.zoom()),z-(h/renderScale()-kMenuH-kStatusH)/(2*mapView.zoom()));
    }
    modal=M_NONE;SDL_StopTextInput();if(edit)openModal(M_UNIT,index);
}

void cart::Editor::previewModel() {
    std::string type;
    if(selectedUnits.indices.size()==1) {
        const int index=*selectedUnits.indices.begin();
        if(index>=0 && index<int(units.size()))type=lowerText(units[index].type);
    }
    if(type.empty() && selectedType>=0 && selectedType<int(unitTypes.size()))type=lowerText(unitTypes[selectedType]);
    if(type.empty()) {openMessage("UNIT MODEL","Choose a unit in the Units browser first.");return;}
    const auto* definition=unitRegistry.find(type);modelTitle=definition?definition->name:type;
    modelPreview.reset();modelWarning.clear();modelCancel=std::make_shared<std::atomic_bool>(false);quitAfterModel=false;
    modelJob=std::async(std::launch::async,[&vfs=vfs,type,cancel=modelCancel] {return ModelView::loadAsset(vfs,type,cancel.get());});
    modal=M_MODEL_LOADING;SDL_StopTextInput();
}

void cart::Editor::buildOverlay(cart::OverlayKind kind) {
    const auto type=selectedType>=0 && selectedType<int(unitTypes.size())?lowerText(unitTypes[selectedType]):std::string{};
    auto snapshot=mapView.map();
    discardOverlay=quitAfterOverlay=false;
    overlayCancel=std::make_shared<std::atomic_bool>(false);
    overlayJob=std::async(std::launch::async,[&,snapshot=std::move(snapshot),occupants=units,type,kind,cancel=overlayCancel] {
        return cart::terrainOverlay(snapshot,unitRegistry,vfs,kind,type,occupants,cancel.get());
    });
    modal=M_ANALYZING;SDL_StopTextInput();
}

void cart::Editor::showMapIssue() {
    if(mapIssues.empty()) {modal=M_ISSUES;return;}
    const auto& issue=mapIssues[issueIndex];
    if(issue.x>=0 && issue.z>=0) {
        int w,h;SDL_GetRendererOutputSize(ren,&w,&h);
        mapView.setOffset(issue.x-(w/renderScale()-kPaletteW)/(2*mapView.zoom()),issue.z-(h/renderScale()-kMenuH-kStatusH)/(2*mapView.zoom()));
    }
    openMessage("MAP ISSUE "+std::to_string(issueIndex+1)+" / "+std::to_string(mapIssues.size()),
        std::string(issue.severity==cart::MapIssue::Severity::Error?"ERROR: ":"WARNING: ")+issue.message+". Scenario > Next issue moves to the next result. Recheck after edits.");
}

void cart::Editor::checkMap() {
    commitHistory();checkRevision=history.revision();discardCheck=quitAfterCheck=false;
    checkJob=std::async(std::launch::async,[&,map=mapView.map(),metadata=scenario,scenario=scen,placed=units,restrictions=useOnly,profiles=aiFiles] {
        auto issues=cart::validateMap(map,metadata,scenario,placed,restrictions,unitRegistry,vfs);
        for(auto& issue:cart::validateCrusadesRoster(scenario,placed,unitRegistry,crusadesRegistry))issues.push_back(std::move(issue));
        for(auto& issue:cart::validateAiProfiles(profiles,metadata.aiProfile,unitRegistry,vfs))issues.push_back(std::move(issue));
        return issues;
    });
    modal=M_CHECKING;SDL_StopTextInput();
}

void cart::Editor::pollJobs() {
    if(generatorJob.valid() && generatorJob.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        try {
            auto result=generatorJob.get();
            if(cancelGeneration)modal=M_NONE;
            else if(!result.map.width)openMessage("GENERATION FAILED","The generator returned no terrain.");
            else {
                const auto palette=cart::loadWorldPalette(vfs,generatingWorld);
                std::vector<uint8_t> rgba(result.map.minimap.size()*4);
                for(size_t i=0;i<result.map.minimap.size();++i)std::copy(palette.rgba[result.map.minimap[i]],palette.rgba[result.map.minimap[i]]+4,rgba.begin()+i*4);
                generatedTexture.reset(SDL_CreateTexture(ren,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,result.map.minimapW,result.map.minimapH));
                if(!generatedTexture)throw std::runtime_error(SDL_GetError());
                SDL_UpdateTexture(generatedTexture.get(),nullptr,rgba.data(),result.map.minimapW*4);
                SDL_SetTextureScaleMode(generatedTexture.get(),SDL_ScaleModeLinear);
                generatedPreview=std::move(result);modal=M_GENERATED;
            }
        } catch(const std::exception& error) {openMessage("GENERATION FAILED",error.what());}
        if(quitAfterGeneration) {SDL_Event quit{};quit.type=SDL_QUIT;SDL_PushEvent(&quit);}
    }
    if(modelJob.valid() && modelJob.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        try {
            auto asset=modelJob.get();modal=M_NONE;
            if(!modelCancel->load()) {
                if(!asset.missingTextures.empty()) {
                    modelWarning="Missing textures (magenta): ";
                    for(const auto& name:asset.missingTextures) {if(modelWarning.size()>65) {modelWarning+="...";break;}modelWarning+=name+" ";}
                }
                modelPreview=std::make_unique<ModelView>(ren,std::move(asset));modelTime=SDL_GetTicks64();modal=M_MODEL;
            }
        } catch(const std::exception& error) {
            modal=M_NONE;if(!modelCancel->load())openMessage("MODEL PREVIEW FAILED",error.what());
        }
        if(quitAfterModel) {SDL_Event quit{};quit.type=SDL_QUIT;SDL_PushEvent(&quit);}
    }
    if(overlayJob.valid() && overlayJob.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        try {
            auto result=overlayJob.get();modal=M_NONE;
            if(!discardOverlay) {
                overlayTexture.reset(SDL_CreateTexture(ren,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,result.width,result.height));
                if(!overlayTexture)throw std::runtime_error(SDL_GetError());
                SDL_UpdateTexture(overlayTexture.get(),nullptr,result.rgba.data(),result.width*4);
                SDL_SetTextureBlendMode(overlayTexture.get(),SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(overlayTexture.get(),SDL_ScaleModeNearest);
                overlayWidth=result.width;overlayHeight=result.height;overlayLegend=std::move(result.legend);
            }
        } catch(const std::exception& error) {openMessage("OVERLAY FAILED",error.what());}
        if(quitAfterOverlay) {SDL_Event quit{};quit.type=SDL_QUIT;SDL_PushEvent(&quit);}
    }
    if(checkJob.valid() && checkJob.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        try {
            auto result=checkJob.get();modal=M_NONE;
            if(!discardCheck) {mapIssues=std::move(result);mapIssueRevision=checkRevision;issueIndex=0;issueScroll=0;modal=M_ISSUES;}
        } catch(const std::exception& error) {openMessage("CHECK FAILED",error.what());}
        if(quitAfterCheck) {SDL_Event quit{};quit.type=SDL_QUIT;SDL_PushEvent(&quit);}
    }
}

// While a background job's busy dialog is up, input only cancels it.
bool cart::Editor::swallowWhileBusy(const SDL_Event& e) {
    if(modelJob.valid()) {
        if(e.type==SDL_QUIT) {modelCancel->store(true);quitAfterModel=true;}
        if((e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_ESCAPE) ||
           (e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT && cart::pointIn(int(e.button.x/kUIScale),int(e.button.y/kUIScale),modelClose)))modelCancel->store(true);
        return true;
    }
    if(checkJob.valid()) {
        if(e.type==SDL_QUIT) {discardCheck=true;quitAfterCheck=true;}
        if(e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_ESCAPE)discardCheck=true;
        return true;
    }
    if(overlayJob.valid()) {
        if(e.type==SDL_QUIT) {discardOverlay=true;quitAfterOverlay=true;}
        if(e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_ESCAPE)discardOverlay=true;
        if(discardOverlay)overlayCancel->store(true);
        return true;
    }
    if(generatorJob.valid()) {
        if(e.type==SDL_QUIT) {cancelGeneration=true;quitAfterGeneration=true;}
        if(e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_ESCAPE)cancelGeneration=true;
        return true;
    }
    return false;
}

bool cart::Editor::handleGeneratedPreview(SDL_Event& e) {
    if(modal==M_GENERATED && generatedPreview) {
        if(e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT && cart::pointIn(e.button.x,e.button.y,mRegenerate)) {
            generatedPreview.reset();generatedTexture.reset();openModal(M_GENERATOR);return true;
        }
        const bool accept=(e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_RETURN) ||
            (e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT && cart::pointIn(e.button.x,e.button.y,mOK));
        const bool cancel=e.type==SDL_QUIT || (e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_ESCAPE) ||
            (e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT && cart::pointIn(e.button.x,e.button.y,mCancel));
        if(accept) {adoptFreshMap(std::move(*generatedPreview),generatingName,generatingWorld);generatedPreview.reset();generatedTexture.reset();}
        if(cancel) {generatedPreview.reset();generatedTexture.reset();modal=M_NONE;}
        if(e.type!=SDL_QUIT)return true;
    }
    return false;
}

void cart::Editor::handleRegionsList(SDL_Event& e) {
    auto focusRegion=[&]() {
        if(regionSelected<0 || regionSelected>=int(scen.regions.size()))return;
        const auto& r=scen.regions[regionSelected];
        mapView.setOffset((r.x1+r.x2+1)*8.f-canvasW/(2*mapView.zoom()),(r.z1+r.z2+1)*8.f-canvasH/(2*mapView.zoom()));
    };
    if(e.type==SDL_KEYDOWN) {
        if(e.key.keysym.sym==SDLK_ESCAPE)regionsOpen=false;
        else if(e.key.keysym.sym==SDLK_n && (e.key.keysym.mod&(KMOD_CTRL|KMOD_GUI)))openModal(M_REGION);
        else if(e.key.keysym.sym==SDLK_RETURN && regionSelected>=0)openModal(M_REGION,regionSelected);
    } else if(e.type==SDL_MOUSEWHEEL)regionScroll=std::clamp(regionScroll-e.wheel.y*36,0,std::max(0,int(scen.regions.size())*20-regionList.h));
    else if(e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT) {
        int x=e.button.x,y=e.button.y;
        if(cart::pointIn(x,y,regionDone))regionsOpen=false;
        else if(cart::pointIn(x,y,regionNew))openModal(M_REGION);
        else if(cart::pointIn(x,y,regionEdit) && regionSelected>=0)openModal(M_REGION,regionSelected);
        else if(cart::pointIn(x,y,regionOnMap)) {regionsOpen=false;regionCanvas=showRegions=true;editMode=MODE_SELECT;}
        else if(cart::pointIn(x,y,regionDelete)) {
            std::string error;
            if(cart::removeRegion(scen,regionSelected,error)) {regionSelected=std::min(regionSelected,int(scen.regions.size())-1);dirty=true;historyPending=true;}
            else openMessage("REGION NOT DELETED",error);
        } else if(cart::pointIn(x,y,regionList)) {
            int index=(y-regionList.y+regionScroll)/20;
            if(index<int(scen.regions.size())) {regionSelected=index;focusRegion();if(e.button.clicks>=2)openModal(M_REGION,index);}
        }
    }
}

// The Use Only checklist overlay swallows input while up.
void cart::Editor::handleUseOnly(SDL_Event& e) {
    if (e.type == SDL_MOUSEWHEEL) {
        int maxS = std::max(0, int(unitTypes.size()) * 14 - uoList.h);
        useOnlyScroll = std::clamp(useOnlyScroll - e.wheel.y * 40, 0, maxS);
    } else if (e.type == SDL_KEYDOWN &&
               (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_RETURN)) {
        useOnlyOpen = false;
    } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        int mx = e.button.x, my = e.button.y;
        if (cart::pointIn(mx, my, uoDone)) useOnlyOpen = false;
        else if (cart::pointIn(mx, my, uoClear)) { useOnly.clear(); dirty = true; historyPending=true; }
        else if (cart::pointIn(mx, my, uoList)) {
            int row = (my - uoList.y + useOnlyScroll) / 14;
            if (row >= 0 && row < int(unitTypes.size())) {
                const std::string& t = unitTypes[size_t(row)];
                if (useOnly.count(t)) useOnly.erase(t); else useOnly.insert(t); dirty = true; historyPending=true;
            }
        }
    }
}

// A modal dialog swallows all input while up.
void cart::Editor::handleModal(SDL_Event& e) {
    if(modal==M_OBJECTS) {
        if(e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_ESCAPE) {modal=M_NONE;SDL_StopTextInput();}
        else if(e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_RETURN)chooseObject(objectRow,(e.key.keysym.mod&KMOD_SHIFT)!=0);
        else if(e.type==SDL_KEYDOWN && (e.key.keysym.sym==SDLK_DOWN || e.key.keysym.sym==SDLK_UP) && !objectMatches.empty()) {
            objectRow=std::clamp(objectRow+(e.key.keysym.sym==SDLK_DOWN?1:-1),0,int(objectMatches.size())-1);
            if(objectRow*30<objectScroll)objectScroll=objectRow*30;
            if((objectRow+1)*30>objectScroll+objectList.h)objectScroll=(objectRow+1)*30-objectList.h;
        } else if(e.type==SDL_MOUSEWHEEL)objectScroll=std::clamp(objectScroll-e.wheel.y*60,0,std::max(0,int(objectMatches.size())*30-objectList.h));
        else if(e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT) {
            if(cart::pointIn(e.button.x,e.button.y,objectClose)) {modal=M_NONE;SDL_StopTextInput();}
            else if(cart::pointIn(e.button.x,e.button.y,objectList))chooseObject((e.button.y-objectList.y+objectScroll)/30,false);
            else if(cart::pointIn(e.button.x,e.button.y,objectSearchBox))objectEditor.click(objectSearch,0,0,(e.button.x-objectSearchBox.x-4)/6);
        } else if(objectEditor.input(e,objectSearch))filterObjects();
        return;
    }
    if(modal==M_MODEL) {
        const bool close=e.type==SDL_QUIT || (e.type==SDL_KEYDOWN && (e.key.keysym.sym==SDLK_ESCAPE || e.key.keysym.sym==SDLK_RETURN)) ||
            (e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT && cart::pointIn(e.button.x,e.button.y,modelClose));
        if(close) {modelPreview.reset();modal=M_NONE;if(e.type==SDL_QUIT)SDL_PushEvent(&e);}
        else if(e.type==SDL_MOUSEWHEEL || (e.type==SDL_MOUSEMOTION && cart::pointIn(e.motion.x,e.motion.y,modelArea)))modelPreview->input(e);
        return;
    }
    if(modal==M_ISSUES) {
        if(e.type==SDL_KEYDOWN && (e.key.keysym.sym==SDLK_ESCAPE || e.key.keysym.sym==SDLK_RETURN))modal=M_NONE;
        else if(e.type==SDL_MOUSEWHEEL)issueScroll=std::clamp(issueScroll-e.wheel.y*36,0,std::max(0,issueContentHeight-issueList.h));
        else if(e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT) {
            if(cart::pointIn(e.button.x,e.button.y,issueClose))modal=M_NONE;
            else if(cart::pointIn(e.button.x,e.button.y,issueRecheck))checkMap();
            else if(cart::pointIn(e.button.x,e.button.y,issueList))for(size_t i=0;i<issueRows.size();++i)if(cart::pointIn(e.button.x,e.button.y,issueRows[i])) {
                issueIndex=i;const auto& issue=mapIssues[i];
                if(issue.x>=0 && issue.z>=0) {
                    mapView.setOffset(issue.x-canvasW/(2*mapView.zoom()),issue.z-canvasH/(2*mapView.zoom()));modal=M_NONE;
                }
                break;
            }
        }
        return;
    }
    // Save-before-exit prompt: SAVE / DON'T SAVE / CANCEL.
    if (modal == M_QUITSAVE) {
        auto saveThenQuit = [&]() {
            // Only exit if the save actually succeeded -- otherwise
            // keep the editor alive and say so, so a failed write
            // (read-only dir, disk full) can't silently lose work.
            if (saveBundle(outDir + "/" + mapName + ".kmp")) {
                dirty = false; running = false; modal = M_NONE;
            } else {
                restartAfterQuit = false;   // a pending RESTART NOW is off too: the work is still unsaved
                openMessage("SAVE FAILED",
                            saveError);
            }
        };
        if (e.type == SDL_KEYDOWN) {
            SDL_Keycode k = e.key.keysym.sym;
            if (k == SDLK_ESCAPE) {modal = M_NONE;restartAfterQuit=false;}   // cancel
            else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) saveThenQuit();
        } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
            int mx = e.button.x, my = e.button.y;
            if (cart::pointIn(mx, my, mOK)) saveThenQuit();
            else if (cart::pointIn(mx, my, mQuit)) { running = false; modal = M_NONE; }
            else if (cart::pointIn(mx, my, mCancel)) {modal = M_NONE;restartAfterQuit=false;}
        }
        return;
    }
    // The folder dialog is up (its own window): leave Preferences exactly as it is.
    if(modal==M_PREFERENCES && folderPick.busy() && (e.type==SDL_KEYDOWN || e.type==SDL_TEXTINPUT || e.type==SDL_MOUSEBUTTONDOWN))return;
    if(modal==M_SCENARIO && e.type==SDL_MOUSEWHEEL) {descriptionScroll=std::max(0,descriptionScroll-e.wheel.y*3);descriptionFollowCaret=false;return;}
    if(e.type==SDL_TEXTINPUT || e.type==SDL_KEYDOWN)descriptionFollowCaret=true;
    if(modal==M_OPENPATH && e.type==SDL_MOUSEWHEEL) {openScroll=std::clamp(openScroll-e.wheel.y*40,0,std::max(0,int(openMatches.size())*20-openList.h));return;}
    if(modal==M_OPENPATH && e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT && cart::pointIn(e.button.x,e.button.y,openList)) {
        const int row=(e.button.y-openList.y+openScroll)/20;
        if(row>=0 && row<int(openMatches.size())) {mf[0]=openMatches[row].second;applyModal();}
        return;
    }
    if(e.type==SDL_MOUSEWHEEL && mDropOpen>=0 && mfChoices[mDropOpen]) {
        mDropScroll=std::clamp(mDropScroll-e.wheel.y*3,0,std::max(0,int(mfChoices[mDropOpen]->size())-8));return;
    }
    if (e.type == SDL_TEXTINPUT && mN > 0 && (!mfChoices[mfocus] || mfEditableChoice[mfocus])) {
        mDropOpen=-1;
        fieldEditor.input(e,mf[mfocus],mfNumeric[mfocus],modal==M_SCENARIO && mfocus==1,std::max(1,(mBox[mfocus].w-8)/6));
    } else if (e.type == SDL_KEYDOWN) {
        SDL_Keycode k = e.key.keysym.sym;
        if (k == SDLK_TAB) {
            mfocus = (mfocus + ((e.key.keysym.mod&KMOD_SHIFT)?std::max(1,mN)-1:1)) % std::max(1, mN);
            mDropOpen = -1;fieldEditor.focus(mf[mfocus]);
        }
        else if(k==SDLK_DOWN && (e.key.keysym.mod&KMOD_ALT) && mN>0 && mfChoices[mfocus]) {mDropOpen=mfocus;mDropScroll=0;}
        else if ((k == SDLK_LEFT || k == SDLK_RIGHT) && mN > 0 && mfChoices[mfocus] && !mfEditableChoice[mfocus]) {
            // Cycle a focused dropdown field with the arrow keys.
            const auto& opts = *mfChoices[mfocus];
            int cur = 0;
            for (int j = 0; j < int(opts.size()); ++j)
                if (opts[size_t(j)] == mf[mfocus]) { cur = j; break; }
            cur = (cur + (k == SDLK_RIGHT ? 1 : int(opts.size()) - 1)) % int(opts.size());
            mf[mfocus] = opts[size_t(cur)];
        }
        else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && !(modal==M_SCENARIO && mfocus==1 && (e.key.keysym.mod&KMOD_SHIFT))) applyModal();
        else if (k == SDLK_ESCAPE) {
            if (mDropOpen >= 0) mDropOpen = -1;   // first Esc closes an open list
            else { modal = M_NONE; SDL_StopTextInput(); }
        } else if(mN>0 && (!mfChoices[mfocus] || mfEditableChoice[mfocus]))fieldEditor.input(e,mf[mfocus],mfNumeric[mfocus],modal==M_SCENARIO && mfocus==1,std::max(1,(mBox[mfocus].w-8)/6));
    } else if (e.type == SDL_MOUSEBUTTONDOWN &&
               e.button.button == SDL_BUTTON_LEFT) {
        int mx = e.button.x, my = e.button.y;
        // An open dropdown list eats the next click: a row selects, any
        // click closes it (so a stray click just dismisses the list).
        if (mDropOpen >= 0) {
            for (size_t i = 0; i < mDropRects.size(); ++i)
                if (cart::pointIn(mx, my, mDropRects[i])) {
                    mf[mDropOpen] = (*mfChoices[mDropOpen])[i+mDropScroll];fieldEditor.focus(mf[mDropOpen]); break;
                }
            mDropOpen = -1;
            return;
        }
        if (cart::pointIn(mx, my, mOK)) { applyModal(); return; }
        if(modal==M_SCENARIO && cart::pointIn(mx,my,mAiProfiles)) {applyModal();if(modal==M_NONE)openAiProfiles();return;}
        if(modal==M_PREFERENCES && cart::pointIn(mx,my,mFolderChange)) {clickDataFolder();return;}
        if(modal==M_PREFERENCES && cart::pointIn(mx,my,mFolderRestart)) {   // apply the fields, as OK would, then restart
            applyModal();if(modal==M_NONE)requestRestart();return;
        }
        if (modal == M_NEW && cart::pointIn(mx, my, mRandom)) {
            if(!mf[0].empty() && !cart::validDocumentName(mf[0])) {modalError="Choose a map name without path separators or reserved filename characters.";return;}
            modalError.clear();
            for(int i=0;i<4;++i)generatorDraft[i]=mf[i];
            openModal(M_GENERATOR); return;
        }
        if (cart::pointIn(mx, my, mCancel)) { modal = M_NONE; SDL_StopTextInput(); return; }
        // A choice field opens its dropdown; a text field takes focus.
        for (int i = 0; i < mN; ++i)
            if (cart::pointIn(mx, my, mBox[i])) {
                if (mfChoices[i] && (!mfEditableChoice[i] || mx>=mBox[i].x+mBox[i].w-24)) { mDropOpen = i; mfocus = i;mDropScroll=0; }
                else {mfocus=i;
                    if(modal==M_SCENARIO && i==1) {
                        fieldEditor.click(mf[i],std::max(1,(mBox[i].w-8)/6),(my-mBox[i].y-4)/12+descriptionScroll,(mx-mBox[i].x-4)/6,SDL_GetModState()&KMOD_SHIFT);
                        descriptionFollowCaret=true;
                    } else fieldEditor.focus(mf[i]);
                }
                break;
            }
    }
}

void cart::Editor::drawRegionsList() {
    if(regionsOpen) {
        const auto ct=cart::drawPanel(ren,w,h,560,350,"REGIONS - CLICK TO LOCATE, DOUBLE CLICK TO EDIT");
        regionNew=cart::drawButton(ren,ct.x,ct.y,64,18,"NEW",false);
        regionEdit=cart::drawButton(ren,ct.x+72,ct.y,64,18,"EDIT",false);
        regionDelete=cart::drawButton(ren,ct.x+144,ct.y,64,18,"DELETE",false);
        regionOnMap=cart::drawButton(ren,ct.x+216,ct.y,96,18,"EDIT ON MAP",false);
        regionDone=cart::drawButton(ren,ct.x+ct.w-64,ct.y,64,18,"DONE",true);
        regionList={ct.x,ct.y+26,ct.w,ct.h-26};SDL_RenderSetClipRect(ren,&regionList);
        for(int i=0;i<int(scen.regions.size());++i) {
            const auto& r=scen.regions[i];int y=regionList.y+i*20-regionScroll;
            if(i==regionSelected) {SDL_SetRenderDrawColor(ren,55,80,105,255);SDL_Rect row{ct.x,y,ct.w,20};SDL_RenderFillRect(ren,&row);}
            cart::drawText(ren,r.name+"  ("+std::to_string(r.x1)+","+std::to_string(r.z1)+") - ("+std::to_string(r.x2)+","+std::to_string(r.z2)+")",ct.x+4,y+6,1,215,230,245);
        }
        SDL_RenderSetClipRect(ren,nullptr);
    }
}

void cart::Editor::drawModal() {
    if(modal==M_SYMMETRIZE)refreshSymmetryDialog();   // sources follow the chosen mirror
    // Save-before-exit prompt: SAVE / DON'T SAVE / CANCEL.
    if(modal==M_GENERATED && generatedPreview) {
        const auto ct=cart::drawPanel(ren,w,h,460,465,"GENERATED MAP PREVIEW");
        const auto& map=generatedPreview->map;
        const float scale=330.f/std::max(map.width,map.height);
        SDL_FRect area{float(ct.x)+(ct.w-map.width*scale)/2, float(ct.y)+28,map.width*scale,map.height*scale};
        SDL_RenderCopyF(ren,generatedTexture.get(),nullptr,&area);
        for(const auto& start:generatedPreview->starts) {
            const float x=area.x+start.xpos*scale,y=area.y+start.zpos*scale;
            SDL_SetRenderDrawColor(ren,255,220,80,255);SDL_FRect dot{x-3,y-3,6,6};SDL_RenderFillRectF(ren,&dot);
            cart::drawText(ren,std::to_string(start.number),int(x)+5,int(y)-3,1,255,240,120);
        }
        cart::drawText(ren,std::to_string(map.width/32)+" x "+std::to_string(map.height/32)+"  "+std::to_string(generatedPreview->starts.size())+" PLAYERS",ct.x,ct.y,1,230,235,245);
        if(!generatedPreview->note.empty())cart::drawText(ren,generatedPreview->note,ct.x,ct.y+352,1,235,215,150);
        cart::drawText(ren,"Accept replaces terrain, objects, starts and scenario rules.",ct.x,ct.y+370,1,195,210,225);
        cart::drawText(ren,"Discard keeps your current map unchanged.",ct.x,ct.y+386,1,195,210,225);
        mRegenerate=cart::drawButton(ren,ct.x,ct.y+ct.h-20,100,18,"SETTINGS",false);
        mOK=cart::drawButton(ren,ct.x+ct.w-150,ct.y+ct.h-20,70,18,"ACCEPT",true);
        mCancel=cart::drawButton(ren,ct.x+ct.w-74,ct.y+ct.h-20,70,18,"DISCARD",false);
    } else if(modal==M_ISSUES) {
        const auto ct=cart::drawPanel(ren,w,h,680,430,"MAP VALIDATION - CLICK A LOCATED ISSUE TO INSPECT IT");
        cart::drawText(ren,mapIssueRevision==0?"Run Check Map to analyze this document.":mapIssueRevision!=history.revision()?"Map changed: these results may be out of date.":std::to_string(mapIssues.size())+" issues. Connectivity warnings may be intentional.",ct.x,ct.y,1,235,220,160);
        issueList={ct.x,ct.y+25,ct.w,ct.h-85};issueRows.clear();issueContentHeight=0;
        SDL_RenderSetClipRect(ren,&issueList);
        if(mapIssues.empty() && mapIssueRevision)cart::drawText(ren,"No issues found by the implemented checks.",ct.x+4,issueList.y+5,1,200,230,205);
        for(size_t i=0;i<mapIssues.size();++i) {
            const auto& issue=mapIssues[i];const bool error=issue.severity==cart::MapIssue::Severity::Error;
            const auto label=std::string(error?"ERROR: ":"WARNING: ")+issue.message;
            const auto lines=cart::TextEdit::lines(label,std::max(1,(ct.w-8)/6));
            SDL_Rect row{ct.x,issueList.y+issueContentHeight-issueScroll,ct.w,int(lines.size())*12+8};
            issueRows.push_back(row);issueContentHeight+=row.h;
            if(row.y+row.h<issueList.y || row.y>issueList.y+issueList.h)continue;
            fillRect(ren,row.x,row.y,row.w,row.h,i==issueIndex?55:30,i==issueIndex?65:34,i==issueIndex?80:42);
            for(size_t line=0;line<lines.size();++line)cart::drawText(ren,label.substr(lines[line].begin,lines[line].end-lines[line].begin),row.x+4,row.y+4+int(line)*12,1,235,error?145:215,error?140:180);
        }
        SDL_RenderSetClipRect(ren,nullptr);
        cart::drawText(ren,"Naval checks use the initial output pose; trigger outcomes need playtesting.",ct.x,ct.y+ct.h-48,1,180,190,205);
        issueRecheck=cart::drawButton(ren,ct.x,ct.y+ct.h-20,90,18,"CHECK MAP",false);
        issueClose=cart::drawButton(ren,ct.x+ct.w-70,ct.y+ct.h-20,70,18,"CLOSE",true);
    } else if(modal==M_OBJECTS) {
        const auto ct=cart::drawPanel(ren,w,h,620,420,"PLACED UNITS - SELECT AND LOCATE");
        objectSearchBox=cart::drawField(ren,ct.x,ct.y,ct.w,"SEARCH NAME, TYPE OR OWNER",objectSearch,true,&objectEditor);
        objectList={ct.x,ct.y+36,ct.w,ct.h-85};
        SDL_RenderSetClipRect(ren,&objectList);
        for(int row=objectScroll/30;row<int(objectMatches.size()) && row*30<objectScroll+objectList.h;++row) {
            const int index=objectMatches[row];const auto& unit=units[index];const auto* type=unitRegistry.find(lowerText(unit.type));
            const int y=objectList.y+row*30-objectScroll;
            fillRect(ren,objectList.x,y,objectList.w,29,row==objectRow?55:27,row==objectRow?65:32,row==objectRow?80:40);
            cart::drawText(ren,std::to_string(index+1)+": "+(unit.name.empty()?(type?type->name:unit.type):unit.name),ct.x+5,y+3,1,235,235,215);
            cart::drawText(ren,unit.type+" - "+objectOwner(unit.player)+" - X "+std::to_string(unit.x)+" Z "+std::to_string(unit.z),ct.x+5,y+16,1,170,195,220);
        }
        if(objectMatches.empty())cart::drawText(ren,"No placed units match.",ct.x+5,objectList.y+5,1,205,210,220);
        SDL_RenderSetClipRect(ren,nullptr);
        cart::drawText(ren,std::to_string(objectMatches.size())+" matches. Click/Enter: select; Shift+Enter: properties.",ct.x,ct.y+ct.h-40,1,205,215,230);
        objectClose=cart::drawButton(ren,ct.x+ct.w-100,ct.y+ct.h-20,100,18,"CLOSE (ESC)",true);
    } else if(modal==M_MODEL_LOADING || modal==M_MODEL) {
        const auto ct=cart::drawPanel(ren,w,h,520,440,"UNIT MODEL: "+modelTitle);
        modelArea={ct.x,ct.y,ct.w,ct.h-65};
        fillRect(ren,modelArea.x,modelArea.y,modelArea.w,modelArea.h,20,24,30);
        if(modelPreview) {
            SDL_RenderSetViewport(ren,&modelArea);
            const auto now=SDL_GetTicks64();modelPreview->draw(modelArea.w,modelArea.h,std::min(.1f,float(now-modelTime)/1000));modelTime=now;
            SDL_RenderSetViewport(ren,nullptr);
        } else cart::drawText(ren,modelCancel->load()?"CANCELLING...":"Loading model and textures...",ct.x+12,ct.y+12,1,230,235,245);
        cart::drawText(ren,"Drag to rotate; wheel to zoom. Visual inspection only.",ct.x,ct.y+ct.h-40,1,200,215,230);
        if(!modelWarning.empty())cart::drawText(ren,modelWarning,ct.x,ct.y+ct.h-55,1,255,155,225);
        modelClose=cart::drawButton(ren,ct.x+ct.w-100,ct.y+ct.h-20,100,18,"CLOSE (ESC)",true);
    } else if(modal==M_ANALYZING || modal==M_CHECKING) {
        const auto ct=cart::drawPanel(ren,w,h,400,105,"ANALYZING TERRAIN");
        cart::drawText(ren,(modal==M_CHECKING?discardCheck:discardOverlay)?"Discarding result when analysis finishes...":"Applying engine terrain and footprint rules...",ct.x,ct.y,1,225,230,240);
        cart::drawText(ren,"Esc cancels. Map editing resumes when ready.",ct.x,ct.y+25,1,190,205,220);
    } else if(modal==M_GENERATING) {
        const auto ct=cart::drawPanel(ren,w,h,400,105,"GENERATING MAP");
        const std::string dots((SDL_GetTicks64()/400)%4,'.');
        cart::drawText(ren,cancelGeneration?"Discarding result when generation finishes...":"Building terrain"+dots,ct.x,ct.y,1,225,230,240);
        cart::drawText(ren,"Esc cancels. Your current map is retained.",ct.x,ct.y+25,1,190,205,220);
    } else if (modal == M_QUITSAVE) {
        SDL_Rect ct = cart::drawPanel(ren, w, h, 380, 92, "UNSAVED CHANGES");
        cart::drawText(ren, restartAfterQuit?"Save changes before restarting?":"Save changes before exiting?", ct.x, ct.y, 1, 225, 228, 236);
        mOK = cart::drawButton(ren, ct.x, ct.y + ct.h - 20, 60, 18, "SAVE", true);
        mQuit = cart::drawButton(ren, ct.x + 66, ct.y + ct.h - 20, 78, 18, "DISCARD", false);
        mCancel = cart::drawButton(ren, ct.x + ct.w - 74, ct.y + ct.h - 20, 70, 18, "CANCEL", false);
    }
    // Message box (Check Map result) / confirmation: wrapped text + OK
    // (message) or OK + CANCEL (confirm).
    else if (modal == M_MESSAGE || modal == M_CONFIRM) {
        int ph = 66 + int(mMsg.size()) * 12;
        SDL_Rect ct = cart::drawPanel(ren, w, h, 360, ph, mTitle);
        for (size_t i = 0; i < mMsg.size(); ++i)
            cart::drawText(ren, mMsg[i], ct.x, ct.y + int(i) * 12, 1, 225, 228, 236);
        if (modal == M_CONFIRM) {
            mOK = cart::drawButton(ren, ct.x + ct.w - 150, ct.y + ct.h - 20, 70, 18, "OK", true);
            mCancel = cart::drawButton(ren, ct.x + ct.w - 74, ct.y + ct.h - 20, 70, 18, "CANCEL", false);
        } else {
            mOK = cart::drawButton(ren, ct.x + ct.w - 74, ct.y + ct.h - 20, 70, 18, "OK", true);
            mCancel = {};   // no cancel on a message box
        }
    } else if (modal != M_NONE) {
        // N-field dialog; height fits the field count.
        // The generator's fifteen fields go in two columns.
        const int rows=modal==M_GENERATOR?(mN+1)/2:mN;
        int ph = modal==M_OPENPATH?420:modal==M_SCENARIO?300:70+rows*40+(modal==M_PREFERENCES?84:modal==M_NEWAIMOD?36:0);
        const int panelWidth=(modal==M_OPENPATH || modal==M_SCENARIO)?560:modal==M_PREFERENCES || modal==M_NEWAIMOD?460:modal==M_GENERATOR?600:320;
        const auto errorLines=cart::TextEdit::lines(modalError,(panelWidth-36)/6);
        const int errorHeight=modalError.empty()?0:int(errorLines.size())*12+8;
        ph+=errorHeight;
        SDL_Rect ct = cart::drawPanel(ren,w,h,panelWidth,ph,mTitle);
        if(errorHeight)for(size_t i=0;i<errorLines.size();++i)
            cart::drawText(ren,modalError.substr(errorLines[i].begin,errorLines[i].end-errorLines[i].begin),ct.x,ct.y+ct.h-24-errorHeight+int(i)*12,1,255,150,135);
        if(modal==M_OPENPATH) {
            openList={ct.x,ct.y+40,ct.w,ct.h-(openForTest?86:70)-errorHeight};filterOpenMaps();
            SDL_RenderSetClipRect(ren,&openList);
            for(int i=0;i<int(openMatches.size());++i) {
                const int y=openList.y+i*20-openScroll;if(y+20<openList.y || y>openList.y+openList.h)continue;
                fillRect(ren,openList.x,y,openList.w,19,25,30,40);
                cart::drawText(ren,openMatches[i].first,openList.x+4,y+6,1,215,225,240);
            }
            SDL_RenderSetClipRect(ren,nullptr);
        }
        if (modal == M_UNIT && editUnit >= 0 && editUnit < int(units.size()))
            cart::drawText(ren, units[size_t(editUnit)].type, ct.x, ct.y - 16, 1, 200, 200, 200);
        const SDL_Rect panel=ct;
        for (int i = 0; i < mN; ++i) {
            ct=panel;
            if(modal==M_GENERATOR) {const int column=i/rows;ct.w=(panel.w-20)/2;ct.x=panel.x+column*(ct.w+20);ct.y=panel.y-column*rows*40;}
            if(modal==M_SCENARIO && i==1)
                mBox[i]=cart::drawTextArea(ren,ct.x,ct.y+40,ct.w,160,"DESCRIPTION (SHIFT+ENTER: NEW LINE)",mf[i],mfocus==i,fieldEditor,descriptionScroll,descriptionFollowCaret);
            else if(mfChoices[i] && mfEditableChoice[i]) {
                mBox[i]=cart::drawField(ren,ct.x,ct.y+i*40,ct.w-24,mLabel[i],mf[i],mfocus==i,mfocus==i?&fieldEditor:nullptr);
                mBox[i].w+=24;
                cart::drawButton(ren,mBox[i].x+mBox[i].w-24,mBox[i].y,24,mBox[i].h,"v",mDropOpen==i);
            }
            else if (mfChoices[i])
                mBox[i] = cart::drawChoice(ren, ct.x, ct.y + i * 40, ct.w, mLabel[i],
                                           mf[i], mDropOpen == i);
            else
                mBox[i] = cart::drawField(ren, ct.x, ct.y + i * 40, ct.w, mLabel[i],
                                          mf[i], mfocus == i,mfocus==i?&fieldEditor:nullptr);
        }
        ct=panel;
        if(modal==M_NEWAIMOD) {
            cart::drawText(ren,"No map is opened: the AI Profiles window edits this pack's ai/ files.",ct.x,ct.y+42,1,190,200,215);
            cart::drawText(ren,"Nothing is written until SAVE TO PACK makes mods/game/<name>/ai/.",ct.x,ct.y+56,1,190,200,215);
        }
        if(modal==M_OPENPATH && openForTest)
            cart::drawText(ren,"The map is sent as a test copy, as Test Map does; the pack is ticked for that room only.",ct.x,ct.y+ct.h-34-errorHeight,1,170,185,200);
        mFolderChange=mFolderRestart={};mAiProfiles={};
        if(modal==M_PREFERENCES)drawDataFolder(ct);
        if(modal==M_SCENARIO) {   // the aiprofile= line: read-only here, edited in the AI Profiles window
            const std::string profile=cart::standardAiProfile(scenario.aiProfile)?"standard (aiprofile="+scenario.aiProfile+")":
                scenario.aiProfile+" -> "+cart::aiProfileFileKey(scenario.aiProfile)+(aiFiles.count(cart::aiProfileFileKey(scenario.aiProfile))?" in the map":" (not in the map)");
            std::string line="DEFAULT AI PROFILE: "+profile;
            if(line.size()>size_t(ct.w/6))line=line.substr(0,size_t(ct.w/6)-2)+"..";
            cart::drawText(ren,line,ct.x,ct.y+218,1,190,195,205);
            mAiProfiles=cart::drawButton(ren,ct.x,ct.y+ct.h-20,150,18,"EDIT AI PROFILES...",false);
        }
        // New Map gets a RANDOM shortcut (procedural terrain, jump straight in).
        if (modal == M_NEW) {
            mOK = cart::drawButton(ren, ct.x, ct.y + ct.h - 20, 74, 18, "CREATE", true);
            mRandom = cart::drawButton(ren, ct.x + 80, ct.y + ct.h - 20, 74, 18, "RANDOM", false);
        } else {
            mOK = cart::drawButton(ren, ct.x + ct.w - 150, ct.y + ct.h - 20, 70, 18, modal==M_GENERATOR?"GENERATE":"OK", true);
            mRandom = {};
        }
        mCancel = cart::drawButton(ren, ct.x + ct.w - 74, ct.y + ct.h - 20, 70, 18,
                                   "CANCEL", false);
        // Open dropdown list, drawn LAST so it overlays the fields below it.
        mDropRects.clear();
        if (mDropOpen >= 0 && mDropOpen < mN && mfChoices[mDropOpen]) {
            const auto& opts = *mfChoices[mDropOpen];
            const SDL_Rect& anchor = mBox[mDropOpen];
            constexpr int kRowH = 14;
            const int count=std::min(8,int(opts.size()));
            mDropScroll=std::clamp(mDropScroll,0,std::max(0,int(opts.size())-count));
            SDL_Rect list{anchor.x, std::min(anchor.y+anchor.h,h-count*kRowH-6), anchor.w,
                          kRowH * count + 2};
            SDL_SetRenderDrawColor(ren, 28, 30, 40, 255); SDL_RenderFillRect(ren, &list);
            SDL_SetRenderDrawColor(ren, 150, 200, 120, 255); SDL_RenderDrawRect(ren, &list);
            for (size_t i = size_t(mDropScroll); i < size_t(mDropScroll+count); ++i) {
                SDL_Rect r{list.x + 1, list.y + 1 + (int(i)-mDropScroll) * kRowH, list.w - 2, kRowH};
                if (opts[i] == mf[mDropOpen]) {
                    SDL_SetRenderDrawColor(ren, 60, 80, 50, 255); SDL_RenderFillRect(ren, &r);
                }
                cart::drawText(ren, opts[i], r.x + 4, r.y + 3, 1, 225, 230, 240);
                mDropRects.push_back(r);
            }
        }
    }
}

void cart::Editor::drawUseOnly() {
    // Use Only checklist overlay: scrollable unit-type list, click to toggle.
    if (useOnlyOpen) {
        SDL_Rect ct = cart::drawPanel(ren, w, h, 320, 460, "USE ONLY UNITS");
        std::string hdr = useOnly.empty()
            ? "ALL UNITS ALLOWED (empty = no restriction)"
            : std::to_string(useOnly.size()) + " ALLOWED  (click to toggle)";
        cart::drawText(ren, hdr, ct.x, ct.y - 16, 1, 180, 205, 185);
        int listH = ct.h - 30;
        uoList = {ct.x, ct.y, ct.w, listH};
        SDL_RenderSetClipRect(ren, &uoList);
        for (int i = 0; i < int(unitTypes.size()); ++i) {
            int ry = ct.y + i * 14 - useOnlyScroll;
            if (ry + 12 < ct.y || ry > ct.y + listH) continue;   // cull
            bool on = useOnly.count(unitTypes[size_t(i)]) > 0;
            cart::drawText(ren, (on ? "[X] " : "[ ] ") + unitTypes[size_t(i)],
                           ct.x + 2, ry, 1, on ? 235 : 128, on ? 235 : 130, on ? 180 : 138);
        }
        SDL_RenderSetClipRect(ren, nullptr);
        uoClear = cart::drawButton(ren, ct.x, ct.y + ct.h - 20, 100, 18, "UNRESTRICT", false);
        uoDone = cart::drawButton(ren, ct.x + ct.w - 74, ct.y + ct.h - 20, 70, 18, "DONE", true);
    }
}

// ---- GAME DATA FOLDER (View > Preferences) -----------------------------------------

void cart::Editor::clickDataFolder() {
    if(!tak::dataFolderOverrideReason(dataOrigin).empty() || folderPick.busy())return;
    if(!hooks.pickFolder && !tak::haveDirPicker()) {
        folderNote="No folder picker: install kdialog or zenity, or start Cartographer with --data.";folderNoteError=true;return;
    }
    folderNote.clear();folderNoteError=false;
    folderPick.start(hooks.pickFolder,folderSaved.empty()?dataRoot:folderSaved,folderSaved);
}

void cart::Editor::pollDataFolder() {
    const auto pick=folderPick.poll();if(!pick)return;
    if(pick->outcome==tak::DataFolderPick::Invalid) {
        folderNote=tak::dataFolderInvalidMessage(pick->reason)+" The current folder is kept.";folderNoteError=true;
    } else if(pick->outcome==tak::DataFolderPick::Accepted) {
        // Saved straight away and only that key: the game reads the same setting.
        if(!tak::saveDataFolderSetting(pick->folder,pick->manifest)) {folderNote="Could not save the setting. The current folder is kept.";folderNoteError=true;return;}
        folderSaved=pick->folder;folderNote.clear();folderNoteError=false;
        std::fprintf(stderr,"preferences: data folder set to %s\n",folderSaved.c_str());
    }
}

// Restart to load the saved folder: ask to save unsaved work first (M_QUITSAVE), then
// leave run(); runEditor relaunches with cart::restartArguments.
void cart::Editor::requestRestart() {
    restartAfterQuit=true;
    if(dirty) {useOnlyOpen=scriptOpen=pickOpen=regionsOpen=false;closeAiProfiles();modal=M_QUITSAVE;SDL_StopTextInput();}
    else running=false;
}

void cart::Editor::drawDataFolder(const SDL_Rect& ct) {
    const std::string locked=tak::dataFolderOverrideReason(dataOrigin);
    const bool busy=folderPick.busy(),pending=tak::dataFolderNeedsRestart(folderSaved,dataRoot);
    const int y=ct.y+mN*40,buttonW=84,boxW=ct.w-buttonW-6;
    cart::drawText(ren,"GAME DATA FOLDER (SHARED WITH THE GAME)",ct.x,y,1,locked.empty()?190:120,locked.empty()?195:124,locked.empty()?205:134);
    // The folder in use, elided at its start (the tail is what tells installs apart).
    SDL_Rect box{ct.x,y+10,boxW,16};
    fillRect(ren,box.x,box.y,box.w,box.h,24,26,34);
    SDL_SetRenderDrawColor(ren,90,92,104,255);SDL_RenderDrawRect(ren,&box);
    cart::drawText(ren,tak::elidePathStart(dataRoot,size_t(std::max(4,(boxW-8)/6))),box.x+4,box.y+4,1,locked.empty()?230:140,locked.empty()?235:145,locked.empty()?245:155);
    if(locked.empty() && !busy)mFolderChange=cart::drawButton(ren,ct.x+ct.w-buttonW,y+10,buttonW,16,"CHANGE...",false);
    else {   // greyed: nothing to click
        fillRect(ren,ct.x+ct.w-buttonW,y+10,buttonW,16,34,36,44);
        cart::drawText(ren,"CHANGE...",ct.x+ct.w-buttonW+(buttonW-cart::textWidth("CHANGE...",1))/2,y+14,1,110,112,122);
    }
    std::string note=!locked.empty()?locked:busy?"Choosing a folder...":!folderNote.empty()?folderNote:
        pending?"Saved. Restart Cartographer to load "+tak::elidePathStart(folderSaved,48)+". The game uses it from its next start.":
        "The game reads this folder too. A new one loads when Cartographer restarts.";
    const auto lines=cart::TextEdit::lines(note,std::max(1,ct.w/6));
    const bool warn=folderNoteError,amber=busy || (pending && folderNote.empty());
    for(size_t i=0;i<std::min<size_t>(3,lines.size());++i)
        cart::drawText(ren,note.substr(lines[i].begin,lines[i].end-lines[i].begin),ct.x,y+32+int(i)*12,1,
                       warn?255:amber?240:170,warn?150:amber?200:180,warn?135:amber?120:195);
    if(pending && locked.empty() && !busy)mFolderRestart=cart::drawButton(ren,ct.x,ct.y+ct.h-20,110,18,"RESTART NOW",false);
}
