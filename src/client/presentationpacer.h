#pragma once
#include <SDL.h>
#include <cstdint>

namespace tak {
// Some drivers/compositors keep reporting VSync enabled while presents run
// ahead (stream readback; macOS Metal/Core Animation). Bound presentation at
// the display refresh whenever VSync is on, without double-pacing a swap that
// has already waited for the monitor.
class PresentationPacer {
public:
    void pace(SDL_Window* window, bool enabled) {
        if (!enabled) { last_ = 0; return; }
        const auto now = SDL_GetTicks64();
        const int display = SDL_GetWindowDisplayIndex(window);
        if (display != display_ || !sampled_ || now - sampled_ >= 1000) {
            SDL_DisplayMode mode{};
            if (SDL_GetCurrentDisplayMode(display, &mode) != 0 || mode.refresh_rate <= 0)
                SDL_GetDesktopDisplayMode(display, &mode);
            refresh_ = mode.refresh_rate;
            display_ = display; sampled_ = now;
        }
        paceAtRefresh(refresh_);
    }
    // Separate from display discovery so the fallback can be tested without
    // requiring a physical display or depending on its compositor.
    void paceAtRefresh(int hz) {
        if (hz <= 0) { last_ = 0; return; }
        const auto frequency = SDL_GetPerformanceFrequency();
        auto now = SDL_GetPerformanceCounter();
        const auto interval = frequency / uint64_t(hz);
        if (last_ && now - last_ < interval) {
            auto remaining = interval - (now - last_);
            // Ignore sub-millisecond swap jitter when hardware VSync is working.
            if (remaining > frequency / 1000) {
                const auto deadline = last_ + interval;
                while (now < deadline) {
                    remaining = deadline - now;
                    if (remaining > frequency / 500)
                        SDL_Delay(Uint32(remaining * 1000 / frequency - 1));
                    now = SDL_GetPerformanceCounter();
                }
            }
        }
        last_ = now;
    }
private:
    uint64_t last_ = 0, sampled_ = 0;
    int display_ = -1, refresh_ = 0;
};
}
