#include "cartographer/savejob.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
static void check(bool good,const char* message) {if(!good)throw std::runtime_error(message);}
static std::vector<uint8_t> read(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};
}
int main() try {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-savejob-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    struct Cleanup {fs::path p;~Cleanup(){std::error_code error;fs::remove_all(p,error);}} cleanup{root};
    cart::SaveSnapshot source;auto& map=source.map;
    map.width=map.height=64;map.blocksX=map.blocksY=32;
    map.heights.assign(4096,40);map.features.assign(4096,0xffff);
    map.tileKeys.assign(1024,0);map.tileCols.assign(1024,0);map.tileRows.assign(1024,0);
    source.metadata.missionDescription="Snapshot before editing";
    source.destination=root/"Saved map.kmp";
    auto progress=std::make_shared<cart::SaveProgress>();
    auto job=cart::saveInBackground(source,progress);
    source.metadata.missionDescription="Later edits";source.map.heights[0]=70;
    check(job.get().ok,"background save failed");
    check(progress->phase==cart::SaveProgress::Finished,"save completion not reported");
    const auto first=read(source.destination);
    tak::hpi::Archive saved(source.destination);bool terrain=false,description=false;
    for(const auto& entry:saved.entries()) {
        if(entry.path.ends_with(".tnt"))terrain=tak::tnt::Map::load(saved.read(entry)).heights[0]==40;
        if(entry.path.ends_with(".ota")) {const auto bytes=saved.read(entry);description=tak::tnt::Scenario::parse(std::string(bytes.begin(),bytes.end())).missionDescription=="Snapshot before editing";}
    }
    check(terrain && description,"worker read changed source instead of its snapshot");
    progress=std::make_shared<cart::SaveProgress>();progress->cancel=true;
    const auto cancelled=cart::saveInBackground(source,progress).get();
    check(!cancelled.ok && cancelled.error=="Save cancelled","cancellation ignored");
    check(read(source.destination)==first && !fs::exists(source.destination.string()+".bak"),"cancelled save modified original or backup");
    check(cart::saveInBackground(source,std::make_shared<cart::SaveProgress>()).get().ok,"replacement save failed");
    check(read(source.destination.string()+".bak")==first,"background replacement lost previous save");
    source.bundle=false;source.destination=root/"Loose map.tnt";
    check(cart::saveInBackground(source,std::make_shared<cart::SaveProgress>()).get().ok,"background loose export failed");
    check(fs::exists(root/"Loose map.crt") && fs::exists(root/"Loose map.editor") && fs::exists(root/"Loose map.tdf"),"loose export dropped companions");
    const auto previous=read(source.destination);
    fs::remove(root/"Loose map.crt");fs::create_directory(root/"Loose map.crt");
    source.map.heights[0]=90;
    auto failed=cart::saveInBackground(source,std::make_shared<cart::SaveProgress>()).get();
    check(!failed.ok && !failed.error.empty() && read(source.destination)==previous,"failed export replaced part of the document");
    for(const auto& entry:fs::directory_iterator(root))check(!entry.path().filename().string().starts_with(".tak-save-"),"save leaked staging directory");
    // AI profiles: root members of the .kmp, an ai/ folder beside a loose export,
    // and refused as loose files inside the install's Maps folder.
    fs::create_directory(root/"Maps");
    const std::vector<uint8_t> level{'w','e','i','g','h','t',' ','x',' ','0','\r','\n'};
    source.aiFiles={{"ai/n.txt",level},{"ai/saved.txt",{'l','i','m','i','t',' ','f','l','y',' ','2'}}};
    source.metadata.aiProfile="saved";source.installMaps=root/"Maps";
    source.bundle=true;source.destination=root/"Profiles.kmp";
    check(cart::saveInBackground(source,std::make_shared<cart::SaveProgress>()).get().ok,"save with AI profiles failed");
    std::set<std::string> rootAi;
    {
        tak::hpi::Archive withAi(source.destination);
        for(const auto& entry:withAi.entries()) if(!entry.isDirectory) {
            const auto key=tak::hpi::MountSet::key(entry.path);
            if(key.starts_with("ai/")) {rootAi.insert(key);check(withAi.read(entry)==source.aiFiles.at(key),"AI profile bytes changed");}
            check(!(key.starts_with("kmap/") && key.find("/ai/")!=std::string::npos),"AI profile saved under kmap/");
        }
    }
    check(rootAi==std::set<std::string>({"ai/n.txt","ai/saved.txt"}),"AI profiles missing from the .kmp root");
    source.bundle=false;source.destination=root/"Profiles.tnt";
    check(cart::saveInBackground(source,std::make_shared<cart::SaveProgress>()).get().ok,"loose export with AI profiles failed");
    check(read(root/"ai"/"n.txt")==level,"loose export did not write ai/n.txt beside the map");
    source.destination=root/"Maps"/"Profiles.tnt";
    const auto refused=cart::saveInBackground(source,std::make_shared<cart::SaveProgress>()).get();
    check(!refused.ok && refused.error.find("Save as .kmp")!=std::string::npos && !fs::exists(root/"Maps"/"Profiles.tnt"),"loose AI profiles accepted in the Maps folder");
    std::cout<<"PASS: background immutable snapshots, cancellation, backup preservation, failed-export atomicity and AI profiles\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
