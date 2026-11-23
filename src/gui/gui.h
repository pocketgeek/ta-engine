#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Loader for the retail Total Annihilation: Kingdoms `.gui` files (the in-game HUD,
// menus and dialogs). NOT the OTA "[GADGET]{ xpos=; }" text format -- these are the
// classic TA length-prefixed TOKEN stream: a flat list of gadget records read to EOF,
// where each string is stored as "<length> <bytes>". Coordinates are in a 640x480
// base space. Format reverse-engineered from the shipped files (validated to parse
// all 106 to EOF) and KINGDOMS.icd RTTI; see docs and the gadget-type table below.
namespace tak::gui {

// A GAF image reference: sequence `seq` frame `frame` in `anims/<gaf>`. `flags` is the
// per-image blit mode (0 = none, else 9/18).
struct ImgRef {
    std::string gaf, seq;
    int frame = 0;
    int flags = 0;
};

// Gadget class ids (from KINGDOMS.icd RTTI): 1=Dialog root, 2=Panel/picture,
// 4=Button, 5=CheckBox, 7=TextBox, 9=GroupBox, 12=Slider, 14=ScrollBar arrow,
// 15=ListBox, 16=ProgressBar, 17=RadioButton/state-toggle, 18=combo, 19=Static
// (label/image), 21=EditBox.
struct Gadget {
    int type = 0, ver = 0;
    std::string name;               // logical id, e.g. "MOVE", "InGameDesktop"
    int x = 0, y = 0, w = 0, h = 0; // rect in 640x480 space
    uint8_t ca = 255, cr = 0, cg = 0, cb = 0;   // colour (ARGB)
    std::vector<ImgRef> imgs;       // state art: 0=normal, 1=hover/pressed, 2=grayed
    std::vector<std::string> texts; // label text entries
    std::vector<std::string> states;// per-state strings (e.g. click-sound in slot 1)
    std::string cmd;                // command binding / tooltip / hotkey macro
};

struct Gui {
    std::vector<Gadget> gadgets;    // gadgets[0] is the screen/dialog root
    // First gadget whose name matches (case-insensitive), or nullptr.
    const Gadget* find(const std::string& name) const;
};

// Parse a .gui byte stream. Throws std::runtime_error on a malformed record.
Gui parse(const std::vector<uint8_t>& bytes, const std::string& origin);

}  // namespace tak::gui
