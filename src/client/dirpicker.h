#pragma once
// Native "choose folder" dialog + simple message box for the first-run data-dir setup.
// SDL2 has no folder-picker, so we spawn each platform's standard helper and read the
// path from stdout -- no extra link dependencies. Messages use SDL's built-in box.
#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace ta {

// Run `cmd`, return its trimmed stdout ("" on failure).
inline std::string dpRunCapture(const std::string& cmd) {
    std::string out;
#ifdef _WIN32
    FILE* p = _popen(cmd.c_str(), "r");
#else
    FILE* p = popen(cmd.c_str(), "r");
#endif
    if (!p) return "";
    char buf[1024];
    while (std::fgets(buf, sizeof buf, p)) out += buf;
#ifdef _WIN32
    _pclose(p);
#else
    pclose(p);
#endif
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

#ifndef _WIN32
inline bool dpHaveCmd(const char* c) {
    return std::system((std::string("command -v ") + c + " >/dev/null 2>&1").c_str()) == 0;
}
#endif

// Pop up the OS folder picker. Returns the chosen absolute path, or "" if cancelled or
// no picker is available. `start` seeds the initial folder where the platform supports it.
inline std::string pickDirectory(const std::string& title, const std::string& start) {
#if defined(_WIN32)
    std::string ps =
        "powershell -NoProfile -STA -Command \""
        "Add-Type -AssemblyName System.Windows.Forms;"
        "$f=New-Object System.Windows.Forms.FolderBrowserDialog;"
        "$f.Description='" + title + "';$f.ShowNewFolderButton=$false;"
        "if($f.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK){[Console]::Out.Write($f.SelectedPath)}\"";
    return dpRunCapture(ps);
#elif defined(__APPLE__)
    (void)start;
    return dpRunCapture("osascript -e 'try' -e 'POSIX path of (choose folder with prompt \""
                        + title + "\")' -e 'end try' 2>/dev/null");
#else
    std::string s = start.empty() ? std::string("$HOME") : ("\"" + start + "\"");
    if (dpHaveCmd("kdialog"))
        return dpRunCapture("kdialog --title \"" + title + "\" --getexistingdirectory " + s + " 2>/dev/null");
    if (dpHaveCmd("zenity"))
        return dpRunCapture("zenity --file-selection --directory --title=\"" + title + "\" 2>/dev/null");
    return "";
#endif
}

// True if a native folder picker is available on this system (so the caller can fall back
// to a clear stderr instruction rather than silently failing).
inline bool haveDirPicker() {
#if defined(_WIN32) || defined(__APPLE__)
    return true;
#else
    return dpHaveCmd("kdialog") || dpHaveCmd("zenity");
#endif
}

inline void infoBox(const std::string& title, const std::string& msg) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title.c_str(), msg.c_str(), nullptr);
}
inline void errorBox(const std::string& title, const std::string& msg) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), msg.c_str(), nullptr);
}

}  // namespace ta
