#pragma once

// Cartographer: New-map creation + minimap generation (docs/cartographer-port.md).

#include "gaf/gaf.h"     // ta::gaf::Palette
#include "tnt/tnt.h"

#include <string>

namespace ta::hpi { class Vfs; }
namespace ta::terrain { class Compositor; }
namespace cart { class SectionLibrary; }

namespace cart {

// The world's 256-colour terrain palette (palettes/<world>.pcx), used to index
// the minimaps. Falls back to a grayscale ramp if the PCX is absent.
ta::gaf::Palette loadWorldPalette(const ta::hpi::Vfs& vfs, const std::string& world);

// (Re)generate a map's small (126x126) and large overview minimaps from its
// tiles: each 32px block is averaged to one colour, the grid is resampled to the
// target size, and each pixel is matched to its nearest palette index. Retail's
// exact downsample/overview-dims are inferred, so this is a functional (loadable,
// visually faithful) minimap, not a byte-exact retail reproduction.
void generateMinimaps(ta::tnt::Map& map, ta::terrain::Compositor& comp,
                      const ta::gaf::Palette& pal);

// Build a fresh flat map of wUnits x hUnits (1 Unit = 32 cells = 512px), tiled
// with a default ground section for `world`, sea level from that side's
// waterheight, then minimaps generated. Returns an empty (width==0) map if no
// section is available.
ta::tnt::Map newBlankMap(const ta::hpi::Vfs& vfs, SectionLibrary& sections,
                          ta::terrain::Compositor& comp, const std::string& world,
                          int wUnits, int hUnits);

// Resize `map` to wUnits x hUnits (1 Unit = 32 cells), keeping the overlapping
// top-left region and filling any new area with the map's first tile + flat
// land, then regenerating the minimaps.
void resizeMap(ta::tnt::Map& map, ta::terrain::Compositor& comp,
               const ta::gaf::Palette& pal, int wUnits, int hUnits);

} // namespace cart
