#include "util/winargv.h"
#include "cartographer/playtest.h"
#include "cartographer/document.h"
#include "net/mappackage.h"
#include "tnt/tnt.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <stdexcept>
#include "util/u8path.h"

static void check(bool good,const char* message) {if(!good)throw std::runtime_error(message);}
static int runTest(int argc,char** argv) try {
    namespace fs=std::filesystem;
    // This executable also stands in for the game, exercising OS argument passing
    // and the full child lifecycle without SDL, assets, or a shell command.
    if(argc>1 && std::string(argv[1])=="--data") {
        // TEST IN GAME adds --game-mod <pack> (an AI mod without a map): this pack only.
        check((argc==5 || (argc==7 && std::string(argv[5])=="--game-mod" && std::string(argv[6])=="My AI \xc3\xbc & $")) &&
              std::string(argv[3])=="--play-map","launch arguments lost");
        check(tak::u8path(argv[2]).filename()==tak::u8path("data spaces ü & $"),"data path quoting/Unicode changed");
        tak::hpi::Archive snapshot(tak::u8path(argv[4]));
        check(!snapshot.entries().empty(),"child could not read snapshot");
        // The game's own import (takclient --play-map): the whitelisted AI profiles
        // reach the test room; a stray ai/ file stays behind.
        tak::hpi::Vfs base;auto art=std::make_shared<tak::hpi::Vfs::Files>();
        (*art)["terrain/00000001.jpg"]={1};base.setModFiles(art);   // the tile art a real install supplies
        const auto package=tak::net::maps::importSnapshot(base,tak::u8path(argv[4]));
        check(package->files->count("ai/takengine.txt") && package->files->at("ai/takengine.txt")==std::vector<uint8_t>({'w',' ','x',' ','0','\n'}),"test room lost ai/takengine.txt");
        check(package->files->count("ai/islands.txt"),"test room lost the map's own DEFAULT profile");
        check(!package->files->count("ai/notes.txt"),"a stray ai/ file travelled into the test room");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return 0;
    }
    {
        // clientPath: macOS bundle layouts (DMG: two apps; ZIP: apps plus bare takclient).
        const auto top=fs::temp_directory_path()/tak::u8path("tak clientpath ü");
        std::error_code ec;fs::remove_all(top,ec);
        const auto editor=top/"dmg"/"TAK Cartographer.app"/"Contents";
        const auto game=top/"dmg"/"Total Annihilation - Kingdoms.app"/"Contents"/"MacOS";
        fs::create_directories(editor/"Resources");fs::create_directories(editor/"MacOS");fs::create_directories(game);
        auto touch=[](const fs::path& f){std::ofstream(f)<<"x";};
        auto fails=[](const fs::path& d){try{cart::Playtest::clientPath(d);}catch(const std::runtime_error&){return true;}return false;};
        check(fails(editor/"Resources"/""),"clientPath found a game that is not there");
        touch(game/"takclient");
        check(cart::Playtest::clientPath(editor/"Resources"/"")==game/"takclient","Resources base path did not find the game app");
        check(cart::Playtest::clientPath(editor/"MacOS")==game/"takclient","MacOS base path did not find the game app");
        fs::create_directories(top/"zip"/"TAK Cartographer.app"/"Contents"/"Resources");
        check(fails(top/"zip"/"TAK Cartographer.app"/"Contents"/"Resources"/""),"clientPath found a game in the wrong folder");
        touch(top/"zip"/"takclient");
        check(cart::Playtest::clientPath(top/"zip"/"TAK Cartographer.app"/"Contents"/"Resources"/"")==top/"zip"/"takclient","bare takclient beside the bundle not found");
#ifdef _WIN32
        const fs::path plain="takclient.exe";
#else
        const fs::path plain="takclient";
#endif
        fs::create_directories(top/"plain");touch(top/"plain"/plain);
        check(cart::Playtest::clientPath(top/"plain"/"")==top/"plain"/plain,"sibling takclient lookup changed");
        fs::remove_all(top,ec);
    }
    cart::Playtest test;
    tak::tnt::Map map;map.width=map.height=32;map.blocksX=map.blocksY=16;
    map.heights.assign(1024,40);map.features.assign(1024,0xffff);
    map.tileKeys.assign(256,1);map.tileCols.assign(256,0);map.tileRows.assign(256,0);
    tak::tnt::Scenario metadata;metadata.aiProfile="islands";
    const auto files=cart::documentFiles(map,metadata, {},{}, {},
        {{"ai/takengine.txt",{'w',' ','x',' ','0','\n'}},{"ai/islands.txt",{'l','i','m','i','t',' ','f','l','y',' ','2','\n'}},{"ai/notes.txt",{'p','l','a','n','\n'}}},"Snapshot");
    const auto root=fs::temp_directory_path()/tak::u8path("data spaces ü & $");
    std::string error;
    check(!test.start(root/"nonexistent",root,files,error) && !error.empty() && !test.running(),"missing executable did not fail cleanly");
    check(test.start(fs::absolute(tak::u8path(argv[0])),root,files,error),error.c_str());
    const auto path=test.snapshot();check(fs::is_regular_file(path),"temporary snapshot missing");
    check(!test.start(fs::absolute(tak::u8path(argv[0])),root,files,error),"second concurrent test accepted");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    std::optional<int> result;
    while(!result && std::chrono::steady_clock::now()<deadline) {
        result=test.poll();std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(result && *result==0,"child launch/argument test failed or timed out");
    check(!fs::exists(path.parent_path()) && !test.running(),"finished snapshot not cleaned up");
    check(map.width==32 && files.members.front().data==map.save(),"playtest modified source document");
    // An AI mod's TEST IN GAME: the same snapshot handoff plus the pack to tick.
    check(test.start(fs::absolute(tak::u8path(argv[0])),root,files,error,"My AI \xc3\xbc & $"),error.c_str());
    result.reset();
    while(!result && std::chrono::steady_clock::now()<deadline+std::chrono::seconds(15)) {
        result=test.poll();std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(result && *result==0,"the --game-mod launch lost an argument or timed out");
    std::cout<<"PASS: temporary playtest snapshot, AI profiles in the test room, Unicode arguments, --game-mod, single child, launch failure and cleanup\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}

int main(int argc,char** argv) {
#ifdef _WIN32
    (void)argc; (void)argv;
    return tak::utf8Main(runTest);
#else
    return runTest(argc,argv);
#endif
}
