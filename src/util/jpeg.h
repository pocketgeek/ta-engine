#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace tak::jpeg {

struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;   // width*height*4
};

// Decode from an in-memory buffer (a VFS-resolved archive entry).
Image load(const std::vector<uint8_t>& d);

// Encode an RGBA8 image (width*height*4 bytes, top row first; alpha is dropped) as a
// baseline JPEG, in memory. `quality` is libjpeg's 1..100. Colour keeps its full
// resolution (4:4:4, no chroma subsampling): what is saved this way is a screen of small
// coloured text on a dark panel, and halving the colour planes (4:2:0) smears a fringe
// around every letter. Throws std::runtime_error on a bad size or an encoder failure.
std::vector<uint8_t> encode(int width, int height, const std::vector<uint8_t>& rgba, int quality);

} // namespace tak::jpeg
