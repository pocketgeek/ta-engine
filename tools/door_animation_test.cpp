#include "client/dooranimation.h"

#include <cstdio>

namespace {

bool check(bool condition, const char* what) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", what);
    return condition;
}

} // namespace

int main() {
    using namespace tak::menu;
    bool ok = true;

    struct Expected {
        const char* gadget;
        DoorHotspot rect;
        float outsideX;
        float outsideY;
    };
    constexpr Expected expected[] = {
        {"PlayComputer", {"PlayComputer", 71, 219, 101, 158}, 41, 193},
        {"PlayStory", {"PlayStory", 289, 217, 62, 168}, 243, 203},
        {"PlayPlayer", {"PlayPlayer", 487, 216, 63, 157}, 420, 137},
        {"Credits", {"Credits", 124, 42, 71, 130}, 68, 21},
    };

    for (const auto& e : expected) {
        const DoorHotspot* actual = retailDoorHotspot(e.gadget);
        ok &= check(actual != nullptr, "all four retail door hotspot names resolve");
        if (!actual) continue;
        ok &= check(actual->x == e.rect.x && actual->y == e.rect.y &&
                    actual->w == e.rect.w && actual->h == e.rect.h,
                    "hotspot coordinates match the native initializer");
        ok &= check(insideRetailDoorHotspot(*actual, float(actual->x), float(actual->y)),
                    "hotspot includes its top-left edge");
        ok &= check(insideRetailDoorHotspot(*actual, float(actual->x + actual->w),
                                             float(actual->y + actual->h)),
                    "hotspot includes its bottom-right edge like retail");
        ok &= check(!insideRetailDoorHotspot(*actual, float(actual->x - 1),
                                              float(actual->y)),
                    "hotspot rejects a point left of the door");
        ok &= check(!insideRetailDoorHotspot(*actual, float(actual->x + actual->w + 1),
                                              float(actual->y)),
                    "hotspot rejects a point right of the door");
        ok &= check(!insideRetailDoorHotspot(*actual, e.outsideX, e.outsideY),
                    "hover/click area stays narrower than the GUI gadget rectangle");
    }
    ok &= check(retailDoorHotspot("missing") == nullptr, "unknown gadget has no hotspot");
    ok &= check(!insideRetailDoorHotspot({nullptr, -1, -1, 0, 0}, -1.0f, -1.0f),
                "unconfigured hotspot never matches the letterbox area");

    // Retail does not interrupt hover-in on mouse-out. The animation reaches its
    // loop state first, and the next update begins the exit clip.
    DoorState state = DoorState::Idle;
    DoorTransition transition = doorTransitionForHover(state, true);
    ok &= check(transition == DoorTransition::StartIn, "hover from idle starts clip 5");
    state = DoorState::In;
    transition = doorTransitionForHover(state, false);
    ok &= check(transition == DoorTransition::None && state == DoorState::In,
                "exit during clip 5 waits for clip 5 to finish");
    transition = doorTransitionAtClipEnd(state);
    ok &= check(transition == DoorTransition::StartLoop,
                "clip 5 completion enters clip 6 even after pointer exit");
    state = DoorState::Loop;
    transition = doorTransitionForHover(state, false);
    ok &= check(transition == DoorTransition::StartOut,
                "the following update starts clip 7 when focus is outside");
    state = DoorState::Out;
    ok &= check(doorTransitionAtClipEnd(DoorState::Loop) == DoorTransition::RestartLoop,
                "clip 6 loops while hovered");
    transition = doorTransitionForHover(state, true);
    ok &= check(transition == DoorTransition::None && state == DoorState::Out,
                "re-entering during clip 7 does not interrupt the exit");
    transition = doorTransitionAtClipEnd(state);
    ok &= check(transition == DoorTransition::StartIdle,
                "clip 7 completion returns to idle before a new hover-in");
    state = DoorState::Idle;
    ok &= check(doorTransitionForHover(state, true) == DoorTransition::StartIn,
                "the next update starts clip 5 after clip 7 completes under the pointer");
    ok &= check(doorTransitionForHover(DoorState::Idle, false) == DoorTransition::None,
                "idle remains held without hover");

    if (!ok) return 1;
    std::puts("PASS: retail door hotspots and hover/exit state timing");
    return 0;
}
