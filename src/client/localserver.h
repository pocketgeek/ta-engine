#pragma once

// The private takserver a single-player / campaign / benchmark session runs on
// (AIs run only on a server). One owner per session: the destructor stops the
// child, so every way out of a session -- a failed launch looping back to the
// menu, an error return, the normal teardown -- takes the server with it. The
// connect poll asks exited() each attempt, so a server that dies on startup (no
// binary, unreadable data, a crash) fails the launch at once instead of after the
// whole connect timeout. Process control is the one genuinely platform-specific
// bit: fork/exec + waitpid on POSIX, CreateProcess + GetExitCodeProcess on Windows.
// On Windows the child also gets the client's standard handles (its log goes where
// the client's does, as a forked child's does on POSIX) and runs in a job that
// closes with the client, so a client that is killed or crashes cannot leave its
// server running, holding the port and its files.
// Header-only and SDL-free (tools/local_server_test.cpp drives it directly).

#include <string>
#include <vector>
#include "util/u8path.h"

#ifdef _WIN32
  #include <filesystem>
  #include "util/winargv.h"   // <windows.h> (NOMINMAX) + quoteWindowsArgument
#else
  #include <csignal>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

namespace tak {

class LocalServer {
public:
    LocalServer() = default;
    ~LocalServer() { stop(); }
    LocalServer(const LocalServer&) = delete;
    LocalServer& operator=(const LocalServer&) = delete;

    // Launch `program` (a path; neither platform searches PATH here) with `args`.
    // False when no process could be created. A missing binary is reported here on
    // Windows; on POSIX the fork succeeds and the child exits 127 (see exited()).
    bool start(const std::string& program, const std::vector<std::string>& args) {
        stop();
#ifdef _WIN32
        const auto executable = tak::u8path(program).wstring();
        std::wstring cmd = quoteWindowsArgument(executable);
        for (const auto& a : args) cmd += L" " + quoteWindowsArgument(tak::u8path(a).wstring());
        // Inheritable copies of the client's standard handles, passed as the only
        // handles the child inherits (a GUI launch with none keeps the child's empty).
        STARTUPINFOEXW si{}; si.StartupInfo.cb = sizeof(si);
        HANDLE std[3] = {};
        std::vector<HANDLE> inherit;
        const DWORD which[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
        for (int i = 0; i < 3; ++i) {
            const HANDLE h = GetStdHandle(which[i]);
            if (h && h != INVALID_HANDLE_VALUE &&
                DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &std[i], 0, TRUE, DUPLICATE_SAME_ACCESS))
                inherit.push_back(std[i]);
        }
        std::vector<char> attributes;
        if (!inherit.empty()) {
            SIZE_T size = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
            attributes.resize(size);
            si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
            if (InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size) &&
                UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(),
                                          inherit.size() * sizeof(HANDLE), nullptr, nullptr)) {
                si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
                si.StartupInfo.hStdInput = std[0];
                si.StartupInfo.hStdOutput = std[1];
                si.StartupInfo.hStdError = std[2];
            } else {
                si.lpAttributeList = nullptr;
            }
        }
        const bool handles = si.lpAttributeList != nullptr;
        const bool made = CreateProcessW(executable.c_str(), cmd.data(), nullptr, nullptr, handles,
                                         CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                                         nullptr, nullptr, &si.StartupInfo, &proc_);
        if (handles) DeleteProcThreadAttributeList(si.lpAttributeList);
        for (HANDLE h : std) if (h) CloseHandle(h);
        if (!made) return false;
        // Into a kill-on-close job before it runs: the client's last handle to the job
        // closes when the client ends in any way, and Windows then ends the server.
        job_ = CreateJobObjectW(nullptr, nullptr);
        if (job_) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
            limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limit, sizeof limit) ||
                !AssignProcessToJobObject(job_, proc_.hProcess)) {
                CloseHandle(job_); job_ = nullptr;   // the server still runs, just without the guard
            }
        }
        ResumeThread(proc_.hThread);
        CloseHandle(proc_.hThread); proc_.hThread = nullptr;
        up_ = true;
#else
        // Build argv BEFORE forking: the client is multi-threaded, and only
        // async-signal-safe calls (no allocation) are allowed in the child.
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(program.c_str()));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        const pid_t pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            execv(program.c_str(), argv.data());
            _exit(127);   // exec failed
        }
        pid_ = pid;
        up_ = true;
#endif
        exited_ = false;
        exitCode_ = 0;
        return true;
    }

    // Non-blocking: has the child exited (or died)? Reaps it on POSIX. *code gets its
    // exit status (128 + signal for a POSIX child killed by a signal). False while it
    // runs, and when nothing was started.
    bool exited(int* code = nullptr) {
        if (!up_) return false;
        if (!exited_) {
#ifdef _WIN32
            if (WaitForSingleObject(proc_.hProcess, 0) != WAIT_OBJECT_0) return false;
            DWORD c = 0;
            GetExitCodeProcess(proc_.hProcess, &c);
            exitCode_ = int(c);
#else
            int st = 0;
            if (waitpid(pid_, &st, WNOHANG) != pid_) return false;
            exitCode_ = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 1;
#endif
            exited_ = true;
        }
        if (code) *code = exitCode_;
        return true;
    }

    // True between a successful start() and stop() (even if the child has since died;
    // ask exited() for that).
    bool started() const { return up_; }

    // Stop the child (if still running) and release it. Safe to call repeatedly.
    void stop() {
        if (!up_) return;
#ifdef _WIN32
        if (!exited()) {
            TerminateProcess(proc_.hProcess, 0);
            WaitForSingleObject(proc_.hProcess, 2000);
        }
        CloseHandle(proc_.hProcess);
        proc_ = {};
        if (job_) { CloseHandle(job_); job_ = nullptr; }
#else
        if (!exited()) { kill(pid_, SIGTERM); waitpid(pid_, nullptr, 0); }
        pid_ = 0;
#endif
        up_ = false;
    }

    // The child's process id (for the benchmark's server-side CPU/memory sampling).
    long pid() const {
        if (!up_) return 0;
#ifdef _WIN32
        return long(proc_.dwProcessId);
#else
        return long(pid_);
#endif
    }

private:
    bool up_ = false, exited_ = false;
    int exitCode_ = 0;
#ifdef _WIN32
    PROCESS_INFORMATION proc_{};
    HANDLE job_ = nullptr;   // kill-on-close job holding the child (see start)
#else
    pid_t pid_ = 0;
#endif
};

}  // namespace tak
