#pragma once

#include <filesystem>
#include <memory>

namespace tak::srv {

// Exclusive process lease on a path (takserver's ACME state directory uses it so two
// servers never share one certificate store). The lock lives in a companion file,
// "<path>.service-lock"; the kernel releases it after a crash, and a leftover
// companion file is harmless and must never be deleted to bypass an active lease.
class FileLease {
public:
    explicit FileLease(const std::filesystem::path& path);
    ~FileLease();
    FileLease(const FileLease&) = delete;
    FileLease& operator=(const FileLease&) = delete;
    const std::filesystem::path& path() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tak::srv
