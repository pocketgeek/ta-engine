#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace ta::jpeg {

struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;   // width*height*4
};

// Decode from an in-memory buffer (a VFS-resolved archive entry).
Image load(const std::vector<uint8_t>& d);

} // namespace ta::jpeg
