#pragma once

// TA's game-loading screen (TotalA.exe 0x497f40, docs/retail-gui.md "Loading screen"):
// the backdrop bitmaps/loadgame2bg.pcx with six labelled progress bars (Textures,
// Terrain, Units, Animation, 3D Data, Explosions), "Map: <name>" at the bottom
// (not in a campaign), and in a network game a strip of per-player bars with the
// "Waiting for other players" line above it.
//
// Used as a scoped object around the heavy part of starting a game: construct it,
// report each part as it finishes, and let it fall out of scope when the first tick
// runs. setPart() and step() draw and present a frame themselves, because the work
// they bracket blocks the render loop.

#include <SDL.h>
#include "net/protocol.h"

#include <array>
#include <functional>
#include <memory>
#include <string>

namespace ta::hpi { class Vfs; }

namespace ta {

struct Settings;
class TaFont;
struct LitFont;

// The six bars, in retail's order (progress bytes game +0x38d6f..+0x38d74).
enum class LoadPart { Textures, Terrain, Units, Animation, Model3D, Explosions };
inline constexpr int kLoadParts = 6;

class LoadScreen {
public:
    // `mapName` empty hides the map line (retail hides it in a campaign).
    LoadScreen(SDL_Renderer* ren, const hpi::Vfs& vfs, const std::string& mapName,
               Settings* settings = nullptr);
    ~LoadScreen();

    LoadScreen(const LoadScreen&) = delete;
    LoadScreen& operator=(const LoadScreen&) = delete;

    // Name the players in the game, in slot order; an empty name means the slot is
    // open/closed and gets no cell. `you` is the local slot (its cell tracks our own
    // progress); pass -1 when spectating. The player strip shows only when some
    // other human is in the game (retail draws it for network games).
    void setPlayers(const std::array<std::string, ta::net::kMaxSlots>& names, int you,
                    bool network);

    // A remote slot's progress (0..100), as the server relays it (MpClient::
    // slotLoadPercent); setSlotDone is 100.
    void setSlotPercent(int slot, int pct);
    void setSlotDone(int slot) { setSlotPercent(slot, 100); }

    // Called with our own percent (the mean of the six parts) whenever it changes,
    // for the other players' screens (retail sends packet 0x2a, 0x456de0).
    std::function<void(int)> onProgress;

    // Set one bar (0..100) without presenting; the six bars show from the first call.
    void setPartQuiet(LoadPart part, int pct);
    // ...and draw and present.
    void setPart(LoadPart part, int pct);

    // A status line instead of the bars (the engine's connect-and-prepare screens,
    // which retail has no equivalent of): drawn where retail's waiting line goes.
    void step(const std::string& status);

    // True under the dummy video driver: the screen draws nothing, so a harness run
    // should skip it entirely rather than hold the frame.
    bool headless() const { return headless_; }

    // Draw the current state into whatever target is bound, WITHOUT presenting --
    // for the render loop, which owns the present (and the AA resolve before it).
    void draw();

private:
    // Draw and present immediately, to the window, past any render target.
    void present();
    int localPercent() const;

    SDL_Renderer* ren_ = nullptr;
    const hpi::Vfs* vfs_ = nullptr;
    Settings* settings_ = nullptr;
    SDL_Texture* bg_ = nullptr;
    SDL_Texture* lightbar_ = nullptr;   // LIGHTBAR frame 0, anims/commongui.gaf
    int lightW_ = 0, lightH_ = 0;
    std::unique_ptr<LitFont> lit_;      // anims/hattfont12.gaf, the current GAF font
    std::unique_ptr<TaFont> font_;      // fonts/smlfont.fnt, if hattfont12 is missing
    int fntHeight_ = 0;                 // smlfont's height (0x4c1450, the map line's y)
    // The label flash (0x498796...): 30 when a part reaches 100, -2 per 30 Hz tick,
    // at most one step a frame (0x4985cc..0x4985fb); the light-table row it draws with.
    std::array<uint8_t, kLoadParts> flash_{};
    std::array<int, kLoadParts> shownPct_{};
    uint64_t flashTick_ = 0;
    // guipal entries through the screen palette (game +0xdcb, 0x4ac7d0).
    SDL_Color red_{255, 85, 85, 255}, green_{85, 255, 85, 255}, darkRed_{170, 0, 0, 255},
              white_{255, 255, 255, 255};
    std::string map_, status_;
    std::array<int, kLoadParts> part_{};
    bool partsShown_ = false;
    std::array<std::string, ta::net::kMaxSlots> names_{};
    std::array<int, ta::net::kMaxSlots> slotPct_{};
    int reported_ = -1;   // the percent last passed to onProgress
    int you_ = -1;
    bool network_ = false;
    bool headless_ = false;
    int presents_ = 0;
};

}  // namespace ta
