#include "server/filelease.h"

#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace tak::srv {
struct FileLease::Impl {
    std::filesystem::path path;
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~Impl() { if(handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
#else
    int fd = -1;
    ~Impl() { if(fd >= 0) close(fd); }
#endif
};

FileLease::FileLease(const std::filesystem::path& path)
    : impl_(std::make_unique<Impl>()) {
    namespace fs = std::filesystem;
    if(path.empty())
        throw std::runtime_error("file lease requires a path");
    impl_->path = fs::weakly_canonical(fs::absolute(path));
    if(!fs::is_directory(impl_->path.parent_path()))
        throw std::runtime_error("lease parent directory does not exist");
    const auto status = fs::status(impl_->path);
    if(fs::exists(status)) {
        if(!fs::is_regular_file(status) || fs::hard_link_count(impl_->path) != 1)
            throw std::runtime_error("leased path must be a regular file without hard-link aliases");
    }
    auto lock = impl_->path;
    lock += ".service-lock";
#ifdef _WIN32
    impl_->handle = CreateFileW(lock.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if(impl_->handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot open lease file (Windows error " + std::to_string(GetLastError()) + ")");
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(impl_->handle, &info) || info.nNumberOfLinks != 1 ||
       (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
        throw std::runtime_error("lease file must be a regular file without aliases");
    OVERLAPPED offset{};
    if(!LockFileEx(impl_->handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                  0, 1, 0, &offset))
        throw std::runtime_error("already in use by another running takserver");
#else
    impl_->fd = open(lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if(impl_->fd < 0)
        throw std::runtime_error("cannot open lease file: " + std::string(std::strerror(errno)));
    struct stat info{};
    if(fstat(impl_->fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1)
        throw std::runtime_error("lease file must be a regular file without aliases");
    if(flock(impl_->fd, LOCK_EX | LOCK_NB) != 0)
        throw std::runtime_error("already in use by another running takserver");
#endif
}

FileLease::~FileLease() = default;
const std::filesystem::path& FileLease::path() const { return impl_->path; }
} // namespace tak::srv
