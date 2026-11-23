#include "sim/matchsetup.h"
#include "cartographer/units.h"
#include "hpi/hpi.h"
#include "tnt/ota.h"
#include "tnt/tnt.h"
#include <iostream>
#include <stdexcept>
using namespace tak;
static void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
int main() try {
    auto files=std::make_shared<hpi::Vfs::Files>();
    auto text=[&](const std::string& path,const std::string& value){(*files)[path]={value.begin(),value.end()};};
    text("units/tower.fbi","[UNITINFO]{\nunitname=tower;\nname=Tower;\nmaxdamage=100;\nfootprintx=2;\nfootprintz=3;\n}");
    tnt::Map map;map.width=map.height=64;map.blocksX=map.blocksY=32;
    map.heights.assign(4096,40);map.features.assign(4096,0xffff);
    map.tileKeys.assign(1024,0);map.tileCols.assign(1024,0);map.tileRows.assign(1024,0);
    (*files)["kmap/placement.tnt"]=map.save();
    tnt::Scenario metadata;metadata.hasScenario=true;
    text("kmap/placement.ota",metadata.write()+"\n[TAKPlaytest]{\nauthoredscenario=1;\n}\n");
    crt::Scenario scenario;scenario.version=1;scenario.players.resize(9);
    crt::Unit placed;placed.objectName="TOWER";placed.uniqueName="The west watchtower with a long name";placed.x=12;placed.z=19;placed.y=999;
    scenario.units={placed};(*files)["kmap/placement.crt"]=crt::write(scenario);
    hpi::Vfs vfs;vfs.setMapFiles(files);sim::TypeRegistry registry;registry.loadDir(vfs,"units");
    const auto* tower=registry.find("tower");
    check(tower && tower->footX==2 && tower->footZ==3 && tower->maxHp==100,"synthetic placement FBI did not load its fields");
    sim::MatchConfig config;config.vfs=&vfs;config.mapPath="kmap/placement.tnt";config.slots={{true,0,0}};
    sim::World world;sim::setupMatch(world,registry,config);
    check(world.units().size()==1,"authored unit missing");const auto& unit=world.units().front();
    check(unit.x.floorInt()==208 && unit.z.floorInt()==328,"CRT footprint origin treated as center cell");
    check(unit.scenarioName==placed.uniqueName.substr(0,31) && unit.displayName()==unit.scenarioName,"authored rename not applied");
    const auto before=world.stateHash();world.unit(unit.id)->scenarioName="Cosmetic";
    check(world.stateHash()==before,"cosmetic name affects lockstep hash");
    scenario.units.front().y=-123;(*files)["kmap/placement.crt"]=crt::write(scenario);
    sim::World other;sim::setupMatch(other,registry,config);
    check(other.stateHash()==before,"unused CRT vertical field changes gameplay");
    auto editor=cart::toPlaced(scenario,&registry);
    check(editor.size()==1 && editor.front().x==208 && editor.front().z==328,"editor center differs from runtime");
    auto saved=crt::parse(cart::saveScenario(scenario,editor));
    check(saved.units.front().x==12 && saved.units.front().z==19 && saved.units.front().y==-123,"editor loses footprint origin or retained vertical field");
    world.unit(unit.id)->scenarioName.clear();check(world.unit(unit.id)->displayName()==unit.type->name,"unnamed unit loses type display name");
    std::cout<<"PASS: named authored placements, footprint centers, retained vertical metadata\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
