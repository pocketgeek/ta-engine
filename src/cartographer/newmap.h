#pragma once

// Cartographer: New-map creation + minimap generation (docs/cartographer-port.md).

#include "gaf/gaf.h"     // ta::gaf::Palette
#include "tnt/tnt.h"
#include "terrain/minimap.h"

#include <string>
#include <atomic>
#include <memory>

namespace ta::hpi { class Vfs; }
namespace ta::terrain { class Compositor; }
namespace cart { class SectionLibrary; }

namespace cart {

// The minimap builders live in the shared library (terrain/minimap.h): the client's
// saved random maps get the same radar pictures.
using ta::terrain::loadWorldPalette;
using ta::terrain::generateMinimaps;
using ta::terrain::MinimapPreview;
using ta::terrain::minimapPreview;

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
