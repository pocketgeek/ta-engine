#pragma once
#include "gaf/gaf.h"
#include "hpi/hpi.h"
#include <stdexcept>

namespace tak::gaf {
inline std::vector<uint16_t> animationTiming(const hpi::Vfs& vfs, const std::string& name) {
    if (name.empty()) return {};
    for (const char* suffix : {"_4444.taf", "_1555.taf", ".taf", ".gaf"}) {
        const auto path = "anims/" + name + suffix;
        if (const auto bytes = vfs.tryRead(path)) try {
            for (const auto& sequence : load(*bytes, Palette{}, -1, path))
                if (hpi::MountSet::key(sequence.name) == name && !sequence.frames.empty()) {
                    std::vector<uint16_t> result;
                    for (const auto& frame : sequence.frames) result.push_back(frame.retailDelayTicks);
                    return result;
                }
        } catch (const std::exception&) {}
    }
    return {};
}
} // namespace tak::gaf
