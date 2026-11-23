#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace tak::png {

// Write an RGBA8 image as a PNG (zlib-compressed, no interlace).
void write(const std::filesystem::path& file, int width, int height,
           const std::vector<uint8_t>& rgba);

// Write an RGB8 image (width*height*3 bytes) as a PNG, compressing as it goes: the
// memory it needs beyond the image is a few megabytes, whatever the size -- for
// the whole-map MakePoster, where an RGBA copy of a big map would be gigabytes.
void writeRgb(const std::filesystem::path& file, int width, int height,
              const std::vector<uint8_t>& rgb);

} // namespace tak::png
