#pragma once

#include "sim/fixed.h"

namespace tak::sim {

// Retail 0x4daf1c: floor((position - (size-1)*8) / 16).
// Keep the subtraction in 64 bits and retain subpixel precision at boundaries.
constexpr int footprintOrigin(Fixed position, int size) {
    return int((int64_t(position.v) - int64_t(size - 1) * 8 * Fixed::kOne) >> 20);
}

// NavGrid stores centre-indexed footprints (origin = cell - size/2).
// Even footprints therefore change nav cells at x=8 mod 16, not x=0 mod 16.
constexpr int footprintCell(Fixed position, int size) {
    return footprintOrigin(position, size) + size / 2;
}

constexpr Fixed footprintWaypoint(int cell, int size) {
    return Fixed::fromInt(cell * 16 + (size % 2 ? 8 : 0));
}

// Retail 0x5085c0 on a building (bmcode other than 1): the footprint-origin
// cell's corner plus half the footprint, so an odd footprint sits on a cell
// centre and an even one on a cell edge. Mobile units are not snapped.
constexpr Fixed footprintSnap(Fixed position, int size) {
    return Fixed::fromInt(footprintOrigin(position, size) * 16 + size * 8);
}

inline int footprintOrigin(float position, int size) {
    return footprintOrigin(Fixed::fromFloat(position), size);
}

inline int footprintCell(float position, int size) {
    return footprintCell(Fixed::fromFloat(position), size);
}

// Refused placement, 0x4db010 (repeated) / 0x4db06f (first). The first
// refusal snaps coordinates outside the inner +/-4px to the old cell's edge;
// subsequent refusals clamp to that edge. Neither changes cell ownership.
constexpr Fixed footprintClamp(Fixed current, Fixed proposed, int size, bool repeated) {
    const int64_t center = footprintWaypoint(footprintCell(current, size), size).v;
    const int64_t limit = (repeated ? 8 : 4) * int64_t(Fixed::kOne) - (repeated ? 1 : 0);
    if (proposed.v > center + limit) return Fixed::raw(int32_t(center + 8 * Fixed::kOne - 1));
    if (proposed.v < center - limit) return Fixed::raw(int32_t(center - 8 * Fixed::kOne + 1));
    return proposed;
}

} // namespace tak::sim
