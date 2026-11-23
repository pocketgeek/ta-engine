#pragma once

// How the pointer is shown, and what the broadcast gets. Header-only and SDL-free so
// the decisions can be tested without a window (tools/streamcursor_test.cpp).

#include "video/streamcursor.h"

#include <memory>
#include <string>

namespace tak {

// HARDWARE CURSOR on, and the platform has not refused our cursor: the OS draws it.
// Streaming is deliberately NOT an input. It used to force the software path, so the
// pointer would be drawn into the frame and so into the capture; the stream worker now
// stamps the OS cursor into the broadcast instead (video/streamcursor.h), and the
// player keeps the hardware cursor on screen.
inline bool useHardwareCursor(bool hardwareSetting, bool hardwareFailed) {
    return hardwareSetting && !hardwareFailed;
}

// Whether a captured frame should have the OS cursor stamped into it: the OS is showing
// a cursor (`osShown`) that a CursorSet baked (`osIsOurs` -- our art, not the system
// arrow), we still hold its picture, and the pointer is over the game window. With the
// software cursor the OS cursor is hidden, so this is false and the frame's own drawn
// cursor is the only one in the broadcast -- never two.
inline bool stampHardwareCursor(bool haveImage, bool osShown, bool osIsOurs, bool pointerInWindow) {
    return haveImage && osShown && osIsOurs && pointerInWindow;
}

// The process-wide mirror of the OS cursor a CursorSet last applied. The OS pointer
// is one per process however many CursorSets exist (game, title, briefing, result),
// so this is one too. `cursor` is identity only (an SDL_Cursor*, compared against
// SDL_GetCursor(), never dereferenced). `pixelScale` is renderer-output pixels per
// image pixel: 1, or the Retina backing scale on macOS, where SDL sizes the cursor
// in points. Main thread only.
struct ShownHardwareCursor {
    const void* cursor = nullptr;
    std::shared_ptr<const video::CursorImage> image;
    float pixelScale = 1;
};
inline ShownHardwareCursor& shownHardwareCursor() {
    static ShownHardwareCursor shown;
    return shown;
}

// What the last hardware-cursor bake did, for the log and the Options row. `tried`
// stays false until a hardware cursor has been asked for at all.
struct HardwareCursorStatus {
    bool tried = false, failed = false;
    int w = 0, h = 0;   // the OS cursor's size in pixels (points on macOS)
};
inline HardwareCursorStatus& hardwareCursorStatus() {
    static HardwareCursorStatus status;
    return status;
}

// The Options HARDWARE CURSOR row's note: what the OS was actually given. Empty while
// the option is off or before the first hardware cursor has been made.
inline std::string hardwareCursorNote(bool hardwareSetting, const HardwareCursorStatus& st) {
    if (!hardwareSetting || !st.tried) return {};
    if (st.failed) return "UNAVAILABLE - SOFTWARE";
    return "OS CURSOR " + std::to_string(st.w) + "X" + std::to_string(st.h);
}

}  // namespace tak
