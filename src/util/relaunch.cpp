#include "util/relaunch.h"

#include <cerrno>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <unistd.h>
  #ifdef __APPLE__
    #include <mach-o/dyld.h>
  #endif
#endif

namespace tak {

std::string selfExecutable() {
#if defined(_WIN32)
    std::wstring buf(32768, L'\0');   // the long-path limit; GetModuleFileNameW truncates silently
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    if (n == 0 || n >= buf.size()) return {};
    buf.resize(n);
    const auto text = std::filesystem::path(buf).u8string();
    return {text.begin(), text.end()};
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);   // asks for the size
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    buf.resize(std::strlen(buf.c_str()));
    std::error_code ec;
    auto real = std::filesystem::canonical(buf, ec);   // resolve the ../ and symlinks dyld may report
    return ec ? buf : real.string();
#else
    std::error_code ec;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : exe.string();
#endif
}

bool relaunchSelf(const std::vector<std::string>& args, std::string& error) {
    const std::string exe = selfExecutable();
    if (exe.empty()) { error = "cannot locate this program's executable"; return false; }
#ifdef _WIN32
    std::vector<std::string> argv{exe};
    argv.insert(argv.end(), args.begin(), args.end());
    const std::string line = windowsCommandLine(argv);
    auto wide = [](const std::string& s) {
        std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0)), L'\0');
        if (!w.empty()) { MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), int(w.size())); w.pop_back(); }
        return w;
    };
    std::wstring wexe = wide(exe), wline = wide(line);
    STARTUPINFOW si{}; si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(wexe.c_str(), wline.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        error = "CreateProcess failed (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    std::vector<std::string> argv{exe};
    argv.insert(argv.end(), args.begin(), args.end());
    std::vector<char*> raw;
    for (auto& a : argv) raw.push_back(a.data());
    raw.push_back(nullptr);
    execv(exe.c_str(), raw.data());
    error = std::string("exec failed: ") + std::strerror(errno);   // only reached on failure
    return false;
#endif
}

}  // namespace tak
