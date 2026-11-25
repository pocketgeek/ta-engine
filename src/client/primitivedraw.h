#pragma once

// How TotalA.exe draws the primitives of one 3DO piece (client side only).
//
// Order. Every objects3d model is loaded through 0x4cb560 -> 0x4cb4c0, which
// ends in 0x4cb370 (call at 0x4cb552) for each piece: when the piece has a
// selection primitive (+0xc != -1) and any primitives, that primitive is
// swapped with primitive 0 and +0xc becomes 0 (0x4cb37b..0x4cb3b7); then
// primitives 1..n-1 are bubble-sorted, a pair swapped only when the first's
// average vertex y is GREATER (0x4cb3e8..0x4cb4a5: raw 16.16 y, summed as an
// int32 and divided by the vertex count with idiv). A strict-greater bubble
// sort is a stable ascending sort. The draw loops then start at primitive 1
// whenever +0xc != -1 (0x459aa5 object bitmap, 0x45908a nanoframe outline,
// 0x458568 direct draw) -- that is the selection primitive, moved to 0 at
// load, so no textured face is lost and the selection quad is never drawn.
//
// Colour. 0x42a140 resolves each primitive's texture name: no name keeps the
// file's flag word (+0x1c); every shipped untextured primitive has bit 0 set
// there (6066 of 6066 over objects3d). A name found nowhere becomes colour
// 0xd1 with bit 0 (0x42a204..0x42a20f). Bit 0 draws a flat polygon of any
// vertex count in the primitive's palette index (0x459b9d -> 0x4c1000,
// 0x45866b -> 0x4c0310; the span writer 0x4c06e0 stores the raw index under
// the height test, unshaded). A textured primitive is drawn only as a quad
// (0x459b1a, 0x4585e2: vertex count != 4 is skipped); the nanoframe outline
// (0x45908a -> 0x4c0820) outlines every drawn-order primitive regardless.
// The object bitmap's transparent key is 1, not 0 (0x4598ae, read back by
// 0x458d30 at +8), so palette index 0 is opaque: TA's palette entry 0, black.

#include "tdo/tdo.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ta::primitivedraw {

constexpr int kMissingTextureColour = 0xd1;   // 0x42a207
constexpr std::string_view kFlatPrefix = "#pal";

// The texture-table name of the flat swatch for palette index `index`.
inline std::string flatTextureName(int index) {
    return std::string(kFlatPrefix) + std::to_string(index & 0xff);
}
// The palette index of a flat swatch name, or -1 for a real texture.
inline int flatColourOf(std::string_view name) {
    if (!name.starts_with(kFlatPrefix)) return -1;
    int value = -1;
    const auto digits = name.substr(kFlatPrefix.size());
    const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    return result.ec == std::errc() && result.ptr == digits.data() + digits.size() && value >= 0 && value < 256
        ? value : -1;
}

// Average raw y of a primitive as 0x4cb3f9..0x4cb40c computes it (int32 sum,
// truncating division; a vertex-less primitive, which would fault there, is 0).
inline int32_t averageY(const ta::tdo::Object& object, const ta::tdo::Primitive& primitive) {
    if (primitive.indices.empty()) return 0;
    uint32_t sum = 0;
    for (uint16_t index : primitive.indices)
        if (index < object.verticesRaw.size()) sum += uint32_t(object.verticesRaw[index][1]);
    return int32_t(sum) / int32_t(primitive.indices.size());
}

// The file indices of the primitives retail draws, in its order (the
// selection primitive left out).
inline std::vector<uint16_t> drawOrder(const ta::tdo::Object& object) {
    const size_t n = object.primitives.size();
    std::vector<uint16_t> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = uint16_t(i);
    const int32_t selection = object.selectionPrimitive;
    const bool hasSelection = selection != -1 && n > 0;
    if (hasSelection && size_t(selection) < n) std::swap(order[0], order[size_t(selection)]);
    if (n > 2) {
        std::vector<int32_t> average(n);
        for (size_t i = 0; i < n; ++i) average[i] = averageY(object, object.primitives[i]);
        std::stable_sort(order.begin() + 1, order.end(),
                         [&](uint16_t a, uint16_t b) { return average[a] < average[b]; });
    }
    if (hasSelection) order.erase(order.begin());
    return order;
}

} // namespace ta::primitivedraw
