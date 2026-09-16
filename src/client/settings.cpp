#include "client/settings.h"

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace ta {

namespace {

// A flat "key = value" file, one per line, '#' comments. Unknown keys are skipped
// on read (forward-compat) and current values re-emitted on write.
template <typename T>
T clampv(T v, T lo, T hi) { return std::max(lo, std::min(hi, v)); }

}  // namespace

std::string settingsPath() {
    char* base = SDL_GetPrefPath("TAKengine", "TAKingdoms");
    if (!base) return {};
    std::string p = std::string(base) + "settings.ini";
    SDL_free(base);
    return p;
}

Settings loadSettings() {
    Settings s;   // defaults
    std::string path = settingsPath();
    if (path.empty()) return s;
    std::ifstream in(path);
    if (!in) return s;

    std::string line;
    while (std::getline(in, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        auto trim = [](std::string& t) {
            size_t a = t.find_first_not_of(" \t\r\n");
            size_t b = t.find_last_not_of(" \t\r\n");
            t = (a == std::string::npos) ? std::string() : t.substr(a, b - a + 1);
        };
        trim(key); trim(val);
        if (key.empty()) continue;

        auto asInt   = [&](int lo, int hi) { return clampv(std::atoi(val.c_str()), lo, hi); };
        auto asFloat = [&](float lo, float hi) { return clampv(float(std::atof(val.c_str())), lo, hi); };
        auto asBool  = [&] { return val == "1" || val == "true" || val == "on"; };

        if      (key == "fullscreen")      s.fullscreen = asBool();
        else if (key == "vsync")           s.vsync = asBool();
        else if (key == "maxFps")          s.maxFps = asInt(30, 480);
        else if (key == "uiScale")         s.uiScale = asFloat(0.75f, 2.0f);
        else if (key == "antiAlias")       { int a = asInt(0, 4); s.antiAlias = (a >= 4) ? 4 : (a >= 2) ? 2 : 0; }
        else if (key == "buildBarAlign")   s.buildBarAlign = asInt(0, 2);
        // 4.0, matching the slider and GameView. This still said 2.0 when the slider
        // went to 400%, so 300-400% survived until the next restart and then silently
        // snapped back to 200%.
        else if (key == "buildBarScale")   s.buildBarScale = asFloat(0.75f, 4.0f);
        else if (key == "bilinear")        s.bilinear = asBool();
        else if (key == "treeSway")        s.treeSway = asBool();
        else if (key == "unitShadows")     s.unitShadows = asBool();
        else if (key == "smoothArt")       s.smoothArt = asBool();
        else if (key == "videoDeblock")    s.videoDeblock = asBool();
        else if (key == "healthBars")      s.healthBars = asInt(0, 2);
        else if (key == "statsPanel")      s.statsPanel = asBool();
        else if (key == "masterVol")       s.masterVol = asInt(0, 256);
        else if (key == "bgmVol")          s.bgmVol = asInt(0, 256);
        else if (key == "sfxVol")          s.sfxVol = asInt(0, 256);
        else if (key == "mouseZoomSpeed")  s.mouseZoomSpeed = asFloat(0.25f, 4.0f);
        else if (key == "edgeScrollSpeed") s.edgeScrollSpeed = asFloat(0.25f, 4.0f);
        else if (key == "edgeScroll")      s.edgeScroll = asBool();
        else if (key == "cursorScale")     s.cursorScale = asInt(1, 8);
        else if (key == "hardwareCursor")  s.hardwareCursor = asBool();
        else if (key == "smoothMotion")    s.smoothMotion = asBool();
        else if (key == "playerName")      s.playerName = val;
        else if (key == "accountName")     s.accountName = val;
        else if (key == "audioDevice")     s.audioDevice = val;
        else if (key == "lastMap")         s.lastMap = val;
        else if (key == "knownServers") {  // comma-joined, most recent first
            std::stringstream ks(val);
            std::string tok;
            while (std::getline(ks, tok, ',') && s.knownServers.size() < 8)
                if (!tok.empty()) s.knownServers.push_back(tok);
        }
        else if (key == "dataDir")         s.dataDir = val;
        else if (key == "dataManifest")    s.dataManifest = val;
        else if (key.rfind("chanGain", 0) == 0 && key.size() == 9) {  // chanGain0..7
            int i = key[8] - '0';
            if (i >= 0 && i < 8) s.chanGain[i] = asFloat(0.0f, 1.0f);
        }
        else if (key.rfind("hotkey.", 0) == 0 && key.size() > 7) {   // hotkey.<action-id> = <chord>
            s.hotkeys[key.substr(7)] = val;
        }
        else if (key.rfind("campaigndone.", 0) == 0 && key.size() > 13) {  // campaigndone.<id> = i,j,k
            std::string id = key.substr(13);
            std::stringstream cs(val);
            std::string tok;
            while (std::getline(cs, tok, ','))
                if (!tok.empty()) s.campaignCompleted[id].insert(std::atoi(tok.c_str()));
        }
        else if (key.rfind("campaign.", 0) == 0 && key.size() > 9) {   // legacy: campaign.<id> = <count>
            std::string id = key.substr(9);
            int n = std::atoi(val.c_str());   // migrate the old completed-count watermark
            for (int i = 0; i < n; ++i) s.campaignCompleted[id].insert(i);
        }
    }
    return s;
}

bool saveSettings(const Settings& s) {
    std::string path = settingsPath();
    if (path.empty()) return false;

    std::ostringstream o;
    o << "# TA:Kingdoms engine settings\n";
    o << "fullscreen = " << (s.fullscreen ? 1 : 0) << "\n";
    o << "vsync = " << (s.vsync ? 1 : 0) << "\n";
    o << "maxFps = " << s.maxFps << "\n";
    o << "uiScale = " << s.uiScale << "\n";
    o << "antiAlias = " << s.antiAlias << "\n";
    o << "buildBarAlign = " << s.buildBarAlign << "\n";
    o << "buildBarScale = " << s.buildBarScale << "\n";
    o << "bilinear = " << (s.bilinear ? 1 : 0) << "\n";
    o << "treeSway = " << (s.treeSway ? 1 : 0) << "\n";
    o << "unitShadows = " << (s.unitShadows ? 1 : 0) << "\n";
    o << "smoothArt = " << (s.smoothArt ? 1 : 0) << "\n";
    o << "videoDeblock = " << (s.videoDeblock ? 1 : 0) << "\n";
    o << "healthBars = " << s.healthBars << "\n";
    o << "statsPanel = " << (s.statsPanel ? 1 : 0) << "\n";
    o << "masterVol = " << s.masterVol << "\n";
    o << "bgmVol = " << s.bgmVol << "\n";
    o << "sfxVol = " << s.sfxVol << "\n";
    for (int i = 0; i < 8; ++i) o << "chanGain" << i << " = " << s.chanGain[i] << "\n";
    o << "mouseZoomSpeed = " << s.mouseZoomSpeed << "\n";
    o << "edgeScrollSpeed = " << s.edgeScrollSpeed << "\n";
    o << "edgeScroll = " << (s.edgeScroll ? 1 : 0) << "\n";
    o << "cursorScale = " << s.cursorScale << "\n";
    o << "hardwareCursor = " << (s.hardwareCursor ? 1 : 0) << "\n";
    o << "smoothMotion = " << (s.smoothMotion ? 1 : 0) << "\n";
    o << "playerName = " << s.playerName << "\n";
    o << "accountName = " << s.accountName << "\n";
    o << "audioDevice = " << s.audioDevice << "\n";
    o << "lastMap = " << s.lastMap << "\n";
    if (!s.knownServers.empty()) {
        o << "knownServers = ";
        for (size_t i = 0; i < s.knownServers.size(); ++i)
            o << (i ? "," : "") << s.knownServers[i];
        o << "\n";
    }
    o << "dataDir = " << s.dataDir << "\n";
    o << "dataManifest = " << s.dataManifest << "\n";
    for (const auto& [id, chord] : s.hotkeys) o << "hotkey." << id << " = " << chord << "\n";
    for (const auto& [id, done] : s.campaignCompleted) {
        if (done.empty()) continue;
        o << "campaigndone." << id << " = ";
        bool first = true;
        for (int m : done) { o << (first ? "" : ",") << m; first = false; }
        o << "\n";
    }

    // Atomic write: temp file + rename, so a crash mid-write can't corrupt the file.
    std::string tmp = path + ".tmp";
    { std::ofstream out(tmp, std::ios::trunc); if (!out) return false; out << o.str(); if (!out) return false; }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace ta
