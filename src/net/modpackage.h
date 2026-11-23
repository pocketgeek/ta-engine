#pragma once
#include "net/mappackage.h"
namespace tak::net::mods {
using Package=maps::Package;
std::shared_ptr<Package> build(const std::filesystem::path& root,const std::vector<std::string>& names);
std::shared_ptr<Package> decode(std::vector<uint8_t> bytes,const std::string& digest);
void saveCache(const std::filesystem::path& root,const Package& package,uint64_t quota=0);
std::shared_ptr<Package> loadCache(const std::filesystem::path& root,const std::string& digest, size_t maxBytes=maps::kMaxBytes, size_t expected=0);
}
