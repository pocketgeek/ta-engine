// Symmetry tools and the terrain clipboard: the Symmetry menu, Symmetrize,
// live mirrored editing, terrain selection, Copy and the clipboard brush. The
// mirror math, section twins and clipboard data live in symmetry.cpp.

#include "cartographer/editorstate.h"
#include "cartographer/dialog.h"

#include <cmath>

namespace {
bool intersects(const cart::Box& a,const cart::Box& b) {return a.x0<b.x1 && b.x0<a.x1 && a.z0<b.z1 && b.z0<a.z1;}
const char* kindName(cart::Mirror kind) {
    static const char* names[]={"VERTICAL AXIS","HORIZONTAL AXIS","DIAGONAL \\","DIAGONAL /","ROTATE 2-WAY","ROTATE 4-WAY","MIRROR 4-WAY"};
    return names[int(kind)];
}
const char* kSelectionSource="CURRENT SELECTION";
}

const cart::SectionMatcher& cart::Editor::symmetrySections() {
    // Rebuilt whenever the browser rescans: scan() frees the cached prefabs
    // the matcher points at. The key (clipboard section indices) survives
    // same-world opens.
    const auto& list=sections.list();
    const std::string key=lowerText(world)+":"+std::to_string(list.size())+(list.empty()?"":":"+list.front().path+":"+list.back().path);
    if(!sectionMatcher.built() || sectionMatcherGeneration!=sections.generation()) {
        sectionMatcher.build(list,[&](size_t i){return sections.load(vfs,list[i].path);});
        sectionMatcherGeneration=sections.generation();
    }
    sectionMatcher.setKey(key);
    return sectionMatcher;
}

cart::FootprintLookup cart::Editor::featureFootprint() {
    return [this](const std::string& name) {
        const auto* ref=features.byName(name);
        return ref?std::pair{ref->footX,ref->footZ}:std::pair{1,1};
    };
}

bool cart::Editor::symmetryFits(cart::Mirror kind) const {
    return !cart::mirrorNeedsSquare(kind) || mapView.map().width==mapView.map().height;
}

bool cart::Editor::symmetrySelection() const {
    return (tool==UNITS && !selectedUnits.indices.empty()) || (tool==FEATURES && !selectedFeatures.cells.empty()) ||
           (tool==TERRAIN && terrainSelection.has_value());
}

void cart::Editor::setSymmetryStatus(std::string text) {symmetryStatus=std::move(text);symmetryStatusAt=SDL_GetTicks64();}

std::string cart::Editor::symmetryStatusText() const {
    std::string text;
    if(liveSymmetry)text+=std::string("   SYMMETRY: ")+kindName(symmetryKind)+(symmetryFits(symmetryKind)?" (M)":" (SQUARE MAPS ONLY: INACTIVE)");
    if(tool==TERRAIN && clipboardBrush())
        text+="   BRUSH: TERRAIN CLIPBOARD "+std::to_string(terrainClip.map.blocksX)+"x"+std::to_string(terrainClip.map.blocksY)+" (ESC: SECTIONS)";
    else if(tool==TERRAIN && terrainSelection)
        text+="   TERRAIN SELECTION "+std::to_string(terrainSelection->x1-terrainSelection->x0)+"x"+std::to_string(terrainSelection->z1-terrainSelection->z0)+" BLOCKS (CTRL+C COPIES)";
    return text;
}

std::string cart::Editor::symmetryMenuLabel(size_t row) const {
    const auto& label=menuRows[4][row];
    if(row==0)return std::string(liveSymmetry?"[X] ":"[ ] ")+label;
    if(row>=2 && row<=8) {
        const auto kind=cart::Mirror(row-2);
        return std::string(symmetryKind==kind?"(*) ":"( ) ")+label+(symmetryFits(kind)?"":" - n/a");
    }
    return label;
}

void cart::Editor::toggleLiveSymmetry() {
    if(!symmetryFits(symmetryKind))symmetryKind=cart::Mirror::Vertical;
    liveSymmetry=!liveSymmetry;
    setSymmetryStatus(liveSymmetry?std::string("LIVE SYMMETRY ON: edits are mirrored across the ")+kindName(symmetryKind):"LIVE SYMMETRY OFF");
}

void cart::Editor::symmetryMenu(int row) {
    if(row==0)toggleLiveSymmetry();
    else if(row==1)openModal(M_SYMMETRIZE);
    else if(row>=2 && row<=8) {
        const auto kind=cart::Mirror(row-2);
        if(!symmetryFits(kind)) {openMessage("SYMMETRY","Diagonal mirrors and 4-way rotation need a square map. Resize the map (R) to equal width and height first.");return;}
        symmetryKind=kind;setSymmetryStatus(std::string("SYMMETRY: ")+kindName(kind));
    } else if(row==9) {
        if(symmetryIssues.empty()) {openMessage("UNMIRRORED TERRAIN","The last Symmetrize mirrored all terrain it was asked to, or none has run yet.");return;}
        mapIssues=symmetryIssues;mapIssueRevision=history.revision();issueIndex=0;issueScroll=0;showMapIssue();
    }
}

void cart::Editor::setupSymmetrizeFields() {
    symmetryKindOpts.clear();
    for(int k=0;k<cart::kMirrorKinds;++k)if(symmetryFits(cart::Mirror(k)))symmetryKindOpts.push_back(cart::mirrorLabel(cart::Mirror(k)));
    mTitle="SYMMETRIZE";mN=3;
    mLabel[0]="MIRROR";mf[0]=cart::mirrorLabel(symmetryFits(symmetryKind)?symmetryKind:cart::Mirror::Vertical);mfChoices[0]=&symmetryKindOpts;mfNumeric[0]=false;
    mLabel[1]="SOURCE (COPIED ONTO THE OTHER SIDE)";mf[1].clear();mfChoices[1]=&symmetrySourceOpts;mfNumeric[1]=false;
    mLabel[2]="TERRAIN";mf[2]=symmetryTerrainOpts[0];mfChoices[2]=&symmetryTerrainOpts;mfNumeric[2]=false;
    refreshSymmetryDialog();
}

// The source list follows the chosen mirror (drawModal keeps it current).
void cart::Editor::refreshSymmetryDialog() {
    auto kind=cart::Mirror::Vertical;
    for(int k=0;k<cart::kMirrorKinds;++k)if(mf[0]==cart::mirrorLabel(cart::Mirror(k)))kind=cart::Mirror(k);
    symmetrySourceOpts=cart::mirrorSources(kind);
    if(symmetrySelection())symmetrySourceOpts.push_back(kSelectionSource);
    if(std::find(symmetrySourceOpts.begin(),symmetrySourceOpts.end(),mf[1])==symmetrySourceOpts.end())mf[1]=symmetrySourceOpts.front();
}

void cart::Editor::applySymmetrize() {
    refreshSymmetryDialog();
    cart::SymmetryRequest request;
    for(int k=0;k<cart::kMirrorKinds;++k)if(mf[0]==cart::mirrorLabel(cart::Mirror(k)))request.kind=cart::Mirror(k);
    if(!symmetryFits(request.kind)) {modalError="This mirror needs a square map.";return;}
    const auto sources=cart::mirrorSources(request.kind);
    request.source=int(std::find(sources.begin(),sources.end(),mf[1])-sources.begin());
    if(request.source>=int(sources.size())) {
        request.source=-1;
        if(tool==UNITS)request.units=selectedUnits.indices;
        if(tool==FEATURES)request.featureCells=selectedFeatures.cells;
        if(tool==TERRAIN)request.blocks=terrainSelection;
    }
    request.terrain=mf[2]==symmetryTerrainOpts[0];
    symmetryKind=request.kind;
    // A dry run on copies gives the exact counts for the confirmation.
    struct Result {tak::tnt::Map map;std::vector<tak::tnt::StartPos> starts;tak::crt::Scenario scenario;std::vector<cart::PlacedUnit> units;cart::SymmetryReport report;};
    auto result=std::make_shared<Result>(Result{mapView.map(),scenario.starts,scen,units,{}});
    result->report=cart::symmetrize(result->map,result->starts,result->scenario,result->units,request,featureFootprint(),request.terrain?&symmetrySections():nullptr);
    const auto& r=result->report;
    if(!(r.units+r.features+r.starts+r.regions+r.blocks+r.blocked+r.removedUnits+r.removedFeatures+r.removedStarts+r.removedRegions)) {
        openMessage("SYMMETRIZE","Nothing to mirror from the "+lowerText(mf[1])+". "+r.summary());return;
    }
    const std::string source=request.source<0?"current selection":lowerText(mf[1]);
    openConfirm("SYMMETRIZE","Mirror the "+source+" ("+lowerText(kindName(request.kind))+"), replacing what is on the other side? "+r.summary()+" Undo (Ctrl+Z) restores the map.",[this,result] {
        mapView.quiesce();
        mapView.editMap()=std::move(result->map);mapView.tilesEdited();minimapSource=nullptr;invalidateMinimap();
        scenario.starts=std::move(result->starts);scen=std::move(result->scenario);units=std::move(result->units);
        selectedUnits.indices.clear();selectedUnits.dragOrigins.clear();selectedFeatures.clear();regionDrag.cancel();regionSelected=-1;
        draggingUnit=draggingStart=-1;overlayInvalidated=true;
        edited=unitsEdited=dirty=historyPending=true;commitHistory();   // one undo step
        const auto& report=result->report;
        symmetryIssues=report.terrainIssues;
        setSymmetryStatus("SYMMETRIZED: "+std::to_string(report.units+report.features+report.starts+report.regions)+" object copies, "+std::to_string(report.blocks)+" terrain blocks, "+
                          std::to_string(report.blocked)+" blocked cells");
        if(report.unmirroredBlocks) {
            mapIssues=symmetryIssues;mapIssueRevision=history.revision();issueIndex=0;issueScroll=0;
            openMessage("SYMMETRIZED",report.summary()+" Symmetry > Locate unmirrored terrain (or Scenario > Next issue) visits each area; Validation results lists them all.");
        }
    });
}

cart::Editor::LiveEdit::LiveEdit(Editor& owner,const SDL_Event& e,bool keyboard):editor(owner) {
    const bool click=(e.type==SDL_MOUSEBUTTONDOWN || e.type==SDL_MOUSEBUTTONUP) && e.button.button==SDL_BUTTON_LEFT;
    const bool drag=e.type==SDL_MOUSEMOTION && (e.motion.state&SDL_BUTTON_LMASK);
    active=editor.liveSymmetry && editor.tool!=TERRAIN && !editor.regionCanvas && (keyboard?e.type==SDL_KEYDOWN:click || drag) &&
           editor.symmetryFits(editor.symmetryKind);
    if(!active)return;
    if(editor.tool==UNITS)units=editor.units;
    else if(editor.tool==STARTS)starts=editor.scenario.starts;
    else {features=editor.mapView.map().features;featureNames=editor.mapView.map().featureNames;}
}

cart::Editor::LiveEdit::~LiveEdit() {
    if(!active)return;
    auto& e=editor;const auto& map=e.mapView.map();cart::LiveMirrorResult result;
    if(e.tool==UNITS) {
        result=cart::mirrorUnitEdits(units,e.units,e.symmetryKind,e.scenario.starts,map.width,map.height);
        if(result.copies)e.unitsEdited=e.dirty=e.historyPending=true;
    } else if(e.tool==STARTS) {
        result=cart::mirrorStartEdits(starts,e.scenario.starts,e.symmetryKind,map.width,map.height);
        if(result.copies)e.dirty=e.historyPending=true;
    } else if(e.tool==BLOCKED) {   // blocked cells change no tiles: the minimaps stay
        result=cart::mirrorBlockerEdits(features,e.mapView.editMap(),e.symmetryKind);
        if(result.copies)e.dirty=e.historyPending=true;
    } else {
        result=cart::mirrorFeatureEdits(features,featureNames,e.mapView.editMap(),e.symmetryKind,e.featureFootprint());
        if(result.copies)e.edited=e.dirty=e.historyPending=true;
    }
    if(result.skipped && e.tool==BLOCKED)e.setSymmetryStatus(std::to_string(result.skipped)+" mirrored blocked cell(s) skipped: a feature or road holds the mirror cell");
    else if(result.skipped)e.setSymmetryStatus(std::to_string(result.skipped)+" mirrored change(s) skipped: off the map, overlapping, or no free start number");
    else if(result.copies)e.setSymmetryStatus("MIRRORED "+std::to_string(result.copies)+" change(s)");
}

// Live symmetry for the section brush: stamp each image's twin section.
void cart::Editor::stampMirrors(int section,int x,int z) {
    if(!liveSymmetry || !symmetryFits(symmetryKind))return;
    const auto& matcher=symmetrySections();auto& map=mapView.editMap();
    const auto* own=matcher.section(section);if(!own)return;
    const cart::Box placed{x,z,x+own->blocksX,z+own->blocksY};
    std::string problem;
    for(auto xf:cart::mirrorImages(symmetryKind)) {
        const auto image=cart::transform(xf,placed,map.blocksX,map.blocksY);
        if(intersects(image,placed)) {problem="Stamp touches the symmetry axis; its mirror was not stamped";continue;}
        const auto* twin=matcher.section(matcher.twin(section,xf));
        if(!twin) {problem="NO MIRROR TWIN for section "+matcher.name(section)+": stamped at the source only";continue;}
        if(cart::stampSection(map,*twin,image.x0,image.z0,stampLayers))mapView.tilesEdited(image.x0,image.z0,twin->blocksX,twin->blocksY);
    }
    if(!problem.empty())setSymmetryStatus(problem);
}

bool cart::Editor::clipboardBrush() const {return clipBrush && selected==clipBrushSection && !terrainClip.empty();}

cart::Box cart::Editor::clipBrushBox(float wx,float wz) const {
    const int w=terrainClip.map.blocksX,h=terrainClip.map.blocksY;
    const int x=int(std::floor(wx/32))-w/2,z=int(std::floor(wz/32))-h/2;
    return {x,z,x+w,z+h};
}

// Select mode with the Terrain tool drags a block-aligned rectangle.
bool cart::Editor::handleTerrainSelection(SDL_Event& e) {
    if(tool!=TERRAIN || editMode!=MODE_SELECT || regionCanvas)return false;
    const auto& map=mapView.map();
    auto block=[&](int x,int y) {
        return std::pair{std::clamp(int(std::floor((mapView.offX()+(x-kPaletteW)/mapView.zoom())/32)),0,map.blocksX-1),
                         std::clamp(int(std::floor((mapView.offY()+(y-kMenuH)/mapView.zoom())/32)),0,map.blocksY-1)};
    };
    auto span=[&](int x,int z) {
        terrainSelection=cart::Box{std::min(terrainSelX,x),std::min(terrainSelZ,z),std::max(terrainSelX,x)+1,std::max(terrainSelZ,z)+1};
    };
    if(e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT && e.button.x>=kPaletteW && e.button.y>=kMenuH && e.button.y<h-kStatusH) {
        const auto [x,z]=block(e.button.x,e.button.y);terrainSelecting=true;terrainSelX=x;terrainSelZ=z;span(x,z);return true;
    }
    if(e.type==SDL_MOUSEMOTION && terrainSelecting && (e.motion.state&SDL_BUTTON_LMASK)) {const auto [x,z]=block(e.motion.x,e.motion.y);span(x,z);return true;}
    if(e.type==SDL_MOUSEBUTTONUP && e.button.button==SDL_BUTTON_LEFT && terrainSelecting) {
        const auto [x,z]=block(e.button.x,e.button.y);span(x,z);terrainSelecting=false;return true;
    }
    return false;
}

bool cart::Editor::handleTerrainKeys(SDL_Event& e) {
    if(e.type!=SDL_KEYDOWN || tool!=TERRAIN || regionCanvas)return false;
    const auto key=e.key.keysym.sym;const bool ctrl=e.key.keysym.mod&(KMOD_CTRL|KMOD_GUI);
    if(ctrl && key==SDLK_c) {copyTerrainSelection();return true;}
    if(ctrl && key==SDLK_v) {useTerrainClipboard();return true;}
    if(key==SDLK_ESCAPE && clipboardBrush()) {clipBrush=false;setSymmetryStatus("BRUSH: SECTIONS");return true;}
    if(key==SDLK_ESCAPE && terrainSelection && editMode==MODE_SELECT) {terrainSelection.reset();return true;}
    return false;
}

void cart::Editor::copyTerrainSelection() {
    if(!terrainSelection) {openMessage("COPY TERRAIN","Choose the Terrain tool and Select mode, then drag a rectangle on the map. Ctrl+C copies its tiles, heights and objects.");return;}
    terrainClip=cart::copyTerrain(mapView.map(),*terrainSelection,units,&symmetrySections());
    terrainClip.world=lowerText(world);clipTexture.reset();
    const auto& clip=terrainClip.map;
    // Translucent brush preview, composed once (large clips show an outline).
    if(clip.blocksX*clip.blocksY<=64*64) {
        std::vector<uint8_t> rgba(size_t(clip.blocksX)*32*clip.blocksY*32*4,0);
        for(int z=0;z<clip.blocksY;++z)for(int x=0;x<clip.blocksX;++x)mapView.compositor().renderBlock(clip,x,z,rgba,clip.blocksX*32,x*32,z*32);
        clipTexture.reset(SDL_CreateTexture(ren,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,clip.blocksX*32,clip.blocksY*32));
        if(clipTexture) {SDL_UpdateTexture(clipTexture.get(),nullptr,rgba.data(),clip.blocksX*32*4);SDL_SetTextureScaleMode(clipTexture.get(),SDL_ScaleModeLinear);}
    }
    int named=0;for(auto v:clip.features)named+=v<0xFFFA && v<clip.featureNames.size();
    setSymmetryStatus("COPIED "+std::to_string(clip.blocksX)+"x"+std::to_string(clip.blocksY)+" BLOCKS, "+std::to_string(named)+" features, "+
                      std::to_string(terrainClip.units.size())+" units. Ctrl+V makes it the brush");
}

void cart::Editor::useTerrainClipboard() {
    if(terrainClip.empty()) {openMessage("PASTE TERRAIN","The terrain clipboard is empty. In Select mode with the Terrain tool, drag a rectangle and press Ctrl+C.");return;}
    tool=TERRAIN;editMode=MODE_PLACE;regionCanvas=false;clearArm=false;clipBrush=true;clipBrushSection=selected;
    setSymmetryStatus("CLIPBOARD BRUSH: click to paste; Edit > Terrain brush chooses whether objects come along");
}

void cart::Editor::pasteTerrainAtMouse(int mx,int my) {
    const auto box=clipBrushBox(mapView.offX()+(mx-kPaletteW)/mapView.zoom(),mapView.offY()+(my-kMenuH)/mapView.zoom());
    const auto here=lowerText(world);
    if(terrainClip.world!=here && clipWorldAccepted!=terrainClip.world+">"+here) {
        openConfirm("PASTE FROM ANOTHER WORLD","This terrain was copied from a "+terrainClip.world+" map. Tile art and features are made for each world, so pasting it into a "+
            here+" map can look wrong in the game. Paste anyway?",[this,box,here] {clipWorldAccepted=terrainClip.world+">"+here;pasteTerrainAt(box.x0,box.z0);});
        return;
    }
    pasteTerrainAt(box.x0,box.z0);
}

void cart::Editor::pasteTerrainAt(int bx,int bz) {
    auto& map=mapView.editMap();
    const int w=terrainClip.map.blocksX,h=terrainClip.map.blocksY;
    const cart::Box placed{bx,bz,bx+w,bz+h};
    std::vector<std::pair<cart::TerrainClip,cart::Box>> mirrors;std::string why;
    if(liveSymmetry && symmetryFits(symmetryKind)) {
        // All images or none: each needs every clipboard section's twin.
        for(auto xf:cart::mirrorImages(symmetryKind)) {
            const auto image=cart::transform(xf,placed,map.blocksX,map.blocksY);
            if(intersects(image,placed)) {why="the paste touches the symmetry axis";break;}
            auto mirrored=cart::mirrorClip(terrainClip,xf,symmetrySections(),featureFootprint(),why);
            if(!mirrored)break;
            const auto owners=cart::mirrorPlayers(scenario.starts,xf,map.width,map.height);
            for(auto& unit:mirrored->units)if(unit.player>=0 && unit.player<=8)unit.player=owners[size_t(unit.player)];
            mirrors.push_back({std::move(*mirrored),image});
        }
        if(!why.empty())mirrors.clear();
    }
    const auto result=cart::pasteTerrain(map,units,terrainClip,bx,bz,stampLayers);
    if(!result.pasted)return;
    mapView.tilesEdited(bx,bz,w,h);
    int pastedUnits=result.units,skipped=result.skippedUnits;
    for(const auto& [clip,box]:mirrors) {
        const auto copy=cart::pasteTerrain(map,units,clip,box.x0,box.z0,stampLayers);
        if(copy.pasted)mapView.tilesEdited(box.x0,box.z0,clip.map.blocksX,clip.map.blocksY);
        pastedUnits+=copy.units;skipped+=copy.skippedUnits;
    }
    invalidateMinimap();edited=dirty=historyPending=true;
    if(pastedUnits)unitsEdited=true;
    std::string status="PASTED "+std::to_string(w)+"x"+std::to_string(h)+" BLOCKS";
    if(!mirrors.empty())status+=" + "+std::to_string(mirrors.size())+" MIRROR(S)";
    else if(!why.empty())status+=" AT THE SOURCE ONLY: "+why;
    if(skipped)status+=", "+std::to_string(skipped)+" unit(s) skipped (off the map or overlapping)";
    setSymmetryStatus(status);
}

// Axis, terrain selection and mirrored brush outlines (canvas viewport).
void cart::Editor::drawSymmetryOverlay() {
    const float zoom=mapView.zoom();const auto& map=mapView.map();
    auto sx=[&](float x){return (x-mapView.offX())*zoom;};
    auto sz=[&](float z){return (z-mapView.offY())*zoom;};
    const float W=map.width*16.f,H=map.height*16.f;
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
    if(liveSymmetry && symmetryFits(symmetryKind)) {
        SDL_SetRenderDrawColor(ren,110,225,255,210);
        auto line=[&](float x0,float z0,float x1,float z1) {
            for(int d=-1;d<=1;++d)SDL_RenderDrawLineF(ren,sx(x0)+d*(x0==x1),sz(z0)+d*(z0==z1),sx(x1)+d*(x0==x1),sz(z1)+d*(z0==z1));
        };
        const auto kind=symmetryKind;
        if(kind==cart::Mirror::Vertical || kind==cart::Mirror::Quad)line(W/2,0,W/2,H);
        if(kind==cart::Mirror::Horizontal || kind==cart::Mirror::Quad)line(0,H/2,W,H/2);
        if(kind==cart::Mirror::Diagonal)line(0,0,W,H);
        if(kind==cart::Mirror::AntiDiagonal)line(W,0,0,H);
        if(kind==cart::Mirror::Rotate2 || kind==cart::Mirror::Rotate4) {
            SDL_SetRenderDrawColor(ren,110,225,255,90);
            SDL_RenderDrawLineF(ren,sx(W/2),sz(0),sx(W/2),sz(H));SDL_RenderDrawLineF(ren,sx(0),sz(H/2),sx(W),sz(H/2));
            SDL_SetRenderDrawColor(ren,110,225,255,230);
            for(int r=8;r<=10;++r)for(int i=0;i<24;++i) {
                const float a0=i*6.2831853f/24,a1=(i+1)*6.2831853f/24;
                SDL_RenderDrawLineF(ren,sx(W/2)+r*std::cos(a0),sz(H/2)+r*std::sin(a0),sx(W/2)+r*std::cos(a1),sz(H/2)+r*std::sin(a1));
            }
        }
    }
    if(tool==TERRAIN && terrainSelection && editMode==MODE_SELECT) {
        const auto& b=*terrainSelection;
        SDL_FRect box{sx(b.x0*32.f),sz(b.z0*32.f),(b.x1-b.x0)*32*zoom,(b.z1-b.z0)*32*zoom};
        SDL_SetRenderDrawColor(ren,255,220,110,40);SDL_RenderFillRectF(ren,&box);
        SDL_SetRenderDrawColor(ren,255,220,110,255);SDL_RenderDrawRectF(ren,&box);
        cart::drawText(ren,std::to_string(b.x1-b.x0)+" x "+std::to_string(b.z1-b.z0)+" blocks",int(box.x)+3,int(box.y)+3,1,255,235,170);
    }
    // Where live symmetry will put the mirrored stamp or paste.
    int mx,my;SDL_GetMouseState(&mx,&my);mx=int(mx/kUIScale);my=int(my/kUIScale);
    if(liveSymmetry && symmetryFits(symmetryKind) && tool==TERRAIN && editMode==MODE_PLACE && modal==M_NONE && menuOpen<0 &&
       mx>=kPaletteW && my>=kMenuH && my<h-kStatusH) {
        const float wx=mapView.offX()+(mx-kPaletteW)/zoom,wz=mapView.offY()+(my-kMenuH)/zoom;
        std::optional<cart::Box> brush;
        if(clipboardBrush())brush=clipBrushBox(wx,wz);
        else if(selected>=0)if(const auto* sec=sections.load(vfs,sections.list()[size_t(selected)].path);sec && sec->blocksX>0 && sec->blocksY>0) {
            const int x=(int(wx/32)/sec->blocksX)*sec->blocksX,z=(int(wz/32)/sec->blocksY)*sec->blocksY;brush=cart::Box{x,z,x+sec->blocksX,z+sec->blocksY};
        }
        if(brush)for(auto xf:cart::mirrorImages(symmetryKind)) {
            const auto b=cart::transform(xf,*brush,map.blocksX,map.blocksY);
            SDL_FRect box{sx(b.x0*32.f),sz(b.z0*32.f),(b.x1-b.x0)*32*zoom,(b.z1-b.z0)*32*zoom};
            SDL_SetRenderDrawColor(ren,110,225,255,200);SDL_RenderDrawRectF(ren,&box);
        }
    }
}
