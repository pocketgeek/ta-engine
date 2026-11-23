#pragma once

// Procedurally-drawn application icons (original art -- no game assets). Each
// app gets a cohesive rounded-badge emblem: a crown for the client, a compass
// rose for the map editor, a signal keep for the server. Rendered SDL-free to
// an RGBA8888 buffer (R,G,B,A byte order, i.e. SDL_PIXELFORMAT_RGBA32) so the
// client/editor can wrap it in an SDL_Surface for SDL_SetWindowIcon and the
// packaging tool can write it out as a PNG.

#include <cstdint>
#include <vector>

namespace tak::appicon {

enum class Kind { Client, Cartographer, Server };

// size x size RGBA8888 pixels. Supersampled + box-downsampled for clean edges.
std::vector<uint8_t> render(Kind kind, int size);

} // namespace tak::appicon
