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

union SDL_Event;

namespace tak {

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
    GiveUnits, ToggleCounts, UnitInfo, FullScreenRadar, Disco, Headbang, RetailZoom,
    PushToTalk,
    MakePoster,
    StreamingPanel,
    ToggleFullscreen,
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

// Keys the game reads itself, outside the rebindable actions, that PUSH TO TALK may not
// take: it is checked before them and swallows its key in a voice room, so on Enter the
// chat composer could never open, on a digit a squad could never be recalled, and on an
// arrow the camera could not pan. (Esc cancels a capture and is never bound.)
bool pushToTalkReserved(int32_t key);

// The effective binding set: defaults overlaid with the user's overrides. Built from
// Settings::hotkeys and consulted by the input handler.
class Hotkeys {
public:
    // Rebuild from the override map (Settings::hotkeys: id -> chord name, "NONE" = unbound).
    void load(const std::map<std::string, std::string>& overrides);
    KeyChord chord(Act a) const { return chords_[size_t(a)]; }
    // The action bound to this pressed key+mod, or Act::Count if none.
    Act match(int32_t key, uint16_t sdlMod) const;
    // Push-to-talk is held, not pressed, so it matches more loosely than match(): its
    // key with AT LEAST its modifiers (Shift-clicking while talking still talks),
    // unless another action is bound to exactly the chord being pressed. Releases match
    // on the key alone, whatever modifiers are down by then.
    bool pushToTalkPress(int32_t key, uint16_t sdlMod) const;
    bool pushToTalkRelease(int32_t key) const {
        const KeyChord c = chord(Act::PushToTalk);
        return c.bound() && c.key == key;
    }

private:
    KeyChord chords_[size_t(Act::Count)] = {};
};

// Does this key press open or close the streaming panel? The panel sits above the menu
// and the game, so it reads the override map itself. `typing` is a text field having the
// keyboard: a plain character key is the field's then, not the panel's.
bool streamingPanelPress(const std::map<std::string, std::string>& overrides, int32_t key,
                         uint16_t sdlMod, bool typing);
// The same for TOGGLE FULLSCREEN, which every screen's event loop reads (menu, lobby,
// game, briefing, results).
bool fullscreenPress(const std::map<std::string, std::string>& overrides, int32_t key,
                     uint16_t sdlMod, bool typing);
// Flips `fullscreen` and the event's window when this event is the TOGGLE FULLSCREEN
// press; the caller saves the setting. Never while the Hotkeys screen waits for a key
// to bind (hotkeyCaptureActive), so the key itself can be bound.
bool toggleFullscreenKey(const SDL_Event& e, const std::map<std::string, std::string>& overrides,
                         bool& fullscreen);
inline bool hotkeyCaptureActive = false;

// Read/write one binding as a settings override (only stores when != default; an
// explicit unbind stores "NONE"). Keeps the map minimal and forward-compatible.
KeyChord effectiveChord(const std::map<std::string, std::string>& overrides, Act a);
void setChordOverride(std::map<std::string, std::string>& overrides, Act a, KeyChord c);

}  // namespace tak
