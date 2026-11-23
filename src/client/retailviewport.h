#pragma once

namespace tak {

// Retail's point admission helper rejects only centers strictly outside the
// viewport rectangle; each edge itself is drawable.
inline bool retailViewportCenterAdmitted(float x, float y, float width, float height) {
    return !(x < 0.0f || x > width || y < 0.0f || y > height);
}

} // namespace tak
