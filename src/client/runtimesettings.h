// runtimesettings -- push Settings into the runtime globals that render code reads.
//
// A few display options cannot be read from Settings at the point they are used: the
// door-video path has no settings pointer, MainMenu::playIntro is static, and the art
// helpers run deep inside texture creation. Those read globals instead (art::g_smoothArt,
// art::g_cursorFactor, video::g_deblock), and something has to keep the globals in step
// with the Settings the user is editing.
//
// That "something" is this one function, called from app startup and from every
// OptionsScreen onChange -- rather than each host remembering to poke each global. The
// per-host version of this is how SMOOTH MOVIES shipped ignoring its own toggle until a
// restart: the flag was refreshed at startup and menu entry, so changing the option and
// replaying the intro in the same session used the stale value. One function means a new
// option is wired once and reaches every host, including the DEFAULTS button (which goes
// through the same onChange).

#pragma once

#include "client/artscale.h"
#include "client/dev.h"
#include "client/gpuvram.h"
#include "client/settings.h"
#include "client/uitext.h"
#include "client/videofilter.h"

namespace tak {

inline size_t gpuMemoryLimitBytes(int mib) {
    mib = snapGpuMemoryLimit(mib);
    return mib < 0 ? gpuvram::kUnlimited : size_t(mib) * gpuvram::kMiB;
}

inline void applyRuntimeSettings(const Settings& s) {
    // Active interface owners reload their art only when smoothArt changes. New
    // screens also build with these values, so smoothing reaches every screen.
    art::setSmoothArt(s.smoothArt);
    art::setCursorFactor(s.cursorScale);
    // Deblocking IS live: it runs per decoded frame, so this reaches the next clip.
    video::setDeblock(s.videoDeblock);
    // GPU MEMORY LIMIT is live too: the caches grow into a raised limit and shed down
    // to a lowered one from the next frame (gpuvram.h). Unchanged = a no-op, so a
    // post-failure tightening survives the other options being tweaked.
    gpuvram::setLimit(gpuMemoryLimitBytes(s.gpuMemoryLimit));
    // UI FONT is live: text is measured and drawn in the chosen face from the next frame.
    setUiFontStyle(s.uiFont == 1 ? UiFontStyle::Classic : UiFontStyle::Modern);
#ifndef NDEBUG
    // Render benchmark only (tools/render_benchmark.py "constrained"): a cap that
    // actually binds. The Options minimum (512 MiB) and the policy floor (384 MiB)
    // sit above what most benchmark scenes hold, so neither constrains them.
    if (const char* cap = devEnv("TAK_GPU_CAP_MIB"); cap && std::atoi(cap) > 0)
        gpuvram::g_policy.limit = gpuvram::g_policy.cap = size_t(std::atoi(cap)) * gpuvram::kMiB;
#endif
}

}  // namespace tak
