#pragma once

// A map's radar pictures (the .tnt's small and large minimaps), built from its tiles.
// Shared by the Cartographer and the client's saved random maps (net/mappackage.cpp).

#include "gaf/gaf.h"     // tak::gaf::Palette
#include "tnt/tnt.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace tak::hpi { class Vfs; }

namespace tak::terrain {

class Compositor;

// The world's 256-colour terrain palette (palettes/<world>.pcx), used to index
// the minimaps. Falls back to a grayscale ramp if the PCX is absent.
tak::gaf::Palette loadWorldPalette(const tak::hpi::Vfs& vfs, const std::string& world);

// (Re)generate a map's small (126x126) and large overview minimaps from its
// tiles: each 32px block is averaged to one colour, the grid is resampled to the
// target size, and each pixel is matched to its nearest palette index. Retail's
// exact downsample/overview-dims are inferred, so this is a functional (loadable,
// visually faithful) minimap, not a byte-exact retail reproduction.
void generateMinimaps(tak::tnt::Map& map, tak::terrain::Compositor& comp,
                      const tak::gaf::Palette& pal, std::shared_ptr<const std::atomic_bool> stop={});

struct MinimapPreview {int width=126,height=126;std::vector<uint8_t> rgba;};
// Display-only preview; does not alter document minimap bytes or undo history.
MinimapPreview minimapPreview(const tak::tnt::Map& map,tak::terrain::Compositor& comp,
                              const tak::gaf::Palette& palette,std::shared_ptr<const std::atomic_bool> stop={});

} // namespace tak::terrain
