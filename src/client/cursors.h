#pragma once

// Retail Total Annihilation: Kingdoms animated mouse cursors. Loads anims/cursors.gaf
// and draws the context-sensitive pointer (point / attack / move / reclaim / guard /
// ...). The cursor set, the per-context selection, the hotspot (each GAF frame's anchor)
// and the wall-clock ~15fps animation are reverse-engineered from KINGDOMS.icd by static
// analysis. Viewer-only -- never touches the deterministic sim or its state hash.

#include <SDL.h>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace tak {

namespace hpi { class Vfs; }

// The wired retail cursor set. Names map to cursors.gaf sequences in cursors.cpp. The
// shipped-but-never-selected placeholders (capture/teleport/pickup/resurrect/reanimate,
// all 1-frame clones of the normal arrow) are omitted.
// PathIcon is not a pointer at all: it is the bead retail strings along a selected
// unit's order line (icd 0x4d5700). It rides here because it lives in the same GAF,
// under the same palette, and wants the same per-frame textures.
enum class CursorId {
    Normal, Select, Move, Attack, Airstrike, TooFar, Patrol, Defend,
    Repair, Load, Unload, Reclaim, Revive, FindSite, Green, Red, Hourglass,
    PathIcon,
    Count
};

class CursorSet {
public:
    CursorSet() = default;
    ~CursorSet();
    CursorSet(const CursorSet&) = delete;
    CursorSet& operator=(const CursorSet&) = delete;

    // Load anims/cursors.gaf + its palette through the VFS and bake per-frame textures.
    // Returns false if the assets are missing/unreadable (the caller keeps the OS arrow).
    bool load(SDL_Renderer* ren, const hpi::Vfs& vfs);
    bool ok() const { return ok_; }

    // Draw `c` with its hotspot on the pixel (mouseX,mouseY) in renderer-output space.
    // `scale` (1..4) integer-magnifies the sprite AND its hotspot (nearest-neighbour, so
    // the pixel art stays crisp). Multi-frame cursors animate from wall-clock time;
    // switching cursor restarts it. `tint` colour-mods the sprite (default white =
    // untinted); fight-move reuses the Attack glyph with a tint to read apart from attack.
    void draw(SDL_Renderer* ren, CursorId c, int mouseX, int mouseY,
              int scale = 1, SDL_Color tint = SDL_Color{255, 255, 255, 255});

    // HARDWARE-cursor mode: instead of drawing into the frame, point the OS cursor at
    // `c`'s current animation frame (scaled + colour-modded like draw()). The OS tracks
    // the pointer position independently of our render loop, so it stays smooth when the
    // game hitches. Call once per frame IN PLACE OF draw(), and make sure the OS cursor is
    // shown (SDL_ShowCursor(SDL_ENABLE)). SDL_Cursors are built + cached lazily per
    // (cursor,tint) at `scale`; a scale change rebuilds. Returns false if the art is
    // missing or the platform rejected the cursor (e.g. size cap) -- the caller should
    // then fall back to draw() and hide the OS arrow.
    bool applyHardware(CursorId c, int scale, SDL_Color tint = SDL_Color{255, 255, 255, 255});

    // Draw one specific frame at (x,y), WITHOUT touching the pointer's animation
    // state. draw() restarts the animation whenever the cursor id changes, so using
    // it to stamp dozens of path beads per frame would reset the real pointer's
    // animation every time. Retail picks the bead's frame from the game tick and
    // steps it once per bead along the line, so the caller supplies the index.
    void drawFrame(SDL_Renderer* ren, CursorId c, size_t frame, int x, int y,
                   int scale = 1, SDL_Color tint = SDL_Color{255, 255, 255, 255}) const;
    // How many frames `c` has (0 if it did not load).
    size_t frameCount(CursorId c) const {
        return size_t(c) < anims_.size() ? anims_[size_t(c)].size() : 0;
    }

    // Free the cached SDL_Cursors and restore the default OS arrow. Call when turning
    // hardware mode off (so the software path can hide the arrow and draw its own).
    void releaseHardware();

private:
    struct Frame {
        SDL_Texture* tex = nullptr;
        int w = 0, h = 0, hx = 0, hy = 0;
        std::vector<uint8_t> rgba;   // kept (RGBA32) so hardware SDL_Cursors can be baked
        // Smooth-art reconstruction of `rgba` at exactly w*smoothScale x h*smoothScale.
        // Tint-INDEPENDENT, which is the whole point: the reconstruction is the expensive
        // part and the tint is a multiply, so caching this shares the cost across every
        // (cursor, tint) combination instead of paying it per combination.
        std::vector<uint8_t> smooth;
        int smoothScale = 0;         // 0 = not built
    };
    // Build every frame's `smooth` for this draw scale. Called once at load (and again
    // if CURSOR SIZE changes) so the work does not land inside a rendered frame.
    void precompute(int scale);
    std::array<std::vector<Frame>, size_t(CursorId::Count)> anims_;
    bool ok_ = false;
    CursorId cur_ = CursorId::Count;   // != any real id, so the first draw seeds the clock
    uint64_t animStartMs_ = 0;

    // Hardware-cursor cache: (CursorId << 32 | packed-RGBA-tint) -> one SDL_Cursor per
    // frame, built for hwScale_. hwSet_ is the currently-applied cursor (skip redundant
    // SDL_SetCursor); hwCur_/hwStartMs_ drive the animation clock in hardware mode.
    std::unordered_map<uint64_t, std::vector<SDL_Cursor*>> hw_;
    int hwScale_ = 0;                  // scale the cache was built for (0 = empty)
    SDL_Cursor* hwSet_ = nullptr;
    CursorId hwCur_ = CursorId::Count;
    uint64_t hwStartMs_ = 0;

    static constexpr int kFps = 15;    // retail cursor cadence (KINGDOMS.icd frame delta)
};

}  // namespace tak
