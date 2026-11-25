#pragma once
// TotalA.exe's unit speech event table (0x5086dc, sixteen rows of 0x18 bytes):
// +0 the priority, +4 the cooldown in seconds, +8 the sound class slot, +0xc the
// default line (none: speech only). 0x47f780(unit, event, text) asks the speech
// queue (client/speechqueue.h) for a row for the local player's own unit
// (+0xff == game +0x2a43, alive) with its line, or the row's when none is given;
// the queue drops it while the game tick is below the row's next-allowed tick
// (0x47faf0) or one of the row is queued. Playing it (0x47fd70): the speech when
// the priority beats 10 - the SPEECH level (0x47fdf9) and the unit's sound class
// has the slot, which sets the next-allowed tick to now + cooldown x 30
// (0x47feee); the line "<unit type name>: <line>" (0x47ff69) when the priority
// beats 10 - the UNITCHAT level (0x47ff26).
#include <array>
#include <cstdint>

namespace ta {

struct UnitSpeechRow { int priority; int cooldownSeconds; const char* slot; const char* text; };

inline constexpr std::array<UnitSpeechRow, 16> kUnitSpeechRows{{
    {0, 0, "", nullptr},
    {10, 0, "select", nullptr},
    {9, 20, "underattack", "Under Attack"},
    {4, 2, "activate", nullptr},
    {4, 2, "deactivate", nullptr},
    {5, 1, "ok", nullptr},
    {3, 4, "arrived", "Arrived"},
    {8, 1, "cant", "Cannot Comply"},
    {3, 3, "unitcomplete", "Nanolathe Complete"},
    {4, 2, "build", nullptr},
    {3, 1, "repair", nullptr},
    {2, 1, "working", nullptr},
    {7, 1, "load", nullptr},
    {7, 1, "unload", nullptr},
    {7, 1, "cloak", "Cloaked"},
    {7, 1, "uncloak", "Visible"},
}};

inline const UnitSpeechRow* unitSpeechRow(int event) {
    return event >= 0 && event < int(kUnitSpeechRows.size()) ? &kUnitSpeechRows[size_t(event)] : nullptr;
}
// 0x47fdf9 / 0x47ff26: a level of 0, 5 or 10.
inline bool unitSpeechHeard(int priority, int speechLevel) { return priority > 10 - speechLevel; }

} // namespace ta
