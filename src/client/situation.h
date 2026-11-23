#pragma once
// TAK_SITUATION=<tick>:<path> -- a debug-only replay hook (src/client/dev.h) that cuts a
// "situation" out of a recording: the world as it stood at <tick>, the next 600 ticks of
// the human commands, and where every body was 300 ticks in, written as a .scn file
// (tools/scenarios/README.scn-format.txt) that tools/legion_scenario runs on any later
// build. It replaces the replay tick-switch A/Bs: a replay only plays back exactly on the
// build that recorded it, a situation runs on whatever the head is.
//
// Header-only and self-contained on purpose: the moments of interest sit in replays
// recorded by older builds (legion-r9 on 02aa55a, legion-r10 on 7aae704) that replay
// exactly only there, so the harvest copies this one header into those trees. It reads the
// World and the recorded bundles and nothing else; it changes no simulation state.
//
// What a situation holds, and what it cannot:
//   * every live body that is not inside a transport or still under construction: type
//     (the FBI name), owner, exact position, heading, hit points and control squad;
//   * the orders those bodies were carrying at the snapshot, re-issued at tick 0 as one
//     click per distinct destination (an army marching at the snapshot keeps marching);
//   * the recorded commands of the next 600 ticks, re-clustered into clicks. The scn runner
//     replays a click through the client's HUD split and the 64-per-tick uplink, so a
//     selection of 300 lands over the same five ticks it did.
//   Not held: economy, scripts, AI, fog (the file explores the map), production and
//   anything built or killed outside the window's commands.
#include "net/lockstep.h"
#include "client/ordershape.h"
#include "net/protocol.h"
#include "sim/sim.h"
#include "util/devenv.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <set>
#include <tuple>
#include <string>
#include <utility>
#include <vector>

namespace tak::situation {

inline constexpr uint32_t kCommandWindow = 600;   // ticks of commands kept (TAK_SITUATION_CMDS=N overrides: timing runs
                                                  // that simulate more than 600 ticks keep the recording's commands for all of them)
inline uint32_t commandWindow() {
    if (const char* e = tak::devEnv("TAK_SITUATION_CMDS")) if (const unsigned long n = std::strtoul(e, nullptr, 10)) return uint32_t(n);
    return kCommandWindow;
}
inline constexpr uint32_t kTruthTicks[] = {100, 300};   // where the recording's positions are sampled (the last ends the harvest)

struct Request {
    uint32_t tick = 0;
    std::string path;
    bool valid = false;
};

// "<tick>:<path>" (the path may itself hold ':' on Windows).
inline Request parseRequest(const char* text) {
    Request r;
    if (!text || !*text) return r;
    const std::string s = text;
    const auto colon = s.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size()) return r;
    char* end = nullptr;
    const unsigned long t = std::strtoul(s.c_str(), &end, 10);
    if (end != s.c_str() + colon) return r;
    r.tick = uint32_t(t);
    r.path = s.substr(colon + 1);
    r.valid = true;
    return r;
}


struct Meta {
    std::string mapId;      // a ~gen1~ recipe; any other map cannot be written (see write)
    std::string source;     // free text for the file's header comment
    bool crusades = false;
    int players = 2;
    uint32_t seed = 1;
};

class Harvester {
public:
    // TAK_SITUATION_SHAPES=N: after the cut, keep logging the order shape every 100 ticks up to +N (judging how fast a rebuilt
    // situation converges on the recording's queues).
    void arm(const Request& r, const Meta& m) {
        req_ = r; meta_ = m; armed_ = r.valid;
        if (const char* e = tak::devEnv("TAK_SITUATION_SHAPES")) shapeEnd_ = r.valid ? uint32_t(std::strtoul(e, nullptr, 10)) : 0;
    }
    bool armed() const { return armed_ || shapeEnd_ != 0; }

    // Call with the world as it stands after `tick` ticks, before applying bundle `tick`.
    // Returns true when the file was written (once).
    bool observe(uint32_t tick, const tak::sim::World& w, const std::vector<tak::net::Bundle>& bundles) {
        if (shapeEnd_ && captured_ && tick > req_.tick) {
            const uint32_t k = tick - req_.tick;
            if (k % 100 == 0) std::fprintf(stderr, "situation: order shape at +%u: %s\n", k, orderShape(w).c_str());
            if (k >= shapeEnd_) shapeEnd_ = 0;
        }
        if (!armed_) return false;
        if (tick == req_.tick && !captured_) capture(w, bundles);
        if (captured_) {
            for (uint32_t k : kTruthTicks)
                if (tick == req_.tick + k) {   // the recording's own positions k ticks in, in file (id) order
                    if (!shapeEnd_) std::fprintf(stderr, "situation: order shape at +%u: %s\n", k, orderShape(w).c_str());
                    std::vector<std::pair<int32_t, int32_t>> pos;
                    std::vector<int> ids;
                    for (const Body& b : bodies_) ids.push_back(b.id);
                    std::sort(ids.begin(), ids.end());
                    for (int id : ids) {
                        const auto* u = w.unit(id);
                        pos.push_back(u && u->alive() ? std::pair<int32_t, int32_t>{u->x.v, u->z.v}
                                                      : std::pair<int32_t, int32_t>{INT32_MIN, INT32_MIN});
                    }
                    truths_.push_back({k, std::move(pos)});
                }
            if (tick == req_.tick + kTruthTicks[std::size(kTruthTicks) - 1]) {
                armed_ = false;
                return write();
            }
        }
        return false;
    }

private:
    struct Body {
        int id = 0, owner = 0, squad = 0;
        std::string type;
        int32_t x = 0, z = 0, heading = 0, hp = 0, speed = 0, stand = 0, vel = 0;
        bool moves = false;
    };
    enum class V { Move, Fight, Patrol, Attack, Guard, Stop, Squad };
    struct Click {
        uint32_t tick = 0;
        V verb = V::Move;
        int owner = 0, squad = 0, target = 0;
        bool queue = false;
        float x = 0, z = 0;
        std::vector<int> units;   // ids
    };

    Request req_;
    Meta meta_;
    bool armed_ = false, captured_ = false;
    uint32_t shapeEnd_ = 0;
    std::vector<Body> bodies_;
    std::vector<Click> clicks_;
    std::vector<std::pair<uint32_t, std::vector<std::pair<int32_t, int32_t>>>> truths_;
    uint32_t clockTick_ = 0, clockRng_ = 0;
    std::vector<int> teams_;   // the recording's team of every player
    std::vector<uint16_t> explored_;   // the recording's navigation exploration masks at the snapshot

    static uint64_t pointKey(float x, float z) {
        uint32_t a, b;
        std::memcpy(&a, &x, 4);
        std::memcpy(&b, &z, 4);
        return (uint64_t(a) << 32) | b;
    }

    void capture(const tak::sim::World& w, const std::vector<tak::net::Bundle>& bundles) {
        captured_ = true;
        std::fprintf(stderr, "situation: order shape at tick %u: %s\n", req_.tick, orderShape(w).c_str());
        explored_ = w.navigationExploration();
        clockTick_ = w.tickCount();
        clockRng_ = w.gameRngState();
        teams_.clear();
        for (int p = 0; p < w.numPlayers(); ++p) teams_.push_back(w.player(p).team);
        std::set<int> inSnapshot;
        for (const auto& u : w.units()) {
            if (!u.alive() || !u.type || u.inTransport || u.underConstruction) continue;
            Body b;
            b.id = u.id;
            b.owner = u.player;
            b.squad = u.squad;
            b.type = u.type->id;
            b.x = u.x.v; b.z = u.z.v; b.heading = u.heading.v; b.hp = u.hp.v; b.speed = u.baseSpeed.v; b.vel = u.speed.v;
            // Stance and standing orders: only when the player changed them from the type's defaults.
            if (u.standingOrder != u.type->defaultStandingOrder || u.moveState != u.type->defaultMove ||
                u.fireState != u.type->defaultFire)
                b.stand = int32_t(uint32_t(u.standingOrder) | uint32_t(u.moveState) << 8 | uint32_t(u.fireState) << 16 |
                                  uint32_t(u.stance + 1) << 24);
            b.moves = u.type->maxVel > tak::sim::Fixed();
            bodies_.push_back(b);
            inSnapshot.insert(u.id);
        }
        // Orders in flight at the snapshot: one tick-0 click per (owner, verb, destination).
        {
            std::map<std::tuple<int, int, uint64_t, int>, Click> pend;
            for (const auto& u : w.units()) {
                if (!inSnapshot.count(u.id) || u.orders.empty() || !u.type || u.type->maxVel <= tak::sim::Fixed()) continue;
                const tak::sim::Order* last = &u.orders.back();
                for (const auto& o : u.orders) if (o.goal) last = &o;
                if (last->buildType) continue;
                Click c;
                c.owner = u.player;
                if (last->guard && last->targetId) { c.verb = V::Guard; c.target = last->targetId; }
                else if (last->targetId && !last->load) { c.verb = V::Attack; c.target = last->targetId; }
                else if (last->patrol) { c.verb = V::Patrol; c.x = last->x.toFloat(); c.z = last->z.toFloat(); }
                else if (last->attackMove) { c.verb = V::Fight; c.x = last->x.toFloat(); c.z = last->z.toFloat(); }
                else { c.verb = V::Move; c.x = last->x.toFloat(); c.z = last->z.toFloat(); }
                if (c.target && !inSnapshot.count(c.target)) continue;
                const auto key = std::make_tuple(c.owner, int(c.verb), pointKey(c.x, c.z), c.target);
                auto it = pend.try_emplace(key, c).first;
                it->second.units.push_back(u.id);
            }
            for (auto& [k, c] : pend) clicks_.push_back(std::move(c));
        }
        // The recorded commands, clustered into clicks.
        struct Open { Click c; std::vector<std::pair<float, float>> pts; uint32_t last = 0; };
        std::map<std::tuple<int, int, int, int>, Open> open;   // (owner, verb, target-or-squad, queue)
        auto flush = [&](Open& o) {
            if (o.c.units.empty()) return;
            if (o.c.verb == V::Move || o.c.verb == V::Fight || o.c.verb == V::Patrol) {
                // The shared point if most units share one (Legion surface movers), else the centroid
                // of the offset targets (offsets from the click: ground clamped +-60 px, flyers retail's spread).
                std::map<uint64_t, int> count;
                for (auto& p : o.pts) ++count[pointKey(p.first, p.second)];
                uint64_t best = 0; int bestN = 0;
                for (auto& [k, n] : count) if (n > bestN) { bestN = n; best = k; }
                if (bestN * 2 >= int(o.pts.size())) {
                    for (auto& p : o.pts) if (pointKey(p.first, p.second) == best) { o.c.x = p.first; o.c.z = p.second; break; }
                } else {
                    double sx = 0, sz = 0;
                    for (auto& p : o.pts) { sx += p.first; sz += p.second; }
                    o.c.x = float(sx / double(o.pts.size())); o.c.z = float(sz / double(o.pts.size()));
                }
            }
            clicks_.push_back(std::move(o.c));
        };
        for (uint32_t k = 0, win = commandWindow(); k < win && req_.tick + k < bundles.size(); ++k) {
            // A cluster that has been quiet for two ticks is a finished click.
            for (auto it = open.begin(); it != open.end();) {
                if (it->second.last + 2 < k) { flush(it->second); it = open.erase(it); }
                else ++it;
            }
            for (const auto& c : bundles[req_.tick + k].cmds) {
                V v;
                if (c.kind == tak::net::Cmd::Order) {
                    // An explicit selection order is a click already: its verb, point,
                    // target and exact members, no clustering needed.
                    if (!c.order) continue;
                    const auto& t = c.order->tmpl;
                    switch (t.kind) {
                    case tak::net::Cmd::Move: v = V::Move; break;
                    case tak::net::Cmd::AttackMove: v = V::Fight; break;
                    case tak::net::Cmd::Patrol: v = V::Patrol; break;
                    case tak::net::Cmd::Attack: v = V::Attack; break;
                    case tak::net::Cmd::Guard: v = V::Guard; break;
                    case tak::net::Cmd::Stop: v = V::Stop; break;
                    case tak::net::Cmd::SetSquad: v = V::Squad; break;
                    default: continue;
                    }
                    if (v == V::Squad && t.targetId == 0) continue;   // eviction of the old members
                    if ((v == V::Attack || v == V::Guard) && !inSnapshot.count(t.targetId)) continue;
                    Click click;
                    click.tick = k; click.verb = v; click.owner = c.player; click.queue = t.queue != 0;
                    click.target = (v == V::Attack || v == V::Guard) ? t.targetId : 0;
                    click.squad = v == V::Squad ? t.targetId : 0;
                    click.x = t.x; click.z = t.z;
                    for (const int32_t id : c.order->members) if (inSnapshot.count(id)) click.units.push_back(id);
                    if (!click.units.empty()) clicks_.push_back(std::move(click));
                    continue;
                }
                switch (c.kind) {
                case tak::net::Cmd::Move: v = V::Move; break;
                case tak::net::Cmd::AttackMove: v = V::Fight; break;
                case tak::net::Cmd::Patrol: v = V::Patrol; break;
                case tak::net::Cmd::Attack: v = V::Attack; break;
                case tak::net::Cmd::Guard: v = V::Guard; break;
                case tak::net::Cmd::Stop: v = V::Stop; break;
                case tak::net::Cmd::SetSquad: v = V::Squad; break;
                default: continue;
                }
                if (!inSnapshot.count(c.unitId)) continue;
                if (v == V::Squad && c.targetId == 0) continue;   // eviction of the old members
                const int sel = v == V::Squad ? c.targetId : (v == V::Attack || v == V::Guard) ? c.targetId : 0;
                if ((v == V::Attack || v == V::Guard) && !inSnapshot.count(c.targetId)) continue;
                const auto key = std::make_tuple(int(c.player), int(v), sel, int(c.queue != 0));
                auto it = open.find(key);
                if (it == open.end()) {
                    Open o;
                    o.c.tick = k; o.c.verb = v; o.c.owner = c.player; o.c.queue = c.queue != 0;
                    o.c.target = (v == V::Attack || v == V::Guard) ? c.targetId : 0;
                    o.c.squad = v == V::Squad ? c.targetId : 0;
                    it = open.emplace(key, std::move(o)).first;
                }
                it->second.c.units.push_back(c.unitId);
                it->second.pts.push_back({c.x, c.z});
                it->second.last = k;
            }
        }
        for (auto& [key, o] : open) flush(o);
        std::stable_sort(clicks_.begin(), clicks_.end(), [](const Click& a, const Click& b) { return a.tick < b.tick; });
    }

    bool write() const {
        // A ~gen1~ recipe names itself; any other map is named and resolved through the install (data-gated, like the fbi types).
        const bool gen1 = meta_.mapId.rfind("~gen1~", 0) == 0;
        if (!gen1 && (meta_.mapId.empty() || meta_.mapId.find('\n') != std::string::npos)) {
            std::fprintf(stderr, "situation: map '%s' cannot be named in a .scn\n", meta_.mapId.c_str());
            return false;
        }
        // Bodies go out in the recording's unit-id order, because spawn order is id order in the new world and
        // plenty of the sim breaks ties by it. A group is a maximal run of consecutive bodies of one class
        // (owner, type, squad); an attack or guard target is a group of its own, so `@GROUP` names it exactly.
        std::vector<Body> bodies = bodies_;
        std::stable_sort(bodies.begin(), bodies.end(), [](const Body& a, const Body& b) { return a.id < b.id; });
        std::set<int> targets;
        for (const auto& c : clicks_) if (c.target) targets.insert(c.target);
        std::map<int, size_t> index;   // body id -> its number in the file
        for (size_t i = 0; i < bodies.size(); ++i) index[bodies[i].id] = i;
        struct Run { size_t first = 0, count = 0; std::string name; };
        std::vector<Run> runs;
        std::map<std::string, int> serial;
        std::set<std::string> types;
        std::vector<size_t> runOf(bodies.size(), 0);
        for (size_t i = 0; i < bodies.size(); ++i) {
            const Body& b = bodies[i];
            const bool same = i > 0 && !targets.count(b.id) && !targets.count(bodies[i - 1].id) &&
                              bodies[i - 1].owner == b.owner && bodies[i - 1].type == b.type && bodies[i - 1].squad == b.squad;
            if (!same) {
                const std::string base = "p" + std::to_string(b.owner) + "_" + b.type;
                runs.push_back({i, 0, base + "_" + std::to_string(serial[base]++)});
                types.insert(b.type);
            }
            ++runs.back().count;
            runOf[i] = runs.size() - 1;
        }
        std::FILE* f = std::fopen(req_.path.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "situation: cannot write %s\n", req_.path.c_str()); return false; }
        std::string name = req_.path;
        if (auto s = name.find_last_of("/\\"); s != std::string::npos) name = name.substr(s + 1);
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".scn") == 0) name.resize(name.size() - 4);
        std::fprintf(f, "scn 1\nname %s\n", name.c_str());
        std::fprintf(f, "# Situation cut by TAK_SITUATION (src/client/situation.h) from %s,\n", meta_.source.c_str());
        std::fprintf(f, "# the world at tick %u of the recording and the next %u ticks of its commands.\n",
                     req_.tick, commandWindow());
        std::fprintf(f, "# Bodies are exact (raw 16.16 px, raw BAM, raw hp, raw individual speed); `truth` holds the recording's own\n"
                        "# positions at +%u and +%u ticks, in group order.\n", kTruthTicks[0], kTruthTicks[std::size(kTruthTicks) - 1]);
        std::fprintf(f, "ticks 3000\nseed %u\nplayers %d\nweapons on\n", meta_.seed, meta_.players);
        for (size_t p = 0; p < teams_.size() && int(p) < meta_.players; ++p) std::fprintf(f, "team %zu %d\n", p, teams_[p]);
        if (explored_.empty()) std::fprintf(f, "explored all\n");
        else {   // what the recording's players had explored: pathing treats unexplored ground differently
            std::fprintf(f, "explored rle");
            for (size_t i = 0; i < explored_.size();) {
                size_t j = i;
                while (j < explored_.size() && explored_[j] == explored_[i]) ++j;
                std::fprintf(f, " %zu:%x", j - i, unsigned(explored_[i]));
                i = j;
            }
            std::fprintf(f, "\n");
        }
        if (meta_.crusades) std::fprintf(f, "crusades on\n");
        std::fprintf(f, gen1 ? "map gen1 %s\n" : "map named %s\n", meta_.mapId.c_str());
        for (const auto& t : types) std::fprintf(f, "type %s fbi %s\n", t.c_str(), t.c_str());
        for (const auto& run : runs) {
            const Body& first = bodies[run.first];
            std::fprintf(f, "group %s %d %s %zu spots", run.name.c_str(), first.owner, first.type.c_str(), run.count);
            for (size_t i = run.first; i < run.first + run.count; ++i) {
                const Body& b = bodies[i];
                std::fprintf(f, " %d,%d,%d,%d,%d,%d,%d,%d", b.x, b.z, b.heading & 0xffff, b.hp, b.speed, b.stand, b.id, b.vel);
            }
            if (first.squad) std::fprintf(f, " squad=%c%d", first.squad > 0 ? 'g' : 'f', std::abs(first.squad));
            std::fprintf(f, "\n");
        }
        // Orders: the squad presses are the groups' squad=; clicks follow, each naming its bodies in the order the
        // recording's commands came (the selection order the HUD split walks).
        for (const auto& c : clicks_) {
            std::string list;
            std::set<size_t> seen;
            for (int id : c.units) {
                auto it = index.find(id);
                if (it == index.end() || !seen.insert(it->second).second) continue;
                if (bodies[it->second].owner != c.owner) continue;   // a click is one player's selection (allied control in a recording)
                list += (list.empty() ? "%" : ",%") + std::to_string(it->second);
            }
            if (list.empty()) continue;
            if ((c.verb == V::Attack || c.verb == V::Guard) && !index.count(c.target)) continue;
            const char* verb = "move";
            switch (c.verb) {
            case V::Move: verb = "move"; break;
            case V::Fight: verb = "fight"; break;
            case V::Patrol: verb = "patrol"; break;
            case V::Attack: verb = "attack"; break;
            case V::Guard: verb = "guard"; break;
            case V::Stop: verb = "stop"; break;
            case V::Squad: verb = "squad"; break;
            }
            std::fprintf(f, "at %u %s %s", c.tick, verb, list.c_str());
            if (c.verb == V::Move || c.verb == V::Fight || c.verb == V::Patrol)
                std::fprintf(f, " %.9g %.9g", double(c.x / 16.f), double(c.z / 16.f));
            else if (c.verb == V::Attack || c.verb == V::Guard)
                std::fprintf(f, " @%s", runs[runOf[index.at(c.target)]].name.c_str());
            else if (c.verb == V::Squad)
                std::fprintf(f, " %c%d", c.squad > 0 ? 'g' : 'f', std::abs(c.squad));
            if (c.queue) std::fprintf(f, " queue");
            std::fprintf(f, "\n");
        }
        std::fprintf(f, "clock %u %u\n", clockTick_, clockRng_);
        for (const auto& [tick, pos] : truths_) {
            std::fprintf(f, "truth %u", tick);
            for (const auto& [x, z] : pos) std::fprintf(f, " %d,%d", x, z);
            std::fprintf(f, "\n");
        }
        std::fclose(f);
        std::fprintf(stderr, "situation: wrote %s (%zu bodies, %zu groups, %zu clicks)\n", req_.path.c_str(),
                     bodies.size(), runs.size(), clicks_.size());
        return true;
    }
};

}  // namespace tak::situation
