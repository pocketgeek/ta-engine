#pragma once

#include <array>
#include <cstdint>

namespace ta::cob {
struct EmissionPose {
    std::array<int32_t,3> offset{},move{};
    std::array<uint16_t,3> turn{};
};
} // namespace ta::cob
