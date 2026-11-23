#include "cartographer/document.h"
#include "util/u8path.h"
#include "cartographer/scenarioinfo.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <fstream>
#include <memory>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cart {
namespace fs = std::filesystem;
namespace {
std::vector<uint8_t> bytes(const std::string& s) { return {s.begin(),s.end()}; }
void replace(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw fs::filesystem_error("replace",from,to,std::error_code(GetLastError(),std::system_category()));
#else
    fs::rename(from,to);
#endif
}
void write(const fs::path& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path,std::ios::binary|std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot create staged map file");
    f.write(reinterpret_cast<const char*>(data.data()),std::streamsize(data.size()));
    f.flush();
    if (!f) throw std::runtime_error("Cannot write map (disk full or write error)");
    f.close();
    if (!f) throw std::runtime_error("Cannot close map file after writing");
}
}

bool validDocumentName(const std::string& name) {
    if (name.empty() || name=="." || name==".." || name.back()=='.' || name.back()==' ') return false;
    for (unsigned char c:name) if(c<32 || std::string("/\\:*?\"<>|").find(char(c))!=std::string::npos) return false;
    auto base=name.substr(0,name.find('.'));
    for(auto& c:base)if(c>='a' && c<='z')c=char(c-'a'+'A');
    if(base=="CON" || base=="PRN" || base=="AUX" || base=="NUL" || base=="CONIN$" || base=="CONOUT$")return false;
    if(base.size()==4 && (base.starts_with("COM") || base.starts_with("LPT")) && base[3]>='1' && base[3]<='9')return false;
    return true;
}

bool aiFileKey(const std::string& path) {
    const auto key=tak::hpi::MountSet::key(path);
    return key.starts_with("ai/") && key.ends_with(".txt") && key.find('/',3)==std::string::npos &&
           key.size()>7 && validDocumentName(key.substr(3));
}

AiFiles aiFilesFromArchive(const tak::hpi::Archive& archive) {
    AiFiles result;
    for(const auto& entry:archive.entries()) if(!entry.isDirectory && aiFileKey(entry.path))
        result[tak::hpi::MountSet::key(entry.path)]=archive.read(entry);
    return result;
}

AiFiles aiFilesFromFolder(const fs::path& mapFolder) {
    AiFiles result;std::error_code ec;
    for(const auto& folder:fs::directory_iterator(mapFolder.empty()?fs::path("."):mapFolder,ec)) {
        const auto name=folder.path().filename().u8string();
        if(tak::hpi::MountSet::key(std::string(name.begin(),name.end()))!="ai" || !folder.is_directory(ec))continue;
        for(const auto& file:fs::directory_iterator(folder.path(),ec)) {
            const auto text=file.path().filename().u8string();const std::string path="ai/"+std::string(text.begin(),text.end());
            if(!file.is_regular_file(ec) || !aiFileKey(path))continue;
            std::ifstream stream(file.path(),std::ios::binary);
            if(stream)result[tak::hpi::MountSet::key(path)]={std::istreambuf_iterator<char>(stream),{}};
        }
    }
    return result;
}

AiFiles aiFilesFromMapArchive(const tak::hpi::Vfs& vfs, const std::string& mapPath) {
    AiFiles result;
    for(const auto& path:vfs.mapArchivePaths(mapPath)) if(aiFileKey(path))
        if(auto data=vfs.readFromMapArchive(mapPath,path))result[tak::hpi::MountSet::key(path)]=std::move(*data);
    return result;
}

AiFiles liftAiFiles(tak::hpi::Vfs::Files& files) {
    AiFiles result;
    for(auto it=files.begin();it!=files.end();) {
        if(aiFileKey(it->first)) {result[tak::hpi::MountSet::key(it->first)]=std::move(it->second);it=files.erase(it);}
        else ++it;
    }
    return result;
}

namespace {
std::string fileText(const fs::path& path) {const auto text=path.u8string();return {text.begin(),text.end()};}
std::string nameOf(const fs::path& path) {return fileText(path.filename());}
std::vector<uint8_t> readAll(const fs::path& path) {
    std::ifstream f(path,std::ios::binary);
    if(!f)throw std::runtime_error("Cannot read "+fileText(path));
    return {std::istreambuf_iterator<char>(f),{}};
}
// A whole file, failing as a filesystem_error so the read-only case reads plainly.
void writeOrThrow(const fs::path& path,const std::vector<uint8_t>& data) {
    errno=0;
    std::ofstream f(path,std::ios::binary|std::ios::trunc);
    if(f)f.write(reinterpret_cast<const char*>(data.data()),std::streamsize(data.size()));
    if(f)f.close();
    if(!f)throw fs::filesystem_error("write",path,std::error_code(errno?errno:EIO,std::generic_category()));
}
// What a failed write says (SAVE FAILED): a read-only install in plain words.
std::string writeFailure(const fs::filesystem_error& e) {
    const auto where=fileText(e.path1().empty()?e.path2():e.path1());
    const auto code=e.code();
    if(code==std::errc::permission_denied || code==std::errc::read_only_file_system || code==std::errc::operation_not_permitted)
        return "Cannot write "+where+": the data folder is read-only (for example a game installed under Program Files). "
               "Nothing in the pack was changed. Give your account write access to the game's mods folder, or use a copy of the game in a folder you can write to.";
    return "Cannot write "+where+": "+code.message()+". Nothing in the pack was changed.";
}
}

std::string modPackNameError(const std::string& name) {
    if(name.empty())return "Type a pack name.";
    if(name.size()>128)return "A pack name has at most 128 bytes.";
    if(name[0]=='.')return "A pack name cannot start with '.' (the game skips such folders).";
    if(!validDocumentName(name))return "A pack name cannot contain / \\ : * ? \" < > |, end in '.' or a space, or be a reserved device name.";
    return {};
}

std::string modPackReservedError(const fs::path& dataRoot, const std::string& name) {
    using tak::hpi::MountSet;
    if(MountSet::key(name)==tak::hpi::kAlwaysEnabledDir)
        return "always_enabled holds the packs the game always applies; pick another name.";
    std::vector<std::string> always;
    try {always=tak::hpi::alwaysEnabledPacks(dataRoot,tak::hpi::ModKind::Game);} catch(const std::exception&) {}
    for(const auto& pack:always)if(MountSet::key(pack)==MountSet::key(name))
        return "mods/game/always_enabled/"+pack+" already uses that name, and its copy is the one the game loads. "
               "Move it out of always_enabled to edit it here.";
    return {};
}

std::string alwaysEnabledGameNote(const fs::path& dataRoot) {
    std::vector<std::string> always;
    try {always=tak::hpi::alwaysEnabledPacks(dataRoot,tak::hpi::ModKind::Game);} catch(const std::exception&) {}
    if(always.empty())return {};
    std::string names;for(const auto& pack:always)names+=(names.empty()?"":", ")+pack;
    return " Always-enabled game packs (mods/game/always_enabled/: "+names+") also apply in the test game.";
}

bool ModProfilePlan::writes() const {
    return std::any_of(changes.begin(),changes.end(),[](const ModProfileChange& c){return c.action!=ModProfileChange::Action::Same;});
}

bool planModProfiles(const fs::path& dataRoot, const std::string& packName, const AiFiles& files,
                     const std::set<std::string>& remove, ModProfilePlan& plan, std::string& error) {
    using tak::hpi::MountSet;using Action=ModProfileChange::Action;
    plan=ModProfilePlan{};error.clear();
    try {
        if(const auto why=modPackNameError(packName);!why.empty()) {error=why;return false;}
        if(const auto why=modPackReservedError(dataRoot,packName);!why.empty()) {error=why;return false;}
        std::error_code ec;
        if(dataRoot.empty() || !fs::is_directory(dataRoot,ec)) {error="The data folder does not exist: "+fileText(dataRoot);return false;}
        // mods/, game/, <pack>/, ai/: an existing real folder of that name in any case
        // (the one hpi's realSubdir mounts), else a folder to create.
        fs::path dir=dataRoot;bool missing=false;
        const std::string parts[]={"mods","game",packName,"ai"};
        for(int i=0;i<4;++i) {
            fs::path next,other;
            if(!missing)for(const auto& entry:fs::directory_iterator(dir,ec)) {
                if(MountSet::key(nameOf(entry.path()))!=MountSet::key(parts[i]))continue;
                std::error_code e;
                if(!entry.is_symlink(e) && entry.is_directory(e)) {next=entry.path();break;}
                if(other.empty())other=entry.path();
            }
            if(next.empty() && !other.empty()) {
                std::error_code e;
                error=fs::is_symlink(fs::symlink_status(other,e))?
                    fileText(other)+" is a symbolic link. The game never reads game mods through links; use a real folder.":
                    fileText(other)+" is not a folder.";
                return false;
            }
            if(next.empty()) {missing=true;next=dir/tak::u8path(parts[i]);plan.create.push_back(next);}
            dir=next;
            if(i==2) {plan.packFolder=dir;plan.pack=nameOf(dir);}
        }
        plan.folder=dir;
        std::map<std::string,fs::path> onDisk;std::set<std::string> links;
        if(!missing)for(const auto& entry:fs::directory_iterator(dir,ec)) {
            const auto key=MountSet::key("ai/"+nameOf(entry.path()));std::error_code e;
            if(entry.is_symlink(e))links.insert(key);
            else if(entry.is_regular_file(e) && !onDisk.emplace(key,entry.path()).second) {
                error=fileText(dir)+" holds two files named "+key.substr(3)+" in different case; remove one first.";return false;
            }
        }
        std::unique_ptr<MountSet> archives;
        if(fs::is_directory(plan.packFolder,ec))
            try {archives=std::make_unique<MountSet>(plan.packFolder,tak::hpi::MountConfig{false,{".hpi",".ufo",".kmp"},{},{}});} catch(const std::exception&) {}
        auto add=[&](const std::string& key,const std::vector<uint8_t>* data) {
            if(key!=MountSet::key(key) || !aiFileKey(key)) {error="Invalid AI profile file name: "+key;return false;}
            if(links.count(key)) {error=fileText(dir/tak::u8path(key.substr(3)))+" is a symbolic link; it was not replaced.";return false;}
            ModProfileChange change;change.key=key;
            const auto found=onDisk.find(key);const bool exists=found!=onDisk.end();
            change.path=exists?found->second:dir/tak::u8path(key.substr(3));
            if(exists)change.existing=readAll(change.path);
            if(data) {change.data=*data;change.action=!exists?Action::Create:change.existing==change.data?Action::Same:Action::Replace;}
            else if(exists)change.action=Action::Delete;
            else return true;   // nothing to delete
            if(archives)if(const int a=archives->archiveOf(key);a>=0)change.archive=nameOf(archives->archiveFiles()[size_t(a)]);
            plan.changes.push_back(std::move(change));return true;
        };
        for(const auto& [key,data]:files)if(!data.empty() && !add(key,&data))return false;
        for(const auto& key:remove) {
            const auto it=files.find(key);
            if((it==files.end() || it->second.empty()) && !add(key,nullptr))return false;
        }
        std::sort(plan.changes.begin(),plan.changes.end(),[](const auto& a,const auto& b){return a.key<b.key;});
    } catch(const std::exception& e) {error=e.what();return false;}
    return true;
}

bool writeModProfiles(const fs::path& dataRoot, const ModProfilePlan& plan, const fs::path& backupRoot,
                      std::string& error, fs::path* backupFolder) {
    using Action=ModProfileChange::Action;
    error.clear();if(backupFolder)backupFolder->clear();
    // The plan the user confirmed must still describe the disk.
    AiFiles files;std::set<std::string> remove;
    for(const auto& change:plan.changes) {if(change.action==Action::Delete)remove.insert(change.key);else files[change.key]=change.data;}
    ModProfilePlan now;
    if(!planModProfiles(dataRoot,plan.pack,files,remove,now,error))return false;
    bool same=now.folder==plan.folder && now.changes.size()==plan.changes.size();
    for(size_t i=0;same && i<now.changes.size();++i)
        same=now.changes[i].key==plan.changes[i].key && now.changes[i].action==plan.changes[i].action && now.changes[i].existing==plan.changes[i].existing;
    if(!same) {error="The pack's files changed on disk since they were compared. Nothing was written; save again to see the new comparison.";return false;}
    if(!now.writes())return true;
    fs::path stage,backup;
    std::vector<size_t> installed;
    bool preserveStage=false;
    try {
        // Backups first, outside the pack: a failure here changes nothing.
        if(std::any_of(now.changes.begin(),now.changes.end(),[](const auto& c){return c.action==Action::Replace || c.action==Action::Delete;})) {
            if(backupRoot.empty())throw std::runtime_error("There is no Cartographer profile folder for the backups, so nothing was replaced.");
            char stamp[32]{};const std::time_t t=std::time(nullptr);
            if(const std::tm* utc=std::gmtime(&t))std::strftime(stamp,sizeof stamp,"%Y%m%d-%H%M%SZ",utc);
            const auto base=backupRoot/"mod-backups"/tak::u8path(now.pack);
            backup=base/stamp;
            for(int n=2;fs::exists(backup);++n)backup=base/(std::string(stamp)+"-"+std::to_string(n));
            fs::create_directories(backup/"ai");
            for(const auto& change:now.changes)if(change.action==Action::Replace || change.action==Action::Delete)
                writeOrThrow(backup/"ai"/change.path.filename(),change.existing);
        }
        for(const auto& folder:now.create)fs::create_directory(folder);
        // Staged beside the packs: files directly in mods/game/ are never mounted.
        const auto game=now.packFolder.parent_path();
        static std::atomic<unsigned> serial{0};
        do stage=game/(".tak-ai-stage-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(serial++));
        while(!fs::create_directory(stage));
        for(size_t i=0;i<now.changes.size();++i) {
            const auto& change=now.changes[i];
            if(change.action==Action::Create || change.action==Action::Replace)writeOrThrow(stage/(std::to_string(i)+".new"),change.data);
            if(change.action==Action::Replace)writeOrThrow(stage/(std::to_string(i)+".old"),change.existing);
        }
        for(size_t i=0;i<now.changes.size();++i) {
            const auto& change=now.changes[i];
            if(change.action==Action::Same)continue;
            if(change.action==Action::Delete)replace(change.path,stage/(std::to_string(i)+".old"));
            else replace(stage/(std::to_string(i)+".new"),change.path);
            installed.push_back(i);
        }
    } catch(const fs::filesystem_error& e) {error=writeFailure(e);}
    catch(const std::exception& e) {error=e.what();}
    if(!error.empty())for(auto it=installed.rbegin();it!=installed.rend();++it) {
        const auto& change=now.changes[*it];
        try {
            if(change.action==Action::Create)fs::remove(change.path);
            else replace(stage/(std::to_string(*it)+".old"),change.path);
        } catch(const std::exception& restore) {
            preserveStage=true;
            error+="\nRollback failed: "+std::string(restore.what())+". Copies remain in "+fileText(stage)+" and "+fileText(backup);
        }
    }
    std::error_code ec;
    if(!stage.empty() && !preserveStage)fs::remove_all(stage,ec);
    if(!error.empty() && !preserveStage && !backup.empty())fs::remove_all(backup,ec);   // nothing was replaced after all
    if(error.empty() && backupFolder)*backupFolder=backup;
    return error.empty();
}

AiFiles readModProfiles(const fs::path& dataRoot, const std::string& pack) {
    const auto packs=tak::hpi::modPacks(dataRoot,tak::hpi::ModKind::Game);
    if(!std::binary_search(packs.begin(),packs.end(),pack))throw std::runtime_error("mod pack not found: "+pack);
    AiFiles result;
    for(auto& [path,data]:tak::hpi::modFiles(dataRoot,tak::hpi::ModKind::Game,{pack}))
        if(aiFileKey(path))result[tak::hpi::MountSet::key(path)]=std::move(data);
    return result;
}

fs::path installMapsFolder(const fs::path& dataRoot) {
    std::error_code ec;
    if(dataRoot.empty())return {};
    for(const auto& entry:fs::directory_iterator(dataRoot,ec)) {
        const auto name=entry.path().filename().u8string();
        if(tak::hpi::MountSet::key(std::string(name.begin(),name.end()))=="maps" && entry.is_directory(ec))return entry.path();
    }
    return {};
}

bool insideFolder(const fs::path& path, const fs::path& folder) {
    if(folder.empty())return false;
    std::error_code ec;
    const auto p=fs::weakly_canonical(fs::absolute(path.empty()?fs::path("."):path),ec);if(ec)return false;
    const auto f=fs::weakly_canonical(fs::absolute(folder),ec);if(ec)return false;
    // Windows and macOS folders usually ignore case; compare names that way there.
    auto same=[](const fs::path& a,const fs::path& b) {
#if defined(_WIN32) || defined(__APPLE__)
        const auto x=a.u8string(),y=b.u8string();
        return tak::hpi::MountSet::key(std::string(x.begin(),x.end()))==tak::hpi::MountSet::key(std::string(y.begin(),y.end()));
#else
        return a==b;
#endif
    };
    auto pi=p.begin();
    for(auto fi=f.begin();fi!=f.end();++fi) {
        if(fi->empty())continue;   // a trailing separator
        if(pi==p.end() || !same(*pi,*fi))return false;
        ++pi;
    }
    return true;
}

DocumentFiles documentFiles(
    const tak::tnt::Map& map, tak::tnt::Scenario metadata,
    const tak::crt::Scenario& scenario, const std::vector<PlacedUnit>& units,
    const std::set<std::string>& useOnly, const AiFiles& aiFiles, const std::string& name) {
    if (!validDocumentName(name)) throw std::runtime_error("Choose a map name without path separators or reserved filename characters");
    metadata.hasScenario = !units.empty() || !scenario.customTypes.empty() || !scenario.regions.empty();
    for (const auto& player:scenario.players) if(!player.empty()) metadata.hasScenario=true;
    metadata.sizeW=map.width/32; metadata.sizeH=map.height/32;
    metadata.useOnlyUnits=useOnly.empty()?"":name+".tdf";
    DocumentFiles files;
    files.members={{name+".tnt",map.save()}, {name+".ota",bytes(metadata.write())},
            {name+".crt",saveScenario(scenario,units)},
            {name+".editor",bytes(scenarioInfo(scenario))},
            // Always write even an empty restriction file to retire stale exports.
            {name+".tdf",bytes(writeUseOnly({useOnly.begin(),useOnly.end()}))},
            {name+".txt",bytes(metadata.missionDescription.empty()?metadata.missionName:metadata.missionDescription)}};
    for(const auto& [path,data]:aiFiles) {
        if(path!=tak::hpi::MountSet::key(path) || !aiFileKey(path))throw std::runtime_error("Invalid AI profile file name: "+path);
        // A map package rejects an empty AI file; empty means deleted.
        if(!data.empty())files.rootMembers.push_back({path,data});
    }
    return files;
}

bool writeDocumentFiles(const fs::path& directory, const std::vector<tak::hpi::PackFile>& files,
                        std::string& error, const std::atomic_bool* cancel) {
    fs::path stage;
    std::vector<bool> existed;
    size_t installed=0;
    bool preserveStage=false;
    error.clear();
    try {
        auto checkCancel=[&] {if(cancel && cancel->load())throw std::runtime_error("Save cancelled");};
        checkCancel();
        const fs::path dir=directory.empty()?fs::path("."):directory;
        if(!fs::is_directory(dir)) throw std::runtime_error("Save folder does not exist");
        std::set<std::string> names;
        for(const auto& file:files) {
            if(!validDocumentName(file.path) || !names.insert(file.path).second)
                throw std::runtime_error("Invalid or duplicate export filename");
            const auto target=dir/tak::u8path(file.path);
            if(fs::exists(target) && !fs::is_regular_file(target))
                throw std::runtime_error("Save destination is not a regular file");
        }
        static std::atomic<unsigned> serial{0};
        do {
            stage=dir/(".tak-save-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(serial++));
        } while(!fs::create_directory(stage));
        for(size_t i=0;i<files.size();++i) {
            checkCancel();
            write(stage/(std::to_string(i)+".new"),files[i].data);
            const auto target=dir/tak::u8path(files[i].path);
            existed.push_back(fs::exists(target));
            if(existed.back()) {
                fs::copy_file(target,stage/(std::to_string(i)+".old"));
                fs::copy_file(target,stage/(std::to_string(i)+".backup"));
            }
        }
        // Cancellation ends at publication: finish this short transaction atomically
        // (or roll it back) instead of abandoning a partly installed document.
        checkCancel();
        // Finish every write/backup before replacing any member of the document.
        for(size_t i=0;i<files.size();++i) if(existed[i])
            replace(stage/(std::to_string(i)+".backup"),dir/tak::u8path(files[i].path+".bak"));
        for(size_t i=0;i<files.size();++i) {
            replace(stage/(std::to_string(i)+".new"),dir/tak::u8path(files[i].path));
            ++installed;
        }
    } catch(const std::exception& e) {
        error=e.what();
        const fs::path dir=directory.empty()?fs::path("."):directory;
        for(size_t i=0;i<installed;++i) {
            try {
                const auto target=dir/tak::u8path(files[i].path);
                if(existed[i]) replace(stage/(std::to_string(i)+".old"),target);
                else fs::remove(target);
            } catch(const std::exception& restore) {
                preserveStage=true;
                error += "\nRollback failed: "+std::string(restore.what())+". Recovery copies remain in "+stage.string();
            }
        }
    }
    if(!stage.empty() && !preserveStage) { std::error_code ec;fs::remove_all(stage,ec); }
    return error.empty();
}

bool writeDocumentBundle(const fs::path& path, const DocumentFiles& files,
                         std::string& error, const std::atomic_bool* cancel) {
    try {
        if(cancel && cancel->load())throw std::runtime_error("Save cancelled");
        const auto name=path.stem().u8string();
        const std::string stem(name.begin(),name.end());
        if(!validDocumentName(stem)) throw std::runtime_error("Invalid map filename");
        auto members=files.members;
        for(auto& member:members) member.path="kmap/"+stem+"/"+member.path;
        // AI profiles sit at the archive root, where the game's VFS reads ai/<x>.txt.
        for(const auto& member:files.rootMembers) members.push_back(member);
        const auto filename=path.filename().u8string();
        return writeDocumentFiles(path.parent_path(),{{std::string(filename.begin(),filename.end()),tak::hpi::pack(members)}},error,cancel);
    } catch(const std::exception& e) { error=e.what();return false; }
}
bool writeDocumentLoose(const fs::path& directory, const DocumentFiles& files, const fs::path& installMaps,
                        std::string& error, const std::atomic_bool* cancel) {
    error.clear();
    if(!files.rootMembers.empty()) {
        if(insideFolder(directory,installMaps)) {
            error="This map has AI profiles. Loose files in the game's Maps folder would apply to every map. Save as .kmp to keep AI profiles with the map.";
            return false;
        }
        try {
            const fs::path dir=directory.empty()?fs::path("."):directory;
            if(!fs::is_directory(dir)) throw std::runtime_error("Save folder does not exist");
            fs::path ai;std::error_code ec;
            for(const auto& entry:fs::directory_iterator(dir,ec)) {
                const auto name=entry.path().filename().u8string();
                if(tak::hpi::MountSet::key(std::string(name.begin(),name.end()))=="ai") {ai=entry.path();break;}
            }
            if(ai.empty()) {ai=dir/"ai";fs::create_directory(ai);}
            std::vector<tak::hpi::PackFile> profiles;
            for(const auto& member:files.rootMembers) profiles.push_back({member.path.substr(3),member.data});
            // The profiles first: a failure here leaves the map files untouched.
            if(!writeDocumentFiles(ai,profiles,error,cancel))return false;
        } catch(const std::exception& e) {error=e.what();return false;}
    }
    return writeDocumentFiles(directory,files.members,error,cancel);
}
} // namespace cart
