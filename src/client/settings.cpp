#include "client/settings.h"

#include "client/dev.h"
#include "client/hiddenunits.h"
#include "client/renderdriver.h"
#include "client/waylandchoice.h"
#include "sim/simpool.h"
#include "util/prefpath.h"
#include "video/youtubebitrate.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace tak {

namespace {

// A flat "key = value" file, one per line, '#' comments. Unknown keys are skipped
// on read (forward-compat) and current values re-emitted on write.
template <typename T>
T clampv(T v, T lo, T hi) { return std::max(lo, std::min(hi, v)); }

void trim(std::string& t) {
    size_t a = t.find_first_not_of(" \t\r\n");
    size_t b = t.find_last_not_of(" \t\r\n");
    t = (a == std::string::npos) ? std::string() : t.substr(a, b - a + 1);
}

// Split one line into key and value; false for a blank or comment line. '#' starts a
// comment, except inside a dataDir value, where it is part of a folder name
// ("D:\Games\TAK #2"). serializeSettings never writes an inline comment, so only a
// hand-edited file could put one there.
bool splitLine(const std::string& line, std::string& key, std::string& val) {
    const size_t eq = line.find('='), hash = line.find('#');
    if (eq == std::string::npos || (hash != std::string::npos && hash < eq)) return false;
    key = line.substr(0, eq);
    trim(key);
    val = line.substr(eq + 1);
    if (hash != std::string::npos && key != "dataDir") val.erase(hash - eq - 1);
    trim(val);
    return !key.empty();
}

std::string& pathOverride() { static std::string p; return p; }

// The data-folder keys as this process last read or wrote them (see saveSettings).
struct DiskDataFolder { bool known = false; std::string dir, manifest; };
DiskDataFolder& diskDataFolder() { static DiskDataFolder d; return d; }
// setLaunchGameModPacks: this run's own pack selection, never saved over the player's.
std::optional<std::vector<std::string>>& launchGameModPacks() { static std::optional<std::vector<std::string>> p; return p; }

// One spRoom.gamePack / mpRoom.gamePack / mods.cosmeticPack value (hex of the pack name)
// into `names`.
void readPackValue(const std::string& val, std::vector<std::string>& names) {
    std::string name;bool valid=val.size()%2==0 && val.size()<=256;
    auto digit=[](char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};
    for(size_t i=0;valid && i<val.size();i+=2){int a=digit(val[i]),b=digit(val[i+1]);if(a<0 || b<0)valid=false;else name+=char(a*16+b);}
    if(valid && !name.empty() && name.find_first_of("/\\\r\n") == name.npos && name.find('\0')==name.npos && names.size()<64 && std::find(names.begin(),names.end(),name)==names.end())names.push_back(name);
}
// One "<key> = <hex of the pack name>" line per pack (readPackValue reads them back).
void writePackLines(std::ostream& o, const std::string& key, const std::vector<std::string>& names) {
    static constexpr char hex[]="0123456789abcdef";
    for(const auto& name:names){o<<key<<" = ";for(unsigned char c:name)o<<hex[c>>4]<<hex[c&15];o<<"\n";}
}

bool boolValue(const std::string& val) { return val == "1" || val == "true" || val == "on"; }
int intValue(const std::string& val, int lo, int hi) { return clampv(std::atoi(val.c_str()), lo, hi); }
// Exactly one stable mode byte; anything else falls back to Retail.
sim::PathfindingMode pathModeValue(const std::string& val) {
    return val.size() == 1 && val[0] >= '0' && sim::validPathfindingMode(uint8_t(val[0] - '0'))
        ? sim::PathfindingMode(val[0] - '0') : sim::PathfindingMode::Retail;
}

// One spRoom.<field> / mpRoom.<field> value into its set. Unknown fields are skipped, as
// every unknown key is.
void parseRoomPref(RoomPrefs& p, const std::string& field, const std::string& val) {
    if      (field == "crusades")          p.crusades = boolValue(val);
    else if (field == "doubleSight")       p.doubleSight = boolValue(val);
    else if (field == "speedUnlock")       p.speedUnlock = boolValue(val);
    else if (field == "monarchExpendable") p.monarchExpendable = boolValue(val);
    else if (field == "allowSpectators")   p.allowSpectators = boolValue(val);
    else if (field == "powerCodes")        p.powerCodes = boolValue(val);
    else if (field == "pathfindingMode")   p.pathfindingMode = pathModeValue(val);
    else if (field == "unitCap")           p.unitCap = intValue(val, 250, 2000);
    else if (field == "fog")               p.fog = intValue(val, 0, 2);
    else if (field == "randomStarts")      p.randomStarts = intValue(val, 0, 2);
    else if (field == "map")               p.map = val;
    else if (field == "generated")         p.generated = boolValue(val);
    else if (field == "generator")         p.generator = val;
    else if (field == "mapSort")           p.mapSort = intValue(val, 0, 2);
    else if (field == "mapSortDir")        p.mapSortDir = intValue(val, -1, 1) < 0 ? -1 : 1;
    else if (field == "gamePack")          readPackValue(val, p.gamePacks);
}

// Every field of one set, as "<prefix>.<field> = <value>" lines.
void writeRoomPrefs(std::ostream& o, const std::string& prefix, const RoomPrefs& p) {
    o << prefix << ".crusades = " << p.crusades << "\n";
    o << prefix << ".doubleSight = " << p.doubleSight << "\n";
    o << prefix << ".speedUnlock = " << p.speedUnlock << "\n";
    o << prefix << ".monarchExpendable = " << p.monarchExpendable << "\n";
    o << prefix << ".allowSpectators = " << p.allowSpectators << "\n";
    o << prefix << ".powerCodes = " << p.powerCodes << "\n";
    o << prefix << ".pathfindingMode = " << int(p.pathfindingMode) << "\n";
    o << prefix << ".unitCap = " << p.unitCap << "\n";
    o << prefix << ".fog = " << p.fog << "\n";
    o << prefix << ".randomStarts = " << p.randomStarts << "\n";
    o << prefix << ".map = " << p.map << "\n";
    o << prefix << ".generated = " << p.generated << "\n";
    o << prefix << ".generator = " << p.generator << "\n";
    o << prefix << ".mapSort = " << p.mapSort << "\n";
    o << prefix << ".mapSortDir = " << p.mapSortDir << "\n";
    writePackLines(o, prefix + ".gamePack", p.gamePacks);
}

}  // namespace

void setSettingsPath(std::string path) {
    pathOverride() = std::move(path);
    diskDataFolder() = {};   // a different file: nothing is known about it yet
}

std::string settingsPath() {
    if (!pathOverride().empty()) return pathOverride();
    if (const char* o = devEnv("TAK_SETTINGS_FILE")) if (*o) return o;   // debug builds: test profile
    const std::string base = prefPath("TAKingdoms");
    return base.empty() ? std::string() : base + "settings.ini";
}

std::string replayDir() {
    std::string dir = settingsPath();
    const size_t cut = dir.find_last_of("/\\");
    if (cut == std::string::npos) return {};
    dir.erase(cut + 1);
    return dir;
}

std::string videoDir() {
    const std::string dir = replayDir();
    return dir.empty() ? dir : dir + "videos" + dir.back();
}

Settings loadSettings(bool* fileFound) {
    Settings s;   // defaults
    if (fileFound) *fileFound = false;
    std::string path = settingsPath();
    if (path.empty()) return s;
    std::ifstream in(path);
    if (!in) { diskDataFolder() = {true, {}, {}}; return s; }
    if (fileFound) *fileFound = true;

    std::string line;
    while (std::getline(in, line)) {
        std::string key, val;
        if (!splitLine(line, key, val)) continue;

        auto asInt   = [&](int lo, int hi) { return intValue(val, lo, hi); };
        auto asFloat = [&](float lo, float hi) { return clampv(float(std::atof(val.c_str())), lo, hi); };
        auto asBool  = [&] { return boolValue(val); };
        auto asPathMode = [&] { return pathModeValue(val); };

        // customMusic.track<n> = hex of the file's path (any character a path may hold).
        if (key.rfind("customMusic.track", 0) == 0) {
            const int track = std::atoi(key.c_str() + 17);
            std::string path;
            bool valid = track >= 1 && track <= 99 && val.size() % 2 == 0 && val.size() <= 8192;
            auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
            for (size_t i = 0; valid && i < val.size(); i += 2) {
                const int a = digit(val[i]), b = digit(val[i + 1]);
                if (a < 0 || b < 0) valid = false; else path += char(a * 16 + b);
            }
            if (valid && !path.empty() && path.find('\0') == path.npos) s.customMusicTracks[track] = path;
            continue;
        }
        // The single-player and multiplayer room sets. The keys they replaced
        // (gameCreate.*, lastMap, mods.gamePack) are not read: both sets start at their
        // defaults, and those keys go with the next save, as every unknown key does.
        if (key.rfind("spRoom.", 0) == 0 || key.rfind("mpRoom.", 0) == 0) {
            parseRoomPref(key[0] == 's' ? s.spRoom : s.mpRoom, key.substr(7), val);
            continue;
        }
        if(key=="mods.cosmeticPack") {
            readPackValue(val,s.cosmeticModPacks);
            continue;
        }
        if      (key == "fullscreen")      s.fullscreen = asBool();
        else if (key == "vsync")           s.vsync = asBool();
        else if (key == "maxFps") {        // 0 = unlimited, else 30..240
            const int v = std::atoi(val.c_str());
            s.maxFps = v <= 0 ? 0 : clampv(v, 30, 240);
        }
        else if (key == "renderer")        s.renderer = renderdriver::normalize(val);
        else if (key == "ignoreDpiScaling") s.ignoreDpiScaling = asBool();
        else if (key == "nativeWayland")   s.nativeWayland = val == "on" ? 1 : val == "off" ? 2 : 0;
        else if (key == "displayMode")     s.displayMode = wayland::parseMode(val);
        else if (key == "displayScale") {
            const float v = float(std::atof(val.c_str()));
            s.displayScale = v >= 1.0f && v <= 8.0f ? v : 0.0f;
        }
        else if (key == "displayX11Width" || key == "displayLogicalWidth") {
            const int v = std::atoi(val.c_str());
            (key == "displayX11Width" ? s.displayX11Width : s.displayLogicalWidth) = v >= 1 && v <= 65536 ? v : 0;
        }
        else if (key == "scorecardScale")  s.scorecardScale = asFloat(0.5f, 2.0f);
        else if (key == "uiScale")         s.uiScale = asFloat(0.5f, 2.0f);
        else if (key == "textureFilter")   s.textureFilter = asInt(0, 2);
        else if (key == "buildBarAlign")   s.buildBarAlign = asInt(0, 2);
        // 4.0, matching the slider and GameView. This still said 2.0 when the slider
        // went to 400%, so 300-400% survived until the next restart and then silently
        // snapped back to 200%.
        else if (key == "buildBarScale")   s.buildBarScale = asFloat(0.5f, 4.0f);
        else if (key == "idleBuilders")    s.idleBuilders = asInt(0, 3);
        else if (key == "idleBuildersScale") s.idleBuildersScale = asFloat(0.5f, 4.0f);
        else if (key == "chatScale")       s.chatScale = asFloat(0.5f, 3.0f);
        else if (key == "chatAlign")       s.chatAlign = asInt(0, 1);
        else if (key == "uiFont")          s.uiFont = val == "classic" ? 1 : 0;
        else if (key == "treeSway")        s.treeSway = asBool();
        else if (key == "tacticalDots")    s.tacticalDots = asBool();
        else if (key == "screenShake")     s.screenShake = asBool();
        else if (key == "tacticalDotsZoom") s.tacticalDotsZoom = asInt(0, 100);
        else if (key == "hiddenUnits")     s.hiddenUnits = hiddenunits::parse(val);
        else if (key == "hiddenFadeOpacity") s.hiddenFadeOpacity = asInt(0, 100);
        else if (key == "unitShadows")     s.unitShadows = asBool();
        else if (key == "gpuMemoryLimit")  s.gpuMemoryLimit = parseGpuMemoryLimit(val);
        else if (key == "smoothArt")       s.smoothArt = asBool();
        else if (key == "videoDeblock")    s.videoDeblock = asBool();
        else if (key == "healthBars")      s.healthBars = asInt(0, 2);
        else if (key == "statsPanel")      s.statsPanel = asBool();
        else if (key == "masterVol")       s.masterVol = asInt(0, 256);
        else if (key == "bgmVol")          s.bgmVol = asInt(0, 256);
        else if (key == "sfxVol")          s.sfxVol = asInt(0, 256);
        else if (key == "soundVoices")     s.soundVoices = asInt(8, 256);
        else if (key == "evenSoundLevels") s.evenSoundLevels = asBool();
        else if (key == "mouseZoomSpeed")  s.mouseZoomSpeed = asFloat(0.25f, 4.0f);
        else if (key == "edgeScrollSpeed") s.edgeScrollSpeed = asFloat(0.25f, 4.0f);
        else if (key == "edgeScroll")      s.edgeScroll = asBool();
        else if (key == "mouseZoom")       s.mouseZoom = asBool();
        else if (key == "cursorScale")     s.cursorScale = asInt(1, 8);
        else if (key == "hardwareCursor")  s.hardwareCursor = asBool();
        else if (key == "smoothMotion")    s.smoothMotion = asBool();
        // 0 (or anything that is not a count) = AUTO; this machine's own limit applies
        // where the value is used, so a profile keeps a bigger machine's choice.
        else if (key == "simThreads")      s.simThreads = asInt(0, int(sim::SimPool::kMaxChoice));
        else if (key == "simPin")          s.simPin = asBool();
        else if (key == "ccdPin")          s.ccdPin = asBool();
        else if (key == "ccdGame")         s.ccdGame = asInt(-1, 63);
        else if (key == "ccdServer")       s.ccdServer = asInt(-1, 63);
        else if (key == "playerName")      s.playerName = val;
        else if (key == "accountName")     s.accountName = val;
        else if (key == "streamRememberKey") s.streamRememberKey = asBool();
        else if (key == "streamLocalCopy") s.streamLocalCopy = asBool();
        else if (key == "streamBitrate") {
            const int v = asInt(0, 1000000);
            s.streamBitrate = Settings::kStreamBitrateDefault;
            for (int r : Settings::kStreamBitrates) if (r == v) s.streamBitrate = v;
            if (video::isYouTubeRecommendedKbps(v)) s.streamBitrate = v; // RECOMMENDED / the cap
        }
        else if (key == "streamResolution") {
            char* end = nullptr;
            const long v = std::strtol(val.c_str(), &end, 10);
            s.streamResolution = (end != val.c_str() && *end == 0 && v >= 0 && v <= 5)
                                     ? int(v) : Settings::kStreamResolutionDefault;
        }
        else if (key == "streamFps") {
            const int v = std::atoi(val.c_str());
            s.streamFps = (v == 30) ? 30 : 60;
        }
        else if (key == "streamCpuEncoder") s.streamCpuEncoder = asBool();
        else if (key == "audioDevice")     s.audioDevice = val;
        else if (key == "voiceEnabled")    s.voiceEnabled = asBool();
        else if (key == "voiceActivation") s.voiceActivation = asBool();
        else if (key == "voiceActivationLevel") s.voiceActivationLevel = asInt(0, 100);
        else if (key == "voiceMicDevice")  s.voiceMicDevice = val;
        else if (key == "voiceOutputSameAsGame") s.voiceOutputSameAsGame = asBool();
        else if (key == "voiceOutputDevice") s.voiceOutputDevice = val;
        else if (key == "voiceMicVolume")  s.voiceMicVolume = asInt(0, 400);   // MIC GAIN %
        else if (key == "voiceMicEnabled") s.voiceMicEnabled = asBool();
        else if (key == "voiceOutputVolume") s.voiceOutputVolume = asInt(0, 100);
        else if (key == "benchmarkPathfinding") s.benchmarkPathfinding = asPathMode();
        else if (key == "benchmarkSpeed")  { const int v = asInt(0, 1000); s.benchmarkSpeed = v >= 100 ? 100 : v > 0 ? 10 : 0; }   // NORMAL, +10 or +100
        else if (key == "knownServers") {  // comma-joined, most recent first
            std::stringstream ks(val);
            std::string tok;
            while (std::getline(ks, tok, ',') && s.knownServers.size() < Settings::kMaxKnownServers)
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
    }
    diskDataFolder() = {true, s.dataDir, s.dataManifest};
    return s;
}

std::string serializeSettings(const Settings& s) {
    std::ostringstream o;
    o << "# TA:Kingdoms engine settings\n";
    o << "fullscreen = " << (s.fullscreen ? 1 : 0) << "\n";
    o << "vsync = " << (s.vsync ? 1 : 0) << "\n";
    o << "maxFps = " << s.maxFps << "\n";
    o << "renderer = " << (s.renderer.empty() ? "auto" : s.renderer) << "\n";
    o << "ignoreDpiScaling = " << (s.ignoreDpiScaling ? 1 : 0) << "\n";
    o << "nativeWayland = " << (s.nativeWayland == 1 ? "on" : s.nativeWayland == 2 ? "off" : "auto") << "\n";
    if (s.displayMode != wayland::kModeUnknown) o << "displayMode = " << wayland::modeName(s.displayMode) << "\n";
    if (s.displayScale > 0.0f) o << "displayScale = " << s.displayScale << "\n";
    if (s.displayX11Width > 0) o << "displayX11Width = " << s.displayX11Width << "\n";
    if (s.displayLogicalWidth > 0) o << "displayLogicalWidth = " << s.displayLogicalWidth << "\n";
    o << "scorecardScale = " << s.scorecardScale << "\n";
    o << "uiScale = " << s.uiScale << "\n";
    o << "textureFilter = " << std::clamp(s.textureFilter, 0, 2) << "\n";
    o << "buildBarAlign = " << s.buildBarAlign << "\n";
    o << "buildBarScale = " << s.buildBarScale << "\n";
    o << "idleBuilders = " << s.idleBuilders << "\n";
    o << "idleBuildersScale = " << s.idleBuildersScale << "\n";
    o << "chatScale = " << std::clamp(s.chatScale, 0.5f, 3.0f) << "\n";
    o << "chatAlign = " << std::clamp(s.chatAlign, 0, 1) << "\n";
    o << "uiFont = " << (s.uiFont == 1 ? "classic" : "modern") << "\n";
    o << "treeSway = " << (s.treeSway ? 1 : 0) << "\n";
    o << "tacticalDots = " << (s.tacticalDots ? 1 : 0) << "\n";
    o << "screenShake = " << (s.screenShake ? 1 : 0) << "\n";
    o << "tacticalDotsZoom = " << s.tacticalDotsZoom << "\n";
    o << "hiddenUnits = " << hiddenunits::name(s.hiddenUnits) << "\n";
    o << "hiddenFadeOpacity = " << std::clamp(s.hiddenFadeOpacity, 0, 100) << "\n";
    o << "unitShadows = " << (s.unitShadows ? 1 : 0) << "\n";
    o << "gpuMemoryLimit = " << gpuMemoryLimitValue(s.gpuMemoryLimit) << "\n";
    o << "smoothArt = " << (s.smoothArt ? 1 : 0) << "\n";
    o << "videoDeblock = " << (s.videoDeblock ? 1 : 0) << "\n";
    o << "healthBars = " << s.healthBars << "\n";
    o << "statsPanel = " << (s.statsPanel ? 1 : 0) << "\n";
    o << "masterVol = " << s.masterVol << "\n";
    o << "bgmVol = " << s.bgmVol << "\n";
    o << "sfxVol = " << s.sfxVol << "\n";
    o << "soundVoices = " << s.soundVoices << "\n";
    o << "evenSoundLevels = " << (s.evenSoundLevels ? 1 : 0) << "\n";
    for (int i = 0; i < 8; ++i) o << "chanGain" << i << " = " << s.chanGain[i] << "\n";
    o << "mouseZoomSpeed = " << s.mouseZoomSpeed << "\n";
    o << "edgeScrollSpeed = " << s.edgeScrollSpeed << "\n";
    o << "edgeScroll = " << (s.edgeScroll ? 1 : 0) << "\n";
    o << "mouseZoom = " << (s.mouseZoom ? 1 : 0) << "\n";
    o << "cursorScale = " << s.cursorScale << "\n";
    o << "hardwareCursor = " << (s.hardwareCursor ? 1 : 0) << "\n";
    o << "smoothMotion = " << (s.smoothMotion ? 1 : 0) << "\n";
    o << "simThreads = " << std::clamp(s.simThreads, 0, int(sim::SimPool::kMaxChoice)) << "\n";
    o << "simPin = " << (s.simPin ? 1 : 0) << "\n";
    o << "ccdPin = " << (s.ccdPin ? 1 : 0) << "\n";
    o << "ccdGame = " << s.ccdGame << "\n";
    o << "ccdServer = " << s.ccdServer << "\n";
    o << "playerName = " << s.playerName << "\n";
    o << "accountName = " << s.accountName << "\n";
    o << "streamRememberKey = " << (s.streamRememberKey ? 1 : 0) << "\n";
    o << "streamLocalCopy = " << (s.streamLocalCopy ? 1 : 0) << "\n";
    o << "streamBitrate = " << s.streamBitrate << "\n";
    o << "streamResolution = " << s.streamResolution << "\n";
    o << "streamFps = " << s.streamFps << "\n";
    o << "streamCpuEncoder = " << (s.streamCpuEncoder ? 1 : 0) << "\n";
    o << "audioDevice = " << s.audioDevice << "\n";
    for (const auto& [track, path] : s.customMusicTracks) {
        static constexpr char hex[] = "0123456789abcdef";
        o << "customMusic.track" << track << " = ";
        for (unsigned char c : path) o << hex[c >> 4] << hex[c & 15];
        o << "\n";
    }
    o << "voiceEnabled = " << (s.voiceEnabled ? 1 : 0) << "\n";
    o << "voiceActivation = " << (s.voiceActivation ? 1 : 0) << "\n";
    o << "voiceActivationLevel = " << std::clamp(s.voiceActivationLevel, 0, 100) << "\n";
    o << "voiceMicDevice = " << s.voiceMicDevice << "\n";
    o << "voiceOutputSameAsGame = " << (s.voiceOutputSameAsGame ? 1 : 0) << "\n";
    o << "voiceOutputDevice = " << s.voiceOutputDevice << "\n";
    o << "voiceMicVolume = " << std::clamp(s.voiceMicVolume, 0, 400) << "\n";
    o << "voiceMicEnabled = " << (s.voiceMicEnabled ? 1 : 0) << "\n";
    o << "voiceOutputVolume = " << std::clamp(s.voiceOutputVolume, 0, 100) << "\n";
    o << "benchmarkPathfinding = " << int(s.benchmarkPathfinding) << "\n";
    o << "benchmarkSpeed = " << s.benchmarkSpeed << "\n";
    writeRoomPrefs(o, "spRoom", s.spRoom);
    writeRoomPrefs(o, "mpRoom", s.mpRoom);
    writePackLines(o, "mods.cosmeticPack", s.cosmeticModPacks);
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
    return o.str();
}

bool saveSettings(const Settings& s) {
    std::string path = settingsPath();
    if (path.empty()) return false;
    Settings out = s;
    auto& seen = diskDataFolder();
    auto& launch = launchGameModPacks();
    if (launch && s.spRoom.gamePacks != *launch) launch.reset();   // the player's own ticks now
    const bool keepDataFolder = seen.known && s.dataDir == seen.dir && s.dataManifest == seen.manifest;
    if (keepDataFolder || launch) {
        // Unchanged by this process since it last read or wrote the file, so the file's
        // CURRENT value wins: another program (Cartographer, a second game) may have set it.
        // A launch-only pack selection likewise leaves the file's ticks as they are.
        std::ifstream in(path);
        std::string line, key, val;
        if (launch) out.spRoom.gamePacks.clear();
        while (in && std::getline(in, line))
            if (splitLine(line, key, val)) {
                if (keepDataFolder && key == "dataDir") out.dataDir = val;
                else if (keepDataFolder && key == "dataManifest") out.dataManifest = val;
                else if (launch && key == "spRoom.gamePack") readPackValue(val, out.spRoom.gamePacks);
            }
    }
    const std::string text = serializeSettings(out);
    // Atomic write: temp file + rename, so a crash mid-write can't corrupt the file.
    std::string tmp = path + ".tmp";
    { std::ofstream o(tmp, std::ios::trunc); if (!o) return false; o << text; if (!o) return false; }
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) != 0) return false;
    seen = {true, out.dataDir, out.dataManifest};
    return true;
}

void setLaunchGameModPacks(std::optional<std::vector<std::string>> packs) {
    launchGameModPacks() = std::move(packs);
}

bool saveDataFolderSetting(const std::string& folder, const std::string& manifest) {
    Settings s = loadSettings();   // the file as it is now; nothing else in it changes
    s.dataDir = folder;
    s.dataManifest = manifest;
    return saveSettings(s);
}

}  // namespace tak
