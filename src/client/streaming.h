#pragma once
#include "video/stream.h"
#include <SDL.h>
#include <functional>
#include <memory>
#include <string>
namespace tak {
struct Settings;
namespace credstore { class Store; }
constexpr int kStreamingEvent = 0x54414b53;
// Owned by the application; survives transitions between title and game.
// REMEMBER KEY (client/streamkey.h) reads/writes settings->streamRememberKey and calls
// `save` after changing it; without settings it still works for the session. `store`
// defaults to streamKeyStore(); tests inject a fake. SAVE A LOCAL COPY likewise keeps
// settings->streamLocalCopy (default off) and saves it; it applies at Start Streaming.
class Streaming {
public:
    explicit Streaming(SDL_Renderer*, Settings* settings = nullptr, std::function<void()> save = {},
                       std::shared_ptr<credstore::Store> store = nullptr);
    ~Streaming();
    video::Stream& stream();
    bool input(const SDL_Event&, int w, int h);
    // Capture BEFORE drawing private setup UI. Never stream the setup panel.
    void frame(int w, int h, bool drawPanel = true);
    void draw(int w, int h);
    bool shown() const;
    // The bottom edge of the status banner draw() puts in the top-left corner while the
    // panel is closed (STREAM LIVE ... - F9), or 0 with none: the game's own top-left
    // overlays go below it (client/hudcorner.h).
    float bannerBottom(int w) const;
    // Tests and screenshots: the REMEMBER KEY row, and a wait for its credential-store work.
    SDL_FRect rememberRect(int w, int h);
    SDL_FRect localCopyRect(int w, int h); // the SAVE A LOCAL COPY row
    // The BITRATE row, its RECOMMENDED button, and why the bitrate was last lowered
    // (empty: it was not, or a choice since cleared it).
    SDL_FRect bitrateRect(int w, int h);
    SDL_FRect recommendedRect(int w, int h);
    const std::string& notice() const;
    bool waitKeyStore(int ms);
#ifdef TAK_STREAM_TESTING
    // Use this cursor for every capture instead of the OS pointer's (null: back to the
    // OS pointer), so a hidden test window can check stamping through each backend.
    void testCursor(const video::StreamCursor*);
#endif
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
}
