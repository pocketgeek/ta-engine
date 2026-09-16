#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Loader for retail Total Annihilation's `.gui` files -- the in-game command
// panel, the menus and the dialogs.
//
// These are TDF TEXT: a flat list of `[GADGETn] { [COMMON] { ... } ... }` blocks,
// where COMMON carries the same fourteen keys for every gadget and the
// type-specific keys sit beside it. GADGET0 is the root, and additionally
// carries `totalgadgets`, a `[VERSION]` block, and `panel=` naming the panel art.
//
// Worth stating plainly because the sibling engine did the opposite: Kingdoms'
// `.gui` files are a length-prefixed binary TOKEN stream, and its loader's own
// header comment went out of its way to say they are "NOT the OTA
// [GADGET]{xpos=;} text format". TA is exactly that text format. Nothing but the
// extension carries over.
//
// Key inventory below is complete: taken from all 368 .gui files in the Commander
// Pack (5840 gadgets), so anything absent here is absent from the shipped data.
namespace ta::gui {

// A GAF image reference: sequence `seq` frame `frame` in `anims/<gaf>`.
struct ImgRef {
    std::string gaf, seq;
    int frame = 0;
    int flags = 0;
};

// Gadget types, from COMMON's `id`. TA reuses one small set across every screen.
enum : int {
    kRoot = 0,       // the window itself (GADGET0)
    kButton = 1,
    kListBox = 2,
    kTextField = 3,
    kScrollBar = 4,
    kLabel = 5,
    kSurface = 6,    // a picture / plain drawing area
    kFont = 7,       // a font declaration: `filename=` names fonts/<name>.FNT
};

struct Gadget {
    int type = 0;                   // COMMON id
    int assoc = 0;                  // association group
    std::string name;               // logical id, e.g. "ARMORDERS", "ARMMOVE"
    int x = 0, y = 0, w = 0, h = 0; // rect in 640x480 space
    int attribs = 0, commonAttribs = 0;
    int colorF = 0, colorB = 0;     // palette indices
    int textureNumber = 0, fontNumber = 0;
    bool active = true;

    // Type-specific, all optional.
    int quickKey = 0;               // hotkey (an ASCII code)
    bool grayedOut = false;
    int stages = 0;                 // multi-state button: how many faces
    int status = 0;
    std::string text;               // label / caption
    std::string help;               // tooltip
    std::string filename;           // kFont: the .FNT stem
    int range = 0, thick = 0, knobPos = 0, knobSize = 0;   // scrollbar geometry
    int maxChars = 0, itemHeight = 0;

    std::vector<ImgRef> imgs;       // state art, when a screen names it explicitly
    std::string cmd;                // command binding / tooltip / hotkey macro
};

struct Gui {
    std::vector<Gadget> gadgets;    // gadgets[0] is the root
    std::string panel;              // root `panel=`: the panel art's GAF sequence
    int totalGadgets = 0;           // root `totalgadgets=` (declared, may differ)
    // First gadget whose name matches (case-insensitive), or nullptr.
    const Gadget* find(const std::string& name) const;
};

// Parse a .gui file. Throws std::runtime_error if it is not TA's text format.
Gui parse(const std::vector<uint8_t>& bytes, const std::string& origin);

}  // namespace ta::gui
