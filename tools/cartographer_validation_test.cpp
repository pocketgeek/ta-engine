#include "cartographer/validation.h"
#include "cartographer/triggers.h"
#include "cartographer/regions.h"
#include "cartographer/generator.h"
#include "cartographer/overlay.h"
#include "cartographer/ruleedit.h"
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "tnt/ota.h"
#include <iostream>
#include <fstream>
#include <chrono>
#include <stdexcept>
static void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
int main() {
    try {
        cart::RegionDrag drag;std::vector<tak::crt::Region> regions{{"Region 1",10,10,20,20}};
        check(drag.begin(regions,-1,30,30,.5f,true,64,64),"begin region drawing");
        drag.update(25,22,64,64);
        check(drag.preview.name=="Region 2" && drag.preview.x1==25 && drag.preview.z1==22 && drag.preview.x2==30 && drag.preview.z2==30,"reversed region drawing and unique default name");
        drag.cancel();check(regions.size()==1 && !drag.active(),"cancel leaves source regions unchanged");
        check(drag.begin(regions,0,15,15,.5f,false,64,64) && drag.part==cart::RegionDrag::Move,"region body starts move");
        drag.update(-100,1000,64,64);
        check(drag.preview.x1==0 && drag.preview.x2==10 && drag.preview.z1==53 && drag.preview.z2==63,"region move clamps without changing size");
        check(drag.begin(regions,0,20.5f,20.5f,1,false,64,64),"pick corner with grab offset");
        drag.update(20.5f,20.5f,64,64);
        check(drag.preview.x2==20 && drag.preview.z2==20,"click near resize handle does not alter bounds");
        drag.update(24.5f,23.5f,64,64);
        check(drag.preview.x2==24 && drag.preview.z2==23 && drag.preview.x1==10,"corner drag changes both dimensions");
        drag.update(-100,-100,64,64);
        check(drag.preview.x2==10 && drag.preview.z2==10,"resize cannot invert region");
        check(!drag.begin(regions,-1,-.1f,3,.5f,true,64,64),"negative canvas coordinate is outside map");
        std::vector<tak::crt::RuleGroup> rules(2);
        rules[0].conditions.push_back({0,{}});rules[0].actions.push_back({18,{"100"}});rules[0].actions.push_back({14,{}});
        rules[1].conditions.push_back({1,{"60"}});rules[1].actions.push_back({5,{}});
        tak::crt::RuleGroup detailed;detailed.conditions.push_back({1,{"12345"}});
        detailed.actions.push_back({25,{"Player 3",std::string(63,'x'),"progress",std::string(63,'y')}});
        const auto detail=cart::ruleDetails(detailed,cart::RuleColumn::Group,-1);
        check(detail.find("Condition 1: Gametime greater than 12345")!=std::string::npos && detail.find(std::string(63,'x'))!=std::string::npos && detail.find(std::string(63,'y'))!=std::string::npos,"full rule details preserve every long operand");
        check(cart::ruleDetails(detailed,cart::RuleColumn::Action,0).find("Condition")==std::string::npos,"selected-action detail does not include other columns");
        check(cart::ruleDetails(detailed,cart::RuleColumn::Action,5).empty(),"stale rule detail selection is safe");
        tak::crt::RuleGroup flags;
        flags.conditions={{17,{"z","2"}},{18,{"a","3"}},{999,{"ignored"}}};
        flags.actions={{2,{"z","1"}},{3,{"5","new_flag"}},{4,{"2",""}}};
        check(cart::ruleFlags({flags})==std::vector<std::string>({"a","new_flag","z"}),"flag choices use typed operands, sort, deduplicate and ignore invalid opcodes/empty values");
        check(cart::ruleFlags({}).empty(),"empty player has no flag suggestions");
        rules[0].editorName="Keep the bridge";
        cart::RuleClipboard namedClipboard;int namedGroup=0,namedRow=-1;
        check(namedClipboard.copy(rules,0,-1,cart::RuleColumn::Group) && namedClipboard.paste(rules,namedGroup,namedRow,cart::RuleColumn::Group) && rules[1].editorName=="Keep the bridge","group copy preserves editor name");
        rules.erase(rules.begin()+1);
        cart::RuleClipboard clipboard;int group=0,row=0;
        check(clipboard.copy(rules,group,row,cart::RuleColumn::Action),"copy action");
        check(!clipboard.paste(rules,group,row,cart::RuleColumn::Condition),"cannot paste action as condition");
        check(clipboard.paste(rules,group,row,cart::RuleColumn::Action) && row==1 && rules[0].actions[1].slot[0]=="100","paste preserves action operands");
        check(cart::moveRule(rules,group,row,cart::RuleColumn::Action,1) && row==2 && rules[0].actions[1].opcode==14,"reorder action updates selection");
        check(!cart::moveRule(rules,group,row,cart::RuleColumn::Action,1),"cannot move beyond action list");
        check(clipboard.copy(rules,0,0,cart::RuleColumn::Group,true),"copy player rules");
        std::vector<tak::crt::RuleGroup> other;group=row=-1;
        check(clipboard.paste(other,group,row,cart::RuleColumn::Group) && other.size()==2,"paste complete player rules");
        other[0].actions[0].slot[0]="200";
        check(rules[0].actions[0].slot[0]=="100","clipboard creates independent records");
        check(cart::moveRule(other,group,row,cart::RuleColumn::Group,1) && group==1 && other[0].conditions[0].opcode==1,"reorder rule groups");
        tak::mapgen::Params params;params.seed=UINT64_MAX;params.players=8;params.layout=tak::mapgen::Islands;
        auto fields=cart::generatorFields(params);std::string generatorError;
        tak::mapgen::Params parsed;
        check(cart::parseGeneratorFields(fields,parsed,generatorError),"generator fields parse");
        check(tak::mapgen::encodeMapId(params)==tak::mapgen::encodeMapId(parsed),"all generator options roundtrip exactly");
        const auto original=tak::mapgen::encodeMapId(parsed);
        tak::mapgen::Params restored;
        check(cart::restoreGeneratorRecipe(original,restored,generatorError) && tak::mapgen::encodeMapId(restored)==original,"saved generator recipe restores");
        check(!cart::restoreGeneratorRecipe(original+"garbage",restored,generatorError) && tak::mapgen::encodeMapId(restored)==original,"invalid recipe does not fall back to defaults");
        check(!cart::restoreGeneratorRecipe("",restored,generatorError) && generatorError.find("no saved generator recipe")!=std::string::npos,"missing recipe gives a visible error");
        {
            // The recipe is the .ota's takgeneratorrecipe only: a description line that
            // looks like one is the author's text, never read as the map's recipe.
            const auto ota=tak::tnt::Scenario::parse("[GlobalHeader]\n{\n\tmissionname=Described;\n\tmissiondescription=Generator recipe: "+original+";\n}\n");
            check(ota.generatorRecipe.empty() && ota.missionDescription=="Generator recipe: "+original,"fixture description parses");
            check(!cart::restoreGeneratorRecipe(ota.generatorRecipe,restored,generatorError) && generatorError.find("no saved generator recipe")!=std::string::npos,
                  "a recipe in the description is not read as the map's recipe");
        }
        fields[0]="18446744073709551616";
        check(!cart::parseGeneratorFields(fields,parsed,generatorError) && tak::mapgen::encodeMapId(parsed)==original,"seed overflow leaves settings unchanged");
        for(int layout=tak::mapgen::Maze;layout<tak::mapgen::kLayouts;++layout) {
            auto themed=params;
            auto themeFields=cart::generatorFields(themed);themeFields[2]=tak::mapgen::layoutName(layout);
            check(cart::parseGeneratorFields(themeFields,themed,generatorError),"themed editor layout rejected");
            check(tak::mapgen::sanitize(themed).layout==layout,"themed editor layout lost");
        }
        {
            // v10 kit amounts: edited, range-checked, and carried in the recipe.
            auto kits=params;auto kitFields=cart::generatorFields(kits);
            kitFields[8]="Max";kitFields[9]="0";kitFields[10]="200";kitFields[11]="255";
            check(cart::parseGeneratorFields(kitFields,kits,generatorError) && kits.manaScatter==tak::mapgen::ScatterMax &&
                  kits.plateauDensity==0 && kits.mountainDensity==200 && kits.townDensity==255,"kit amounts parse");
            const auto decoded=tak::mapgen::decodeMapId(tak::mapgen::encodeMapId(kits));
            check(decoded.plateauDensity==0 && decoded.mountainDensity==200 && decoded.townDensity==255,"kit amounts ride in the recipe");
            kitFields[8]="Lots";check(!cart::parseGeneratorFields(kitFields,kits,generatorError),"unknown scattered mana level accepted");
            kitFields[8]="None";kitFields[10]="256";check(!cart::parseGeneratorFields(kitFields,kits,generatorError),"kit amount range checked");
            // Only the current recipe version restores; any other is malformed.
            check(!cart::restoreGeneratorRecipe("~gen1~080001000000000000000100030003020000ff608003",restored,generatorError) &&
                  generatorError.find("malformed or cannot be restored exactly")!=std::string::npos,"another recipe version is refused as malformed");
        }
        fields=cart::generatorFields(params);fields[3]="256";
        check(!cart::parseGeneratorFields(fields,parsed,generatorError),"density range checked");
        fields=cart::generatorFields(params);fields[1]="3players";
        check(!cart::parseGeneratorFields(fields,parsed,generatorError),"partial numeric input rejected");
        tak::hpi::Vfs vfs;tak::sim::TypeRegistry registry;
        const auto root=std::filesystem::temp_directory_path()/("tak-editor-generator-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code e;std::filesystem::remove_all(path,e);}} cleanup{root};
        std::filesystem::create_directories(root/"features"/"aramon");
        {
            std::ofstream file(root/"features"/"aramon"/"fixture.tdf");
            for(int i=1;i<=3;++i)file<<"[AraMana0"<<i<<"] {\nfootprintx=2;\nfootprintz=2;\nblocking=0;\n}\n";
            for(int i=1;i<=9;++i)file<<"[AraHenge0"<<i<<"] {\nfootprintx=3;\nfootprintz=3;\nblocking=1;\n}\n";
        }
        vfs.addLayer(tak::hpi::MountSet(root));
        params.layout=tak::mapgen::Mainland;params.waterDensity=params.reliefDensity=0;
        fields=cart::generatorFields(params);check(cart::parseGeneratorFields(fields,parsed,generatorError),"edited generator settings parse");
        const auto first=tak::mapgen::generate(params,vfs),second=tak::mapgen::generate(parsed,vfs);
        check(first.starts==second.starts && first.map.heights==second.map.heights && first.map.features==second.map.features &&
              first.map.tileKeys==second.map.tileKeys && first.map.tileCols==second.map.tileCols && first.map.tileRows==second.map.tileRows,
              "editor settings reproduce actual generator output");
        auto files=std::make_shared<tak::hpi::Vfs::Files>();
        (*files)["kmap/recipe-test.recipe"]={original.begin(),original.end()};vfs.setMapFiles(files);
        check(cart::mapGeneratorRecipe(vfs,"kmap/recipe-test.tnt","")==original,"game-generated map recipe sidecar imports");
        check(cart::mapGeneratorRecipe(vfs,"kmap/recipe-test.tnt","embedded")=="embedded","explicit metadata recipe wins over sidecar");
        const std::string fbi="[UNITINFO] { \nUnitName=TEST;\nName=Test Unit;\nFootprintX=1;\nFootprintZ=1;\nMaxSlope=20;\nMaxWaterDepth=0;\nCanMove=1;\nMaxVelocity=1;\n }";
        // Synthetic solid-color JPEG; no retail artwork.
        const std::vector<uint8_t> terrainImage={0xff,0xd8,0xff,0xe0,0x0,0x10,0x4a,0x46,0x49,0x46,0x0,0x1,0x1,0x0,0x0,0x1,0x0,0x1,0x0,0x0,0xff,0xdb,0x0,0x43,0x0,0x8,0x6,0x6,0x7,0x6,0x5,0x8,0x7,0x7,0x7,0x9,0x9,0x8,0xa,0xc,0x14,0xd,0xc,0xb,0xb,0xc,0x19,0x12,0x13,0xf,0x14,0x1d,0x1a,0x1f,0x1e,0x1d,0x1a,0x1c,0x1c,0x20,0x24,0x2e,0x27,0x20,0x22,0x2c,0x23,0x1c,0x1c,0x28,0x37,0x29,0x2c,0x30,0x31,0x34,0x34,0x34,0x1f,0x27,0x39,0x3d,0x38,0x32,0x3c,0x2e,0x33,0x34,0x32,0xff,0xdb,0x0,0x43,0x1,0x9,0x9,0x9,0xc,0xb,0xc,0x18,0xd,0xd,0x18,0x32,0x21,0x1c,0x21,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0xff,0xc0,0x0,0x11,0x8,0x0,0x20,0x0,0x20,0x3,0x1,0x22,0x0,0x2,0x11,0x1,0x3,0x11,0x1,0xff,0xc4,0x0,0x15,0x0,0x1,0x1,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x6,0xff,0xc4,0x0,0x14,0x10,0x1,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0xff,0xc4,0x0,0x16,0x1,0x1,0x1,0x1,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x4,0x5,0xff,0xc4,0x0,0x14,0x11,0x1,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0xff,0xda,0x0,0xc,0x3,0x1,0x0,0x2,0x11,0x3,0x11,0x0,0x3f,0x0,0x94,0x1,0x9a,0x94,0x0,0x0,0x0,0x0,0x1f,0xff,0xd9};
        (*files)["terrain/00000001.jpg"]=terrainImage;
        // Minimal synthetic model with an empty root and no geometry.
        std::vector<uint8_t> emptyModel(52);emptyModel[0]=1;
        (*files)["objects3d/test.3do"]=emptyModel;
        (*files)["units/test.fbi"]={fbi.begin(),fbi.end()};vfs.setMapFiles(files);registry.loadDir(vfs,"units");
        check(registry.find("test")!=nullptr,"synthetic unit loaded");
        {
            tak::crt::Scenario authored;authored.regions.push_back({"Bridge",1,1,3,3});
            const auto templates=cart::ruleTemplates(2,"TEST","Bridge");
            check(templates.size()==6,"objective catalog covers common tasks");
            for(const auto& item:templates) {
                check(!item.group.editorName.empty(),"objective has an editable rule name");
                for(const auto& condition:item.group.conditions)
                    check(cart::validateRuleOperands(false,condition,authored,registry).empty(),"template condition uses valid authored operands");
                for(const auto& action:item.group.actions)
                    check(cart::validateRuleOperands(true,action,authored,registry).empty(),"template action uses valid authored operands");
                check(item.group.actions.back().opcode==14,"templates explicitly disable after firing");
            }
            const auto matches=cart::matchingRuleTemplates(templates,"ReInFoRcE");
            check(matches.size()==1 && templates[matches[0]].group.actions[0].slot[0]=="TEST" &&
                  templates[matches[0]].group.actions[0].slot[1]=="Bridge","template search and contextual unit/region");
            check(cart::matchingRuleTemplates(templates,"nothing matches this").empty(),"template empty search results");
            check(cart::ruleTemplates(0,"","").size()==5,"no invalid unit-spawn template when the catalog is empty");
            check(templates[0].group.actions[0].slot[0]=="Player 2","opening briefing targets selected player");
            check(cart::ruleTemplates(8,"TEST","Bridge")[0].group.actions[0].slot[0]=="Player 8","last rule bank targets Player 8");
            check(cart::ruleTemplates(0,"TEST","Bridge")[0].group.actions[0].slot[0]=="All Players","broadcast rule bank targets All Players");
        }
        auto operandHas=[&](bool action,const tak::crt::Rule& r,const std::string& message) {
            const auto issues=cart::validateRuleOperands(action,r,{},registry);
            return std::any_of(issues.begin(),issues.end(),[&](const auto& issue){return issue.message.find(message)!=std::string::npos;});
        };
        check(operandHas(false,{1,{"12seconds"}},"whole number"),"partially parsed numeric operand rejected");
        check(operandHas(false,{1,{"999999999999999999"}},"whole number"),"numeric overflow rejected");
        check(operandHas(false,{1,{"2147483647"}},"tick range"),"game time conversion overflow diagnosed");
        check(!operandHas(true,{2,{"score"," -12 "}},"whole number"),"signed flag values remain allowed");
        check(!operandHas(true,{2,{"score","+12"}},"whole number"),"explicit positive values remain allowed");
        check(operandHas(true,{2,{"score","+-12"}},"whole number"),"double sign rejected");
        check(operandHas(false,{23,{"101"}},"probability"),"out-of-range probability warned");
        check(operandHas(true,{7,{"missing","Anywhere"}},"unknown unit type"),"rule type references validated");
        check(!operandHas(true,{7,{"TEST","Anywhere"}},"unknown unit type"),"rule type matching ignores case");
        check(operandHas(true,{13,{"Player 99","Message"}},"Player 1 through"),"invalid display player rejected");
        check(!operandHas(true,{13,{"All Players","Message"}},"Player 1 through"),"broadcast display remains valid");
        check(operandHas(true,{2,{"","1"}},"flag name is empty"),"empty flag diagnosed");
        check(operandHas(true,{2,{"score","1"}},"first byte"),"multi-byte flag names explain aliasing");
        check(!operandHas(true,{2,{"s","1"}},"first byte"),"one-byte flag needs no alias warning");
        check(!operandHas(false,{9,{"Any Unit","0","Anywhere"}},"not supported"),"Any Unit control wildcard accepted");
        check(!operandHas(true,{8,{"Any Unit","Anywhere"}},"not supported"),"Any Unit action wildcard accepted");
        check(operandHas(true,{7,{"Any Unit","Anywhere"}},"not supported"),"creation requires a concrete type");
        tak::crt::Rule truncated{13,{"Player 1",std::string("a\0b",3)}};
        check(operandHas(true,truncated,"embedded NUL"),"embedded NUL cannot silently truncate CRT operand");
        tak::tnt::Map map;map.width=map.height=32;map.heights.resize(1024,60);map.features.resize(1024,0xffff);
        map.blocksX=map.blocksY=16;map.tileKeys.assign(256,1);map.tileCols.assign(256,0);map.tileRows.assign(256,0);
        check(cart::validateTerrainResources(map,vfs).empty(),"valid synthetic terrain image resolves and decodes");
        {
            auto broken=map;broken.tileKeys[0]=broken.tileKeys[1]=2;
            const auto missing=cart::validateTerrainResources(broken,vfs);
            check(missing.size()==1 && missing[0].severity==cart::MapIssue::Severity::Error && missing[0].x==16 && missing[0].z==16 && missing[0].message.find("2 blocks")!=std::string::npos,"missing terrain keys deduplicate with affected count and location");
            (*files)["terrain/00000002.jpg"]={1,2,3};
            check(cart::validateTerrainResources(broken,vfs).size()==1,"present but corrupt JPEG is diagnosed");
            files->erase("terrain/00000002.jpg");broken.tileRows.clear();
            check(cart::validateTerrainResources(broken,vfs)[0].message.find("arrays")!=std::string::npos,"malformed tile arrays rejected before indexing");
        }
        {
            auto isolated=map;for(int z=0;z<32;++z)isolated.heights[z*32+16]=255;
            tak::sim::World observer;observer.setTerrain(isolated.heights,32,32,0,&isolated.features);observer.buildNavClasses(registry);
            const auto* type=registry.find("test");const auto& grid=observer.navFor(type);
            const auto regions=cart::movementRegions(grid,1);
            check(regions.size()==2,"whole-map scan finds both isolated land regions");
            int total=0;for(const auto& region:regions)total+=region.cells;
            int passable=0;for(int z=0;z<32;++z)for(int x=0;x<32;++x)passable+=grid.fits(x,z,1);
            check(total==passable,"component scan accounts for every engine-passable cell");
            for(size_t i=0;i<regions.size();++i)for(size_t j=0;j<regions.size();++j)
                check(observer.pathExists(type,regions[i].x*16.f+8,regions[i].z*16.f+8,regions[j].x*16.f+8,regions[j].z*16.f+8)==(i==j),"region representatives agree with engine connectivity");
            auto wide=cart::movementRegions(grid,3);int wideTotal=0;for(const auto& region:wide)wideTotal+=region.cells;
            check(wide.size()==2 && wideTotal<total,"larger footprints use engine clearance");
            auto tiny=tak::sim::NavGrid(std::vector<uint8_t>(16,60),4,4);tiny.block(1,0,1,1,true);tiny.block(0,1,1,1,true);
            check(cart::movementRegions(tiny,1).size()==2,"diagonal contact does not join regions through blocked corners");
            std::vector<uint8_t> water(64,0);for(int z=0;z<8;++z)water[z*8+4]=100;
            tak::sim::NavGrid::Limits limits;limits.minWaterDepth=1;limits.maxWaterDepth=100;limits.maxSlope=limits.maxWaterSlope=20;
            tak::sim::NavGrid naval(water,8,8,50,limits);
            const auto pools=cart::movementRegions(naval,1);
            check(pools.size()==2 && pools[0].cells+pools[1].cells==48,"naval profiles detect two pools separated by land");
        }
        auto movement=cart::terrainOverlay(map,registry,vfs,cart::OverlayKind::Movement,"test");
        check(movement.width==32 && movement.rgba[(12*32+12)*4+1]==210,"flat ground movement overlay is passable");
        map.seaLevel=100;
        movement=cart::terrainOverlay(map,registry,vfs,cart::OverlayKind::Movement,"test");
        check(movement.rgba[(12*32+12)*4]==235,"ground movement overlay rejects deep water");
        auto depth=cart::terrainOverlay(map,registry,vfs,cart::OverlayKind::WaterDepth,"");
        check(depth.rgba[2]==160 && depth.rgba[3]==150,"water depth uses sea minus low corner");
        map.seaLevel=0;
        map.heights[12*32+12]=100;
        auto slope=cart::terrainOverlay(map,registry,vfs,cart::OverlayKind::Slope,"");
        check(slope.rgba[(11*32+11)*4]==255 && slope.rgba[0]==0,"slope overlay reads four-corner spread");
        auto build=cart::terrainOverlay(map,registry,vfs,cart::OverlayKind::Buildability,"test");
        tak::sim::World reference;reference.setTerrain(map.heights,32,32,0,&map.features);reference.buildNavClasses(registry);
        for(int z=0;z<32;++z)for(int x=0;x<32;++x)
            check((build.rgba[(z*32+x)*4+1]==210)==reference.canPlace(registry.find("test"),x*16.f+8,z*16.f+8),"buildability overlay agrees with engine placement at every cell");
        std::vector<cart::PlacedUnit> occupants{{"TEST",0,88,88}};
        reference.spawn(registry.find("test"),88,88);
        const auto occupiedOverlay=cart::terrainOverlay(map,registry,vfs,cart::OverlayKind::Buildability,"test",occupants);
        for(int z=0;z<32;++z)for(int x=0;x<32;++x)
            check((occupiedOverlay.rgba[(z*32+x)*4+1]==210)==reference.canPlace(registry.find("test"),x*16.f+8,z*16.f+8),"occupied buildability overlay agrees with engine at every cell");
        check(occupiedOverlay.rgba[(5*32+5)*4]==235 && build.rgba[(5*32+5)*4+1]==210,"preplaced unit changes buildability");
        {
            // Batch broad phase must retain the scalar predicate, including
            // edge buckets, dead/airborne occupants and larger footprints.
            auto flyer=*registry.find("test");flyer.canFly=true;
            reference.spawn(&flyer,128,128);
            for(int i=0;i<50;++i) {
                const int id=reference.spawn(registry.find("test"),float((i*79)%600-40),float((i*131)%600-40));
                if(i%9==0)reference.unit(id)->hp={};
            }
            const auto hash=reference.stateHash();
            for(int footprint:{1,3,9})for(bool structure:{false,true}) {
                auto type=*registry.find("test");type.footX=footprint;type.footZ=footprint;
                if(structure)type.maxVel={};
                const auto cells=reference.placementCells(&type);
                for(int z=0;z<32;++z)for(int x=0;x<32;++x)
                    check(bool(cells[size_t(z)*32+x])==reference.canPlace(&type,x*16.f+8,z*16.f+8),"batched placement differs from scalar engine predicate");
            }
            check(reference.stateHash()==hash,"placement analysis changes simulation state");
            std::atomic_bool cancel=true;
            check(reference.placementCells(registry.find("test"),&cancel).empty(),"cancelled placement batch produces no partial overlay");
            check(cart::terrainOverlay(map,registry,vfs,cart::OverlayKind::Buildability,"test",occupants,&cancel).rgba.empty(),"cancelled overlay produces no pixels");
        }
        map.heights[12*32+12]=60;
        tak::tnt::Scenario metadata;metadata.starts={{1,12,12},{2,24,24}};
        tak::crt::Scenario scenario;scenario.regions.push_back({"Area",15,15,5,5});
        std::vector<cart::PlacedUnit> units(1);units[0].type="TEST";units[0].x=200;units[0].z=200;units[0].name="First";
        auto run=[&] {return cart::validateMap(map,metadata,scenario,units,{},registry,vfs);};
        auto has=[](const auto& issues,const std::string& text) {for(const auto& issue:issues)if(issue.message.find(text)!=std::string::npos)return true;return false;};
        check(run().empty(),"flat map, unit and reversed inclusive region validate");
        units[0].name=std::string(32,'N');check(has(run(),"limited to 31 bytes"),"runtime display-name truncation warned");units[0].name="First";
        units[0].vertical=99;check(has(run(),"vertical placement is preserved but ignored"),"unused vertical metadata warned");units[0].vertical=200;
        files->erase("objects3d/test.3do");
        check(has(run(),"unit model is missing"),"missing placed-unit model diagnosed");
        (*files)["objects3d/test.3do"]=emptyModel;
        metadata.starts[1].number=1;check(has(run(),"duplicate start number"),"duplicate start numbers cannot silently replace a slot");
        metadata.starts[1].number=9;check(has(run(),"must be 1 through 8"),"start number range");metadata.starts[1].number=2;
        map.features[0]=7;check(has(run(),"Missing feature definition"),"invalid feature reference diagnosed");map.features[0]=0xffff;
        map.seaLevel=100;check(has(run(),"engine placement rejects"),"engine rejects ground unit in deep water");map.seaLevel=0;
        units.push_back(units.front());check(!has(run(),"duplicate unique name"),"duplicate cosmetic display names were rejected");
        check(has(run(),"blocked by earlier preplaced units"),"preplaced occupancy diagnosed separately from terrain");
        units.back().x=400;units.back().name="Second";
        check(!has(run(),"blocked by earlier preplaced units"),"separated preplaced units remain clear");units.pop_back();
        units[0].x=-1;check(has(run(),"outside map"),"out of bounds detected");units[0].x=200;
        units[0].type="MISSING";check(has(run(),"unknown unit type"),"missing type detected");units[0].type="TEST";
        scenario.players.resize(1);scenario.players[0].resize(1);
        auto& rule=scenario.players[0][0].conditions.emplace_back();
        const auto& definitions=cart::conditionDefs();size_t location=0;
        for(size_t i=0;i<definitions.size();++i) {
            bool found=false;
            for(size_t s=0;s<definitions[i].params.size();++s)if(definitions[i].params[s]==cart::PKind::Location) {rule.opcode=int(i);location=s;found=true;break;}
            if(found)break;
        }
        rule.slot[location]="Missing region";check(has(run(),"unknown region"),"unresolved location diagnosed");
        rule.slot[location]="AREA";check(!has(run(),"unknown region"),"region matching ignores case");
        rule.slot[location].clear();check(!has(run(),"unknown region"),"empty location means whole map in engine");
        std::string error;
        rule.slot[location]="AREA";
        check(cart::setRegion(scenario,0,{"Renamed",12,13,4,5},32,32,error),"region rename and resize");
        check(rule.slot[location]=="Renamed" && scenario.regions[0].x1==4 && scenario.regions[0].x2==12,"rename updates location references and normalizes bounds");
        check(!cart::removeRegion(scenario,0,error),"cannot delete a referenced region");
        check(!cart::setRegion(scenario,-1,{"renamed",1,1,2,2},32,32,error),"case insensitive unique region names");
        check(!cart::setRegion(scenario,-1,{"Anywhere",1,1,2,2},32,32,error),"whole map token reserved");
        check(!cart::setRegion(scenario,0,{"Outside",0,0,32,32},32,32,error),"inclusive bounds stay within map");
        check(scenario.regions[0].name=="Renamed" && rule.slot[location]=="Renamed","failed region edit is atomic");
        auto roundtrip=tak::crt::parse(tak::crt::write(scenario));
        check(roundtrip.regions[0].x2==12 && roundtrip.players[0][0].conditions[0].slot[location]=="Renamed","region and rewritten rules survive CRT roundtrip");
        rule.slot[location]="Anywhere";check(cart::removeRegion(scenario,0,error),"unused region can be deleted");
        check(map.heights[0]==60 && units[0].name=="First","validation preserves document");
        const std::string monarch="[UNITINFO] {\nUnitName=araking;\nName=Test Monarch;\nSide=ARA;\nFootprintX=1;\nFootprintZ=1;\nMaxSlope=20;\nMaxWaterDepth=0;\nCanMove=1;\nMaxVelocity=1;\n}";
        const std::string stone="[UNITINFO] {\nUnitName=teststone;\nName=Test Lodestone;\nSide=ARA;\nFootprintX=2;\nFootprintZ=2;\nYardMap=SSSS;\nMaxSlope=20;\nMaxWaterDepth=0;\nMaxVelocity=0;\n}";
        const std::string deposit="[TestMana] {\ncategory=mana;\nanimating=1;\nblocking=0;\nfootprintx=1;\nfootprintz=1;\n}";
        (*files)["units/araking.fbi"]={monarch.begin(),monarch.end()};(*files)["units/teststone.fbi"]={stone.begin(),stone.end()};
        (*files)["features/aramon/testmana.tdf"]={deposit.begin(),deposit.end()};vfs.setMapFiles(files);
        tak::sim::TypeRegistry diagnostics;diagnostics.loadDir(vfs,"units");
        check(diagnostics.find("teststone")->onMana,"fixture is a lodestone");
        check(has(cart::validateMap(map,metadata,{}, {},{},diagnostics,vfs),"No recognized mana deposits"),"missing mana economy diagnosed");
        auto disconnected=map;disconnected.featureNames={"TestMana"};disconnected.features[8*32+24]=0;
        for(int z=0;z<32;++z)disconnected.heights[z*32+16]=255;
        check(has(cart::validateMap(disconnected,metadata,{}, {},{},diagnostics,vfs),"disconnected movement regions"),"Check Map reports disconnected regions beyond start-route checks");
        metadata.starts={{1,4,8},{2,27,8}};
        auto connectivity=cart::validateMap(disconnected,metadata,{}, {},{},diagnostics,vfs);
        check(has(connectivity,"no ground approach route to another start") && has(connectivity,"no ground approach route to a mana deposit"),"engine connectivity diagnoses isolated starts and mana");
        check(!has(connectivity,"rejects lodestone footprint"),"flat mana deposit accepts lodestone footprint");
        tak::sim::World deposits;deposits.setTerrain(disconnected.heights,32,32,0,&disconnected.features);deposits.buildNavClasses(diagnostics);
        tak::sim::registerMapFeatures(deposits,disconnected,vfs,&diagnostics);
        check(!deposits.manaSpots().empty(),"occupancy fixture has mana spot");
        const auto [mx,mz]=deposits.manaSpots().front();
        occupants={{"TEST",0,mx,mz}};
        check(has(cart::validateMap(disconnected,metadata,{},occupants,{},diagnostics,vfs),"preplaced units currently block"),"preplaced units blocking lodestones diagnosed");
        auto atStart=occupants;atStart[0].x=metadata.starts[0].xpos*16.f;atStart[0].z=metadata.starts[0].zpos*16.f;
        check(has(cart::validateMap(disconnected,metadata,{},atStart,{},diagnostics,vfs),"Start 1: preplaced units currently block"),"preplaced units blocking monarch starts diagnosed");
        occupants[0].type="TESTSTONE";
        check(!has(cart::validateMap(disconnected,metadata,{},occupants,{},diagnostics,vfs),"preplaced units currently block"),"existing lodestone is intentional mana occupancy");
        disconnected.heights[8*32+24]=255;
        connectivity=cart::validateMap(disconnected,metadata,{}, {},{},diagnostics,vfs);
        check(has(connectivity,"rejects lodestone footprint"),"steep mana deposit fails actual engine placement");
        {
            const std::string port="[UNITINFO]{\nunitname=port;\nname=Sea Dock;\nmaxdamage=100;\nfootprintx=2;\nfootprintz=2;\nmaxvelocity=0;\nminwaterdepth=1;\nmaxwaterdepth=255;\nfloater=1;\nbuilder=1;\nworkertime=100;\n}";
            const std::string boat="[UNITINFO]{\nunitname=boat;\nname=Longship;\nmaxdamage=100;\nfootprintx=2;\nfootprintz=2;\nmaxvelocity=1;\nminwaterdepth=8;\nmaxwaterdepth=255;\nmovementclass=water;\nfloater=1;\nbuildtime=30000;\n}";
            (*files)["units/port.fbi"]={port.begin(),port.end()};(*files)["units/boat.fbi"]={boat.begin(),boat.end()};
            (*files)["canbuild/port/boat.tdf"]={};
            tak::sim::TypeRegistry navy;navy.loadDir(vfs,"units");navy.loadBuildTree(vfs,"canbuild");
            auto water=map;water.seaLevel=100;water.heights.assign(water.heights.size(),95);
            water.features.assign(water.features.size(),0xffff);
            std::vector<cart::PlacedUnit> harbor{{"PORT",0,264,264}};
            check(has(cart::validateMap(water,{}, {},harbor,{},navy,vfs),"initial naval output site"),"shallow naval factory output not diagnosed");
            water.heights.assign(water.heights.size(),30);
            check(!has(cart::validateMap(water,{}, {},harbor,{},navy,vfs),"initial naval output site"),"deep-water output incorrectly rejected");
            tak::sim::World production;production.setTerrain(water.heights,water.width,water.height,water.seaLevel);
            production.buildNavClasses(navy);
            const int id=production.spawn(navy.find("port"),264,264,0,0);
            tak::sim::Fixed x,y,z;
            check(production.productionPosition(id,navy.find("boat"),x,y,z),"deep-water output query failed");
            production.train(id,navy.find("boat"));production.tick(1.f/30);
            const auto* site=production.unit(production.unit(id)->productionSiteId);
            check(site && site->x==x && site->z==z && site->flightY==y,"production differs from editor's shared output predicate");
            check(water.heights[0]==30 && harbor[0].x==264,"naval diagnostics changed the source document");
        }
        {   // Check Map carries the AI Profiles checks (aiprofiles.h checkAiMap), notes left out.
            tak::hpi::Vfs none;tak::sim::TypeRegistry empty;
            auto bytes=[](std::string_view text){return std::vector<uint8_t>(text.begin(),text.end());};
            const cart::AiFiles ai{{"ai/takengine.txt",bytes("Weight araarch 3\n")},{"ai/notes.txt",bytes("x\n")}};
            const auto issues=cart::validateAiProfiles(ai,"islands",empty,none);
            auto has=[&](std::string_view text){return std::any_of(issues.begin(),issues.end(),[&](const auto& i){return i.message.find(text)!=std::string::npos;});};
            check(has("AI profile ai/takengine.txt: line 1: levels need lowercase 'weight'"),"Check Map reports a level line the parser skips");
            check(has("AI profile ai/notes.txt: ai/notes.txt is kept in the .kmp but not sent"),"Check Map reports an ai/ file that does not travel");
            check(has("AI profile aiprofile: no ai/islands.txt in this map or the install"),"Check Map reports an aiprofile name with no file");
            check(cart::validateAiProfiles({},"DEFAULT",empty,none).empty(),"a map on the standard profile with no AI files has no AI issues");
        }
        {   // Check Map names what the scenario loses with CRUSADES on (unitscb/ alone).
            auto roster=[](std::initializer_list<const char*> names) {
                auto files=std::make_shared<tak::hpi::Vfs::Files>();
                for(const char* name:names) {
                    const auto text=std::string("[UNITINFO]{\nunitname=")+name+";\nobjectname="+name+";\nname=Display "+name+
                        ";\nmaxdamage=100;\nfootprintx=1;\nfootprintz=1;\ncanmove=1;\nmaxvelocity=1;\n}";
                    (*files)[std::string("units/")+name+".fbi"]={text.begin(),text.end()};
                }
                tak::hpi::Vfs vfs;vfs.setMapFiles(files);tak::sim::TypeRegistry registry;registry.loadDir(vfs,"units");
                return registry;
            };
            const auto standard=roster({"zonhand","zonspide"}),crusades=roster({"zonhand"});
            tak::crt::Scenario scenario;scenario.players.resize(9);
            scenario.customTypes.push_back({"ZONSPIDE",{100,200,200,0}});
            scenario.customTypes.push_back({"zonhand",{100,200,200,0}});
            tak::crt::RuleGroup group;
            group.conditions.push_back({15,{"1","zonspide","Anywhere"}});
            group.actions.push_back({7,{"ZONSPIDE","Anywhere"}});
            group.actions.push_back({7,{"ZONHAND","Anywhere"}});
            scenario.players[2].push_back(group);
            std::vector<cart::PlacedUnit> placed(3);
            placed[0].type="ZONHAND";placed[1].type="ZONSPIDE";placed[1].x=40;placed[1].z=56;placed[2].type="zonspide";
            const auto issues=cart::validateCrusadesRoster(scenario,placed,standard,crusades);
            auto has=[&](std::string_view text){return std::any_of(issues.begin(),issues.end(),[&](const auto& i){return i.message.find(text)!=std::string::npos;});};
            check(issues.size()==4 && std::all_of(issues.begin(),issues.end(),[](const auto& i){return i.severity==cart::MapIssue::Severity::Warning;}),
                  "Crusades roster check reports one line per lost placed type, custom type and rule operand, as warnings");
            check(has("Display zonspide (ZONSPIDE) does not exist with CRUSADES on: 2 placed units stop a CRUSADES game from starting this scenario"),
                  "Crusades roster check names a placed unit Crusades lacks");
            check(issues[0].x==40 && issues[0].z==56,"Crusades placement warning points at the first such unit");
            check(has("Custom stats for Display zonspide (ZONSPIDE) are skipped with CRUSADES on"),"Crusades roster check names a skipped custom type");
            check(has("Player 2, rule 1, action 1: Display zonspide (ZONSPIDE) does not exist with CRUSADES on, so there this rule creates nothing"),
                  "Crusades roster check names a Create of a lost type");
            check(has("Player 2, rule 1, condition 1: Display zonspide (ZONSPIDE) does not exist with CRUSADES on, so there this rule counts other units instead"),
                  "Crusades roster check names a condition on a lost type");
            check(cart::validateCrusadesRoster(scenario,placed,standard,standard).empty(),"an install without a Crusades balance has no Crusades warnings");
        }
        std::cout<<"PASS: editor engine placement and scenario validation\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
