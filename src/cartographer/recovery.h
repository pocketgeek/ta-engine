#pragma once
#include "hpi/hpi.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace cart {
// An OS-held exclusive lease. Process termination releases it automatically;
// timestamps or PID files alone cannot establish whether an editor is alive.
class RecoveryFile {
public:
    static std::unique_ptr<RecoveryFile> create(const std::filesystem::path& folder);
    static std::unique_ptr<RecoveryFile> claimNewest(const std::filesystem::path& folder);
    ~RecoveryFile();
    RecoveryFile(const RecoveryFile&)=delete;
    RecoveryFile& operator=(const RecoveryFile&)=delete;
    const std::filesystem::path& path() const {return file_;}
    bool discard(std::string& error);
private:
    RecoveryFile(std::filesystem::path file,bool grouped):file_(std::move(file)),grouped_(grouped) {}
    bool acquire(bool createLock);
    std::filesystem::path file_;
    bool grouped_=false;
    intptr_t handle_=-1;
};

struct RecoveryDestination {std::string name,directory;};
struct RecoverySnapshot {
    std::shared_ptr<tak::hpi::Vfs::Files> files;
    std::string mapPath;
    std::optional<RecoveryDestination> destination;
    bool fromBackup=false;
};
// Validate before exposing any recovered members to the editor. Fall back to
// the previous atomic-save generation without changing either file on disk.
RecoverySnapshot readRecoverySnapshot(const std::filesystem::path& file);
tak::hpi::PackFile recoveryInfo(const std::string& name,const std::filesystem::path& directory);
std::optional<RecoveryDestination> readRecoveryInfo(const std::vector<uint8_t>& bytes);
}
