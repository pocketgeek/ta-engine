#include "cartographer/recovery.h"
#include "util/u8path.h"
#include "cartographer/document.h"
#include "cartographer/scenarioinfo.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace cart {
namespace fs=std::filesystem;
namespace {
fs::path lockPath(const fs::path& file) {auto result=file;result+=".lock";return result;}
fs::path backupPath(const fs::path& file) {auto result=file;result+=".bak";return result;}
std::string utf8(const fs::path& path) {const auto s=path.u8string();return {s.begin(),s.end()};}
}
bool RecoveryFile::acquire(bool createLock) {
    const auto lock=lockPath(file_);
#ifdef _WIN32
    HANDLE h=CreateFileW(lock.c_str(),GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,createLock?OPEN_ALWAYS:OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE)return false;
    OVERLAPPED offset{};
    if(!LockFileEx(h,LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,0,1,0,&offset)) {CloseHandle(h);return false;}
    handle_=reinterpret_cast<intptr_t>(h);
#else
    const int fd=open(lock.c_str(),O_RDWR|O_CLOEXEC|(createLock?O_CREAT:0),0600);
    if(fd<0)return false;
    if(flock(fd,LOCK_EX|LOCK_NB)!=0) {close(fd);return false;}
    handle_=fd;
#endif
    return true;
}
RecoveryFile::~RecoveryFile() {
    if(handle_==-1)return;
    std::error_code ec;
    const bool empty=!fs::exists(file_,ec) && !ec && !fs::exists(backupPath(file_),ec) && !ec;
    // Remove the pathname while still leased. Scanners of a session directory
    // never recreate its lock; after acquiring they also recheck the snapshot.
    if(empty)fs::remove(lockPath(file_),ec);
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
    close(int(handle_));
#endif
    if(empty && grouped_)fs::remove(file_.parent_path(),ec); // never remove unrelated contents
}
std::unique_ptr<RecoveryFile> RecoveryFile::create(const fs::path& folder) {
    if(folder.empty())return {};
    static std::atomic<unsigned> serial=0;
    fs::path directory;
    do {
        directory=folder/("recovery-"+std::to_string(std::chrono::system_clock::now().time_since_epoch().count())+"-"+std::to_string(serial++));
    } while(!fs::create_directory(directory));
    auto lease=std::unique_ptr<RecoveryFile>(new RecoveryFile(directory/"recovery-map.kmp",true));
    if(!lease->acquire(true)) {
        std::error_code ec;fs::remove(lockPath(lease->file_),ec);fs::remove(directory,ec);
        throw std::runtime_error("Cannot reserve a recovery session");
    }
    return lease;
}
std::unique_ptr<RecoveryFile> RecoveryFile::claimNewest(const fs::path& folder) {
    if(folder.empty())return {};
    std::vector<std::pair<fs::file_time_type,std::pair<fs::path,bool>>> candidates;
    std::error_code ec;
    for(fs::directory_iterator it(folder,ec),end; !ec && it!=end;it.increment(ec)) {
        const auto& entry=*it;
        if(entry.is_symlink(ec) || !utf8(entry.path().filename()).starts_with("recovery-"))continue;
        const bool grouped=entry.is_directory(ec);
        auto file=grouped?entry.path()/"recovery-map.kmp":entry.path();
        if(!grouped && file.extension()==".bak")file.replace_extension();
        if(file.extension()!=".kmp")continue;
        const auto available=fs::is_regular_file(file,ec)?file:backupPath(file);
        ec.clear();
        if(!fs::is_regular_file(available,ec) || fs::is_symlink(available,ec))continue;
        const auto time=fs::last_write_time(available,ec);if(!ec)candidates.push_back({time,{file,grouped}});
    }
    std::sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b){return a.first>b.first;});
    for(const auto& [time,candidate]:candidates) {
        auto lease=std::unique_ptr<RecoveryFile>(new RecoveryFile(candidate.first,candidate.second));
        if(!lease->acquire(!candidate.second))continue; // old flat recovery files have no lease file
        if(fs::is_regular_file(lease->path(),ec) || fs::is_regular_file(backupPath(lease->path()),ec))return lease;
    }
    return {};
}
bool RecoveryFile::discard(std::string& error) {
    error.clear();
    for(const auto& file:{file_,backupPath(file_)}) {
        std::error_code ec;fs::remove(file,ec);
        if(ec) {error="Could not remove recovery file: "+ec.message();return false;}
    }
    return true;
}
RecoverySnapshot readRecoverySnapshot(const fs::path& file) {
    std::string errors;
    for(const auto& source:{file,backupPath(file)}) {
        try {
            RecoverySnapshot result;result.files=std::make_shared<tak::hpi::Vfs::Files>();
            tak::hpi::Archive archive(source);
            for(const auto& entry:archive.entries())if(!entry.isDirectory) {
                auto bytes=archive.read(entry);const auto key=tak::hpi::MountSet::key(entry.path);
                if(key.ends_with(".tnt")) {
                    if(!result.mapPath.empty())throw std::runtime_error("Recovery contains multiple maps");
                    tak::tnt::Map::load(bytes,key);result.mapPath=key;
                }
                // Recovery is written by our canonical serializer. The legacy
                // parser returns an empty default on malformed input, so check
                // the round trip instead of mistaking that default for success.
                if(key.ends_with(".crt") && tak::crt::write(tak::crt::parse(bytes))!=bytes)throw std::runtime_error("Recovery scenario is damaged");
                if(key=="recovery-info.txt" || key.ends_with("/recovery-info.txt"))result.destination=readRecoveryInfo(bytes);
                (*result.files)[key]=std::move(bytes);
            }
            if(result.mapPath.empty())throw std::runtime_error("Recovery contains no map terrain");
            const auto stem=result.mapPath.substr(0,result.mapPath.size()-4);
            if(const auto info=result.files->find(stem+".editor");info!=result.files->end()) {
                auto scenario=tak::crt::parse(result.files->at(stem+".crt"));
                if(!applyScenarioInfo(scenario,std::string(info->second.begin(),info->second.end())))throw std::runtime_error("Recovery rule names are damaged");
            }
            result.fromBackup=source!=file;return result;
        } catch(const std::exception& e) {
            if(!errors.empty())errors+="\nPrevious recovery: ";
            errors+=e.what();
        }
    }
    throw std::runtime_error("Cannot read recovery; both files have been preserved.\n"+errors);
}
tak::hpi::PackFile recoveryInfo(const std::string& name,const fs::path& directory) {
    std::ostringstream out;
    out<<"TAK_CARTOGRAPHER_RECOVERY 1\n"<<std::quoted(name)<<'\n'<<std::quoted(utf8(fs::absolute(directory)))<<'\n';
    const auto text=out.str();return {"recovery-info.txt",{text.begin(),text.end()}};
}
std::optional<RecoveryDestination> readRecoveryInfo(const std::vector<uint8_t>& bytes) {
    if(bytes.size()>65536)return {};
    std::istringstream in(std::string(bytes.begin(),bytes.end()));std::string magic;int version=0;
    RecoveryDestination result;
    if(!(in>>magic>>version) || magic!="TAK_CARTOGRAPHER_RECOVERY" || version!=1 ||
       !(in>>std::quoted(result.name)>>std::quoted(result.directory)) || !validDocumentName(result.name) ||
       result.directory.empty() || result.directory.size()>4096 || result.directory.find('\0')!=std::string::npos ||
       !tak::u8path(result.directory).is_absolute())return {};
    return result;
}
}
