#pragma once

// Lobby start markers: where each of a map's start positions sits on a preview, and
// which room slot opens there (so the marker can wear that slot's colour). SDL-free so
// start_markers_test can pin both.
//
// Who-goes-where is NOT re-derived here: the room's slot table goes through the same
// matchConfigFor the referee and every client build the match with, and the spot each
// slot takes comes from sim::startSpotsFor, the rule setupMatch itself indexes with. A
// change to either moves the markers with it.

#include <algorithm>
#include <utility>
#include <vector>

#include "net/matchconfig.h"
#include "net/protocol.h"
#include "sim/matchsetup.h"

namespace tak::startmarkers {

// The room slot that opens at each of `starts` start positions (index = start, in the
// .ota StartPosN order parseStartPositions returns; FIXED and BY SLOT both follow
// sim::startSpotsFor), or -1 when no slot will, or nobody
// can know yet: RANDOM shuffles with the match seed, which is only drawn at START, and a
// benchmark spreads its armies on a ring of its own instead of the map's starts.
inline std::vector<int> occupants(const net::GameOptions& o, const net::SlotInfo* slots,
                                  size_t starts) {
    std::vector<int> who(starts, -1);
    if (net::normalizeStartMode(o.randomStarts) == net::kStartsRandom || o.benchmark) return who;
    const sim::MatchConfig cfg = net::matchConfigFor(o, slots, 0);
    const std::vector<int> spot = sim::startSpotsFor(cfg.slots, starts, cfg.startMode);
    for (size_t i = 0; i < spot.size(); ++i)
        if (spot[i] >= 0 && size_t(spot[i]) < starts) who[size_t(spot[i])] = int(i);
    return who;
}

struct Point { float x, y; };

// A start at world pixel `pos` on a worldW x worldH map, drawn over the preview rect
// (x, y, w, h) that shows the whole map. The centre is kept `inset` inside the rect so a
// start on the map edge still shows its whole marker.
inline Point place(std::pair<float, float> pos, float worldW, float worldH,
                   float x, float y, float w, float h, float inset) {
    float px = worldW > 0 ? x + pos.first / worldW * w : x + w / 2;
    float py = worldH > 0 ? y + pos.second / worldH * h : y + h / 2;
    const float ix = std::min(inset, w / 2), iy = std::min(inset, h / 2);
    return {std::clamp(px, x + ix, x + w - ix), std::clamp(py, y + iy, y + h - iy)};
}

}  // namespace tak::startmarkers
