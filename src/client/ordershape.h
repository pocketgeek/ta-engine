#pragma once
// orderShape(World): a one-line summary of the movers' order queues. Used to judge a harvested situation against its
// recording (src/client/situation.h logs it at the snapshot and +300; legion_scenario prints it with TAK_ORDER_STATS).
#include "sim/sim.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace tak::situation {
// A one-line shape of the world's order queues, for judging a situation against its recording (TAK_ORDER_STATS in
// legion_scenario prints the same line for the rebuilt world): bodies by queue length and by queued player goals, and
// how many carry an attack target, an attack-move or a patrol.
inline std::string orderShape(const tak::sim::World& w) {
    int n = 0, moving = 0, len[5] = {}, goals[4] = {}, atk = 0, amove = 0, patrol = 0, guard = 0;
    for (const auto& u : w.units()) {
        if (!u.alive() || !u.type || u.type->maxVel <= tak::sim::Fixed()) continue;
        ++n;
        if (u.speed.v) ++moving;
        ++len[std::min<size_t>(u.orders.size() == 0 ? 0 : u.orders.size() == 1 ? 1 : u.orders.size() <= 3 ? 2 : u.orders.size() <= 7 ? 3 : 4, 4)];
        int g = 0; bool a = false, m = false, p = false, gd = false;
        for (const auto& o : u.orders) {
            g += o.goal;
            a |= o.targetId && !o.load && !o.guard; m |= o.attackMove; p |= o.patrol; gd |= o.guard;
        }
        ++goals[std::min(g, 3)];
        atk += a; amove += m; patrol += p; guard += gd;
    }
    char buf[256];
    std::snprintf(buf, sizeof buf, "movers %d moving %d qlen 0:%d 1:%d 2-3:%d 4-7:%d 8+:%d goals 0:%d 1:%d 2:%d 3+:%d attack %d amove %d patrol %d guard %d",
                  n, moving, len[0], len[1], len[2], len[3], len[4], goals[0], goals[1], goals[2], goals[3], atk, amove, patrol, guard);
    return buf;
}


}  // namespace tak::situation
