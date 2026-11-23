#include "cartographer/recovery.h"
#include "cartographer/blockers.h"
#include "cartographer/scenarioinfo.h"
#include "cartographer/document.h"
#include "cartographer/history.h"
#include "cartographer/selection.h"
#include "cartographer/featureselection.h"
#include "cartographer/sections.h"
#include "cartographer/preferences.h"
#include "cartographer/scrolling.h"
#include "cartographer/symmetry.h"
#include <cmath>
#include "cartographer/sectiongroups.h"
#include <random>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include "util/u8path.h"

static void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
static std::vector<uint8_t> read(const std::filesystem::path& path) {
    std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};
}

// Symmetry math, section twins, Symmetrize, live mirroring and the terrain
// clipboard, on synthetic maps (no retail data).
static tak::tnt::Map blankMap(int blocksX,int blocksY) {
    tak::tnt::Map map;map.blocksX=blocksX;map.blocksY=blocksY;map.width=blocksX*2;map.height=blocksY*2;
    map.heights.assign(size_t(map.width)*map.height,50);map.features.assign(map.heights.size(),0xFFFF);
    map.tileKeys.assign(size_t(blocksX)*blocksY,7);map.tileCols.assign(map.tileKeys.size(),0);map.tileRows.assign(map.tileKeys.size(),0);
    return map;
}
static tak::tnt::Map prefab(uint32_t key,uint8_t height) {   // 2x2 blocks, one key, distinct cols/rows
    auto map=blankMap(2,2);
    for(int i=0;i<4;++i) {map.tileKeys[size_t(i)]=key;map.tileCols[size_t(i)]=uint8_t(i%2);map.tileRows[size_t(i)]=uint8_t(i/2);}
    for(size_t i=0;i<map.heights.size();++i)map.heights[i]=uint8_t(height+i);
    map.features[1]=0xFFFB;   // a road marker in the second cell
    return map;
}
// The browser scrollbar and edge-scroll band (SDL-free math in scrolling.h).
static void scrollingTests() {
    cart::ScrollBar bar;bar.x=190;bar.w=8;bar.y=120;bar.h=500;bar.view=500;bar.content=400;
    check(!bar.active() && bar.maxScroll()==0 && bar.clamp(90)==0 && !bar.inTrack(194,200),"a list that fits has no scrollbar");
    bar.content=2000;
    check(bar.active() && bar.maxScroll()==1500 && bar.clamp(-5)==0 && bar.clamp(9999)==1500,"scroll clamps to the content");
    check(bar.thumbHeight()==125 && bar.thumbTop(0)==120 && bar.thumbTop(1500)==120+500-125 && bar.thumbTop(750)==120+187,"thumb size and position follow the viewport share");
    check(bar.inTrack(190,120) && bar.inTrack(197,619) && !bar.inTrack(189,300) && !bar.inTrack(198,300) && !bar.inTrack(194,620),"track hit area");
    check(bar.inThumb(194,130,0) && !bar.inThumb(194,300,0) && bar.inThumb(194,600,1500),"thumb hit area moves with the scroll");
    check(bar.scrollForThumb(120)==0 && bar.scrollForThumb(120+375)==1500 && bar.scrollForThumb(120+188)==752 && bar.scrollForThumb(-50)==0 && bar.scrollForThumb(900)==1500,"thumb drag maps back to a clamped scroll");
    for(int s=0;s<=1500;s+=7)check(std::abs(bar.scrollForThumb(bar.thumbTop(s))-s)<=4,"thumb position and drag invert each other");
    check(bar.page(0,400)==500 && bar.page(1500,130)==1000 && bar.page(1400,600)==1500 && bar.page(100,150)==100,"track clicks page one viewport toward the pointer");
    bar.content=200000;check(bar.thumbHeight()==cart::ScrollBar::kMinThumb && bar.thumbTop(bar.maxScroll())+bar.thumbHeight()==bar.y+bar.h,"long lists keep a grabbable thumb inside the track");
    // Canvas at (200,44) 800x600, 16px band.
    auto edge=[](int x,int y) {const auto d=cart::edgeScrollDirection(x,y,200,44,800,600,16);return std::pair{d.x,d.z};};
    check(edge(600,300)==std::pair{0,0},"canvas middle does not scroll");
    check(edge(200,300)==std::pair{-1,0} && edge(215,300)==std::pair{-1,0} && edge(216,300)==std::pair{0,0},"left band is 16px inside the canvas");
    check(edge(999,300)==std::pair{1,0} && edge(984,300)==std::pair{1,0} && edge(983,300)==std::pair{0,0},"right band");
    check(edge(600,44)==std::pair{0,-1} && edge(600,643)==std::pair{0,1} && edge(205,640)==std::pair{-1,1},"top, bottom and corners");
    check(edge(199,300)==std::pair{0,0} && edge(100,300)==std::pair{0,0} && edge(600,43)==std::pair{0,0} && edge(600,644)==std::pair{0,0} && edge(1000,300)==std::pair{0,0},"browser, menus, status bar and outside never scroll");
    const auto tiny=cart::edgeScrollDirection(20,20,0,0,40,40,16);
    check(!tiny.any() && cart::edgeScrollDirection(5,20,0,0,40,40,16).x==-1,"a tiny canvas shrinks the band and keeps a still middle");
    // The window rim (fullscreen: the screen edge over the browser, menus and status bar).
    auto rim=[](int x,int y) {const auto d=cart::windowRimDirection(x,y,1600,900);return std::pair{d.x,d.z};};
    check(rim(0,450)==std::pair{-1,0} && rim(1599,450)==std::pair{1,0} && rim(800,0)==std::pair{0,-1} && rim(800,899)==std::pair{0,1} && rim(0,0)==std::pair{-1,-1},"the window's outermost pixels scroll toward their edge");
    check(rim(1,450)==std::pair{0,0} && rim(800,1)==std::pair{0,0} && rim(-1,450)==std::pair{0,0} && rim(1600,450)==std::pair{0,0} && !cart::windowRimDirection(0,0,0,0).any(),"one pixel in, outside the window or no window: no rim scroll");
}

static void symmetryTests() {
    using cart::Xf;using cart::Mirror;
    // Facing: CRT angle a faces (sin a, cos a); 0 = south (+z), 90 = east (+x).
    check(cart::mirrorAngle(Xf::FlipX,90)==270 && cart::mirrorAngle(Xf::FlipX,0)==0,"left-right mirror turns east to west");
    check(cart::mirrorAngle(Xf::FlipZ,0)==180 && cart::mirrorAngle(Xf::FlipZ,90)==90,"top-bottom mirror turns south to north");
    check(cart::mirrorAngle(Xf::Rot180,45)==225 && cart::mirrorAngle(Xf::Transpose,0)==90 && cart::mirrorAngle(Xf::AntiTranspose,0)==270,"half turn and diagonals");
    check(cart::mirrorAngle(Xf::Rot90,90)==0 && cart::mirrorAngle(Xf::Rot270,0)==90,"clockwise quarter turn: east becomes south");
    for(int f=1;f<8;++f)for(int a=0;a<360;a+=15) {
        const auto [dx,dz]=cart::transformDirection(Xf(f),1000,0);(void)dx;(void)dz;
        const double r=a*3.14159265358979/180,s=std::sin(r),c=std::cos(r);
        // Apply the transform's linear part to the facing vector.
        const auto x=cart::transformDirection(Xf(f),1,0),z=cart::transformDirection(Xf(f),0,1);
        const double fx=s*x.first+c*z.first,fz=s*x.second+c*z.second;
        int expected=int(std::lround(std::atan2(fx,fz)*180/3.14159265358979))%360;if(expected<0)expected+=360;
        check(cart::mirrorAngle(Xf(f),a)==expected,"mirrored angle matches the transformed facing vector");
    }
    // Source regions and their images tile the map; only axis points repeat.
    for(int k=0;k<cart::kMirrorKinds;++k)for(int src=0;src<int(cart::mirrorSources(Mirror(k)).size());++src)
        for(int z=0;z<=24;++z)for(int x=0;x<=24;++x) {
            const cart::PointF p{x*.5,z*.5};
            int covered=cart::inSource(Mirror(k),src,p,12,12);
            for(Xf xf:cart::mirrorImages(Mirror(k)))covered+=cart::inSource(Mirror(k),src,cart::transform(cart::inverse(xf),p,12,12),12,12);
            check(covered>=1,"every point is a source point or the image of one");
            bool fixed=false;for(Xf xf:cart::mirrorImages(Mirror(k))) {const auto q=cart::transform(xf,p,12,12);fixed|=q.x==p.x && q.z==p.z;}
            const bool axisLine=Mirror(k)==Mirror::Quad && (p.x==6 || p.z==6);
            check(covered==1 || fixed || axisLine,"source and images overlap only on the axis");
        }
    // Footprints: a 3x2 structure occupies the mirrored cells (save-cell exact).
    cart::PlacedUnit keep;keep.type="KEEP";keep.footX=3;keep.footZ=2;keep.x=2*16+24;keep.z=5*16+16;keep.angle=90;keep.name="North keep";
    const auto flipped=cart::mirrorUnit(keep,Xf::FlipX,64,64);
    check(flipped && int((flipped->x-24)/16)==59 && int((flipped->z-16)/16)==5 && flipped->angle==270,"left-right mirror keeps the footprint inside the mirrored cells");
    const auto turned=cart::mirrorUnit(keep,Xf::Rot180,64,64);
    check(turned && int((turned->x-24)/16)==59 && int((turned->z-16)/16)==57,"half turn mirrors the footprint origin");
    const auto transposed=cart::mirrorUnit(keep,Xf::Transpose,64,64);
    check(transposed && std::abs(transposed->x/16-keep.z/16)<=.5 && std::abs(transposed->z/16-keep.x/16)<=.5 && transposed->footX==3,"non-square footprint keeps its shape, centred on the mirror");
    check(!cart::mirrorStart({1,0,5},Xf::FlipX,64,64) && cart::mirrorStart({1,1,5},Xf::FlipX,64,64)->xpos==63,"a start on the map edge mirrors off the map");
    check(cart::mirrorFeature(10,4,2,3,Xf::FlipX,64,64)==std::pair{52,4} && cart::mirrorFeature(10,4,2,3,Xf::FlipZ,64,64)==std::pair{10,57},"feature origin mirrors its footprint");
    tak::crt::Region region{"Base",2,3,5,10};
    const auto mirroredRegion=cart::mirrorRegion(region,Xf::FlipX,64,64),transposedRegion=cart::mirrorRegion(region,Xf::Transpose,64,64);
    check(mirroredRegion && mirroredRegion->x1==58 && mirroredRegion->x2==61 && mirroredRegion->z1==3 && mirroredRegion->z2==10,"region inclusive corners mirror");
    check(transposedRegion && transposedRegion->x1==3 && transposedRegion->x2==10 && transposedRegion->z1==2 && transposedRegion->z2==5,"region transposes");
    check(cart::uniqueRegionName({{"Base mirror",0,0,0,0}},"Base mirror")=="Base mirror 2","mirrored region names stay unique");
    // Section twins by direction tokens.
    const std::vector<std::string> cliffs={"1neclif1trans","1nwclif1trans","1seclif1trans","1swclif1trans","nclif1","sclif1","sclif4","eclif1","wclif1"};
    check(cart::sectionTwinName("1NEclif1trans","Coast Cliffs",cliffs,Xf::FlipX)=="1nwclif1trans","ne becomes nw");
    check(cart::sectionTwinName("1neclif1trans","Coast Cliffs",cliffs,Xf::Rot180)=="1swclif1trans","half turn: ne becomes sw");
    check(cart::sectionTwinName("sclif4","Coast Cliffs",cliffs,Xf::FlipX)=="sclif4","an invariant direction is its own twin");
    check(!cart::sectionTwinName("sclif4","Coast Cliffs",cliffs,Xf::FlipZ),"a missing twin is reported");
    check(cart::sectionTwinName("eclif1","Coast Cliffs",cliffs,Xf::Rot90)=="sclif1","quarter turn: east becomes south");
    const std::vector<std::string> roads={"hwye_n01","hwyn_w01","hwye_s01","hwys_w01","e_n01","n_w01","trans_e01","trans_s01","trans_w01","nee01","nwe01","y_n_lft","y_n_rgt"};
    check(cart::sectionTwinName("hwye_n01","Low Roads",roads,Xf::FlipX)=="hwyn_w01","connection sets are reordered");
    check(cart::sectionTwinName("e_n01","Low Roads",roads,Xf::FlipX)=="n_w01","underscore runs are sorted like the shipped names");
    check(cart::sectionTwinName("trans_e01","Low Roads",roads,Xf::Rot90)=="trans_s01","letters inside words are not directions");
    check(cart::sectionTwinName("nee01","Low Roads",roads,Xf::FlipX)=="nwe01","exterior suffix is kept");
    check(cart::sectionTwinName("y_n_lft","Low Roads",roads,Xf::FlipX)=="y_n_rgt","left/right words swap");
    // Letter order and readings, from the shipped Zhon/Taros/Creon names.
    const std::vector<std::string> stairs={"1enstair","1esstair","1nestair","1nwstair","1sestair","1swstair","1wnstair","1wsstair","1nex","1ne"};
    check(cart::sectionTwinName("1nestair","Low Plazas",stairs,Xf::FlipZ)=="1sestair" && cart::sectionTwinName("1enstair","Low Plazas",stairs,Xf::FlipZ)=="1esstair",
          "a stair keeps its side: the s of stair is not a direction");
    check(cart::sectionTwinName("1enstair","Low Plazas",stairs,Xf::Rot180)=="1wsstair","half turn keeps the stair side order");
    const std::vector<std::string> exterior={"nee01","nwe01","see01","swe01","ne01","nw01","se01","sw01"};
    check(cart::sectionTwinName("nwe01","Coast Cliffs",exterior,Xf::FlipX)=="nee01" && cart::sectionTwinName("swe01","Coast Cliffs",exterior,Xf::Rot180)=="nee01",
          "an exterior corner is not permuted back into itself");
    const std::vector<std::string> highways={"hwye_n01","hwye_s01","hwyn_w01","hwys_w01","hwye_w01","hwyn_s01"};
    check(cart::sectionTwinName("hwye_n01","Low Roads",highways,Xf::Rot180)=="hwys_w01","an anchored mid-word run turns with the rest");
    const std::vector<std::string> corners={"cornne","cornnw","cornse","cornsw","cornne_se","cornne_sw","cornnw_ne","cornnw_se","cornnw_sw","cornse_sw","cornall"};
    check(cart::sectionTwinName("cornnw_ne","Low Paths",corners,Xf::FlipZ)=="cornse_sw" && cart::sectionTwinName("cornse","Low Paths",corners,Xf::Rot180)=="cornnw",
          "the n of corn stays with its word");
    const std::vector<std::string> patches={"patchne","patchnw","patchse","patchsw","patchns","patchwe"};
    check(cart::sectionTwinName("patchne","Low Paths",patches,Xf::Rot180)=="patchsw","a run is not split when its first letter varies");
    const std::vector<std::string> gates={"ne_gate_e","ne_gate_n","nw_gate_n","nw_gate_w","se_gate_e","se_gate_s","sw_gate_s","sw_gate_w"};
    check(cart::sectionTwinName("ne_gate_e","Towns",gates,Xf::Rot180)=="sw_gate_w" && cart::sectionTwinName("ne_gate_n","Towns",gates,Xf::Rot90)=="se_gate_e",
          "the e of gate is not a direction");
    check(!cart::sectionTwinName("ariver01a","Low Rivers",{"ariver01a","ariver01b"},Xf::FlipX),"direction-free specials have no twin");
    check(cart::sectionTwinName("cobb_80","Low Flats",{"cobb_80"},Xf::Rot90)=="cobb_80","direction-free flat ground is its own twin");

    // Terrain attribution and twins on a synthetic library.
    // A rescan frees the cached prefabs, so the editor rebuilds its matcher.
    {tak::hpi::Vfs none;cart::SectionLibrary library;const auto generation=library.generation();library.scan(none,"zhon");
     check(library.generation()!=generation,"a section rescan is visible to the matcher's owner");}
    const auto ne=prefab(100,10),nw=prefab(200,40),river=prefab(300,70),flat=blankMap(2,2);
    std::vector<cart::SectionRef> refs{{"Cliffs","ne01","a"},{"Cliffs","nw01","b"},{"Rivers","river01","c"},{"Low Flats","flat01","d"}};
    const tak::tnt::Map* prefabs[]={&ne,&nw,&river,&flat};
    cart::SectionMatcher matcher;matcher.build(refs,[&](size_t i){return prefabs[i];});matcher.setKey("test");
    check(matcher.twin(0,Xf::FlipX)==1 && matcher.twin(1,Xf::FlipX)==0 && matcher.twin(2,Xf::FlipX)<0 && matcher.twin(3,Xf::FlipX)==3,"matcher resolves twins by index");
    auto map=blankMap(8,4);
    cart::stampSection(map,ne,0,0);cart::stampSection(map,river,0,2);
    map.tileKeys[size_t(3)*8+2]=999;   // an unknown tile at block (2,3)
    map.featureNames={"Tree","Rock"};map.features[size_t(1)*16+2]=0;map.features[size_t(1)*16+14]=1;   // Rock on the target side
    std::vector<tak::tnt::StartPos> starts{{1,4,4},{2,12,4},{3,5,6}};
    tak::crt::Scenario scenario;scenario.regions={{"Left camp",1,1,3,3},{"Right camp",10,1,12,3}};
    std::vector<cart::PlacedUnit> units(3);
    units[0].type="ARCH";units[0].player=0;units[0].x=3*16+8;units[0].z=2*16+8;units[0].angle=90;units[0].name="Hero";
    units[1].type="ARCH";units[1].player=1;units[1].x=13*16+8;units[1].z=2*16+8;   // replaced
    units[2].type="ARCH";units[2].player=0;units[2].x=7*16+8;units[2].z=7*16+8;units[2].footX=2;units[2].x=8*16;   // centred on the axis
    cart::SymmetryRequest request;request.kind=Mirror::Vertical;request.source=0;
    const auto report=cart::symmetrize(map,starts,scenario,units,request,{},&matcher);
    check(map.tileKeys[6]==200 && map.tileCols[6]==0 && map.tileCols[7]==1 && map.tileRows[size_t(8)+6]==1,"mirrored blocks take the twin section at the mirrored placement");
    check(map.heights[12]==40 && map.heights[size_t(3)*16+15]==40+3*4+3,"twin heights move with its tiles");
    check(map.features[13]==0xFFFB,"twin terrain markers come along");
    check(report.unmirroredBlocks==5 && report.terrainIssues.size()==1,"unknown tiles and twinless sections are counted");
    check(map.tileKeys[size_t(3)*8+5]==7,"unmirrored blocks are left unchanged");
    check(report.terrainIssues[0].message.find("river01")!=std::string::npos,"twinless section is named in the report");
    check(map.features[size_t(1)*16+14]==0xFFFF && map.features[size_t(1)*16+13]==0 && report.removedFeatures==1 && report.features==1,"target features replaced by mirrored copies");
    check(starts.size()==4 && starts[0].number==1 && starts[1].number==3 && starts[2].number==2 && starts[2].xpos==12 && starts[2].zpos==4 && starts[3].number==4 && starts[3].xpos==11,"source starts keep numbers; mirrors take the next free one");
    check(scenario.regions.size()==2 && scenario.regions[1].name=="Left camp mirror" && scenario.regions[1].x1==12 && scenario.regions[1].x2==14,"target region replaced by a uniquely named mirror");
    check(units.size()==3 && report.removedUnits==1,"target-side unit replaced, axis unit kept without a copy");
    const auto& copy=units.back();
    check(copy.name.empty() && units[0].name=="Hero" && copy.x==12*16+8 && copy.angle==270,"copies drop unique names and mirror facing");
    check(copy.player==1,"copy belongs to the owner of the mirrored start");
    auto map2=blankMap(8,4);std::vector<tak::tnt::StartPos> noStarts;std::vector<cart::PlacedUnit> noUnits;
    // Eight starts on one side leave no number for their mirrors.
    std::vector<tak::tnt::StartPos> crowded;for(int n=1;n<=8;++n)crowded.push_back({n,1+(n-1)%4,n<=4?1:3});
    tak::crt::Scenario empty;
    const auto full=cart::symmetrize(map2,crowded,empty,noUnits,request,{},nullptr);
    check(full.skippedStarts==8 && crowded.size()==8,"mirrored starts beyond eight are skipped");
    // Selection source: the image replaces an unselected unit under it.
    std::vector<cart::PlacedUnit> picked(2);
    picked[0].type="ARCH";picked[0].x=2*16+8;picked[0].z=8;picked[1].type="WALL";picked[1].x=13*16+8;picked[1].z=8;
    cart::SymmetryRequest selection;selection.kind=Mirror::Vertical;selection.source=-1;selection.units={0};selection.terrain=false;
    const auto picks=cart::symmetrize(map2,noStarts,empty,picked,selection,{},nullptr);
    check(picks.units==1 && picks.removedUnits==1 && picked.size()==2 && picked[1].type=="ARCH","selection images replace what they cover");

    // Live mirroring: add, move (no index shift), erase.
    std::vector<cart::PlacedUnit> live,before;
    cart::PlacedUnit tower;tower.type="TOWER";tower.x=2*16+8;tower.z=3*16+8;
    live.push_back(tower);
    check(cart::mirrorUnitEdits(before,live,Mirror::Vertical,{},16,8).copies==1 && live.size()==2 && live[1].x==13*16+8,"placing a unit places its mirror");
    before=live;live[0].x+=16;
    check(cart::mirrorUnitEdits(before,live,Mirror::Vertical,{},16,8).copies==0 && live.size()==2 && live[1].x==12*16+8,"moving a unit moves its mirror in place");
    auto dragged=live;dragged[0].x+=32;dragged[1].x+=32;   // a selection holding both partners
    check(cart::mirrorUnitEdits(live,dragged,Mirror::Vertical,{},16,8).copies==0 && dragged.size()==2 && dragged[1].x==14*16+8,"dragging a unit with its mirror adds no partner");
    before=live;live.erase(live.begin());
    cart::mirrorUnitEdits(before,live,Mirror::Vertical,{},16,8);
    check(live.empty(),"erasing a unit erases its mirror");
    auto fmap=blankMap(8,4);fmap.featureNames={"Tree"};auto fbefore=fmap.features;fmap.features[size_t(2)*16+1]=0;
    cart::mirrorFeatureEdits(fbefore,fmap.featureNames,fmap,Mirror::Quad,[](const std::string&){return std::pair{2,1};});
    check(fmap.features[size_t(2)*16+13]==0 && fmap.features[size_t(5)*16+1]==0 && fmap.features[size_t(5)*16+13]==0,"4-way mirror places three copies with mirrored footprints");
    fbefore=fmap.features;fmap.features[size_t(2)*16+1]=0xFFFF;
    cart::mirrorFeatureEdits(fbefore,fmap.featureNames,fmap,Mirror::Quad,[](const std::string&){return std::pair{2,1};});
    check(std::count(fmap.features.begin(),fmap.features.end(),uint16_t(0))==0,"erasing a feature erases its mirrors");
    std::vector<tak::tnt::StartPos> liveStarts{{1,3,3}},beforeStarts;
    cart::mirrorStartEdits(beforeStarts,liveStarts,Mirror::Rotate2,16,8);
    check(liveStarts.size()==2 && liveStarts[1].number==2 && liveStarts[1].xpos==13 && liveStarts[1].zpos==5,"a new start mirrors with the next number");
    beforeStarts=liveStarts;liveStarts[0].xpos=4;
    cart::mirrorStartEdits(beforeStarts,liveStarts,Mirror::Rotate2,16,8);
    check(liveStarts.size()==2 && liveStarts[1].xpos==12,"moving a start moves its mirror");

    // Terrain clipboard: copy, paste with/without objects, and its mirror.
    auto source=blankMap(8,4);cart::stampSection(source,ne,2,0);source.featureNames={"Tree"};source.features[size_t(1)*16+5]=0;
    std::vector<cart::PlacedUnit> sourceUnits{tower};sourceUnits[0].x=5*16+8;sourceUnits[0].z=16+8;sourceUnits[0].name="Watch";
    const auto clip=cart::copyTerrain(source,{2,0,4,2},sourceUnits,&matcher);
    check(clip.map.blocksX==2 && clip.units.size()==1 && clip.units[0].name.empty() && clip.attribution.size()==4 && clip.attribution[0].section==0,"copy captures tiles, objects and section placement");
    auto target=blankMap(8,4);target.featureNames={"Rock"};target.features[size_t(5)*16+9]=0;
    std::vector<cart::PlacedUnit> targetUnits;
    check(cart::pasteTerrain(target,targetUnits,clip,4,2,{false}).pasted && targetUnits.empty() && target.features[size_t(5)*16+9]==0 && target.tileKeys[size_t(2)*8+4]==100,"protected paste keeps objects and adds none");
    const auto pasted=cart::pasteTerrain(target,targetUnits,clip,4,2,{true});
    check(pasted.units==1 && target.featureNames[target.features[size_t(5)*16+9]]=="Tree" && target.heights[size_t(4)*16+8]==10,"paste with objects brings features, units and heights");
    std::string why;
    const auto mirroredClip=cart::mirrorClip(clip,Xf::FlipX,matcher,{},why);
    check(mirroredClip && mirroredClip->map.tileKeys[0]==200 && mirroredClip->units.size()==1 && mirroredClip->units[0].x==64-(5*16+8-64),"clipboard mirror uses twins");
    auto unknown=clip;unknown.attribution[1].section=-1;
    check(!cart::mirrorClip(unknown,Xf::FlipX,matcher,{},why) && why.find("not a known section")!=std::string::npos,"clipboard with an unknown block is not mirrored");
    std::cout<<"PASS: symmetry angles, regions, footprints, twins, Symmetrize, live mirroring and terrain clipboard\n";
}

// The BLOCKED brush on the feature plane: paint only empty cells, erase only
// blockers; features, roads and other markers are never touched. Clipping,
// gap-free strokes, the save / recovery round trip, undo, live symmetry and
// Symmetrize.
static void blockerTests(const std::filesystem::path& root) {
    auto map=blankMap(4,4);   // 8x8 cells
    map.featureNames={"Tree"};
    map.features[9]=0;        // (1,1) a named feature
    map.features[10]=0xFFFB;  // (2,1) a road
    map.features[11]=0xFFFC;  // (3,1) a section's blocker
    map.features[12]=0xFFFE;  // (4,1) a value only the game makes; kept as it is
    const auto original=map.features;
    check(cart::applyBlockers(map,0,0,7,7,cart::BlockOp::Paint)==60,"paint fills every empty cell");
    bool kept=true;
    for(size_t i=0;i<map.features.size();++i)kept&=i>=9 && i<=12?map.features[i]==original[i]:map.features[i]==0xFFFC;
    check(kept,"paint never touches a feature, a road, a blocker or another marker");
    check(cart::countBlocked(map)==61,"blocked cells are counted");
    check(cart::applyBlockers(map,7,7,0,0,cart::BlockOp::Erase)==61,"erase clears every blocker, painted or from a section");
    kept=true;
    for(size_t i=0;i<map.features.size();++i)kept&=map.features[i]==(i==9 || i==10 || i==12?original[i]:0xFFFF);
    check(kept,"erase never touches a feature, a road or another marker");
    check(cart::applyBlockers(map,-5,-3,1,0,cart::BlockOp::Paint)==2 && map.features[0]==0xFFFC && map.features[1]==0xFFFC,"a box past the map edge is clipped");
    check(cart::applyBlockers(map,8,8,20,20,cart::BlockOp::Paint)==0,"a box off the map changes nothing");
    const auto brush=cart::brushCells(5,5,4);
    check(brush.x0==4 && brush.x1==7 && brush.z0==4 && brush.z1==7 && cart::brushCells(5,5,1).x0==5 && cart::brushCells(5,5,1).x1==5,"brush cells centre on the pointer cell");
    // Strokes: every cell from one pointer position to the next, 4-connected.
    auto stroke=blankMap(8,4);   // 16x8 cells
    check(cart::applyBlockerLine(stroke,0,0,15,6,1,cart::BlockOp::Paint)==15+6+1 && stroke.features[0]==0xFFFC && stroke.features[size_t(6)*16+15]==0xFFFC,
          "a 1-cell stroke is a 4-connected wall between the two pointer cells");
    auto band=blankMap(8,4);
    cart::applyBlockerLine(band,2,2,14,2,4,cart::BlockOp::Paint);
    bool solid=true;for(int z=0;z<8;++z)for(int x=0;x<16;++x)solid&=(band.features[size_t(z)*16+x]==0xFFFC)==(x>=1 && x<=15 && z>=1 && z<=4);
    check(solid,"a 4x4 stroke paints a solid band, clipped at the map edge");
    // Save and recovery keep the plane exactly, roads and other markers included.
    map=blankMap(4,4);map.featureNames={"Tree"};map.features[9]=0;map.features[10]=0xFFFB;map.features[12]=0xFFFE;
    cart::applyBlockers(map,0,2,7,3,cart::BlockOp::Paint);
    tak::tnt::Scenario meta;tak::crt::Scenario scenario;
    const auto files=cart::documentFiles(map,meta,scenario,{},{},{},"Blocked");
    const auto tnt=std::find_if(files.members.begin(),files.members.end(),[](const auto& f){return f.path.ends_with(".tnt");});
    check(tnt!=files.members.end() && tak::tnt::Map::load(tnt->data).features==map.features,"blocked cells survive save and load");
    {
        auto recovery=cart::RecoveryFile::create(root);std::string error;
        check(cart::writeDocumentBundle(recovery->path(),files,error),error.c_str());
        const auto snapshot=cart::readRecoverySnapshot(recovery->path());
        check(tak::tnt::Map::load(snapshot.files->at(snapshot.mapPath)).features==map.features,"blocked cells survive a recovery snapshot");
        check(recovery->discard(error),error.c_str());
    }
    // One stroke is one undo step.
    cart::History history;history.reset(cart::historyState(map,meta,scenario,{},{},{},"Blocked"),true);
    const auto before=map.features;
    cart::applyBlockerLine(map,0,0,7,0,2,cart::BlockOp::Paint);
    check(history.commit(cart::historyState(map,meta,scenario,{},{},{},"Blocked")),"a stroke commits to the history");
    const auto* undone=history.undo();
    check(undone && tak::tnt::Map::load(undone->terrain).features==before,"undo restores the blocked cells");
    const auto* redone=history.redo();
    check(redone && tak::tnt::Map::load(redone->terrain).features==map.features,"redo paints them again");
    // Live symmetry: painted and erased cells mirror; a feature keeps its cell.
    auto mirror=blankMap(8,4);mirror.featureNames={"Rock"};mirror.features[size_t(2)*16+12]=0;   // on the image of (3,2)
    auto liveBefore=mirror.features;
    cart::applyBlockers(mirror,1,1,3,2,cart::BlockOp::Paint);
    const auto painted=cart::mirrorBlockerEdits(liveBefore,mirror,cart::Mirror::Vertical);
    check(painted.copies==5 && painted.skipped==1 && mirror.features[size_t(1)*16+14]==0xFFFC && mirror.features[size_t(2)*16+13]==0xFFFC &&
          mirror.features[size_t(2)*16+12]==0,"live symmetry paints the mirror cells and skips one holding a feature");
    liveBefore=mirror.features;
    cart::applyBlockers(mirror,1,1,3,2,cart::BlockOp::Erase);
    const auto erased=cart::mirrorBlockerEdits(liveBefore,mirror,cart::Mirror::Vertical);
    check(erased.copies==5 && cart::countBlocked(mirror)==0 && mirror.features[size_t(2)*16+12]==0,"erasing blocked cells erases their mirrors");
    auto quad=blankMap(8,4);liveBefore=quad.features;quad.features[size_t(1)*16+2]=0xFFFC;
    check(cart::mirrorBlockerEdits(liveBefore,quad,cart::Mirror::Quad).copies==3 && quad.features[size_t(1)*16+13]==0xFFFC &&
          quad.features[size_t(6)*16+2]==0xFFFC && quad.features[size_t(6)*16+13]==0xFFFC,"4-way symmetry paints three mirror cells");
    // Symmetrize: mirrored twin sections bring only their own markers, so the
    // source side's blocked cells are copied onto empty image cells.
    const auto ne=prefab(100,10),nw=prefab(200,40);
    std::vector<cart::SectionRef> refs{{"Cliffs","ne01","a"},{"Cliffs","nw01","b"}};
    const tak::tnt::Map* prefabs[]={&ne,&nw};
    cart::SectionMatcher matcher;matcher.build(refs,[&](size_t i){return prefabs[i];});matcher.setKey("blocked");
    auto sym=blankMap(8,4);cart::stampSection(sym,ne,0,0);
    sym.features[size_t(2)*16+1]=0xFFFC;   // painted inside the section
    sym.features[2]=0xFFFC;                 // its image (13,0) gets the twin's road
    std::vector<tak::tnt::StartPos> noStarts;std::vector<cart::PlacedUnit> noUnits;tak::crt::Scenario noScenario;
    cart::SymmetryRequest request;request.kind=cart::Mirror::Vertical;request.source=0;
    const auto report=cart::symmetrize(sym,noStarts,noScenario,noUnits,request,{},&matcher);
    check(report.blocks>0 && sym.tileKeys[7]==200 && sym.features[size_t(2)*16+14]==0xFFFC && report.blocked==1,"Symmetrize copies a painted blocker onto its twin terrain");
    check(sym.features[13]==0xFFFB && sym.features[2]==0xFFFC,"a twin's road keeps its cell");
    check(report.summary().find("1 blocked cell")!=std::string::npos,"the Symmetrize summary counts blocked cells");
    auto leave=blankMap(8,4);cart::applyBlockers(leave,2,3,4,3,cart::BlockOp::Paint);
    request.terrain=false;
    const auto objects=cart::symmetrize(leave,noStarts,noScenario,noUnits,request,{},nullptr);
    check(objects.blocked==3 && leave.features[size_t(3)*16+11]==0xFFFC && leave.features[size_t(3)*16+13]==0xFFFC,"Symmetrize without terrain copies blocked cells too");
    std::cout<<"PASS: BLOCKED brush paint/erase rules, clipping, strokes, save, recovery, undo, live symmetry and Symmetrize\n";
}
// AI profiles (root ai/<x>.txt): a .kmp's files re-save byte for byte, travel at
// the archive root, survive undo/redo and recovery, and export loose beside the
// map except into the install's Maps folder. Synthetic file contents only.
static void aiFileTests(const std::filesystem::path& root) {
    namespace fs=std::filesystem;
    auto bytes=[](std::string_view text){return std::vector<uint8_t>(text.begin(),text.end());};
    const auto map=blankMap(4,4);
    tak::tnt::Scenario meta;meta.aiProfile="islands";
    const auto n=bytes("// level file\r\nweight testarch 0\r\nlimit testarch 2"),          // CRLF, no final newline
               islands=bytes("weight all 0.5 # tail\n// caf\xc3\xa9 \xe2\x80\x94 note\nlimit fly 2\n"),
               notes=bytes("plan attack 3\n");
    check(cart::aiFileKey("ai/n.txt") && cart::aiFileKey("AI/Islands.TXT") && !cart::aiFileKey("ai/sub/x.txt") &&
          !cart::aiFileKey("kmap/x/ai/n.txt") && !cart::aiFileKey("ai/.txt") && !cart::aiFileKey("ai/n.tdf"),"AI file keys are root ai/<name>.txt only");
    // A hand-packed source map, member names in mixed case.
    const auto source=root/"Islands.kmp";
    {
        const auto base=cart::documentFiles(map,meta,{},{},{},{},"Islands");
        std::vector<tak::hpi::PackFile> members;
        for(auto member:base.members) {member.path="kmap/Islands/"+member.path;members.push_back(member);}
        members.push_back({"AI/N.txt",n});members.push_back({"ai/islands.txt",islands});members.push_back({"ai/Notes.txt",notes});
        members.push_back({"ai/sub/deep.txt",notes});members.push_back({"ai/empty.txt",{}});
        const auto packed=tak::hpi::pack(members);
        std::ofstream out(source,std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char*>(packed.data()),std::streamsize(packed.size()));
    }
    auto opened=cart::aiFilesFromArchive(tak::hpi::Archive(source));
    check(opened.size()==4 && opened.at("ai/n.txt")==n && opened.at("ai/islands.txt")==islands && opened.at("ai/notes.txt")==notes &&
          opened.count("ai/empty.txt"),"open keeps every root ai/ file, stray ones included, and nothing nested");
    const cart::AiFiles expected{{"ai/n.txt",n},{"ai/islands.txt",islands},{"ai/notes.txt",notes}};
    auto files=cart::documentFiles(map,meta,{},{},{},opened,"Islands");
    check(files.rootMembers.size()==3,"an empty AI file is dropped, never written");
    std::string error;
    const auto resaved=root/"Resaved.kmp";
    check(cart::writeDocumentBundle(resaved,files,error),error.c_str());
    {
        tak::hpi::Archive archive(resaved);bool profile=false;
        for(const auto& entry:archive.entries()) if(!entry.isDirectory) {
            const auto key=tak::hpi::MountSet::key(entry.path);
            check(!(key.starts_with("kmap/") && key.find("/ai/")!=std::string::npos),"AI files are not under kmap/");
            if(key.ends_with(".ota")) {const auto ota=archive.read(entry);profile=tak::tnt::Scenario::parse(std::string(ota.begin(),ota.end())).aiProfile=="islands";}
        }
        check(profile,"aiprofile= survives the save");
    }
    check(cart::aiFilesFromArchive(tak::hpi::Archive(resaved))==expected,"all three AI files re-save byte for byte");
    // Undo/redo.
    cart::History history;history.reset(cart::historyState(map,meta,{},{},{},expected,"Islands"),true);
    auto edited=expected;edited["ai/n.txt"]=bytes("weight testarch 5\n");edited.erase("ai/notes.txt");
    check(history.commit(cart::historyState(map,meta,{},{},{},edited,"Islands")) && history.dirty(),"an AI file edit is an undo step");
    check(history.undo()->aiFiles==expected && !history.dirty(),"undo restores the AI files");
    check(history.redo()->aiFiles==edited,"redo reapplies the AI file edit");
    // Recovery: written by the autosave serializer, lifted back out of the snapshot.
    {
        auto recovery=cart::RecoveryFile::create(root);
        auto autosave=cart::documentFiles(map,meta,{},{},{},expected,"Islands");
        autosave.members.push_back(cart::recoveryInfo("Islands",root));
        check(cart::writeDocumentBundle(recovery->path(),autosave,error),error.c_str());
        auto snapshot=cart::readRecoverySnapshot(recovery->path());
        check(cart::liftAiFiles(*snapshot.files)==expected,"recovery carries the AI files");
        for(const auto& [key,data]:*snapshot.files)check(!key.starts_with("ai/"),"lifted AI files leave the map files");
        check(snapshot.files->count(snapshot.mapPath) && snapshot.destination,"the rest of the recovery is intact");
        check(recovery->discard(error),error.c_str());
    }
    // Loose export: a sibling ai/ folder, read back by a loose open.
    const auto loose=root/"loose";fs::create_directory(loose);
    check(cart::writeDocumentLoose(loose,files,{},error),error.c_str());
    check(fs::is_regular_file(loose/"Islands.tnt") && cart::aiFilesFromFolder(loose)==expected,"loose export writes <dir>/ai/<x>.txt");
    check(cart::writeDocumentLoose(loose,files,{},error) && read(loose/"ai"/"n.txt.bak")==n,"a loose re-export keeps backups like the map files");
    const auto data=root/"install";fs::create_directories(data/"MAPS"/"mine");
    const auto maps=cart::installMapsFolder(data);
    check(maps==data/"MAPS","the install's Maps folder is found in any case");
    for(const auto& folder:{maps,maps/"mine"}) {
        check(!cart::writeDocumentLoose(folder,files,maps,error) && error.find("Save as .kmp")!=std::string::npos,"loose AI profiles are refused in the Maps folder");
        check(!fs::exists(folder/"Islands.tnt") && !fs::exists(folder/"ai"),"a refused loose export writes nothing");
    }
    check(cart::insideFolder(maps/"mine",maps) && !cart::insideFolder(root,maps) && !cart::insideFolder(data/"MAPS2",maps),"folder containment is by component");
    const auto plain=cart::documentFiles(map,meta,{},{},{},{},"Islands");
    check(cart::writeDocumentLoose(maps,plain,maps,error) && !fs::exists(maps/"ai"),"a map without AI profiles still exports into Maps");
    bool rejected=false;
    try {cart::documentFiles(map,meta,{},{},{},{{"ai/../x.txt",notes}},"Islands");} catch(const std::exception&) {rejected=true;}
    check(rejected,"an invalid AI file name cannot be saved");
    // By name: two installed maps carrying the same AI file names (date 0, as saved
    // here). Each opens with its own archive's files, not the mount's winner.
    {
        const auto clash=root/"clash";fs::create_directories(clash/"Maps");
        for(const char* name:{"Clash A","Clash B"}) {
            auto own=cart::documentFiles(map,meta,{},{},{},{{"ai/n.txt",bytes(std::string("// ")+name+"\n")},{"ai/islands.txt",bytes(name)}},name);
            check(cart::writeDocumentBundle(clash/"Maps"/(std::string(name)+".kmp"),own,error),error.c_str());
        }
        fs::create_directories(clash/"Maps"/"ai");std::ofstream(clash/"Maps"/"ai"/"h.txt")<<"limit x 0\n";
        const auto vfs=tak::hpi::mountRetailRoot(clash);
        for(const char* name:{"Clash A","Clash B"}) {
            const auto path=tak::hpi::findMap(vfs,name);
            const cart::AiFiles want{{"ai/n.txt",bytes(std::string("// ")+name+"\n")},{"ai/islands.txt",bytes(name)}};
            check(!path.empty() && cart::aiFilesFromMapArchive(vfs,path)==want,"an installed map opens by name with its own archive's AI files");
        }
        tak::hpi::Vfs view(&vfs);view.setMapFiles(std::make_shared<tak::hpi::Vfs::Files>(tak::hpi::Vfs::Files{{"kmap/x.tnt",{1}},{"ai/e.txt",{2}}}));
        check(cart::aiFilesFromMapArchive(view,"kmap/x.tnt")==cart::AiFiles{{"ai/e.txt",{2}}},"a view's own map files are its archive");
        check(cart::aiFilesFromMapArchive(tak::hpi::Vfs(&vfs,true),tak::hpi::findMap(vfs,"Clash A")).empty(),"a view without map resources has none");
    }
    std::cout<<"PASS: AI profiles re-save byte for byte at the .kmp root, undo/redo, recovery, loose ai/ export, the Maps-folder refusal and by-name opens of clashing maps\n";
}
int main() {
    namespace fs=std::filesystem;
    auto root=fs::temp_directory_path()/("tak-cartographer-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    try {
        auto lease=cart::RecoveryFile::create(root);const auto snapshot=lease->path();
        std::ofstream(snapshot)<<"snapshot";
        check(!cart::RecoveryFile::claimNewest(root),"live session cannot be recovered");
        lease.reset();
        lease=cart::RecoveryFile::claimNewest(root);
        check(lease && lease->path()==snapshot,"abandoned session can be recovered");
        check(!cart::RecoveryFile::claimNewest(root),"claimed recovery stays exclusive");
        std::string recoveryError;check(lease->discard(recoveryError),"discard claimed recovery");lease.reset();
        check(!fs::exists(snapshot.parent_path()),"empty session directory cleaned");
        const auto legacy=root/"recovery-old.kmp";std::ofstream(legacy)<<"legacy";
        lease=cart::RecoveryFile::claimNewest(root);check(lease && lease->path()==legacy,"legacy recovery supported");
        check(lease->discard(recoveryError),"discard legacy recovery");lease.reset();
        const auto destination=root/tak::u8path("été maps");
        const auto info=cart::recoveryInfo("Recovered map",destination);
        const auto decoded=cart::readRecoveryInfo(info.data);
        check(decoded && decoded->name=="Recovered map" && tak::u8path(decoded->directory)==destination,"recovery destination UTF-8 roundtrip");
        check(!cart::readRecoveryInfo({1,2,3}),"bad recovery metadata rejected");
        cart::EditorPreferences preferences;preferences.guideSeen=true;preferences.tracePlaytest=true;preferences.width=1440;preferences.height=900;preferences.scalePercent=125;preferences.showFeatures=false;preferences.showUnits=false;preferences.showStarts=false;preferences.showRegions=false;preferences.showGrid=true;preferences.showBlocked=true;
        for(int i=0;i<20;++i)preferences.remember("Map "+std::to_string(i));
        preferences.remember("Carte été \"quote\".kmp");preferences.remember("Map 19");
        check(preferences.recent.size()==12 && preferences.recent[0]=="Map 19","recent maps bounded and deduplicated");
        std::string prefsError;check(cart::saveEditorPreferences(root,preferences,prefsError),prefsError.c_str());
        const auto loadedPreferences=cart::loadEditorPreferences(root);
        check(loadedPreferences.tracePlaytest && loadedPreferences.guideSeen && loadedPreferences.width==1440 && loadedPreferences.height==900 && loadedPreferences.scalePercent==125 && loadedPreferences.recent==preferences.recent,"window, scale and Unicode recent paths roundtrip");
        check(!loadedPreferences.showFeatures && !loadedPreferences.showUnits && !loadedPreferences.showStarts && !loadedPreferences.showRegions && loadedPreferences.showGrid && loadedPreferences.showBlocked,"layer preferences roundtrip");
        check(!loadedPreferences.edgeScroll && !loadedPreferences.fullscreen,"edge scrolling and fullscreen default off");
        {
            auto display=preferences;display.edgeScroll=display.fullscreen=true;
            check(cart::saveEditorPreferences(root,display,prefsError),prefsError.c_str());
            const auto loaded=cart::loadEditorPreferences(root);
            check(loaded.edgeScroll && loaded.fullscreen && loaded.width==1440 && loaded.height==900,"edge scrolling, fullscreen and windowed size roundtrip");
            std::ofstream(root/"preferences.txt")<<"edgescroll 2\nfullscreen yes\n";
            const auto rejected=cart::loadEditorPreferences(root);
            check(!rejected.edgeScroll && !rejected.fullscreen,"malformed display preferences keep the defaults");
            check(!rejected.showBlocked,"blocked cells are hidden by default");
            auto wl=preferences;wl.displayMode=4;wl.displayScale=1.45f;wl.displayX11Width=7680;wl.displayLogicalWidth=5297;
            check(cart::saveEditorPreferences(root,wl,prefsError),prefsError.c_str());
            const auto wlLoaded=cart::loadEditorPreferences(root);
            check(wlLoaded.displayMode==4 && std::abs(wlLoaded.displayScale-1.45f)<.001f && wlLoaded.displayX11Width==7680 &&
                  wlLoaded.displayLogicalWidth==5297,"display kind, scale and desktop widths roundtrip");
            std::ofstream(root/"preferences.txt")<<"display wayland 1.45\n";
            const auto older=cart::loadEditorPreferences(root);
            check(older.displayMode==3 && older.displayX11Width==0 && older.displayLogicalWidth==0,"display row without widths still loads");
        }
        {
            auto groups=preferences;groups.sectionGroups={{"Zhon","folder:coast cliffs sandy"},{"Aramon","kind:Water & coast"}};
            check(cart::saveEditorPreferences(root,groups,prefsError),prefsError.c_str());
            check(cart::loadEditorPreferences(root).sectionGroups==groups.sectionGroups,"section browser group per world roundtrip");
        }
        tak::tnt::Map map;map.width=map.height=32;map.blocksX=map.blocksY=16;
        map.heights.resize(1024,60);map.features.resize(1024,0xffff);
        map.tileKeys.resize(256);map.tileCols.resize(256);map.tileRows.resize(256);
        auto featureMap=map;featureMap.featureNames={"Tree","Rock"};featureMap.features[33]=0;featureMap.features[34]=1;
        featureMap.features[35]=0xFFFB;
        cart::FeatureSelection features;features.box(featureMap,0,0,5,5,false);
        check(features.cells==std::set<int>{33,34},"feature box excludes terrain markers");
        const auto originalFeatures=featureMap.features;
        check(!features.move(featureMap,1,0) && featureMap.features==originalFeatures,"group cannot overwrite terrain marker");
        check(features.move(featureMap,0,1) && featureMap.features[65]==0 && featureMap.features[66]==1 && featureMap.features[33]==0xFFFF,"group move preserves types and spacing");
        check(features.move(featureMap,1,0) && featureMap.features[66]==0 && featureMap.features[67]==1 && featureMap.features[65]==0xFFFF,"overlapping selected cells move without losing a feature");
        features.copy(featureMap);check(features.remove(featureMap),"delete selected features");
        check(featureMap.features[35]==0xFFFB,"feature deletion preserves terrain markers");
        featureMap.featureNames={"Rock","Tree"};
        check(features.paste(featureMap,5,5) && featureMap.features[165]==1 && featureMap.features[166]==0,"clipboard resolves feature names across map tables");
        const auto beforeRejected=featureMap.features;
        check(!features.paste(featureMap,5,5) && featureMap.features==beforeRejected,"paste collision is atomic");
        check(!features.paste(featureMap,31,31) && featureMap.features==beforeRejected,"off-map paste is atomic");
        auto painted=map,prefab=map;
        painted.featureNames={"Tree"};painted.features[0]=0;painted.features[1]=0xFFFB;
        prefab.featureNames={"Rock"};prefab.features[0]=0xFFFF;prefab.features[1]=0;prefab.features[2]=0xFFFB;
        prefab.heights.assign(1024,90);prefab.tileKeys.assign(256,42);
        check(cart::stampSection(painted,prefab,0,0,{false}),"protected terrain stamp succeeds");
        check(painted.features[0]==0 && painted.featureNames==std::vector<std::string>{"Tree"},"protected stamp keeps existing object and adds no prefab names");
        check(painted.features[1]==0xFFFF && painted.features[2]==0xFFFB,"protected stamp updates terrain markers without adding objects");
        check(painted.heights[0]==90 && painted.tileKeys[0]==42,"terrain art and heights change together");
        check(cart::stampSection(painted,prefab,0,0),"normal terrain stamp succeeds");
        check(painted.features[0]==0xFFFF && painted.featureNames[painted.features[1]]=="Rock","normal stamp replaces/remaps objects");
        auto untouched=painted.features;
        check(!cart::stampSection(painted,prefab,16,16,{false}) && painted.features==untouched,"off-map protected stamp does nothing");
        tak::tnt::Scenario meta;meta.kingdom="zhon";
        meta.generatorRecipe="~gen1~test-metadata";
        check(tak::tnt::Scenario::parse(meta.write()).generatorRecipe==meta.generatorRecipe,"generator metadata survives OTA roundtrip");
        for(const std::string text : {"First line\nSecond line", "\r\n\t", "  Leading and trailing ; ", "https://example.test/{map}", "été 水 ; test", "}\n[Other]\n{\nkey=value;\n}"}) {
            auto rich=meta;rich.missionDescription=text;rich.missionName=text;
            const auto encoded=rich.write();const auto decoded=tak::tnt::Scenario::parse(encoded);
            check(decoded.missionName==text && decoded.missionDescription==text,"OTA free text roundtrip without interpreting syntax");
            const auto extension=encoded.find("[TAKText]");
            const auto legacy=tak::tnt::Scenario::parse(encoded.substr(0,extension));
            check(legacy.kingdom=="zhon" && legacy.missionDescription.find('\n')==std::string::npos,"legacy metadata remains parseable with one-line description");
        }
        check(meta.write().find("[TAKText]")==std::string::npos,"ordinary OTA output needs no text extension");
        check(tak::tnt::Scenario::parse(meta.write()+"[TAKText]\n{\nmissiondescription=zz;\n}\n").missionDescription.empty(),"invalid text extension ignored");
        {
            auto files=std::make_shared<tak::hpi::Vfs::Files>();
            (*files)["kmap/damaged.crt"]={0,1,2};
            tak::hpi::Vfs vfs;vfs.setMapFiles(files);
            bool rejected=false;
            try {(void)cart::loadScenario(vfs,"kmap/damaged.crt");}
            catch(const std::exception&) {rejected=true;}
            check(rejected,"damaged scenario must not silently become an empty editable map");
            check(cart::loadScenario(vfs,"kmap/absent.crt").version==1,"optional absent scenario remains supported");
        }
        tak::crt::Scenario scenario;scenario.players.resize(9);
        tak::crt::RuleGroup group;group.conditions.push_back({});group.actions.push_back({});
        scenario.players[0].push_back(group);
        const auto unnamedCrt=tak::crt::write(scenario);
        scenario.players[0][0].editorName="Defend the bridge — été";
        check(tak::crt::write(scenario)==unnamedCrt,"editor names do not change retail CRT bytes");
        const auto names=cart::scenarioInfo(scenario);auto named=tak::crt::parse(unnamedCrt);
        check(cart::applyScenarioInfo(named,names) && named.players[0][0].editorName==scenario.players[0][0].editorName,"Unicode rule names roundtrip in sidecar");
        check(!cart::applyScenarioInfo(named,"TAK_EDITOR_RULE_NAMES 1\n99 0 \"bad\"\n") && named.players[0][0].editorName==scenario.players[0][0].editorName,"invalid rule metadata is atomic");
        cart::History namedHistory;namedHistory.reset(cart::historyState(map,meta,scenario,{}, {},{},"Review"),true);
        scenario.players[0][0].editorName="Victory";namedHistory.commit(cart::historyState(map,meta,scenario,{}, {},{},"Review"));
        check(cart::applyScenarioInfo(named,namedHistory.undo()->editorMetadata) && named.players[0][0].editorName=="Defend the bridge — été","rule name undo restores metadata");
        check(cart::applyScenarioInfo(named,namedHistory.redo()->editorMetadata) && named.players[0][0].editorName=="Victory","rule name redo restores metadata");
        auto files=cart::documentFiles(map,meta,scenario,{}, {},{},"Review");
        std::string error;
        check(cart::writeDocumentFiles(root,files.members,error),error.c_str());
        auto crt=tak::crt::parse(read(root/"Review.crt"));
        tak::hpi::Vfs namedVfs;auto namedFiles=std::make_shared<tak::hpi::Vfs::Files>();
        (*namedFiles)["review.crt"]=read(root/"Review.crt");(*namedFiles)["review.editor"]=read(root/"Review.editor");namedVfs.setMapFiles(namedFiles);
        check(cart::loadScenario(namedVfs,"review.crt").players[0][0].editorName=="Victory","normal scenario loading restores names");
        check(crt.players.size()==9 && crt.players[0].size()==1,"rule-only scenario saved");
        auto ota=read(root/"Review.ota");
        check(tak::tnt::Scenario::parse(std::string(ota.begin(),ota.end())).hasScenario,"scenario metadata follows content");
        check(!meta.hasScenario,"saving does not mutate document metadata");
        check(cart::writeDocumentBundle(root/"Review.kmp",files,error),error.c_str());
        const auto original=read(root/"Review.kmp");
        {
            auto recovery=cart::RecoveryFile::create(root);const auto file=recovery->path();
            check(cart::writeDocumentBundle(file,files,error),error.c_str());
            const auto recovered=cart::readRecoverySnapshot(file);
            check(!recovered.fromBackup && recovered.mapPath.ends_with("review.tnt"),"valid recovery is preferred");
            fs::copy_file(file,fs::path(file.string()+".bak"));
            std::ofstream(file,std::ios::binary|std::ios::trunc)<<"HAPI";
            check(cart::readRecoverySnapshot(file).fromBackup,"truncated archive falls back to previous snapshot");
            check(read(file)==std::vector<uint8_t>({'H','A','P','I'}),"fallback preserves damaged primary for diagnosis");
            fs::remove(file);recovery.reset();
            recovery=cart::RecoveryFile::claimNewest(root);
            check(recovery && recovery->path()==file && cart::readRecoverySnapshot(file).fromBackup,"backup-only abandoned session is recoverable");
            check(recovery->discard(error),error.c_str());
            auto broken=files;
            for(auto& member:broken.members)if(member.path.ends_with(".crt"))member.data={1,2,3};
            check(cart::writeDocumentBundle(file,broken,error),error.c_str());
            bool rejected=false;try {cart::readRecoverySnapshot(file);}catch(const std::exception&) {rejected=true;}
            check(rejected && fs::exists(file),"damaged scenario rejected without deleting snapshot");
            broken=files;
            for(auto& member:broken.members)if(member.path.ends_with(".tnt"))member.data={1,2,3};
            check(cart::writeDocumentBundle(file,broken,error),error.c_str());
            rejected=false;try {cart::readRecoverySnapshot(file);}catch(const std::exception&) {rejected=true;}
            check(rejected && fs::exists(file.string()+".bak"),"damaged terrain and scenario generations remain available for diagnosis");
            check(recovery->discard(error),error.c_str());
            const auto backup=fs::path(file.string()+".bak");fs::create_directory(backup);std::ofstream(backup/"unrelated")<<"keep";
            check(!recovery->discard(error) && !error.empty() && fs::exists(backup/"unrelated"),"cleanup failure preserves unrelated contents and reports error");
            fs::remove_all(backup);
        }
        tak::hpi::Archive openArchive(root/"Review.kmp"); // editor may retain a mapped archive while saving over it
        const auto oldEntry=*std::find_if(openArchive.entries().begin(),openArchive.entries().end(),[](const auto& entry){return !entry.isDirectory;});const auto oldBytes=openArchive.read(oldEntry);
        scenario.players[0].clear();
        files=cart::documentFiles(map,meta,scenario,{}, {},{},"Review");
        check(cart::writeDocumentBundle(root/"Review.kmp",files,error),error.c_str());
        check(read(root/"Review.kmp.bak")==original,"previous bundle retained");
        check(openArchive.read(oldEntry)==oldBytes,"open archive remains a stable snapshot after replacement");
        check(cart::writeDocumentFiles(root,files.members,error),error.c_str());
        check(tak::crt::parse(read(root/"Review.crt")).players[0].empty(),"deletion replaces stale scenario");
        check(tak::crt::parse(read(root/"Review.crt.bak")).players[0].size()==1,"loose backup preserved");
        const auto unicodeDir=root/std::filesystem::path(u8"Cartes été");fs::create_directory(unicodeDir);
        const auto unicodePath=unicodeDir/std::filesystem::path(u8"Île.kmp");
        check(cart::writeDocumentBundle(unicodePath,files,error),error.c_str());
        check(fs::file_size(unicodePath)>0,"UTF-8 destination directory and filename save");
        const auto saved=read(root/"Review.tnt");
        fs::create_directory(root/"blocked");
        check(!cart::writeDocumentFiles(root,{{"Review.tnt",{1,2,3}},{"blocked",{4}}},error),"invalid destination must fail");
        check(read(root/"Review.tnt")==saved,"failed export preserves previous file");
        // Fail after staging all members, while publishing their backups.
        // Originals must still form a complete document and scratch files vanish.
        const auto backupFailure=root/"backup-failure";fs::create_directory(backupFailure);
        const std::vector<tak::hpi::PackFile> previous{{"a.tnt",{1,2,3}},{"a.ota",{4,5,6}}};
        check(cart::writeDocumentFiles(backupFailure,previous,error),error.c_str());
        fs::create_directory(backupFailure/"a.ota.bak");
        std::ofstream(backupFailure/"a.ota.bak"/"keep")<<"unrelated";
        check(!cart::writeDocumentFiles(backupFailure,{{"a.tnt",{9}},{"a.ota",{8}}},error),"backup publication failure must be reported");
        for(const auto& member:previous)check(read(backupFailure/member.path)==member.data,"backup failure changed an original document member");
        check(fs::exists(backupFailure/"a.ota.bak"/"keep"),"backup failure removed unrelated content");
        for(const auto& entry:fs::directory_iterator(backupFailure))
            check(!entry.path().filename().string().starts_with(".tak-save-"),"failed save leaked staging files");
        check(!cart::writeDocumentFiles(root,{{"../escape",{1}}},error),"reject traversal");
        check(!cart::writeDocumentFiles(root,{{"duplicate",{1}},{"duplicate",{2}}},error),"reject duplicate members");
        check(!cart::writeDocumentBundle(root/"missing"/"Review.kmp",files,error),"missing folder fails");
        check(cart::validDocumentName("My Map") && !cart::validDocumentName("bad/name"),"document names");
        std::vector<cart::PlacedUnit> selectionUnits(3);
        selectionUnits[0].x=20;selectionUnits[0].z=30;selectionUnits[0].name="Named";
        selectionUnits[0].health=73;selectionUnits[0].player=4;
        selectionUnits[1].x=60;selectionUnits[1].z=50;
        selectionUnits[2].x=90;selectionUnits[2].z=90;
        cart::UnitSelection selection;
        selection.box(selectionUnits,65,55,15,25,false);
        check(selection.indices==std::set<int>({0,1}),"reversed box selects contained units");
        selection.beginDrag(selectionUnits,20,30);
        check(selection.drag(selectionUnits,-100,500,100,100),"group drag moves");
        check(selectionUnits[0].x==8 && selectionUnits[1].x==48 && selectionUnits[1].z==92,
              "group drag clamps together and preserves spacing");
        selection.copy(selectionUnits);
        check(selection.paste(selectionUnits,95,95,100,100),"paste group near edge");
        check(selectionUnits.size()==5 && selectionUnits[3].name.empty() && selectionUnits[3].health==73 && selectionUnits[3].player==4,
              "paste preserves properties without duplicating trigger names");
        check(selectionUnits[4].x-selectionUnits[3].x==40 && selectionUnits[4].z-selectionUnits[3].z==20,
              "paste preserves relative placement");
        check(selection.remove(selectionUnits) && selectionUnits.size()==3 && selection.indices.empty(),"delete selected group only");
        check(!selection.paste(selectionUnits,0,0,30,30),"reject group too large for map");
        selectionUnits[1].x=200;selection.indices={0,1};selection.beginDrag(selectionUnits,0,0);
        check(!selection.drag(selectionUnits,50,50,100,100),"oversize selection cannot distort group");
        cart::History history;
        auto initial=cart::historyState(map,meta,scenario,{}, {},{},"Review");
        history.reset(initial,true);
        auto changed=initial;changed.terrain[500]^=7;changed.metadata+="changed";
        changed.scenario.resize(changed.scenario.size()+500,17);
        check(history.commit(changed) && history.dirty(),"history captures an edit");
        check(history.retainedBytes()<initial.terrain.size()*2,"small edits retain deltas");
        check(history.undo() && *history.redo()==changed,"redo restores all fields");
        check(*history.undo()==initial && !history.dirty(),"undo returns to saved state");
        check(history.redo()!=nullptr,"redo exists");history.markSaved();
        check(history.undo()!=nullptr && history.dirty(),"undo away from saved revision is dirty");
        auto branch=initial;branch.name="Branch";history.commit(branch);
        check(!history.canRedo() && history.dirty(),"branch invalidates redo and stays dirty");
        std::mt19937 random(47);std::vector<cart::HistoryState> expected{branch};
        for(int n=0;n<80;++n) {
            auto next=expected.back();next.terrain.resize(200+random()%900,0);
            for(int j=0;j<30;++j) next.terrain[random()%next.terrain.size()]=uint8_t(random());
            next.scenario.resize(random()%350,uint8_t(n));next.name=std::to_string(n);
            history.commit(next);expected.push_back(next);
        }
        for(size_t i=expected.size()-1;i>0;--i)check(*history.undo()==expected[i-1],"variable-length delta undo");
        for(size_t i=1;i<expected.size();++i)check(*history.redo()==expected[i],"variable-length delta redo");
        cart::History bounded(2500);bounded.reset(initial,true);
        for(int i=0;i<20;++i) {auto state=initial;state.name=std::to_string(i);bounded.commit(state);}
        check(bounded.retainedBytes()<2500,"history respects memory budget");
        symmetryTests();
        blockerTests(root);
        aiFileTests(root);
        scrollingTests();
        // Section browser naming: retail names decompose into kind/direction/corner
        // and numbered or lettered variants collapse into one piece.
        {
            auto name=[](const char* raw,bool strip=false){return cart::parseSectionName(raw,strip);};
            auto cliff=name("1neclif2trans");
            check(cliff.kind=="Cliff" && cliff.direction=="NE" && cliff.corner=="outer" && cliff.variant=="2" && cliff.key=="1neclif#trans","Zhon corner cliff");
            auto trans=name("2swtrans1x");check(trans.kind=="Transition" && trans.direction=="SW" && trans.corner=="inner" && trans.variant=="1","inner transition");
            auto road=name("hwy_e_n_s01_200");check(road.kind=="Road" && road.direction=="E+N+S" && road.variant=="1","road junction, elevation suffix dropped");
            check(name("e_n_s_02").key==name("e_n_s01").key,"stray underscore before a variant number");
            auto river=name("ariver01e_512x_80",true);check(river.kind=="River" && river.direction.empty() && river.number=="1" && river.tag=="e","river letter variant after footprint");
            check(name("nswe1").direction=="N+S+W+E" && name("ene").direction=="ENE" && name("nee01").direction=="NEE","compact and 16-point directions");
            auto mouth=name("SMOUTH");check(mouth.kind=="Mouth" && mouth.direction=="S","direction before a word");
            check(name("sett_ne01_200").kind=="Settlement" && name("sett_ne01_200").direction=="NE","settlement is a word, not a direction");
            auto water=name("h2o1ne2");check(water.kind=="Water" && water.direction=="NE" && water.corner=="outer" && water.variant=="2","h2o prefix");
            auto top=name("mttp_1nse2");check(top.kind=="Mountain top" && top.direction=="N+S+E" && top.corner=="outer","mountain top junction");
            check(name("savannah12").direction.empty() && name("savannah12").number=="12" && name("savannah12").variant.empty(),"undirected specials keep their number");
            for(auto [folder,kind]:{std::pair{"Coast Cliffs Jungle","Cliffs"},{"Low Jungle to High Sandy","Transitions"},{"Coast Towns","Towns & forts"},
                {"Coast Walkways","Paths & roads"},{"Sea Berms","Water & coast"},{"high tile breakers","Specials & details"},{"flatties","Flats"},
                {"snow line","Transitions"},{"High Sinkholes","Hills & mountains"},{"walls","Cliffs"},{"Mystery","Other"}})
                check(cart::sectionKindGroup(folder)==kind,(std::string("kind group for ")+folder).c_str());
            std::vector<cart::SectionRef> refs;
            for(auto [folder,names]:{std::pair<const char*,std::vector<const char*>>{"Coast Cliffs Sandy",{"nclif1","nclif2","nclif3","1neclif1","1neclif2"}},
                {"Low Rivers",{"ariver01a_512x_80","ariver01b_512x_80","ariver02a_512x_80"}},{"Low Sandy to High Savannah",{"1netrans1a","1netrans2a"}},
                {"Low Plazas Jungle",{"1ne","1nex"}},{"walls",{"1ne2","1ne1lit","1ne1"}},{"Low Canals",{"BRIDGENS","bridgens2"}}})
                for(const char* n:names)refs.push_back({folder,n,std::string("sections/x/")+folder+"/"+n+".tnt"});
            cart::SectionCatalog catalog;catalog.build(refs);
            auto index=[&](const char* n){for(size_t i=0;i<refs.size();++i)if(refs[i].name==n)return int(i);throw std::runtime_error(std::string("missing ")+n);};
            auto variants=[&](const char* n){return catalog.pieces()[size_t(catalog.pieceOf(index(n)))].variants;};
            check(variants("nclif2").size()==3 && catalog.label(index("nclif2"))=="Cliff \xc2\xb7 N \xc2\xb7 variant 2","numbered cliff variants share a piece");
            check(catalog.label(index("1neclif2"))=="Cliff \xc2\xb7 NE outer \xc2\xb7 variant 2" && catalog.shortLabel(index("1neclif2"))=="Cliff NE out","corner label");
            check(variants("ariver01b_512x_80").size()==2 && catalog.pieceOf(index("ariver02a_512x_80"))!=catalog.pieceOf(index("ariver01a_512x_80")),"river shapes split, letters are variants");
            check(catalog.label(index("ariver01b_512x_80"))=="River \xc2\xb7 #1 \xc2\xb7 variant b","river label");
            check(variants("1netrans2a").size()==2 && catalog.variantText(index("1netrans2a"))=="2","constant tileset letter is not a variant");
            check(variants("1nex").size()==2,"plaza x alternate");
            const auto walls=variants("1ne2");
            check(walls.size()==3 && walls[0]==index("1ne1") && walls[1]==index("1ne1lit") && walls[2]==index("1ne2"),"lit variant ordered beside its number");
            check(variants("BRIDGENS").size()==2,"unnumbered piece joins numbered sibling");
            const auto& groups=catalog.groups();
            check(groups.front().id.empty() && groups.front().sections==int(refs.size()),"All group first with every section");
            check(groups.size()>2 && groups[1].id=="kind:Cliffs" && groups[2].id=="folder:coast cliffs sandy" && groups[2].depth==1,"kind followed by its folders");
            const auto* coast=catalog.group("kind:Water & coast");
            check(coast && coast->sections==5 && catalog.group("folder:low rivers")->pieces.size()==2,"group counts");
            check(catalog.searchText(index("1neclif2")).find("1neclif2")!=std::string::npos && catalog.searchText(index("1neclif2")).find("cliff ")!=std::string::npos,"search sees raw and readable names");
        }
        fs::remove_all(root);
        std::cout<<"PASS: rule-only saves, metadata, stale deletion, backups, failed writes, names, section browser naming\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';fs::remove_all(root);return 1;}
}
