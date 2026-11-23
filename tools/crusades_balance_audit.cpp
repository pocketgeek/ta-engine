// Research-only metadata diff using the engine's retail VFS and TDF parser.
// Never emits whole definitions or descriptive game prose.
#include "hpi/hpi.h"
#include "tdf/tdf.h"
#include "net/crypto.h"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <fstream>
#include <array>
#include <map>
#include <set>
#include <string>

using Fields=std::map<std::string,std::string>;
static std::string lower(std::string s) {
    for(auto& c:s)c=char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
static std::string escaped(const std::string& s) {
    std::string out;
    for(char c:s) {
        if(c=='\\')out+="\\\\";
        else if(c=='\t')out+="\\t";
        else if(c=='\r')out+="\\r";
        else if(c=='\n')out+="\\n";
        else out+=c;
    }
    return out;
}
static void flatten(const tak::tdf::Node& n,const std::string& prefix,Fields& out) {
    for(const auto& [key,value]:n.values)out.emplace(prefix+key,value);
    std::map<std::string,int> counts;
    for(const auto& [name,child]:n.orderedChildren()) {
        auto label=std::string(name);
        int occurrence=++counts[label];
        flatten(*child,prefix+label+"#"+std::to_string(occurrence)+"/",out);
    }
}
static Fields fields(const tak::hpi::Vfs& vfs,const std::string& path) {
    const auto bytes=vfs.read(path);
    Fields out;
    flatten(tak::tdf::parseText(std::string(bytes.begin(),bytes.end()),path),"",out);
    return out;
}
static bool prose(const std::string& key) {
    const auto leaf=key.substr(key.find_last_of('/')+1);
    return leaf=="name" || leaf=="description" || leaf=="designation";
}
int main(int argc,char**argv) {
    const bool manifest=argc==3 && std::string(argv[2])=="--manifest";
    if(argc!=2 && !manifest){std::cerr<<"usage: crusades_balance_audit <retail-data-root> [--manifest]\n";return 2;}
    try {
        auto root=tak::hpi::mountRetailRoot(argv[1]);
        tak::hpi::Vfs vfs(&root,true); // downloaded map resources cannot change this audit
        if(manifest) {
            tak::hpi::MountSet sources(argv[1],tak::hpi::MountConfig{
                .includeLoose=false,.archiveExts={".hpi"},.keep={},
                .archiveNames=tak::hpi::kRootHpiNames});
            std::cout<<"kind\tpath\tsource\tsize\tsha256\n";
            for(const auto& path:sources.archiveFiles()) {
                std::ifstream file(path,std::ios::binary);
                if(!file)throw std::runtime_error("Cannot fingerprint archive");
                tak::crypto::Sha256 hash;
                std::array<char,65536> buffer{};size_t size=0;
                while(file.read(buffer.data(),buffer.size()) || file.gcount()) {
                    const auto count=size_t(file.gcount());size+=count;hash.update(buffer.data(),count);
                }
                if(!file.eof())throw std::runtime_error("Archive fingerprint read failed");
                std::cout<<"archive\t"<<escaped(path.filename().string())<<"\t-\t"<<size
                         <<'\t'<<tak::crypto::toHex(hash.final())<<'\n';
            }
            for(const auto* prefix:{"units","unitscb","canbuild","canbuildcb","gamedata"})
                for(const auto& path:vfs.list(prefix)) {
                    const auto bytes=vfs.read(path);
                    if(!sources.has(path) || sources.read(path)!=bytes)
                        throw std::runtime_error("VFS/source disagreement: "+path);
                    std::cout<<"member\t"<<escaped(path)<<'\t'<<escaped(sources.sourceOf(path))
                             <<'\t'<<bytes.size()<<'\t'
                             <<tak::crypto::toHex(tak::crypto::sha256(bytes.data(),bytes.size()))<<'\n';
                }
            return 0;
        }
        std::cout<<"kind\tpath\tfield\tstandard\tcrusades\n";
        size_t compared=0,changedFiles=0,changedFields=0,addedFiles=0,removedFiles=0;
        // Only the unit roster differs between the balances. Retail never reads the
        // shipped canbuildcb/ tree: both modes build menus from canbuild/, filtered
        // by the roster. A unit with no unitscb FBI does not exist in Crusades (no
        // fallback to units/), so it is reported as a removed definition.
        const std::string prefix="units",cb="unitscb",extension=".fbi";
        std::map<std::string,std::string> paths;
        for(const auto& path:vfs.list(cb)) {
            const auto key=lower(path);
            if(key.starts_with(cb+"/") && key.ends_with(extension))paths.emplace(key,path);
        }
        for(const auto& path:vfs.list(prefix)) {
            const auto key=lower(path);
            if(!key.starts_with(prefix+"/") || !key.ends_with(extension))continue;
            if(paths.count(cb+key.substr(prefix.size())))continue;
            ++removedFiles;++changedFiles;
            std::cout<<"removed-definition\t"<<escaped(key)<<"\t<definition>\tpresent\t<absent>\n";
        }
        for(const auto& [key,path]:paths) {
            const auto base=prefix+key.substr(cb.size());
            const bool exists=vfs.has(base);
            const auto a=exists?fields(vfs,base):Fields{};
            const auto b=fields(vfs,path);
            ++compared;
            if(!exists)++addedFiles;
            std::set<std::string> keys;
            for(const auto& [k,v]:a)keys.insert(k);
            for(const auto& [k,v]:b)keys.insert(k);
            bool changed=!exists;
            if(changed)std::cout<<"added-definition\t"<<escaped(base)<<"\t<definition>\t<absent>\tpresent\n";
            for(const auto& k:keys) {
                const auto left=a.find(k),right=b.find(k);
                if(left!=a.end() && right!=b.end() && left->second==right->second)continue;
                changed=true;++changedFields;
                const auto value=[&](const auto& it,const auto& end) {
                    if(it==end)return std::string("<absent>");
                    return prose(k)?std::string("<presentation text omitted>"):escaped(it->second);
                };
                std::cout<<(prose(k)?"presentation":(!exists?"added-definition":"changed"))
                    <<'\t'<<escaped(base)<<'\t'<<escaped(k)<<'\t'
                    <<value(left,a.end())<<'\t'<<value(right,b.end())<<'\n';
            }
            if(changed)++changedFiles;
        }
        if(compared==0)throw std::runtime_error("No Crusades definitions found");
        std::cerr<<compared<<" unitscb files compared; "<<changedFiles<<" changed; "
                 <<addedFiles<<" without base; "<<removedFiles<<" without unitscb; "
                 <<changedFields<<" field differences\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
