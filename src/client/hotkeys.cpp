#include "client/hotkeys.h"

#include <SDL.h>

#include <algorithm>
#include <sstream>

namespace tak {

const std::vector<HotkeyDef>& hotkeyDefs() {
    // key chords use SDL keycodes; sections group the config screen.
    static const std::vector<HotkeyDef> defs = {
        {Act::Move,           "order.move",         "MOVE",            "ORDERS",    {SDLK_m, 0}},
        {Act::Attack,         "order.attack",       "ATTACK",          "ORDERS",    {SDLK_a, 0}},
        {Act::FightMove,      "order.fightmove",    "FIGHT-MOVE",      "ORDERS",    {SDLK_f, 0}},
        {Act::Patrol,         "order.patrol",       "PATROL",          "ORDERS",    {SDLK_p, 0}},
        {Act::Guard,          "order.guard",        "GUARD",           "ORDERS",    {SDLK_g, 0}},
        {Act::Stop,           "order.stop",         "STOP",            "ORDERS",    {SDLK_s, 0}},
        {Act::Heal,           "order.heal",         "HEAL",            "ORDERS",    {SDLK_h, 0}},
        {Act::Load,           "order.load",         "LOAD",            "ORDERS",    {SDLK_l, 0}},
        {Act::Unload,         "order.unload",       "UNLOAD",          "ORDERS",    {SDLK_u, 0}},
        {Act::ClearOrders,    "order.clear",        "CLEAR ORDERS",    "ORDERS",    {SDLK_c, 0}},
        {Act::CycleWeapon,    "order.cycleweapon",  "CYCLE WEAPON",    "ORDERS",    {SDLK_w, 0}},
        {Act::NextUnit,       "order.nextunit",     "NEXT UNIT",       "ORDERS",    {SDLK_n, 0}},
        {Act::TrackSelection, "view.track",         "TRACK SELECTION", "ORDERS",    {SDLK_t, 0}},
        {Act::ToggleCloak,    "unit.togglecloak",   "TOGGLE CLOAK",    "ORDERS",    {SDLK_k, 0}},
        {Act::ToggleGate,     "unit.togglegate",    "OPEN/CLOSE GATE", "ORDERS",    {SDLK_o, 0}},

        {Act::SelectAll,      "select.all",         "SELECT ALL",      "SELECTION", {SDLK_a, HK_CTRL}},
        {Act::SelectSameType, "select.sametype",    "SELECT SAME TYPE","SELECTION", {SDLK_z, HK_CTRL}},
        {Act::SelectOnScreen, "select.onscreen",    "SELECT ON SCREEN","SELECTION", {SDLK_u, HK_CTRL}},
        {Act::SelectMonarch,  "select.monarch",     "SELECT MONARCH",  "SELECTION", {SDLK_m, HK_CTRL}},
        {Act::SelectBuilders, "select.builders",    "SELECT BUILDERS", "SELECTION", {SDLK_b, HK_CTRL}},
        {Act::SelectFactory,  "select.factories",   "SELECT FACTORIES","SELECTION", {SDLK_f, HK_CTRL}},
        {Act::SelectMelee,    "select.melee",       "SELECT MELEE",    "SELECTION", {SDLK_e, HK_CTRL}},
        {Act::SelectMagic,    "select.magic",       "SELECT MAGIC",    "SELECTION", {SDLK_g, HK_CTRL}},
        {Act::SelectBoats,    "select.boats",       "SELECT BOATS",    "SELECTION", {SDLK_n, HK_CTRL}},
        {Act::SelectBallistic,"select.ballistic",   "SELECT BALLISTIC","SELECTION", {SDLK_r, HK_CTRL}},
        {Act::SelectTroops,   "select.troops",      "SELECT TROOPS",   "SELECTION", {SDLK_t, HK_CTRL}},
        {Act::SelectArmed,    "select.armed",       "SELECT ATTACKERS","SELECTION", {SDLK_w, HK_CTRL}},
        {Act::SelectOnScreenType,"select.onscreentype","ON-SCREEN TYPE","SELECTION",{SDLK_x, HK_CTRL}},
        {Act::SelectFlying,   "select.flying",      "SELECT FLYING",   "SELECTION", {SDLK_y, HK_CTRL}},
        {Act::SelfDestruct,   "unit.selfdestruct",  "SELF-DESTRUCT",   "SELECTION", {SDLK_d, HK_CTRL | HK_SHIFT}},

        {Act::GiveUnits,      "unit.give",          "DIPLOMACY",       "VIEW",      {SDLK_d, 0}},
        {Act::RetailZoom,     "view.retailzoom",      "RETAIL ZOOM",     "VIEW",      {SDLK_z, HK_SHIFT}},
        {Act::ToggleCounts,   "view.counts",        "UNIT COUNTS",     "VIEW",      {SDLK_F4, 0}},
        // Retail: `TAB = fsr;` in gamedata/keys.tdf, commented "Full-screen radar
        // toggle". The command is registered twice in the binary, as the console
        // alias "fsr" (handler 0x427f40) and as "FullScreenRadar" (0x48e8c0);
        // both flip one bool and nothing else.
        {Act::FullScreenRadar,"view.fullradar",     "FULL-SCREEN MAP", "VIEW",      {SDLK_TAB, 0}},
        // Retail ships "UnitInfo" unbound (it's only in translate/customkeys.tdf as a
        // rebindable function), so the default key is ours to pick.
        {Act::UnitInfo,       "view.unitinfo",      "UNIT INFO",       "VIEW",      {SDLK_F1, 0}},
        // Read by the streaming panel itself, at the title screen too (streamingPanelPress).
        {Act::StreamingPanel,"view.streaming",     "STREAMING PANEL", "VIEW",      {SDLK_F9, 0}},
        // Read by every screen's event loop, the title screen's too (fullscreenPress).
        {Act::ToggleFullscreen,"view.fullscreen",  "FULLSCREEN",      "VIEW",      {SDLK_F11, 0}},
        {Act::Disco,          "emote.disco",        "DISCO",           "EMOTES",    {SDLK_d, HK_SHIFT}},
        {Act::Headbang,       "emote.headbang",     "HEADBANG",        "EMOTES",    {SDLK_h, HK_SHIFT}},
        // Voice chat, held while talking (client/voice.h). Space is free in the game:
        // retail binds nothing to it (gamedata/keys.tdf) and nothing else here does.
        {Act::PushToTalk,     "voice.ptt",          "PUSH TO TALK",    "VOICE",     {SDLK_SPACE, 0}},
        // The +MakePoster power code on a key: a whole-map picture into the screenshots
        // folder. Only in a match with POWER CODES on. Retail has no key for it; Print
        // Screen is free (gamedata/keys.tdf binds nothing to it).
        {Act::MakePoster,     "view.poster",        "MAKE POSTER",     "POWER CODES",{SDLK_p, HK_SHIFT}},
    };
    return defs;
}

uint16_t normMod(uint16_t sdlMod) {
    uint16_t m = 0;
    if (sdlMod & KMOD_CTRL) m |= HK_CTRL;
    if (sdlMod & KMOD_SHIFT) m |= HK_SHIFT;
    if (sdlMod & KMOD_ALT) m |= HK_ALT;
    return m;
}

std::string chordName(const KeyChord& c) {
    if (!c.bound()) return "UNBOUND";
    std::string s;
    if (c.mod & HK_CTRL) s += "Ctrl+";
    if (c.mod & HK_SHIFT) s += "Shift+";
    if (c.mod & HK_ALT) s += "Alt+";
    const char* kn = SDL_GetKeyName(SDL_Keycode(c.key));
    s += (kn && *kn) ? kn : "?";
    return s;
}

bool parseChord(const std::string& s, KeyChord& out) {
    out = {};
    if (s.empty() || s == "NONE") return true;   // valid: unbound
    std::stringstream ss(s);
    std::string tok, keyName;
    while (std::getline(ss, tok, '+')) {
        std::string low = tok;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        if (low == "ctrl") out.mod |= HK_CTRL;
        else if (low == "shift") out.mod |= HK_SHIFT;
        else if (low == "alt") out.mod |= HK_ALT;
        else keyName = tok;   // the last non-modifier token is the key
    }
    if (keyName.empty()) return false;
    SDL_Keycode k = SDL_GetKeyFromName(keyName.c_str());
    if (k == SDLK_UNKNOWN) return false;
    out.key = int32_t(k);
    return true;
}

void Hotkeys::load(const std::map<std::string, std::string>& overrides) {
    for (const auto& d : hotkeyDefs())
        chords_[size_t(d.act)] = effectiveChord(overrides, d.act);
}

Act Hotkeys::match(int32_t key, uint16_t sdlMod) const {
    uint16_t m = normMod(sdlMod);
    for (const auto& d : hotkeyDefs()) {
        const KeyChord& c = chords_[size_t(d.act)];
        if (c.bound() && c.key == key && c.mod == m) return d.act;
    }
    return Act::Count;
}

bool pushToTalkReserved(int32_t key) {
    return key == SDLK_RETURN || key == SDLK_KP_ENTER || (key >= SDLK_0 && key <= SDLK_9) ||
           key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_UP || key == SDLK_DOWN;
}

bool Hotkeys::pushToTalkPress(int32_t key, uint16_t sdlMod) const {
    const KeyChord c = chord(Act::PushToTalk);
    const uint16_t m = normMod(sdlMod);
    if (!c.bound() || c.key != key || (m & c.mod) != c.mod) return false;
    const Act exact = match(key, sdlMod);
    return exact == Act::Count || exact == Act::PushToTalk;
}

// --- override map helpers ---------------------------------------------------

static const HotkeyDef& defFor(Act a) {
    for (const auto& d : hotkeyDefs()) if (d.act == a) return d;
    return hotkeyDefs()[0];   // unreachable
}

KeyChord effectiveChord(const std::map<std::string, std::string>& overrides, Act a) {
    const HotkeyDef& d = defFor(a);
    auto it = overrides.find(d.id);
    if (it == overrides.end()) return d.def;   // no override: the default
    KeyChord c;
    if (!parseChord(it->second, c)) return d.def;   // corrupt override: fall back
    return c;
}

void setChordOverride(std::map<std::string, std::string>& overrides, Act a, KeyChord c) {
    const HotkeyDef& d = defFor(a);
    if (c == d.def) { overrides.erase(d.id); return; }   // back to default: drop the override
    overrides[d.id] = c.bound() ? chordName(c) : "NONE";
}

static bool globalPress(const std::map<std::string, std::string>& overrides, Act a, int32_t key,
                        uint16_t sdlMod, bool typing) {
    const KeyChord c = effectiveChord(overrides, a);
    if (!c.bound() || c.key != key || normMod(sdlMod) != c.mod) return false;
    const bool character = key >= SDLK_SPACE && key <= SDLK_z;
    return !(typing && character && !(c.mod & (HK_CTRL | HK_ALT)));
}

bool streamingPanelPress(const std::map<std::string, std::string>& overrides, int32_t key,
                         uint16_t sdlMod, bool typing) {
    return globalPress(overrides, Act::StreamingPanel, key, sdlMod, typing);
}

bool fullscreenPress(const std::map<std::string, std::string>& overrides, int32_t key,
                     uint16_t sdlMod, bool typing) {
    return globalPress(overrides, Act::ToggleFullscreen, key, sdlMod, typing);
}

bool toggleFullscreenKey(const SDL_Event& e, const std::map<std::string, std::string>& overrides,
                         bool& fullscreen) {
    if (e.type != SDL_KEYDOWN || e.key.repeat || hotkeyCaptureActive ||
        !fullscreenPress(overrides, e.key.keysym.sym, e.key.keysym.mod, SDL_IsTextInputActive()))
        return false;
    fullscreen = !fullscreen;
    if (SDL_Window* w = SDL_GetWindowFromID(e.key.windowID))
        SDL_SetWindowFullscreen(w, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    return true;
}

}  // namespace tak
