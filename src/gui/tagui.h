#pragma once

// Total Annihilation .GUI screens (docs/retail-gui.md section 1): TDF text with
// [GADGET0] .. [GADGETn] read until the first gap (TotalA.exe parser 0x4aeac0).
// GADGET0 is the container (screen header); its name is replaced by the file's
// base name, which is how the screen's GAF (anims\<name>.gaf) is found.
// This is the data model; client/taguiview draws and drives it.

#include <cstdint>
#include <string>
#include <vector>

namespace ta::hpi { class Vfs; }

namespace ta::tagui {

enum class Type : uint8_t {
    Container = 0, Button = 1, ListBox = 2, TextField = 3, ScrollBar = 4, Label = 5,
    Surface = 6, Font = 7, Picture = 8, None9 = 9, Nuttin = 10, BackgroundPic = 12,
};

// Button attribs (section 1.3).
inline constexpr uint32_t kAttrCheckbox = 0x80;
inline constexpr uint32_t kAttrGrayLastFrame = 0x100;
inline constexpr uint32_t kAttrNoPressOffset = 0x1800;
inline constexpr uint32_t kAttrRadio = 0x12;
inline constexpr uint32_t kAttrTab = 0x10;
inline constexpr uint32_t kAttrLatched = 0x40;
inline constexpr uint32_t kAttrBuildPic = 0x20;

struct Gadget {
    Type type = Type::None9;
    int index = 0;                 // GADGETn
    int assoc = 0;                 // radio group
    std::string name;              // uppercased as authored (lookup is case-insensitive)
    int x = 0, y = 0, w = 0, h = 0; // child coordinates relative to the container
    uint32_t attribs = 0;
    uint32_t colorf = 0, colorb = 0;
    int textureNumber = 0, fontNumber = 0;
    bool active = true;
    int commonAttribs = 0;
    std::string help;
    // Button
    int status = 0, stages = 0;
    std::string text;              // label; stage buttons split it on '|'
    int quickKey = 0;              // ASCII (upper-cased letter) or a key code (13 Enter, 27 Esc)
    bool grayedOut = false;
    // ListBox
    int itemHeight = 0;
    // TextField
    int maxChars = 0;
    // ScrollBar
    int range = 0, thick = 0, knobPos = 0, knobSize = 0;
    // Label
    std::string link;
    // Surface
    bool hotOrNot = false;
    // Font / Picture
    std::string filename;
    // Container
    int totalGadgets = 0;
    std::string panel, crDefault, escDefault, defaultFocus;
    std::vector<std::string> stageLabels() const;
};

struct Screen {
    std::string name;               // file base name (upper-case), e.g. "MAINMENU"
    std::vector<Gadget> gadgets;    // gadgets[0] is the container
    const Gadget* find(const std::string& name) const;
    Gadget* find(const std::string& name);
    const Gadget& container() const { return gadgets.front(); }
    // Enter default: crdefault, else the first button named OK* or NEXT* (0x4aacd2).
    std::string enterDefault() const;
};

// Parse a .GUI text. Throws on a file with no GADGET0.
Screen parse(const std::string& text, const std::string& baseName);
// Load guis\<name>.GUI from the VFS ("" extension or any case accepted).
Screen load(const hpi::Vfs& vfs, const std::string& name);
bool exists(const hpi::Vfs& vfs, const std::string& name);

}  // namespace ta::tagui
