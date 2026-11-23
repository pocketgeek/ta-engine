#pragma once

// Start this program again with the given arguments: how takclient and Cartographer
// apply a new GAME DATA FOLDER (client/datafolder.h says why a restart and not a live
// remount). Called after the old process has torn SDL down, so no window, audio
// device or socket is held twice.
//
//   * Linux / macOS: exec in place. The process image is replaced, keeping the PID,
//     the working directory and the terminal, so a launcher or shell waiting on the
//     game keeps waiting on the new one. Only returns on failure.
//   * Windows: CreateProcessW a new copy (same working directory), then return true;
//     the caller exits normally.

#include <string>
#include <vector>

namespace tak {

// The running executable's own path (/proc/self/exe, _NSGetExecutablePath, the module
// file name), UTF-8. Empty if the platform will not say.
std::string selfExecutable();

// See above. `args` excludes argv[0]. On failure returns false and says why in `error`.
bool relaunchSelf(const std::vector<std::string>& args, std::string& error);

// A Windows command line that CommandLineToArgvW / the MSVC CRT split back into
// exactly `argv` (argv[0] included): arguments with spaces, tabs or quotes are quoted,
// and backslashes are doubled only where they precede a quote. Pure; tested everywhere.
inline std::string windowsCommandLine(const std::vector<std::string>& argv) {
    std::string out;
    for (size_t i = 0; i < argv.size(); ++i) {
        const std::string& a = argv[i];
        if (i) out += ' ';
        if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos) { out += a; continue; }
        out += '"';
        size_t slashes = 0;
        for (char c : a) {
            if (c == '\\') { ++slashes; continue; }
            if (c == '"') out.append(slashes * 2 + 1, '\\');   // escape the run and the quote
            else out.append(slashes, '\\');
            slashes = 0;
            out += c;
        }
        out.append(slashes * 2, '\\');   // a trailing run must not escape the closing quote
        out += '"';
    }
    return out;
}

}  // namespace tak
