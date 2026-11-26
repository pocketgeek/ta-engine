#pragma once

// Draws and drives one Total Annihilation .GUI screen (gui/tagui.h) the way
// TotalA.exe does (docs/retail-gui.md): screen GAF anims\<SCREEN>.gaf, then
// anims\commongui.gaf; per-gadget anims\<NAME>_gadget.gaf overrides; BUTTONS0 /
// stagebuttnN / CHECKBOX fallbacks with best-size resizing; frame per state;
// GAF (hattfont12/11) and 1-bpp .FNT fonts; Enter/Esc defaults and quickkeys.
// Coordinates are the GUI's own 640x480 space, mapped onto the window by GuiLayout.

#include <SDL.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "client/guiart.h"
#include "gaf/gaf.h"
#include "gui/tagui.h"

namespace ta::hpi { class Vfs; }

namespace ta {

// A .FNT font (1-bpp glyphs) or a GAF font, rendered to per-glyph textures.
class TaFont {
public:
    bool loadFnt(SDL_Renderer* ren, const std::vector<uint8_t>& bytes, SDL_Color color);
    bool loadGaf(SDL_Renderer* ren, const hpi::Vfs& vfs, const std::string& gaf, const gaf::Palette& pal);
    ~TaFont();
    int height() const { return height_; }
    int width(const std::string& s) const;
    // The rows a string's glyphs cover, relative to draw()'s y (top inclusive, bottom exclusive).
    void inkSpan(const std::string& s, int& top, int& bottom) const;
    void draw(SDL_Renderer* ren, const GuiLayout& L, float x, float y, const std::string& s, SDL_Color tint,
              uint8_t alpha = 255, bool shadow = false) const;
private:
    struct Glyph { SDL_Texture* tex = nullptr; int w = 0, h = 0, xoff = 0, yoff = 0, advance = 0; };
    Glyph glyphs_[256];
    int height_ = 0;
    bool gafFont_ = false;
};

class TaGuiView {
public:
    // menuPalette: front-end screens use palettes/guipal.pal, in-game palette.pal.
    TaGuiView(SDL_Renderer* ren, const hpi::Vfs& vfs, tagui::Screen screen, bool menuPalette = true);
    ~TaGuiView();
    TaGuiView(const TaGuiView&) = delete;
    TaGuiView& operator=(const TaGuiView&) = delete;

    tagui::Screen& screen() { return screen_; }
    const tagui::Screen& screen() const { return screen_; }
    // A gadget's centre in 640x480 screen space (false if there is no such gadget).
    bool gadgetCentre(const std::string& name, float& x, float& y) const;
    // Full-screen backdrop (e.g. bitmaps/frontendx.pcx) drawn instead of the panel.
    void setBackdrop(const std::string& pcxPath);
    // Place the container: per section 1.2, -1 centres; negative y anchors to the
    // bottom; otherwise as authored. (screen size is the GUI base, 640x480.)
    void place(int screenW = 640, int screenH = 480);
    int originX() const { return ox_; }
    int originY() const { return oy_; }

    // Widget state.
    void setText(const std::string& gadget, const std::string& text);
    void setGrayed(const std::string& gadget, bool grayed);
    void setStage(const std::string& gadget, int stage);
    int stage(const std::string& gadget) const;
    void setPressed(const std::string& gadget, bool down);   // latched/radio state
    bool pressed(const std::string& gadget) const;
    void setVisible(const std::string& gadget, bool visible);
    bool visible(const std::string& gadget) const;
    void setList(const std::string& gadget, std::vector<std::string> items);
    int selection(const std::string& gadget) const;
    void setSelection(const std::string& gadget, int index);
    // Code-drawn content for surfaces (map previews, radar, unit pics): called with
    // the gadget's window rect.
    std::function<void(const tagui::Gadget&, SDL_FRect)> drawSurface;

    void draw(const GuiLayout& L);
    // Feed an SDL event; returns the name of the activated gadget ("" for none).
    // Buttons activate on release over themselves, quickkeys and Enter/Esc at once.
    std::string handle(const SDL_Event& e, const GuiLayout& L);
    // Draw frame `frame` of `gaf`'s sequence `seq` with its top-left at (x, y) in GUI space.
    void drawSequence(const std::string& gaf, const std::string& seq, int frame, float x, float y, const GuiLayout& L);
    // Width and height of that frame ({0, 0} when it is missing).
    SDL_Point sequenceSize(const std::string& gaf, const std::string& seq, int frame = 0);
    // Colour `index` of this view's palette (palette.pal in game, guipal.pal in menus).
    SDL_Color paletteColor(int index) const {
        const auto& c = pal_.rgba[index & 255];
        return SDL_Color{c[0], c[1], c[2], 255};
    }
    // Press gadget `name` as a click would (stage/radio/latch effects); returns the
    // name, or "" when it is absent or grayed. For scripted front-end runs.
    std::string activate(const std::string& name);
    // The gadget under (mx, my) in window coordinates, or nullptr.
    const tagui::Gadget* at(float mx, float my, const GuiLayout& L) const;
    // Help text of the gadget under the mouse (labels follow their link).
    std::string hoverHelp(float mx, float my, const GuiLayout& L) const;

private:
    struct Frames { std::vector<SDL_Texture*> tex; std::vector<SDL_Rect> geo; };   // geo: xoff,yoff,w,h
    const Frames* sequence(const std::string& gaf, const std::string& seq);
    const Frames* buttonArt(const tagui::Gadget& g, int& base);
    void drawFrame(const Frames& f, int frame, const SDL_FRect& r, const GuiLayout& L, bool anchor);
    void drawText(const tagui::Gadget& g, const std::string& s, float x, float y, bool centre, const GuiLayout& L);
    struct Rect { float x, y, w, h; };
    Rect rectOf(const tagui::Gadget& g) const;

    struct State { int stage = 0; bool pressed = false, visible = true, down = false; std::vector<std::string> items; int sel = -1, scroll = 0; };
    SDL_Renderer* ren_;
    const hpi::Vfs& vfs_;
    tagui::Screen screen_;
    gaf::Palette pal_{};
    std::map<std::string, std::vector<gaf::Sequence>> gafs_;   // loaded GAF files
    std::map<std::string, Frames> seqs_;                      // "gaf/seq" -> textures
    std::map<std::string, State> state_;                      // by upper-case gadget name
    std::map<int, int> resized_;                              // gadget index -> BUTTONS0 base frame
    SDL_Texture* backdrop_ = nullptr;
    int backW_ = 0, backH_ = 0;
    int ox_ = 0, oy_ = 0;
    std::unique_ptr<TaFont> font_, font11_;
    std::string downGadget_;
    std::string focus_;   // text field taking typed input
    State& st(const std::string& name);
    const State* stc(const std::string& name) const;
};

}  // namespace ta
