#pragma once

#include "gaf/gaf.h"
#include "hpi/hpi.h"
#include "tdf/tdf.h"
#include <map>

namespace tak::gaf {

// Nimbus art availability affects native projectile launch timing. Share this
// resolution between simulation, display and the multiplayer data checksum.
inline std::map<std::string, std::string> factionNimbus(const hpi::Vfs& vfs) {
    std::map<std::string, std::string> result;
    const auto data = vfs.tryRead("gamedata/sidedata.tdf");
    if (!data) return result;
    const auto sides = tdf::parseText(std::string(data->begin(), data->end()), "gamedata/sidedata.tdf");
    for (const auto& [key, side] : sides.children) {
        const auto name = hpi::MountSet::key(side.valueOr("nimbus", ""));
        bool present = false;
        if (!name.empty()) for (const char* suffix : {"_4444.taf", "_1555.taf", ".taf", ".gaf"}) {
            const auto path = "anims/" + name + suffix;
            if (const auto bytes = vfs.tryRead(path)) {
                try {
                    for (const auto& sequence : load(*bytes, Palette{}, -1, path))
                        if (hpi::MountSet::key(sequence.name) == name && !sequence.frames.empty())
                            present = true;
                } catch (const std::exception&) {}
                if (present) break;
            }
        }
        for (const char* field : {"name", "nameprefix"}) {
            const auto alias = hpi::MountSet::key(side.valueOr(field, ""));
            if (!alias.empty()) result[alias] = present ? name : std::string();
        }
    }
    return result;
}

} // namespace tak::gaf
