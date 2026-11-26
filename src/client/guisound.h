#pragma once

// Total Annihilation's GUI sounds (docs/retail-gui.md section 1.6): which
// gamedata/allsound.tdf entry a pressed gadget plays, and the WAV that entry names.
//
// Retail has no per-gadget or per-gadget-type sound: each screen's handler calls
// TotalA.exe 0x47f1a0(entry, 0) with a hard-coded entry name right after
// 0x49fd60(screen, "GADGET") reports the press. 0x47f1a0 looks the name up in the
// table loaded from allsound.tdf (game+0x33e13, 0x20-byte names; case-insensitive
// 0x4f8a70 compare, first match wins) and hands the index to 0x47f0c0, which plays
// it through the sound object at game+0x10 (0x4cf570), gated by the Options sound
// mode (game+0x37f19 & 7). An unknown name plays nothing (index 0xffff).
// The tables below are those hard-coded names, per screen, with the call sites.
// SDL-free; the client plays the WAV (MenuSfx in the menus, SoundBank in game).

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "hpi/hpi.h"
#include "tdf/tdf.h"

namespace ta::guisound {

inline std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// The allsound.tdf entries the GUI handlers name.
inline constexpr const char* kBigButton = "BIGBUTTON";
inline constexpr const char* kSmlButton = "SMLBUTTON";
inline constexpr const char* kSmallButton = "SMALLBUTTON";
inline constexpr const char* kSkirmish = "SKIRMISH";
inline constexpr const char* kMulti = "MULTI";
inline constexpr const char* kOptions = "OPTIONS";
inline constexpr const char* kExit = "EXIT";
inline constexpr const char* kPrevious = "PREVIOUS";
inline constexpr const char* kSideSelect = "SIDESELECT";
inline constexpr const char* kSideSelect2 = "SIDESELECT2";
inline constexpr const char* kMore = "MORE";
inline constexpr const char* kAlly = "ALLY";
inline constexpr const char* kOrdersButton = "ORDERSBUTTON";
inline constexpr const char* kBuildButton = "BUILDBUTTON";
inline constexpr const char* kAddBuild = "ADDBUILD";
inline constexpr const char* kSubBuild = "SUBBUILD";
inline constexpr const char* kNextBuildMenu = "NEXTBUILDMENU";
inline constexpr const char* kImmediateOrders = "IMMEDIATEORDERS";
inline constexpr const char* kSpecialOrders = "SPECIALORDERS";
inline constexpr const char* kSetFireOrders = "SETFIREORDERS";
inline constexpr const char* kSetMoveOrders = "SETMOVEORDERS";
inline constexpr const char* kOkToBuild = "OKTOBUILD";
inline constexpr const char* kNotOkToBuild = "NOTOKTOBUILD";

// The entry retail plays when `gadget` of front-end screen `screen` (the .GUI's base
// name, e.g. "MAINMENU") is pressed; "" when its handler plays none.
inline std::string frontEnd(const std::string& screen, const std::string& gadget) {
    const std::string s = upper(screen), g = upper(gadget);
    if (s == "MAINMENU") {   // handler 0x425d80
        if (g == "SINGLE") return kBigButton;            // 0x425dbd
        if (g == "MULTI") return kBigButton;             // 0x425dfa
        if (g == "INTRO") return kSmlButton;             // 0x425ee4 "smlButton"
        if (g == "EXIT") return kExit;                   // 0x426076 "exit"
        if (g == "CREDITS") return kSmlButton;           // 0x4260b2
        // The engine's additions to the title screen (tafrontend.cpp): CAMPAIGN is a
        // copy of SINGLE that opens a campaign screen, as SINGLE.GUI's NewCamp does
        // (BigButton, 0x4775d0); OPTIONS plays what SINGLE.GUI's Options does (0x4776c1).
        if (g == "CAMPAIGN") return kBigButton;
        if (g == "OPTIONS") return kOptions;
        return {};
    }
    if (s == "SINGLE") {     // handler 0x4775a0
        if (g == "NEWCAMP" || g == "LOADGAME" || g == "ANYMSN") return kBigButton;   // 0x4775d0, 0x477690, 0x477734
        if (g == "SKIRMISH") return kSkirmish;           // 0x477622 "skirmish"
        if (g == "OPTIONS") return kOptions;             // 0x4776c1 "options"
        if (g == "PREVMENU") return kPrevious;           // 0x4776f2
        return {};
    }
    if (s == "NEWGAME") {    // handler 0x477ab0 (and the any-mission picker on the same GUI)
        if (g == "START") return kBigButton;             // 0x477b6e "bigButton"
        if (g == "PREVMENU") return kPrevious;           // 0x477cd7
        if (g == "DIFFICULTY") return kSmlButton;        // 0x477d0b "SmlButton"
        if (g == "SIDE0" || g == "ARM") return kSideSelect;     // 0x47802a
        if (g == "SIDE1" || g == "CORE") return kSideSelect2;   // 0x477def
        return {};
    }
    if (s == "MSNBRIEF") {   // handler 0x478cb0
        if (g == "START") return kBigButton;             // 0x478cfd
        if (g == "SHUTUP") return kOptions;              // 0x478d8e (then SmallButton, 0x478de2)
        if (g == "PREVMENU") return kPrevious;           // 0x478e0f
        if (g == "MOREBAR") return kMore;                // 0x478e5f
        return {};
    }
    if (s == "SKIRMISH") {   // handler 0x47ae60
        if (g == "START") return kBigButton;             // 0x47aee1
        if (g == "PREVMENU") return kPrevious;           // 0x47b10b
        // Every setting, the player rows (0x47b16d..0x47b5bd) and SelectMap.
        if (g == "COMMANDERDEATH" || g == "STARTLOCATION" || g == "MAPPING" || g == "LINEOFSIGHT" ||
            g == "SELECTMAP" || g == "DIFFICULTY" || g == "PLAYERROW")
            return kSkirmish;                            // 0x47b66e, 0x47b6ec, 0x47b76a, 0x47b7ec, 0x47b8ec, 0x47b92f
        return {};
    }
    if (s == "SELMAP") {     // 0x47a988
        if (g == "PREVMENU") return kPrevious;           // 0x47a9b7
        if (g == "LOAD") return kSmallButton;            // 0x47a9f0 (a MAPNAMES click plays none)
        return {};
    }
    if (s == "ENDMSN") {     // 0x41ed80 (endgame.cpp)
        if (g == "LOADGAME" || g == "SAVEGAME" || g == "MAINMENU" || g == "START") return kBigButton;   // 0x41edb2, 0x41ede5, 0x41ee3e, 0x41ef77
        if (g == "DIFFICULTY") return kSkirmish;         // 0x41ee8b "SKirmish"
        return {};
    }
    if (s == "SELPROV") {    // 0x444634
        if (g == "SELECT" || g == "DPLAY") return kBigButton;   // 0x4444a4
        if (g == "SETTINGS") return kOptions;            // 0x44443d
        if (g == "PREVMENU") return kPrevious;           // 0x444425
        return {};
    }
    if (s == "TCP") {        // 0x442204
        if (g == "OK") return kSmlButton;                // 0x442106 "Smlbutton"
        if (g == "PREV") return kPrevious;               // 0x4421ca
        return {};
    }
    return {};
}

// The in-game sidebar (<SIDE>GEN orders, the build pages), handler 0x419be0 and the
// page handler around 0x41aa60. `gadget` is the gadget name without the side prefix
// ("MOVE", "BUILD", ...); `buildPicture` when it is a unit on a build page;
// `remove` for a right click on one (one fewer in a factory queue).
inline std::string sidebar(const std::string& gadget, bool buildPicture = false, bool remove = false) {
    if (buildPicture) return remove ? kSubBuild : kAddBuild;   // 0x419b27 / 0x419b2e; a builder's pick 0x41ab97
    const std::string g = upper(gadget);
    // Plain orders, armed or disarmed by the press (both branches play it).
    if (g == "MOVE" || g == "ATTACK" || g == "BLAST" || g == "DEFEND" || g == "PATROL" || g == "LOAD" ||
        g == "STOP")
        return kImmediateOrders;   // 0x419c65, 0x419d3f, 0x419db4, 0x419e28, 0x419f11, 0x41a0ec; STOP 0x419cf2 -> 0x41a0ea
    if (g == "REPAIR" || g == "RECLAIM" || g == "CAPTURE" || g == "UNLOAD") return kSpecialOrders;   // 0x419e9c, 0x419f85, 0x419ff6, 0x41a074
    if (g == "CLOAK" || g == "ONOFF") return kSpecialOrders;   // 0x41a7ae, 0x41a898
    if (g == "MOVEORD") return kSetMoveOrders;                 // 0x41a5ac
    if (g == "FIREORD") return kSetFireOrders;                 // 0x41a6af
    if (g == "ORDERS") return kOrdersButton;                   // 0x41aaf8
    if (g == "BUILD") return kBuildButton;                     // 0x41ab39
    if (g == "PREV" || g == "NEXT") return kNextBuildMenu;     // flag 0x41aa8d / 0x41aac2, played on the flip 0x41befc
    if (g == "MAKENUKE" || g == "MAKEANTI") return remove ? kSubBuild : kAddBuild;   // 0x41abd9 -> 0x419b00
    return {};
}

// The engine's own lobby / game browser buttons (no .GUI): the sounds of their
// retail counterparts in the battle room (0x447xxx..0x448xxx) and SELGAME (0x443xxx).
inline std::string lobby(const std::string& label) {
    const std::string l = upper(label);
    if (l == "START" || l == "CREATE" || l == "JOIN" || l == "PICK" || l == "YES")
        return kBigButton;    // battle room START 0x448732; SELGAME STARTNEW / JOINGAME 0x4439dc
    if (l == "BACK" || l == "CANCEL" || l == "NO" || l == "QUIT" || l == "LEAVE" || l == "HIDE")
        return kPrevious;     // battle room PREVMENU 0x448389; SELGAME PREVMENU 0x44393c
    if (l == "DONE") return kSmlButton;   // 0x49426e
    return kMulti;            // battle room settings 0x4484f1..0x4486de; SELGAME WATCH 0x443bfa
}

// The engine menus' clicks (client/menuclick.h names Kingdoms .gui WAVs that TA does
// not ship): the retail entry for each kind -- a page-opening button and a toggle as
// the Options pages' (0x45fced SPEEDS / 0x45e183 ANTI: Options), a confirm as a big
// screen button (LoadGame 0x44afe9, JOINGAME 0x4439dc: BigButton), a back-out as
// CANCEL / PrevMenu (0x45fd8c: Previous). "" for any other name.
inline std::string engineClick(const std::string& wav) {
    std::string w = wav;
    for (char& c : w) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (w == "menubutton.wav" || w == "toggle.wav") return kOptions;
    if (w == "ok.wav") return kBigButton;
    if (w == "cancel.wav") return kPrevious;
    return {};
}

// gamedata/allsound.tdf: entry -> WAV file under sounds/ (".wav" added when the
// entry names a bare stem, as the shipped entries mostly do).
class Table {
public:
    void parse(const std::string& text) {
        wavs_.clear();
        const tdf::Node root = tdf::parseText(text, "allsound.tdf");
        for (const auto& [name, node] : root.orderedChildren()) {
            const std::string key = upper(std::string(name));
            if (wavs_.count(key) || !node) continue;   // 0x47f1a0: the first match wins
            const std::string* v = node->value("sound");
            if (!v || v->empty()) continue;
            std::string w = *v;
            for (char& c : w) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (w.size() < 4 || w.compare(w.size() - 4, 4, ".wav") != 0) w += ".wav";
            wavs_[key] = w;
        }
        loaded_ = true;
    }
    void load(const hpi::Vfs& vfs) {
        loaded_ = true;
        try {
            const auto b = vfs.read("gamedata/allsound.tdf");
            parse(std::string(b.begin(), b.end()));
        } catch (...) { wavs_.clear(); }
    }
    bool loaded() const { return loaded_; }
    // The WAV ("butmain1.wav") for an entry, any case; "" when allsound has no such entry.
    std::string wav(const std::string& entry) const {
        auto it = wavs_.find(upper(entry));
        return it == wavs_.end() ? std::string() : it->second;
    }
    const std::map<std::string, std::string>& all() const { return wavs_; }

private:
    std::map<std::string, std::string> wavs_;
    bool loaded_ = false;
};

// TA_GUISOUNDLOG=1: one stderr line per requested GUI sound (headless checks).
inline bool logging() {
    static const bool on = [] { const char* e = std::getenv("TA_GUISOUNDLOG"); return e && *e && *e != '0'; }();
    return on;
}
// decoded: the WAV is shipped and decodes; queued: it went to an open audio device
// (none under SDL's dummy driver).
inline void log(const char* where, const std::string& gadget, const std::string& entry, const std::string& wav,
                bool decoded, bool queued) {
    if (logging())
        std::fprintf(stderr, "gui sound: %s %s -> %s (%s) decoded=%d queued=%d\n", where, gadget.c_str(),
                     entry.c_str(), wav.empty() ? "-" : wav.c_str(), decoded ? 1 : 0, queued ? 1 : 0);
}

}  // namespace ta::guisound
