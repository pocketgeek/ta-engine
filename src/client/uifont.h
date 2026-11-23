#pragma once

// The client's TrueType interface face: Noto Sans Bold, embedded in takclient
// (res/chat-font, SIL OFL 1.1 -- CHAT-FONT-LICENSE.txt) and rasterized by stb_truetype
// (src/vendor/stb) at the exact pixel size drawn. The in-game chat box always uses it;
// with UI FONT = MODERN (client/uitext.h) every plain interface text does too. It
// replaced the retail bold Times GAF in the chat box, which players found hard to read.
//
// Its interface is the slice of Font's that text drawing uses -- width(), vbounds()
// and drawSolid(), with the same meaning of `scale` (the face is set 16 px tall, ascent
// to descent, at scale 1) and `y` (the top of the line; the baseline sits ascent below).
//
// Pixels. Glyphs are rasterized for DEVICE pixels: `scale` is in the renderer's
// logical units, and under a render scale (the lobby and the menus draw through
// SDL_RenderSetScale; on a macOS Retina window the UI SCALE carries the backing scale,
// 1 or 2 per display -- client/hidpi.h) the sheet is set at the scaled size and drawn
// back 1:1 on screen (nearest, whole device pixels), so text is sharp at 100%, at UI
// SCALE 200% and on Retina alike. Pen positions are kept in logical units at the
// logical size, so width() is exactly what drawSolid() advances, at every render
// scale; under 32 device px a pen's fraction of a pixel is kept through three
// horizontal subpixel phases.
//
// Text is UTF-8 (what SDL text input produces); a byte that does not start a valid
// sequence stands for itself as Latin-1, so legacy 8-bit text still shows. The face
// carries printable ASCII, Latin-1 and the typographic punctuation an OS keyboard or
// paste produces; any other character (CJK, emoji, control codes) draws as '?', one
// glyph per code point, so a line's measured and drawn widths always agree.
//
// Glyph sheets. One texture per device pixel size (whole pixels; 2 or 4 px steps for
// big text, within ~2%, so a live resize rebuilds rarely), built on the first draw at
// that size and kept: an interface draws a dozen sizes a frame, so they are cached together
// (least recently used goes first past kMaxSheets), never rebuilt per frame or per
// colour. destroyGlyphs() frees them all (renderer reset, session teardown, exit); the
// next draw rebuilds what it needs. Display only: nothing here touches the simulation.

#include <SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct stbtt_fontinfo;

namespace tak {

// Decode UTF-8 to code points. Only well-formed sequences decode (no overlongs,
// surrogates or values past U+10FFFF); any other byte stands for itself (Latin-1).
std::vector<uint32_t> decodeUtf8Lenient(const std::string& s);

class UiFont {
public:
    // The embedded Noto Sans Bold.
    UiFont();
    // Any TrueType face; `data` must outlive the font. `basePx` is the pixel height
    // (ascent - descent) at scale 1.
    UiFont(const unsigned char* data, size_t size, float basePx);
    ~UiFont();
    UiFont(const UiFont&) = delete;
    UiFont& operator=(const UiFont&) = delete;

    bool ok() const { return info_ != nullptr; }
    float width(const std::string& text, float scale = 1) const;
    // Ink extent of `text` below the line top: offset of its highest ink and its height.
    void vbounds(const std::string& text, float scale, float& topOff, float& h) const;
    void drawSolid(SDL_Renderer* ren, const std::string& text, float x, float y,
                   float scale, SDL_Color color) const;
    // Free every glyph sheet (gpuvram-accounted); the next draw rebuilds what it needs.
    void destroyGlyphs();

    // Vertical metrics as fractions of the line (ascent - descent): the ascent (line
    // top to baseline), the descent (baseline to line bottom), and the cap height ('H').
    float ascentFrac() const;
    float descentFrac() const { return 1 - ascentFrac(); }
    float capFrac() const { return capFrac_; }
    float basePx() const { return basePx_; }

    // The code point drawn for `cp`: itself when the face carries it, else '?'.
    static uint32_t mapped(uint32_t cp);
    // The pixel size `scale` is set at (quarter-pixel steps).
    float pixelSize(float scale) const;

    // The most sheets kept at once. A busy screen (the Options page at UI SCALE 200%,
    // the lobby with its chat) draws about a dozen sizes.
    static constexpr int kMaxSheets = 24;
    // Diagnostics for tests: sheets built so far, how many are held, and the size of
    // the sheet built last.
    int sheetBuilds() const { return builds_; }
    int sheetCount() const { return int(sheets_.size()); }
    int sheetW() const { return sheetW_; }
    int sheetH() const { return sheetH_; }

private:
    struct Metrics { int glyph = 0, advance = 0, y0 = 0, y1 = 0; bool box = false; };
    struct Glyph { int sx = 0, sy = 0, w = 0, h = 0, xoff = 0, yoff = 0; };
    static constexpr int kMaxPhases = 3;
    struct Sheet {
        SDL_Texture* tex = nullptr;
        SDL_Renderer* ren = nullptr;
        int key = -1;                  // sheet pixel size * 4 (sheetPx)
        int phases = 1;
        uint64_t used = 0;             // last-use stamp, for eviction
        std::vector<std::array<Glyph, kMaxPhases>> glyphs;   // per slot
    };
    static int slot(uint32_t mappedCp);         // index into metrics_ (mapped code points)
    static float sheetPx(float devicePx);       // the sheet size a device size draws from
    float sc(float px) const;                   // stbtt scale for a pixel height
    float kern(int a, int b) const;             // font units between two slots
    const Sheet* sheet(SDL_Renderer* ren, float devicePx) const;

    stbtt_fontinfo* info_ = nullptr;   // owned (incomplete here, so not a unique_ptr)
    float basePx_ = 16;
    int ascent_ = 0, descent_ = 0;
    float capFrac_ = 0.7f;
    std::vector<Metrics> metrics_;     // per slot
    mutable std::unordered_map<uint32_t, int16_t> kern_;   // slot pair -> font units

    mutable std::vector<Sheet> sheets_;
    mutable uint64_t clock_ = 0;
    mutable int builds_ = 0, sheetW_ = 0, sheetH_ = 0;
};

// The process-wide interface face (the embedded Noto Sans Bold), shared by the chat
// box and every MODERN text draw so they share glyph sheets. It is never destroyed
// (it outlives the renderer at exit); call destroyGlyphs() on it before the renderer
// goes.
UiFont& uiFont();

}  // namespace tak
