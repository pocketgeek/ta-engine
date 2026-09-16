#pragma once
// 8-bit PCX reader.
//
// TA's unit portraits are `unitpics/<UNITNAME>.PCX` -- 282 of them, the
// hand-drawn pictures retail shows on every build button. Kingdoms instead kept
// them as `anims/buildpic/<id>.jpg`, which is what the client asked for, and a
// TA install has no such directory at all: every lookup missed and the build
// menu fell back to rendering the unit's 3DO as an icon.
//
// Only the shape TA actually ships is handled: version 5, RLE-encoded, one
// plane of 8 bits, with the 256-colour VGA palette appended after the pixels.
// Anything else returns an empty image rather than guessing.

#include <cstdint>
#include <string>
#include <vector>

namespace ta::pcx {

struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;   // width*height*4
    bool ok() const { return width > 0 && height > 0 && !rgba.empty(); }
};

// Decode an 8-bit PCX. `origin` only names the source in error messages.
// Returns an empty Image on anything unexpected.
Image load(const std::vector<uint8_t>& bytes, const std::string& origin = "<memory>");

}  // namespace ta::pcx
