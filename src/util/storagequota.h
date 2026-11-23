#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace tak {
// Count only regular files without following symlinks. Fail closed on I/O errors
// or an excessive directory, rather than doing unbounded work on the game loop.
inline uint64_t storageUsage(const std::filesystem::path& directory,
                             std::string_view prefix={},size_t maxEntries=20000) {
    if(!std::filesystem::exists(directory))return 0;
    uint64_t total=0;size_t entries=0;
    for(const auto& entry:std::filesystem::directory_iterator(directory)) {
        if(++entries>maxEntries)throw std::runtime_error("storage directory entry limit reached");
        const auto name=entry.path().filename().u8string();
        const std::string_view text(reinterpret_cast<const char*>(name.data()),name.size());
        if(!text.starts_with(prefix))continue;
        if(std::filesystem::is_symlink(entry.symlink_status()))throw std::runtime_error("storage contains a symlink");
        if(entry.is_regular_file()) {
            const auto size=entry.file_size();
            if(size>UINT64_MAX-total)throw std::runtime_error("storage size overflow");
            total+=size;
        }
    }
    return total;
}
inline void storageRoom(uint64_t used,uint64_t added,uint64_t limit) {
    if(limit && (used>limit || added>limit-used))throw std::runtime_error("storage quota reached");
}
}
