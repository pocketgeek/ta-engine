#pragma once
// Native "choose folder" dialog + simple message box for the first-run data-dir setup.
// SDL2 has no folder-picker, so we spawn each platform's standard helper and read the
// path from stdout -- no extra link dependencies. Messages use SDL's built-in box.
#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace tak {

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

// Quote one argument for the POSIX shell popen runs: single quotes, with each embedded
// ' closed, escaped and reopened. The start folder comes from the saved settings (a
// folder name may hold quotes, spaces or $), so nothing is pasted in unquoted.
inline std::string dpShellQuote(const std::string& s) {
    std::string q = "'";
    for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
}
// The same for a PowerShell single-quoted string literal ('' is a literal quote), which
// itself sits inside the double-quoted -Command argument: a " would end that, so it is
// left out (no Windows path contains one).
inline std::string dpPowerShellQuote(const std::string& s) {
    std::string q = "'";
    for (char c : s) { if (c == '"') continue; q += c; if (c == '\'') q += c; }
    return q + "'";
}

// Pop up the OS folder picker. Returns the chosen absolute path, or "" if cancelled or
// no picker is available. `start` seeds the initial folder where the platform supports it
// (Windows, kdialog, zenity; macOS's choose folder opens where it last was).
//
// Blocks until the dialog closes: first-run startup calls it before any window exists,
// and the in-app GAME DATA FOLDER rows (Options, Cartographer's Preferences) call it from
// a worker thread so the window keeps drawing behind the dialog.
inline std::string pickDirectory(const std::string& title, const std::string& start) {
#if defined(_WIN32)
    std::string ps =
        "powershell -NoProfile -STA -Command \""
        "Add-Type -AssemblyName System.Windows.Forms;"
        "[Console]::OutputEncoding=New-Object Text.UTF8Encoding $false;"   // UTF-8 (no BOM), not the ANSI code page
        "$f=New-Object System.Windows.Forms.FolderBrowserDialog;"
        "$f.Description=" + dpPowerShellQuote(title) + ";$f.ShowNewFolderButton=$false;"
        + (start.empty() ? std::string() : "$f.SelectedPath=" + dpPowerShellQuote(start) + ";") +
        "if($f.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK){[Console]::Out.Write($f.SelectedPath)}\"";
    std::string picked = dpRunCapture(ps);
    if (picked.rfind("\xEF\xBB\xBF", 0) == 0) picked.erase(0, 3);   // in case a host still adds a BOM
    return picked;
#elif defined(__APPLE__)
    (void)start;
    std::string t;   // an AppleScript string literal: escape \ and "
    for (char c : title) { if (c == '"' || c == '\\') t += '\\'; t += c; }
    return dpRunCapture("osascript -e 'try' -e " + dpShellQuote("POSIX path of (choose folder with prompt \"" + t + "\")")
                        + " -e 'end try' 2>/dev/null");
#else
    const std::string s = start.empty() ? std::string("\"$HOME\"") : dpShellQuote(start);
    if (dpHaveCmd("kdialog"))
        return dpRunCapture("kdialog --title " + dpShellQuote(title) + " --getexistingdirectory " + s + " 2>/dev/null");
    if (dpHaveCmd("zenity"))   // a trailing / makes --filename open INSIDE the folder
        return dpRunCapture("zenity --file-selection --directory --title=" + dpShellQuote(title) +
                            (start.empty() ? std::string() : " --filename=" + dpShellQuote(start + "/")) + " 2>/dev/null");
    return "";
#endif
}

// Pop up the OS "open file" dialog for an audio file (WAV, MP3, FLAC, Ogg). Returns the
// chosen absolute path, or "" if cancelled or no picker is available. `start` seeds the
// folder where the platform supports it. Blocks like pickDirectory: call it from a worker.
inline std::string pickAudioFile(const std::string& title, const std::string& start) {
#if defined(_WIN32)
    std::string ps =
        "powershell -NoProfile -STA -Command \""
        "Add-Type -AssemblyName System.Windows.Forms;"
        "[Console]::OutputEncoding=New-Object Text.UTF8Encoding $false;"
        "$f=New-Object System.Windows.Forms.OpenFileDialog;"
        "$f.Title=" + dpPowerShellQuote(title) + ";"
        "$f.Filter='Audio files (*.wav;*.mp3;*.flac;*.ogg)|*.wav;*.mp3;*.flac;*.ogg|All files (*.*)|*.*';"
        + (start.empty() ? std::string() : "$f.InitialDirectory=" + dpPowerShellQuote(start) + ";") +
        "if($f.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK){[Console]::Out.Write($f.FileName)}\"";
    std::string picked = dpRunCapture(ps);
    if (picked.rfind("\xEF\xBB\xBF", 0) == 0) picked.erase(0, 3);
    return picked;
#elif defined(__APPLE__)
    (void)start;
    std::string t;
    for (char c : title) { if (c == '"' || c == '\\') t += '\\'; t += c; }
    return dpRunCapture("osascript -e 'try' -e " +
                        dpShellQuote("POSIX path of (choose file with prompt \"" + t + "\" of type {\"public.audio\"})") +
                        " -e 'end try' 2>/dev/null");
#else
    const std::string s = start.empty() ? std::string("\"$HOME\"") : dpShellQuote(start);
    if (dpHaveCmd("kdialog"))
        return dpRunCapture("kdialog --title " + dpShellQuote(title) + " --getopenfilename " + s +
                            " " + dpShellQuote("*.wav *.mp3 *.flac *.ogg|Audio files") + " 2>/dev/null");
    if (dpHaveCmd("zenity"))
        return dpRunCapture("zenity --file-selection --title=" + dpShellQuote(title) +
                            " --file-filter=" + dpShellQuote("Audio files | *.wav *.mp3 *.flac *.ogg") +
                            (start.empty() ? std::string() : " --filename=" + dpShellQuote(start + "/")) + " 2>/dev/null");
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

inline void errorBox(const std::string& title, const std::string& msg) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), msg.c_str(), nullptr);
}

}  // namespace tak
