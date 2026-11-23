#include "hpi/hpi.h"
#include "util/virtualpath.h"
#include "sim/matchsetup.h"
#include <chrono>
#include <clocale>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include "util/u8path.h"

static void require(bool b,const char* m) { if(!b) throw std::runtime_error(m); }
int main(int argc,char** argv) {
    namespace fs=std::filesystem;
    std::setlocale(LC_ALL,"C"); // Match the default Windows release process locale.
    const auto root=fs::temp_directory_path()/ta::u8path("ta-vfs-\xc3\xa9-\xe6\xb0\xb4-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        fs::create_directories(root/"Maps");
        const std::string unicode="Maps/\xc3\x89tang \xe6\xb0\xb4.tnt";
        const std::string legacy="Maps/Legacy \xe9.tnt";
        const std::string archived="Maps/Archive \xe6\xb0\xb4.tnt";
        const std::vector<uint8_t> data{'m','a','p'};
        { std::ofstream f(root/ta::u8path(unicode),std::ios::binary); f.write("map",3); }
        { auto bytes=ta::hpi::pack({{legacy,data},{archived,data}});
          std::ofstream f(root/ta::u8path("\xe6\xb0\xb4.hpi"),std::ios::binary);
          f.write(reinterpret_cast<const char*>(bytes.data()),bytes.size()); }
        { // Close mapped archives before deleting their directory on Windows.
            ta::hpi::Vfs vfs;
            vfs.addLayer(ta::hpi::MountSet(root));
            auto maps=ta::hpi::listMaps(vfs);
            require(maps.size()==3,"all Unicode and legacy maps listed");
            for(const auto& path:{unicode,legacy,archived}) {
                const auto name=ta::vpath::stem(path);
                require(ta::hpi::findMap(vfs,name)==path,"map lookup preserves name bytes");
                require(vfs.has(path),"map exists");
                require(vfs.read(path)==data,"map read succeeds");

                require(ta::vpath::extension(ta::vpath::replaceExtension(path,".ota"))==".ota","metadata sibling");
            }
            require(ta::vpath::stem("Maps\\a.b.tnt")=="a.b","backslash path");
            require(ta::vpath::extension("dir.dot/file").empty(),"directory dot is not extension");
            require(ta::vpath::stem(".hidden")==".hidden","dotfile stem");
            // Downloaded-map terrain may replace stock keys for that map, but
            // generated maps must see the retail tile (explicit user overrides win).
            const auto writeTile=[&](const char* dir,const char* value) {
                fs::create_directories(root/dir/"terrain");
                std::ofstream f(root/dir/"terrain"/"12345678.jpg");f<<value;
            };
            writeTile("base","stock");writeTile("download","map");writeTile("override","user");
            ta::hpi::Vfs scoped;
            scoped.addLayer(ta::hpi::MountSet(root/"base"));
            scoped.addLayer(ta::hpi::MountSet(root/"download"),"",true);
            const auto tile=[&](bool stock) {auto b=scoped.read("terrain/12345678.jpg",stock);return std::string(b.begin(),b.end());};
            require(tile(false)=="map"&&tile(true)=="stock","generated terrain ignores map reskins");
            scoped.addLayer(ta::hpi::MountSet(root/"override"));
            require(tile(false)=="user"&&tile(true)=="user","explicit user terrain override retained");
            // The retail Boneyards/ folder (the Darien Crusades campaign art) is not mounted.
            const auto artRoot = root / "crusades-root";
            fs::create_directories(artRoot / "Boneyards" / "Metagame");
            { std::ofstream f(artRoot / "Boneyards" / "Metagame" / "Darien.def"); f << "synthetic"; }
            {
                auto artVfs = ta::hpi::mountRetailRoot(artRoot);
                require(!artVfs.has("Boneyards/Metagame/Darien.def", true), "Boneyards campaign art mounted");
                require(artVfs.list("Boneyards/Metagame", true).empty(), "Boneyards folder listed");
            }
            if(argc>1) {
                auto real=ta::hpi::mountRetailRoot(ta::u8path(argv[1]));
                auto all=ta::hpi::listMaps(real);
                unsigned checked=0;
                for(const auto& [name,path]:all) {
                    if(name.find_first_of("\xba\xe9") == std::string::npos) continue;
                    require(ta::hpi::findMap(real,name)==path,"installed map lookup");
                    require(!real.read(path).empty(),"installed map data readable");
                    require(!ta::sim::parseStartPositions(real,path).empty(),"installed map start positions");
                    ++checked;
                }
                require(checked>=2,"installed legacy-named maps covered");
                std::cout<<"Installed map catalog: "<<all.size()<<" maps, "<<checked<<" legacy-named maps read\n";
            }
        }
        fs::remove_all(root);
        std::cout<<"PASS: Unicode loose maps and byte-preserved archive maps in C locale\n";
    } catch(const std::exception& e) {
        std::cerr<<"FAIL: "<<e.what()<<'\n';
        std::error_code ec;fs::remove_all(root,ec);return 1;
    }
}
