#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace ta::png {

// Write an RGBA8 image as a PNG (zlib-compressed, no interlace).
void write(const std::filesystem::path& file, int width, int height,
           const std::vector<uint8_t>& rgba);

} // namespace ta::png
