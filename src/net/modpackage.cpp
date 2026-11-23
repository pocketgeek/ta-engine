#include "net/modpackage.h"
#include "util/storagequota.h"
#include <algorithm>
#include <fstream>
#include <stdexcept>
namespace tak::net::mods {
namespace {
// "TOV2": every file is a u32-length blob. Any other magic is rejected.
constexpr uint32_t kPackageMagic = 0x32564f54;
std::string hash(const std::vector<uint8_t>& b){return crypto::toHex(crypto::sha256(b.data(),b.size()));}
bool safe(const std::string& p) {
    if(p.empty() || p.size()>512 || p!=hpi::MountSet::key(p) || p.front()=='/' || p.find('\\')!=p.npos || p.find(':')!=p.npos || p.find('\0')!=p.npos)return false;
    size_t start=0;while(start<p.size()){auto end=p.find('/',start);if(end==p.npos)end=p.size();auto part=p.substr(start,end-start);if(part.empty() || part=="." || part=="..")return false;start=end+1;}
    return p.back()!='/';
}
}
std::shared_ptr<Package> decode(std::vector<uint8_t> bytes,const std::string& digest) {
    if(bytes.size()>maps::kMaxBytes || !maps::validDigest(digest) || hash(bytes)!=digest)throw std::runtime_error("mod package checksum mismatch");
    Reader r(bytes.data(),bytes.size());if(r.u32()!=kPackageMagic)throw std::runtime_error("invalid mod package version");
    const auto names=r.u32();if(names>64)throw std::runtime_error("too many mod packs");
    std::string description,previous;
    for(uint32_t i=0;i<names;++i){auto name=r.str();if(!r.ok || name.empty() || name.size()>128 || name.find_first_of("/\\\r\n")!=name.npos || name.find('\0')!=name.npos || (!previous.empty() && name<=previous))throw std::runtime_error("invalid mod pack name");previous=name;if(i)description+=", ";description+=name;}
    auto files=std::make_shared<hpi::Vfs::Files>();const auto count=r.u32();if(count>65536)throw std::runtime_error("too many mod files");previous.clear();
    for(uint32_t i=0;i<count;++i){auto path=r.str();if(!r.ok || !safe(path) || (!previous.empty() && path<=previous))throw std::runtime_error("invalid mod file path");previous=path;auto data=r.blob(maps::kMaxBytes);if(!r.ok)throw std::runtime_error("truncated mod file");files->emplace(std::move(path),std::move(data));}
    if(!r.ok || r.p!=r.end)throw std::runtime_error("invalid mod package length");
    auto result=std::make_shared<Package>();result->mapPath=description;result->digest=digest;result->files=std::move(files);result->bytes=std::move(bytes);return result;
}
std::shared_ptr<Package> build(const std::filesystem::path& root,const std::vector<std::string>& selected) {
    auto names=selected;std::sort(names.begin(),names.end());auto files=hpi::modFiles(root,hpi::ModKind::Game,names);
    Writer w;w.u32(kPackageMagic);w.u32(uint32_t(names.size()));for(const auto& name:names)w.str(name);
    w.u32(uint32_t(files.size()));for(const auto& [path,data]:files){if(data.size()>maps::kMaxBytes-w.b.size())throw std::runtime_error("mod package too large");w.str(path);w.blob(data);if(w.b.size()>maps::kMaxBytes)throw std::runtime_error("mod package too large");}
    auto digest=hash(w.b);return decode(std::move(w.b),digest);
}
void saveCache(const std::filesystem::path& root,const Package& package,uint64_t quota) {
    if(!maps::validDigest(package.digest))throw std::runtime_error("invalid mod cache key");
    auto dir=root/"ModCache",path=dir/(package.digest+".takmods");
    // The caller already verified these bytes, so an identical file is reused
    // without decoding it. A damaged entry is replaced by the rename below, never
    // deleted first: a quota or disk failure must leave the old file in place.
    std::error_code ec;const auto existing=std::filesystem::is_regular_file(path,ec)?std::filesystem::file_size(path,ec):0;
    if(!ec && existing==package.bytes.size()) {
        std::vector<uint8_t> b(existing);std::ifstream in(path,std::ios::binary);in.read(reinterpret_cast<char*>(b.data()),std::streamsize(existing));
        if(in && b==package.bytes)return;
    }
    const auto used=quota?tak::storageUsage(dir):0;
    tak::storageRoom(used-std::min<uint64_t>(used,ec?0:existing),package.bytes.size(),quota);
    std::filesystem::create_directories(dir);
    const auto tmp=dir/(package.digest+"."+crypto::toHex(crypto::randomVec(8))+".tmp");
    try {std::ofstream out(tmp,std::ios::binary);out.write(reinterpret_cast<const char*>(package.bytes.data()),std::streamsize(package.bytes.size()));out.close();if(!out)throw std::runtime_error("cannot save mod cache");std::filesystem::rename(tmp,path);}
    catch(...){std::error_code ec;std::filesystem::remove(tmp,ec);throw;}
}
std::shared_ptr<Package> loadCache(const std::filesystem::path& root,const std::string& digest,size_t maxBytes,size_t expected) {
    if(!maps::validDigest(digest))return {};
    auto path=root/"ModCache"/(digest+".takmods");
    std::error_code ec;auto n=std::filesystem::file_size(path,ec);
    if(ec || !n || n>maps::kMaxBytes)return {};
    if(n>maxBytes || (expected && n!=expected))throw std::runtime_error("cached mod size/budget mismatch");
    try {std::vector<uint8_t> b(n);std::ifstream in(path,std::ios::binary);in.read(reinterpret_cast<char*>(b.data()),std::streamsize(n));if(!in)return {};return decode(std::move(b),digest);}catch(const std::exception&){return {};}
}
}
