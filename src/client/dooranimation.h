#pragma once

#include <array>
#include <string_view>

namespace tak::menu {

struct DoorHotspot {
    const char* gadget;
    int x;
    int y;
    int w;
    int h;
};

// Hit rectangles written by retail's main-menu initializer (KINGDOMS.icd
// 0x4a4ef0, 0x4a4f71, 0x4a4ff2, 0x4a5068). The GUI gadget rectangles are larger;
// these are the actual door hit areas.
inline constexpr std::array<DoorHotspot, 4> kRetailDoorHotspots{{
    {"PlayComputer", 71, 219, 101, 158},
    {"PlayStory", 289, 217, 62, 168},
    {"PlayPlayer", 487, 216, 63, 157},
    {"Credits", 124, 42, 71, 130},
}};

constexpr const DoorHotspot* retailDoorHotspot(std::string_view gadget) {
    for (const auto& hotspot : kRetailDoorHotspots)
        if (gadget == hotspot.gadget) return &hotspot;
    return nullptr;
}

// Retail's hit-test accepts x <= left + width and y <= top + height.
constexpr bool insideRetailDoorHotspot(const DoorHotspot& r, float x, float y) {
    return r.gadget && r.w > 0 && r.h > 0 &&
           x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

enum class DoorState { Idle, In, Loop, Out };
enum class DoorTransition { None, StartIn, StartOut, StartLoop, RestartLoop, StartIdle };

// Mirror the native state polling: hover starts clip 5 only from idle, and
// leaving starts clip 7 only from the hover loop. The hover-in clip is allowed
// to finish before the next update observes the leave and starts clip 7.
constexpr DoorTransition doorTransitionForHover(DoorState state, bool hovered) {
    if (state == DoorState::Idle && hovered) return DoorTransition::StartIn;
    if (state == DoorState::Loop && !hovered) return DoorTransition::StartOut;
    return DoorTransition::None;
}

constexpr DoorTransition doorTransitionAtClipEnd(DoorState state) {
    switch (state) {
    case DoorState::In: return DoorTransition::StartLoop;
    case DoorState::Loop: return DoorTransition::RestartLoop;
    case DoorState::Out: return DoorTransition::StartIdle;
    case DoorState::Idle: return DoorTransition::None;
    }
    return DoorTransition::None;
}

} // namespace tak::menu
