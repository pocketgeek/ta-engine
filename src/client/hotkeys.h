#pragma once

// Rebindable in-game hotkeys. A small fixed set of command/selection/emote actions,
// each with a default key chord, overridable per user and edited in the Hotkeys
// screen (src/client/hotkeysscreen). The number-row control groups (Ctrl/Alt+digit),
// game-speed +/- and a few dev keys are structural and intentionally NOT rebindable.
//
// Bindings persist by ACTION ID (stable strings), so reordering the table or adding
// actions never disturbs a saved config; only entries that differ from the default
// are written (so a later default change propagates to anyone who didn't rebind).

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ta {

// Modifier bits we model (normalized from SDL's left/right KMOD_* pairs).
enum : uint16_t { HK_CTRL = 1, HK_SHIFT = 2, HK_ALT = 4 };

// A key plus its modifier set. key is an SDL_Keycode; key == 0 means UNBOUND.
struct KeyChord {
    int32_t key = 0;
    uint16_t mod = 0;
    bool operator==(const KeyChord& o) const { return key == o.key && mod == o.mod; }
    bool operator!=(const KeyChord& o) const { return !(*this == o); }
    bool bound() const { return key != 0; }
};

// Every rebindable action. Order here is the display order in the config screen.
enum class Act {
    Move, Attack, FightMove, Patrol, Guard, Stop, Heal, Load, Unload, ClearOrders,
    CycleWeapon, NextUnit, TrackSelection,
    ToggleCloak, ToggleGate,
    SelectAll, SelectSameType, SelectOnScreen, SelectMonarch,
    SelectBuilders, SelectFactory, SelectMelee, SelectMagic, SelectBoats,
    SelectBallistic, SelectTroops, SelectArmed, SelectOnScreenType, SelectFlying,
    SelfDestruct,
    ToggleCounts, UnitInfo, FullScreenRadar, Disco, Headbang,
    Count
};

struct HotkeyDef {
    Act act;
    const char* id;        // stable persist id, e.g. "order.move"
    const char* label;     // shown in the config screen
    const char* section;   // group header
    KeyChord def;          // factory default chord
};

// The action table (fixed, in display order).
const std::vector<HotkeyDef>& hotkeyDefs();

// Normalize a raw SDL keymod to our HK_* bits (folds L/R, drops caps/num/gui).
uint16_t normMod(uint16_t sdlMod);

// Human-readable chord ("Ctrl+A", "Shift+D", "F4", "UNBOUND"), and its inverse.
std::string chordName(const KeyChord& c);
bool parseChord(const std::string& s, KeyChord& out);

// The effective binding set: defaults overlaid with the user's overrides. Built from
// Settings::hotkeys and consulted by the input handler.
class Hotkeys {
public:
    // Rebuild from the override map (Settings::hotkeys: id -> chord name, "NONE" = unbound).
    void load(const std::map<std::string, std::string>& overrides);
    KeyChord chord(Act a) const { return chords_[size_t(a)]; }
    // The action bound to this pressed key+mod, or Act::Count if none.
    Act match(int32_t key, uint16_t sdlMod) const;

private:
    KeyChord chords_[size_t(Act::Count)] = {};
};

// Read/write one binding as a settings override (only stores when != default; an
// explicit unbind stores "NONE"). Keeps the map minimal and forward-compatible.
KeyChord effectiveChord(const std::map<std::string, std::string>& overrides, Act a);
void setChordOverride(std::map<std::string, std::string>& overrides, Act a, KeyChord c);

}  // namespace ta
