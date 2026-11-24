#pragma once

// A map's radar pictures (the .tnt's small and large minimaps), built from its tiles.
// Shared by the Cartographer and the client's saved random maps (net/mappackage.cpp).

#include "gaf/gaf.h"     // ta::gaf::Palette
#include "tnt/tnt.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace ta::hpi { class Vfs; }

namespace ta::terrain {

class Compositor;

// The world's 256-colour terrain palette (palettes/<world>.pcx), used to index
// the minimaps. Falls back to a grayscale ramp if the PCX is absent.
ta::gaf::Palette loadWorldPalette(const ta::hpi::Vfs& vfs, const std::string& world);

// (Re)generate a map's small (126x126) and large overview minimaps from its
// tiles: each 32px block is averaged to one colour, the grid is resampled to the
// target size, and each pixel is matched to its nearest palette index. Retail's
// exact downsample/overview-dims are inferred, so this is a functional (loadable,
// visually faithful) minimap, not a byte-exact retail reproduction.
void generateMinimaps(ta::tnt::Map& map, ta::terrain::Compositor& comp,
                      const ta::gaf::Palette& pal, std::shared_ptr<const std::atomic_bool> stop={});

// A TA map's radar picture: kTaMinimap square, the map aspect-fitted from the
// top-left (taMinimapFit gives the fitted part) and the rest padded, as in the
// shipped .tnt files.
constexpr int kTaMinimap = 252;
struct MinimapFit {int width=0,height=0;};
MinimapFit taMinimapFit(int blocksX, int blocksY);
// The part of `map`'s minimap that shows the map: the fitted corner of a TA
// minimap, else the whole picture.
MinimapFit minimapContent(const ta::tnt::Map& map);

// The live preview matches what generateMinimaps saves: 252x252 for a map with its
// own tiles (TA), Kingdoms' 126x126 otherwise. contentW/H: the part showing the map.
struct MinimapPreview {int width=126,height=126,contentW=126,contentH=126;std::vector<uint8_t> rgba;};
// Display-only preview; does not alter document minimap bytes or undo history.
MinimapPreview minimapPreview(const ta::tnt::Map& map,ta::terrain::Compositor& comp,
                              const ta::gaf::Palette& palette,std::shared_ptr<const std::atomic_bool> stop={});

} // namespace ta::terrain
