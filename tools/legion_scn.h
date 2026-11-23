#pragma once
// .scn scenario files: parser, canonical writer, world builder and order feed.
// Header-only, tools only; the grammar is in tools/scenarios/README.scn-format.txt.
//
//   auto scn   = tak::scn::load("tools/scenarios/corner.scn");
//   auto built = tak::scn::build(scn, {PathfindingMode::Legion, true, offset, data});
//   if (!built->skipped.empty()) { report it; return; }       // data-gated file
//   tak::scn::OrderFeed feed(scn, *built);
//   for (uint32_t t = 0; t < scn.ticks; ++t) { feed.apply(t); built->world->tick(1.f/30); }
//
// A file never names a pathfinding mode: the runner builds it once per mode.
// The world is a deterministic function of the file, the mode, the install
// (data-gated parts only) and the start offset (0, +1, -1 cells, applied to
// every spawn on both axes). Orders go through issueSelection's HUD mirror and
// uplink, so a 200-body click lands over four ticks as it does in play.
#include "hpi/hpi.h"
#include "legion_issue_selection.h"
#include "net/mappackage.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"
#include "tnt/mapgen.h"

#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <unordered_map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace tak::scn {

constexpr int kFormatVersion = 1;

// ---- the parsed file -------------------------------------------------------

struct TypeSpec {
    enum Kind { Mover, Flyer, Boat, Hover, Structure, Fbi, Roster };
    std::string name;
    Kind kind = Mover;
    int foot = 2, footZ = 0;          // footZ 0 = square
    int turn = 2500, turnInPlace = -1;// -1 = kind default
    float accel = 10, speed = 1.8f;   // px/tick^2 (Fixed), px/tick
    int sight = 4096, hp = 100;
    int depth = 13, cruise = 80;      // boat keel, flyer cruise altitude
    bool hasGun = false;              // explicit gun= (else the default gun)
    int gunRange = 120, gunDamage = 100;
    float gunReload = 0.1f;
    std::string fbi;                  // Fbi: the unit's FBI name
};

// A harvested body (`spots`): exact fixed-point position (raw 16.16 px), heading (raw BAM, 65536 = 360
// degrees), hit points (raw 16.16; 0 = the type's full health), individual speed (raw 16.16 px/tick: the
// +-10% roll every body gets at birth, which decides who leads a column) and stance (0 = the type's defaults, else
// standingOrder | moveState << 8 | fireState << 16 | (stance + 1) << 24: hold position, hold fire and the like) and
// the recorded unit id (0 = next free) and the current speed (raw 16.16 px/tick: a body already on the move).
struct Spot { int32_t x = 0, z = 0, heading = 0, hp = 0, speed = 0, stand = 0, id = 0, vel = 0; };   // speed: raw baseSpeed (0 = the spawn roll)

struct GroupSpec {
    std::string name, type;
    int owner = 0, count = 0;
    bool rect = true;
    std::vector<Spot> spots;                         // non-empty: `spots` form (rect = false, cells empty)
    int x0 = 0, z0 = 0, x1 = 0, z1 = 0, pitch = 0;   // rect: [x0,x1) x [z0,z1) in cells
    std::vector<std::pair<int, int>> cells;          // explicit cells
    int squad = 0;      // tick-0 SetSquad: +N group N (Ctrl+N), -N formation N (Alt+N)
    int weapons = -1;   // -1 = the file's `weapons`, 0 off, 1 on
};

struct OrderSpec {
    uint32_t tick = 0;
    Verb verb = Verb::Move;
    std::vector<std::string> selection;   // group names, or {"all"}, or body numbers "%N" (0-based, across groups in
                                          // file order; a harvested click keeps the recording's selection order)
    bool point = false;                   // x/z given (cells)
    float x = 0, z = 0;
    std::string target;                   // @group (centroid for points, first member for units)
    bool queue = false;
    int squad = 0;
    bool append = false;
};

struct Shape {   // gate/line: a segment; region: a rectangle; lane: two segments; pair: two group lists
    std::string kind, name;
    float x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    // gate options (0 or -1: the observer's default): lateral axis 0 x / 1 z, file band, members inside for a
    // files sample, end-window depth, ticks between two entries of a pair, flip distance in cells.
    int lateral = -1, band = 0, minCount = 0, edge = 0, pairWindow = 0, flip = 0;
    // lane: the second segment (the "after" line), the sign of each line's lateral cell, the entry window in
    // ticks and the least lateral distance of a swap (0: the observer's defaults).
    float ax0 = 0, az0 = 0, ax1 = 0, az1 = 0;
    int beforeSign = 1, afterSign = 1, window = 0, minCells = 0;
    bool acrossGroups = false;       // lane across=all: pairs of different groups count too
    // hug (W8 step 0, the MV-13 hug carry-over): the lane's two lines, one per vertex, and the lateral cell of
    // each vertex's tip; a member hugs a vertex when its lateral cell on first entering the vertex's line is
    // within `cells` (default 2) of the tip, and carries over when it hugs both.
    int tip1 = 0, tip2 = 0;
    bool haveTip1 = false, haveTip2 = false;
    // pair: comma lists of group names (a trailing '*' matches a name prefix) and the contact radius in cells.
    std::string a, b;
    int cells = 0;
    // reach (W5 AR-06): `a` attackers, `b` targets (comma lists, trailing '*' a prefix); range = px, 0: the
    // attacker's weapon reach as tickCombat reads it; marks = the ticks at which the ever-in-reach count and
    // the damage dealt are also reported.
    int range = 0;
    std::vector<int> marks;
};

// `churn`: a blocking map feature placed (and, with toggle, lifted again) every `every` ticks, as corpses and
// burning features churn the static map in a battle. Event k lands at tick from + k*every (before that tick's
// orders); its cell is (x + dx*(k % row), z + dz*(k / row)), or for toggle the cell of event k/2: even events
// place, odd events lift.
struct ChurnSpec {
    int x = 0, z = 0, w = 1, h = 1, every = 1;
    int dx = 0, dz = 0, row = 0;      // row 0: one row, never wraps
    bool toggle = false;
    uint32_t from = 0, until = 0;     // until 0: the run's end
};

struct MapSpec {
    enum Kind { Ascii, Flat, Gen1, Snapshot, Named };
    Kind kind = Flat;
    int width = 0, height = 0;
    std::vector<std::string> rows;    // ascii
    std::string recipe, file;         // gen1 / snapshot
    struct Overlay { std::string kind; int x = 0, z = 0, w = 0, h = 0, value = 0; };
    std::vector<Overlay> overlays;    // wall / height, ascii and flat only
};

struct Scenario {
    std::string name, origin, dir;
    uint32_t ticks = 3000, seed = 7;
    int players = 2;
    std::vector<std::pair<int, int>> teams;
    bool weapons = false, explored = true;
    std::vector<std::pair<uint32_t, uint16_t>> exploredRle;   // `explored rle N:HEX ...`: the recording's per-cell owner masks
    bool crusades = false;            // `crusades on`: the Crusades balance (unitscb roster, standard canbuild)
    bool wanderers = true;            // `wanderers off`: fbi types lose Standby_wander (their home-pull)
    int roundTrip = -1;               // `uplink R`: the 512-command window
    uint64_t navBytes = 0;            // `navbytes N`: the Legion field byte allowance (0: the game's)
    bool probeApproach = false;       // `probe approach`: the runner reports per-unit approach arrival and order completion
    bool probeLanes = false;          // `probe lanes`: the runner reports the lanes.* counters (W8 step 0)
    bool probeW9 = false;             // `probe w9`: the runner reports the w9.* counters and the debug-build census.* keys (W9 step 0)
    bool probeScale = false;          // `probe scale`: the runner reports the scale.* keys (tools/legion_scale.h; Legion 10,000 phase 0)
    bool probeAware = false;          // `probe aware`: the runner reports the aware.* counters (W7 step 0)
    bool probeClaims = false;         // `probe claims`: the runner audits the arrival-slot claims every 10 ticks (W5 step 0)
    std::vector<int> gateOffsets;     // `gateoffsets O,O,..`: legion_scenario's gate offsets where 0,+-1..+-5 leave the map
    MapSpec map;
    std::vector<TypeSpec> types;
    std::vector<GroupSpec> groups;
    std::vector<OrderSpec> orders;
    std::vector<Shape> shapes;
    std::vector<ChurnSpec> churns;
    // `truth TICK X,Z ...`: where the recording had every body (raw 16.16 px, group then member order) TICK
    // ticks into the situation. The runner reports how many of our bodies are within 2 cells of theirs.
    bool hasClock = false;            // `clock TICK RNG`: start the world on the recording's tick counter and game RNG
    uint32_t clockTick = 0, clockRng = 0;
    struct Truth {
        uint32_t tick = 0;
        std::vector<std::pair<int32_t, int32_t>> pos;
    };
    std::vector<Truth> truths;        // ascending ticks; a file may carry several (`truth 100 ...`, `truth 300 ...`)
    mutable std::vector<size_t> bodyStart_;                       // groups[g]'s first body number (indexGroups)
    mutable std::unordered_map<std::string, size_t> nameIndex_;   // group name -> index

    const TypeSpec* type(const std::string& n) const {
        for (const auto& t : types) if (t.name == n) return &t;
        return nullptr;
    }
    // Group lookups by name and by body number stay O(log n) however many groups a situation holds (a harvested 10k-body
    // situation has 10k groups and hundreds of clicks naming bodies): the index grows with `groups` and is never rebuilt.
    void indexGroups() const {
        while (bodyStart_.size() < groups.size()) {
            const size_t g = bodyStart_.size();
            bodyStart_.push_back(g ? bodyStart_[g - 1] + size_t(groups[g - 1].count) : 0);
            nameIndex_.emplace(groups[g].name, g);
        }
    }
    // The group a body number "#N" falls in (bodies are numbered across the groups in file order) and the body's
    // index inside it; group -1 when out of range.
    std::pair<int, size_t> locateBody(size_t n) const {
        indexGroups();
        if (groups.empty()) return {-1, 0};
        if (n >= bodyStart_.back() + size_t(groups.back().count)) return {-1, 0};
        const size_t g = size_t(std::upper_bound(bodyStart_.begin(), bodyStart_.end(), n) - bodyStart_.begin()) - 1;
        return {int(g), n - bodyStart_[g]};
    }
    int groupOfBody(size_t n) const { return locateBody(n).first; }
    static bool isBodyToken(const std::string& t) { return t.size() > 1 && t[0] == '%'; }
    static size_t bodyNumber(const std::string& t) { return size_t(std::strtoul(t.c_str() + 1, nullptr, 10)); }
    // Does the order's selection include (a body of) group index `g`?
    bool selects(const OrderSpec& o, size_t g) const {
        if (o.selection[0] == "all") return true;
        for (const auto& t : o.selection)
            if (isBodyToken(t) ? groupOfBody(bodyNumber(t)) == int(g) : t == groups[g].name) return true;
        return false;
    }
    const GroupSpec* group(const std::string& n) const {
        indexGroups();
        const auto it = nameIndex_.find(n);
        return it == nameIndex_.end() ? nullptr : &groups[it->second];
    }
    const Shape* shape(const std::string& n) const {
        for (const auto& s : shapes) if (s.name == n) return &s;
        return nullptr;
    }
    // Why the file cannot be built without an install (empty when it can). A
    // generated or snapshot map takes its features (and gen1 its coast and
    // relief prefabs) from the install; ascii and flat maps never need one.
    std::string needsData() const {
        for (const auto& t : types)
            if (t.kind == TypeSpec::Fbi) return "fbi type '" + t.fbi + "'";
        if (map.kind == MapSpec::Gen1) return "gen1 map";
        if (map.kind == MapSpec::Named) return "map '" + map.recipe + "'";
        if (map.kind == MapSpec::Snapshot) return "snapshot " + map.file;
        return {};
    }
};

// ---- parsing ---------------------------------------------------------------

namespace detail {
inline std::vector<std::string> words(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream in(line);
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}
struct Cursor {
    const std::string& origin;
    int line = 0;
    [[noreturn]] void fail(const std::string& why) const {
        throw std::runtime_error(origin + ":" + std::to_string(line) + ": " + why);
    }
    long long integer(const std::string& s, long long lo, long long hi) const {
        char* end = nullptr;
        const long long v = std::strtoll(s.c_str(), &end, 10);
        if (s.empty() || *end) fail("not an integer: '" + s + "'");
        if (v < lo || v > hi) fail("'" + s + "' outside " + std::to_string(lo) + ".." + std::to_string(hi));
        return v;
    }
    float number(const std::string& s) const {
        char* end = nullptr;
        const float v = std::strtof(s.c_str(), &end);
        if (s.empty() || *end || !std::isfinite(v)) fail("not a number: '" + s + "'");
        return v;
    }
};
inline std::string fmt(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.9g", double(v));
    return b;
}
inline int squadValue(const Cursor& c, const std::string& s) {
    if (s == "none") return 0;
    if (s.size() < 2 || (s[0] != 'f' && s[0] != 'g')) c.fail("squad is f<N>, g<N> or none: '" + s + "'");
    const int n = int(c.integer(s.substr(1), 1, 10));
    return s[0] == 'f' ? -n : n;
}
inline std::string squadName(int v) {
    return v == 0 ? "none" : (v < 0 ? "f" : "g") + std::to_string(std::abs(v));
}
inline const char* verbName(Verb v) {
    switch (v) {
    case Verb::Move: return "move";     case Verb::Fight: return "fight";
    case Verb::Patrol: return "patrol"; case Verb::Attack: return "attack";
    case Verb::Guard: return "guard";   case Verb::Stop: return "stop";
    case Verb::Squad: return "squad";
    }
    return "?";
}
inline const char* kindName(TypeSpec::Kind k) {
    static const char* n[] = {"mover", "flyer", "boat", "hover", "structure", "fbi", "roster"};
    return n[k];
}
}  // namespace detail

inline Scenario parse(const std::string& text, const std::string& origin = "<scn>",
                      const std::string& dir = {}) {
    using detail::words;
    Scenario s;
    s.origin = origin;
    s.dir = dir;
    detail::Cursor c{origin};
    std::istringstream in(text);
    bool versioned = false, haveMap = false, inAscii = false;
    for (std::string raw; std::getline(in, raw);) {
        ++c.line;
        if (inAscii) {
            std::string row = raw;
            while (!row.empty() && (row.back() == '\r' || row.back() == ' ' || row.back() == '\t')) row.pop_back();
            size_t lead = row.find_first_not_of(" \t");
            if (lead == std::string::npos) c.fail("blank ascii row (end the map with 'end')");
            row = row.substr(lead);
            if (row == "end") {
                inAscii = false;
                if (s.map.rows.empty()) c.fail("ascii map has no rows");
                s.map.width = int(s.map.rows[0].size());
                s.map.height = int(s.map.rows.size());
                continue;
            }
            if (!s.map.rows.empty() && row.size() != s.map.rows[0].size()) c.fail("ascii rows differ in width");
            for (char ch : row)
                if (std::string(".#~,^").find(ch) == std::string::npos)
                    c.fail(std::string("ascii cell '") + ch + "' is not one of . # ~ , ^");
            s.map.rows.push_back(row);
            continue;
        }
        if (auto hash = raw.find('#'); hash != std::string::npos) raw.erase(hash);
        auto w = words(raw);
        if (w.empty()) continue;
        const std::string& k = w[0];
        auto need = [&](size_t lo, size_t hi) {
            if (w.size() < lo || w.size() > hi) c.fail("'" + k + "' takes " + std::to_string(lo - 1) +
                (hi == lo ? "" : ".." + std::to_string(hi - 1)) + " arguments");
        };
        if (!versioned) {
            if (k != "scn" || w.size() != 2) c.fail("the first line must be 'scn 1'");
            if (c.integer(w[1], 0, 1 << 20) != kFormatVersion) c.fail("unsupported scn version " + w[1]);
            versioned = true;
            continue;
        }
        if (k == "name") { need(2, 2); s.name = w[1]; }
        else if (k == "ticks") { need(2, 2); s.ticks = uint32_t(c.integer(w[1], 1, 10'000'000)); }
        else if (k == "seed") { need(2, 2); s.seed = uint32_t(c.integer(w[1], 0, 0xffffffffLL)); }
        else if (k == "players") { need(2, 2); s.players = int(c.integer(w[1], 1, 8)); }
        else if (k == "team") {
            need(3, 3);
            s.teams.push_back({int(c.integer(w[1], 0, 7)), int(c.integer(w[2], 0, 7))});
        } else if (k == "weapons") {
            need(2, 2);
            if (w[1] != "on" && w[1] != "off") c.fail("weapons on|off");
            s.weapons = w[1] == "on";
        } else if (k == "crusades") {
            need(2, 2);
            if (w[1] != "on" && w[1] != "off") c.fail("crusades on|off");
            s.crusades = w[1] == "on";
        } else if (k == "wanderers") {
            need(2, 2);
            if (w[1] != "on" && w[1] != "off") c.fail("wanderers on|off");
            s.wanderers = w[1] == "on";
        } else if (k == "explored") {
            need(2, 1 << 20);
            if (w[1] == "rle") {
                // explored rle COUNT:HEX ... -- run-length coded owner masks, row-major over the whole map.
                s.explored = false;
                for (size_t at = 2; at < w.size(); ++at) {
                    const auto colon = w[at].find(':');
                    if (colon == std::string::npos) c.fail("explored rle entries are COUNT:HEX: '" + w[at] + "'");
                    s.exploredRle.push_back({uint32_t(c.integer(w[at].substr(0, colon), 1, 1 << 28)),
                                             uint16_t(std::strtoul(w[at].substr(colon + 1).c_str(), nullptr, 16))});
                }
            } else {
                if (w[1] != "all" && w[1] != "none") c.fail("explored all|none|rle ...");
                s.explored = w[1] == "all";
                s.exploredRle.clear();
            }
        } else if (k == "probe") {
            need(2, 2);
            if (w[1] != "approach" && w[1] != "claims" && w[1] != "aware" && w[1] != "lanes" && w[1] != "w9" &&
                w[1] != "scale")
                c.fail("probe approach|claims|aware|lanes|w9|scale");
            (w[1] == "approach" ? s.probeApproach : w[1] == "aware" ? s.probeAware : w[1] == "lanes" ? s.probeLanes
             : w[1] == "w9" ? s.probeW9 : w[1] == "scale" ? s.probeScale : s.probeClaims) = true;
        } else if (k == "uplink") { need(2, 2); s.roundTrip = int(c.integer(w[1], 1, 600)); }
        else if (k == "navbytes") { need(2, 2); s.navBytes = uint64_t(c.integer(w[1], 1, int64_t(1) << 40)); }
        else if (k == "gateoffsets") {
            need(2, 2);
            s.gateOffsets.clear();
            for (size_t at = 0; at <= w[1].size();) {
                const size_t comma = std::min(w[1].find(',', at), w[1].size());
                s.gateOffsets.push_back(int(c.integer(w[1].substr(at, comma - at), -64, 64)));
                at = comma + 1;
            }
        }
        else if (k == "map") {
            if (haveMap) c.fail("a second 'map'");
            haveMap = true;
            if (w.size() < 2) c.fail("map ascii | flat W H | gen1 RECIPE | snapshot FILE");
            if (w[1] == "ascii") { need(2, 2); s.map.kind = MapSpec::Ascii; inAscii = true; }
            else if (w[1] == "flat") {
                need(4, 4);
                s.map.kind = MapSpec::Flat;
                s.map.width = int(c.integer(w[2], 8, 2048));
                s.map.height = int(c.integer(w[3], 8, 2048));
            } else if (w[1] == "gen1") {
                need(3, 3);
                if (!tak::mapgen::isGeneratedMapId(w[2])) c.fail("not a ~gen1~ recipe: " + w[2]);
                s.map.kind = MapSpec::Gen1;
                s.map.recipe = w[2];
            } else if (w[1] == "named") {   // a map of the install, by name (situations cut from retail-map recordings)
                if (w.size() < 3) c.fail("map named NAME");
                s.map.kind = MapSpec::Named;
                s.map.recipe = w[2];
                for (size_t i = 3; i < w.size(); ++i) s.map.recipe += " " + w[i];
            } else if (w[1] == "snapshot") {
                need(3, 3);
                s.map.kind = MapSpec::Snapshot;
                s.map.file = w[2];
            } else c.fail("unknown map kind '" + w[1] + "'");
        } else if (k == "wall" || k == "height") {
            need(k == "wall" ? 5 : 6, k == "wall" ? 5 : 6);
            MapSpec::Overlay o;
            o.kind = k;
            o.x = int(c.integer(w[1], 0, 4095)); o.z = int(c.integer(w[2], 0, 4095));
            o.w = int(c.integer(w[3], 1, 4096)); o.h = int(c.integer(w[4], 1, 4096));
            if (k == "height") o.value = int(c.integer(w[5], 0, 255));
            s.map.overlays.push_back(o);
        } else if (k == "type") {
            if (w.size() < 3) c.fail("type NAME KIND ...");
            if (s.type(w[1])) c.fail("type '" + w[1] + "' defined twice");
            TypeSpec t;
            t.name = w[1];
            const std::string& kind = w[2];
            size_t at = 3;
            if (kind == "mover" || kind == "flyer" || kind == "boat" || kind == "hover") {
                t.kind = kind == "mover" ? TypeSpec::Mover : kind == "flyer" ? TypeSpec::Flyer
                       : kind == "boat" ? TypeSpec::Boat : TypeSpec::Hover;
                if (w.size() < 7) c.fail(kind + " takes FOOT TURN ACCEL SPEED");
                t.foot = int(c.integer(w[3], 1, 15));
                t.turn = int(c.integer(w[4], 1, 65535));
                t.accel = c.number(w[5]);
                t.speed = c.number(w[6]);
                // A mover with speed 0 is the Keep: canmove set, maxVel 0 (isStructure).
                if (t.accel <= 0 || t.speed < 0 || (t.speed == 0 && t.kind != TypeSpec::Mover))
                    c.fail("accel and speed must be positive (a mover may have speed 0)");
                at = 7;
            } else if (kind == "structure") {
                if (w.size() < 5) c.fail("structure takes FOOTX FOOTZ");
                t.kind = TypeSpec::Structure;
                t.foot = int(c.integer(w[3], 1, 15));
                t.footZ = int(c.integer(w[4], 1, 15));
                at = 5;
            } else if (kind == "fbi") {
                if (w.size() < 4) c.fail("fbi takes the unit's FBI name");
                t.kind = TypeSpec::Fbi;
                t.fbi = w[3];
                at = 4;
            } else if (kind == "roster") {
                t.kind = TypeSpec::Roster;
            } else c.fail("unknown type kind '" + kind + "'");
            for (; at < w.size(); ++at) {
                const auto eq = w[at].find('=');
                if (eq == std::string::npos) c.fail("expected key=value: '" + w[at] + "'");
                const std::string key = w[at].substr(0, eq), val = w[at].substr(eq + 1);
                if (key == "sight") t.sight = int(c.integer(val, 1, 100000));
                else if (key == "hp") t.hp = int(c.integer(val, 1, 10'000'000));
                else if (key == "footz" && t.kind != TypeSpec::Fbi && t.kind != TypeSpec::Roster)
                    t.footZ = int(c.integer(val, 1, 15));
                else if (key == "turninplace") t.turnInPlace = int(c.integer(val, 0, 65535));
                else if (key == "depth" && t.kind == TypeSpec::Boat) t.depth = int(c.integer(val, 0, 255));
                else if (key == "cruise" && t.kind == TypeSpec::Flyer) t.cruise = int(c.integer(val, 1, 4096));
                else if (key == "gun" && t.kind != TypeSpec::Fbi) {
                    auto parts = val;
                    std::replace(parts.begin(), parts.end(), ',', ' ');
                    auto g = words(parts);
                    if (g.size() != 3) c.fail("gun=RANGE,DAMAGE,RELOAD");
                    t.hasGun = true;
                    t.gunRange = int(c.integer(g[0], 1, 100000));
                    t.gunDamage = int(c.integer(g[1], 1, 10'000'000));
                    t.gunReload = c.number(g[2]);
                } else c.fail("unknown option '" + key + "' for a " + kind);
            }
            s.types.push_back(t);
        } else if (k == "group") {
            // group NAME OWNER TYPE COUNT rect X0 Z0 X1 Z1 | cells X,Z ... [opts]
            if (w.size() < 6) c.fail("group NAME OWNER TYPE COUNT rect|cells ...");
            if (s.group(w[1]) || w[1] == "all") c.fail("group name '" + w[1] + "' taken");
            GroupSpec g;
            g.name = w[1];
            g.owner = int(c.integer(w[2], 0, 7));
            g.type = w[3];
            if (!s.type(g.type)) c.fail("unknown type '" + g.type + "'");
            g.count = int(c.integer(w[4], 1, 20000));
            size_t at = 6;
            if (w[5] == "rect") {
                if (w.size() < 10) c.fail("rect X0 Z0 X1 Z1");
                g.x0 = int(c.integer(w[6], 0, 4095)); g.z0 = int(c.integer(w[7], 0, 4095));
                g.x1 = int(c.integer(w[8], 1, 4096)); g.z1 = int(c.integer(w[9], 1, 4096));
                if (g.x1 <= g.x0 || g.z1 <= g.z0) c.fail("empty rect");
                at = 10;
            } else if (w[5] == "cells") {
                g.rect = false;
                for (; at < w.size() && w[at].find('=') == std::string::npos; ++at) {
                    const auto comma = w[at].find(',');
                    if (comma == std::string::npos) c.fail("cells are X,Z: '" + w[at] + "'");
                    g.cells.push_back({int(c.integer(w[at].substr(0, comma), 0, 4095)),
                                       int(c.integer(w[at].substr(comma + 1), 0, 4095))});
                }
                if (int(g.cells.size()) != g.count) c.fail("count " + std::to_string(g.count) + " but " +
                                                          std::to_string(g.cells.size()) + " cells");
            } else if (w[5] == "spots") {
                g.rect = false;
                for (; at < w.size() && w[at].find('=') == std::string::npos; ++at) {
                    std::vector<std::string> f;
                    {
                        std::string item = w[at];
                        std::replace(item.begin(), item.end(), ',', ' ');
                        f = words(item);
                    }
                    if (f.size() < 4 || f.size() > 8) c.fail("spots are X,Z,HEADING,HP[,SPEED[,STAND[,ID[,VEL]]]] (raw fixed-point): '" + w[at] + "'");
                    g.spots.push_back({int32_t(c.integer(f[0], INT32_MIN, INT32_MAX)),
                                       int32_t(c.integer(f[1], INT32_MIN, INT32_MAX)),
                                       int32_t(c.integer(f[2], 0, 65535)), int32_t(c.integer(f[3], 0, INT32_MAX)),
                                       f.size() >= 5 ? int32_t(c.integer(f[4], 0, INT32_MAX)) : 0,
                                       f.size() >= 6 ? int32_t(c.integer(f[5], 0, INT32_MAX)) : 0,
                                       f.size() >= 7 ? int32_t(c.integer(f[6], 0, INT32_MAX)) : 0,
                                       f.size() == 8 ? int32_t(c.integer(f[7], INT32_MIN, INT32_MAX)) : 0});
                }
                if (int(g.spots.size()) != g.count) c.fail("count " + std::to_string(g.count) + " but " +
                                                          std::to_string(g.spots.size()) + " spots");
            } else c.fail("spawn is 'rect', 'cells' or 'spots'");
            for (; at < w.size(); ++at) {
                const auto eq = w[at].find('=');
                if (eq == std::string::npos) c.fail("expected key=value: '" + w[at] + "'");
                const std::string key = w[at].substr(0, eq), val = w[at].substr(eq + 1);
                if (key == "pitch" && g.rect) g.pitch = int(c.integer(val, 1, 64));
                else if (key == "squad") g.squad = detail::squadValue(c, val);
                else if (key == "weapons") {
                    if (val != "on" && val != "off") c.fail("weapons=on|off");
                    g.weapons = val == "on";
                } else c.fail("unknown group option '" + key + "'");
            }
            s.groups.push_back(g);
        } else if (k == "at") {
            // at TICK VERB SELECTION [X Z | @GROUP | f<N>|g<N>|none] [queue|append]
            if (w.size() < 4) c.fail("at TICK VERB SELECTION ...");
            OrderSpec o;
            o.tick = uint32_t(c.integer(w[1], 0, 10'000'000));
            const std::string& v = w[2];
            o.verb = v == "move" ? Verb::Move : v == "fight" ? Verb::Fight : v == "patrol" ? Verb::Patrol
                   : v == "attack" ? Verb::Attack : v == "guard" ? Verb::Guard : v == "stop" ? Verb::Stop
                   : v == "squad" ? Verb::Squad : (c.fail("unknown verb '" + v + "'"), Verb::Move);
            {
                auto list = w[3];
                std::replace(list.begin(), list.end(), ',', ' ');
                o.selection = words(list);
                for (const auto& n : o.selection) {
                    if (Scenario::isBodyToken(n)) {
                        char* end = nullptr;
                        std::strtoul(n.c_str() + 1, &end, 10);
                        if (*end || s.groupOfBody(Scenario::bodyNumber(n)) < 0) c.fail("no body '" + n + "'");
                    } else if (n != "all" && !s.group(n)) c.fail("unknown group '" + n + "' (groups come before orders)");
                }
                if (std::count(o.selection.begin(), o.selection.end(), "all") && o.selection.size() > 1)
                    c.fail("'all' stands alone");
            }
            size_t at = 4;
            auto flag = [&](const char* f) {
                if (at < w.size() && w[at] == f) { ++at; return true; }
                return false;
            };
            switch (o.verb) {
            case Verb::Move: case Verb::Fight: case Verb::Patrol:
                if (at < w.size() && w[at][0] == '@') { o.target = w[at].substr(1); ++at; }
                else if (at + 1 < w.size()) {
                    o.point = true;
                    o.x = c.number(w[at]); o.z = c.number(w[at + 1]);
                    at += 2;
                } else c.fail(v + " needs X Z or @GROUP");
                o.queue = flag("queue");
                break;
            case Verb::Attack: case Verb::Guard:
                if (at >= w.size() || w[at][0] != '@') c.fail(v + " needs @GROUP");
                o.target = w[at++].substr(1);
                o.queue = flag("queue");
                break;
            case Verb::Stop:
                break;
            case Verb::Squad:
                if (at >= w.size()) c.fail("squad needs f<N>, g<N> or none");
                o.squad = detail::squadValue(c, w[at++]);
                o.append = flag("append");
                break;
            }
            if (!o.target.empty() && !s.group(o.target)) c.fail("unknown target group '" + o.target + "'");
            if (at != w.size()) c.fail("unexpected '" + w[at] + "'");
            s.orders.push_back(o);
        } else if (k == "clock") {
            need(3, 3);
            s.hasClock = true;
            s.clockTick = uint32_t(c.integer(w[1], 0, 0xffffffffLL));
            s.clockRng = uint32_t(c.integer(w[2], 0, 0xffffffffLL));
        } else if (k == "truth") {
            if (w.size() < 2) c.fail("truth TICK X,Z ...");
            Scenario::Truth tr;
            tr.tick = uint32_t(c.integer(w[1], 1, 10'000'000));
            if (!s.truths.empty() && tr.tick <= s.truths.back().tick) c.fail("truth ticks must ascend");
            for (size_t at = 2; at < w.size(); ++at) {
                const auto comma = w[at].find(',');
                if (comma == std::string::npos) c.fail("truth entries are X,Z: '" + w[at] + "'");
                tr.pos.push_back({int32_t(c.integer(w[at].substr(0, comma), INT32_MIN, INT32_MAX)),
                                  int32_t(c.integer(w[at].substr(comma + 1), INT32_MIN, INT32_MAX))});
            }
            s.truths.push_back(std::move(tr));
        } else if (k == "churn") {
            need(6, 10);
            ChurnSpec ch;
            ch.x = int(c.integer(w[1], 0, 4095)); ch.z = int(c.integer(w[2], 0, 4095));
            ch.w = int(c.integer(w[3], 1, 8)); ch.h = int(c.integer(w[4], 1, 8));
            bool haveEvery = false;
            for (size_t at = 5; at < w.size(); ++at) {
                if (w[at] == "toggle") { ch.toggle = true; continue; }
                const auto eq = w[at].find('=');
                if (eq == std::string::npos) c.fail("expected key=value or toggle: '" + w[at] + "'");
                const std::string key = w[at].substr(0, eq), val = w[at].substr(eq + 1);
                if (key == "every") { ch.every = int(c.integer(val, 1, 1'000'000)); haveEvery = true; }
                else if (key == "from") ch.from = uint32_t(c.integer(val, 0, 10'000'000));
                else if (key == "until") ch.until = uint32_t(c.integer(val, 1, 10'000'000));
                else if (key == "walk") {
                    const auto a = val.find(','), b = val.find(',', a == std::string::npos ? a : a + 1);
                    if (a == std::string::npos || b == std::string::npos) c.fail("walk=DX,DZ,ROW");
                    ch.dx = int(c.integer(val.substr(0, a), -4096, 4096));
                    ch.dz = int(c.integer(val.substr(a + 1, b - a - 1), -4096, 4096));
                    ch.row = int(c.integer(val.substr(b + 1), 1, 1'000'000));
                } else c.fail("unknown churn option '" + key + "'");
            }
            if (!haveEvery) c.fail("churn needs every=N");
            if (ch.until && ch.until <= ch.from) c.fail("churn until must follow from");
            s.churns.push_back(ch);
        } else if (k == "gate" || k == "line" || k == "region" || k == "lane" || k == "hug" || k == "pair" || k == "reach" || k == "creep" || k == "behind") {
            if (k == "creep") need(3, 3); else if (k == "pair" || k == "reach" || k == "behind") need(4, k == "reach" ? 6 : 5); else if (k == "lane" || k == "hug") need(10, 16); else need(6, k == "gate" ? 14 : 6);
            if (s.shape(w[1])) c.fail("shape '" + w[1] + "' defined twice");
            Shape sh;
            sh.kind = k;
            sh.name = w[1];
            size_t at = 0;
            if (k == "creep") {
                sh.a = w[2];
                at = 3;
            } else if (k == "pair" || k == "reach" || k == "behind") {
                sh.a = w[2]; sh.b = w[3];
                at = 4;
            } else {
                sh.x0 = c.number(w[2]); sh.z0 = c.number(w[3]); sh.x1 = c.number(w[4]); sh.z1 = c.number(w[5]);
                at = 6;
                if (k == "region" && (sh.x1 <= sh.x0 || sh.z1 <= sh.z0)) c.fail("empty region");
                if (k == "lane" || k == "hug") {
                    sh.ax0 = c.number(w[6]); sh.az0 = c.number(w[7]); sh.ax1 = c.number(w[8]); sh.az1 = c.number(w[9]);
                    at = 10;
                    if (sh.x0 != sh.x1 && sh.z0 != sh.z1) c.fail("lane's before line must be vertical or horizontal");
                    if (sh.ax0 != sh.ax1 && sh.az0 != sh.az1) c.fail("lane's after line must be vertical or horizontal");
                }
            }
            for (; at < w.size(); ++at) {
                const auto eq = w[at].find('=');
                if (eq == std::string::npos) c.fail("expected key=value: '" + w[at] + "'");
                const std::string key = w[at].substr(0, eq), val = w[at].substr(eq + 1);
                if (k == "gate" && key == "lateral") {
                    if (val != "x" && val != "z") c.fail("lateral=x|z");
                    sh.lateral = val == "z" ? 1 : 0;
                } else if (k == "gate" && key == "band") sh.band = int(c.integer(val, 1, 64));
                else if (k == "gate" && key == "mincount") sh.minCount = int(c.integer(val, 1, 100000));
                else if (k == "gate" && key == "edge") sh.edge = int(c.integer(val, 1, 64));
                else if (k == "gate" && key == "pairwindow") sh.pairWindow = int(c.integer(val, 1, 1'000'000));
                else if (k == "gate" && key == "flip") sh.flip = int(c.integer(val, 1, 64));
                else if (k == "lane" && key == "bsign") sh.beforeSign = int(c.integer(val, -1, 1));
                else if (k == "lane" && key == "asign") sh.afterSign = int(c.integer(val, -1, 1));
                else if (k == "lane" && key == "window") sh.window = int(c.integer(val, 1, 1'000'000));
                else if (k == "lane" && key == "mincells") sh.minCells = int(c.integer(val, 1, 64));
                else if (k == "lane" && key == "across") {
                    if (val != "all" && val != "group") c.fail("across=all|group");
                    sh.acrossGroups = val == "all";
                } else if (k == "hug" && key == "tip1") { sh.tip1 = int(c.integer(val, 0, 100000)); sh.haveTip1 = true; }
                else if (k == "hug" && key == "tip2") { sh.tip2 = int(c.integer(val, 0, 100000)); sh.haveTip2 = true; }
                else if ((k == "pair" || k == "behind" || k == "hug") && key == "cells") sh.cells = int(c.integer(val, 0, 32));
                else if (k == "reach" && key == "range") sh.range = int(c.integer(val, 1, 100000));
                else if (k == "reach" && key == "marks") {
                    auto list = val;
                    std::replace(list.begin(), list.end(), ',', ' ');
                    for (const auto& m : words(list)) sh.marks.push_back(int(c.integer(m, 1, 10'000'000)));
                } else c.fail("unknown option '" + key + "' for " + k);
            }
            if ((k == "lane" && (sh.beforeSign == 0 || sh.afterSign == 0)))
                c.fail("bsign and asign are 1 or -1");
            if (k == "hug" && (!sh.haveTip1 || !sh.haveTip2)) c.fail("hug needs tip1=N and tip2=N");
            s.shapes.push_back(sh);
        } else c.fail("unknown directive '" + k + "'");
    }
    if (inAscii) c.fail("ascii map not closed with 'end'");
    if (!versioned) c.fail("empty file (expected 'scn 1')");
    if (!haveMap) c.fail("no 'map'");
    if (!s.map.overlays.empty() && s.map.kind != MapSpec::Ascii && s.map.kind != MapSpec::Flat)
        c.fail("wall/height overlays apply to ascii and flat maps only");
    for (const auto& g : s.groups)
        if (g.owner >= s.players) c.fail("group '" + g.name + "' owner " + std::to_string(g.owner) +
                                        " but players " + std::to_string(s.players));
    for (const auto& [p, t] : s.teams)
        if (p >= s.players || t >= s.players) c.fail("team outside players");
    for (const auto& o : s.orders) {   // one click is one player's selection
        int owner = -1;
        auto take = [&](const GroupSpec& g) {
            if (owner >= 0 && g.owner != owner) c.fail("an order's selection spans two players");
            owner = g.owner;
        };
        if (o.selection[0] == "all") { for (const auto& g : s.groups) take(g); continue; }
        for (const auto& t : o.selection) {
            if (Scenario::isBodyToken(t)) { if (const int gi = s.groupOfBody(Scenario::bodyNumber(t)); gi >= 0) take(s.groups[size_t(gi)]); }
            else if (const GroupSpec* g = s.group(t)) take(*g);
        }
    }
    std::stable_sort(s.orders.begin(), s.orders.end(),
                     [](const OrderSpec& a, const OrderSpec& b) { return a.tick < b.tick; });
    return s;
}

inline Scenario load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    std::ostringstream text;
    text << in.rdbuf();
    return parse(text.str(), path.string(), path.parent_path().string());
}

// The canonical text of a parsed file: parse(format(s)) formats identically.
inline std::string format(const Scenario& s) {
    using detail::fmt;
    std::ostringstream o;
    o << "scn " << kFormatVersion << "\n";
    if (!s.name.empty()) o << "name " << s.name << "\n";
    o << "ticks " << s.ticks << "\nseed " << s.seed << "\nplayers " << s.players << "\n";
    for (const auto& [p, t] : s.teams) o << "team " << p << " " << t << "\n";
    o << "weapons " << (s.weapons ? "on" : "off") << "\nwanderers " << (s.wanderers ? "on" : "off")
      << (s.crusades ? "\ncrusades on" : "")
      << "\n";
    if (!s.exploredRle.empty()) {
        o << "explored rle";
        for (const auto& [n, v] : s.exploredRle) { char h[16]; std::snprintf(h, sizeof h, "%x", unsigned(v)); o << " " << n << ":" << h; }
        o << "\n";
    } else o << "explored " << (s.explored ? "all" : "none") << "\n";
    if (s.roundTrip >= 0) o << "uplink " << s.roundTrip << "\n";
    if (s.navBytes) o << "navbytes " << s.navBytes << "\n";
    if (s.probeApproach) o << "probe approach\n";
    if (s.probeClaims) o << "probe claims\n";
    if (s.probeAware) o << "probe aware\n";
    if (s.probeLanes) o << "probe lanes\n";
    if (s.probeW9) o << "probe w9\n";
    if (s.probeScale) o << "probe scale\n";
    if (!s.gateOffsets.empty()) {
        o << "gateoffsets ";
        for (size_t i = 0; i < s.gateOffsets.size(); ++i) o << (i ? "," : "") << s.gateOffsets[i];
        o << "\n";
    }
    switch (s.map.kind) {
    case MapSpec::Ascii:
        o << "map ascii\n";
        for (const auto& r : s.map.rows) o << r << "\n";
        o << "end\n";
        break;
    case MapSpec::Flat: o << "map flat " << s.map.width << " " << s.map.height << "\n"; break;
    case MapSpec::Gen1: o << "map gen1 " << s.map.recipe << "\n"; break;
    case MapSpec::Named: o << "map named " << s.map.recipe << "\n"; break;
    case MapSpec::Snapshot: o << "map snapshot " << s.map.file << "\n"; break;
    }
    for (const auto& v : s.map.overlays) {
        o << v.kind << " " << v.x << " " << v.z << " " << v.w << " " << v.h;
        if (v.kind == "height") o << " " << v.value;
        o << "\n";
    }
    for (const auto& t : s.types) {
        o << "type " << t.name << " " << detail::kindName(t.kind);
        switch (t.kind) {
        case TypeSpec::Mover: case TypeSpec::Flyer: case TypeSpec::Boat: case TypeSpec::Hover:
            o << " " << t.foot << " " << t.turn << " " << fmt(t.accel) << " " << fmt(t.speed);
            if (t.footZ) o << " footz=" << t.footZ;
            break;
        case TypeSpec::Structure: o << " " << t.foot << " " << t.footZ; break;
        case TypeSpec::Fbi: o << " " << t.fbi; break;
        case TypeSpec::Roster: break;
        }
        if (t.kind != TypeSpec::Fbi) {
            if (t.sight != 4096) o << " sight=" << t.sight;
            if (t.hp != 100) o << " hp=" << t.hp;
            if (t.turnInPlace >= 0) o << " turninplace=" << t.turnInPlace;
            if (t.kind == TypeSpec::Boat && t.depth != 13) o << " depth=" << t.depth;
            if (t.kind == TypeSpec::Flyer && t.cruise != 80) o << " cruise=" << t.cruise;
            if (t.hasGun) o << " gun=" << t.gunRange << "," << t.gunDamage << "," << fmt(t.gunReload);
        }
        o << "\n";
    }
    for (const auto& g : s.groups) {
        o << "group " << g.name << " " << g.owner << " " << g.type << " " << g.count;
        if (g.rect) o << " rect " << g.x0 << " " << g.z0 << " " << g.x1 << " " << g.z1;
        else if (!g.spots.empty()) {
            o << " spots";
            for (const auto& sp : g.spots) {
                o << " " << sp.x << "," << sp.z << "," << sp.heading << "," << sp.hp;
                if (sp.speed || sp.stand || sp.id || sp.vel) o << "," << sp.speed;
                if (sp.stand || sp.id || sp.vel) o << "," << sp.stand;
                if (sp.id || sp.vel) o << "," << sp.id;
                if (sp.vel) o << "," << sp.vel;
            }
        } else {
            o << " cells";
            for (const auto& [x, z] : g.cells) o << " " << x << "," << z;
        }
        if (g.pitch) o << " pitch=" << g.pitch;
        if (g.squad) o << " squad=" << detail::squadName(g.squad);
        if (g.weapons >= 0) o << " weapons=" << (g.weapons ? "on" : "off");
        o << "\n";
    }
    for (const auto& sh : s.shapes) {
        o << sh.kind << " " << sh.name;
        if (sh.kind == "creep") o << " " << sh.a;
        else if (sh.kind == "pair" || sh.kind == "reach" || sh.kind == "behind") o << " " << sh.a << " " << sh.b;
        else o << " " << fmt(sh.x0) << " " << fmt(sh.z0) << " " << fmt(sh.x1) << " " << fmt(sh.z1);
        if (sh.kind == "lane" || sh.kind == "hug")
            o << " " << fmt(sh.ax0) << " " << fmt(sh.az0) << " " << fmt(sh.ax1) << " " << fmt(sh.az1);
        auto opt = [&](const char* key, int v, int none) { if (v != none) o << " " << key << "=" << v; };
        if (sh.kind == "gate") {
            if (sh.lateral >= 0) o << " lateral=" << (sh.lateral ? "z" : "x");
            opt("band", sh.band, 0); opt("mincount", sh.minCount, 0); opt("edge", sh.edge, 0);
            opt("pairwindow", sh.pairWindow, 0); opt("flip", sh.flip, 0);
        } else if (sh.kind == "lane") {
            opt("bsign", sh.beforeSign, 1); opt("asign", sh.afterSign, 1);
            opt("window", sh.window, 0); opt("mincells", sh.minCells, 0);
            if (sh.acrossGroups) o << " across=all";
        } else if (sh.kind == "hug") {
            o << " tip1=" << sh.tip1 << " tip2=" << sh.tip2;
            opt("cells", sh.cells, 0);
        } else if (sh.kind == "pair" || sh.kind == "behind") opt("cells", sh.cells, 0);
        else if (sh.kind == "reach") {
            opt("range", sh.range, 0);
            if (!sh.marks.empty()) {
                o << " marks=";
                for (size_t i = 0; i < sh.marks.size(); ++i) o << (i ? "," : "") << sh.marks[i];
            }
        }
        o << "\n";
    }
    for (const auto& ch : s.churns) {
        o << "churn " << ch.x << " " << ch.z << " " << ch.w << " " << ch.h << " every=" << ch.every;
        if (ch.row) o << " walk=" << ch.dx << "," << ch.dz << "," << ch.row;
        if (ch.toggle) o << " toggle";
        if (ch.from) o << " from=" << ch.from;
        if (ch.until) o << " until=" << ch.until;
        o << "\n";
    }
    if (s.hasClock) o << "clock " << s.clockTick << " " << s.clockRng << "\n";
    for (const auto& tr : s.truths) {
        o << "truth " << tr.tick;
        for (const auto& [x, z] : tr.pos) o << " " << x << "," << z;
        o << "\n";
    }
    for (const auto& r : s.orders) {
        o << "at " << r.tick << " " << detail::verbName(r.verb) << " ";
        for (size_t i = 0; i < r.selection.size(); ++i) o << (i ? "," : "") << r.selection[i];
        if (r.point) o << " " << fmt(r.x) << " " << fmt(r.z);
        if (!r.target.empty()) o << " @" << r.target;
        if (r.verb == Verb::Squad) o << " " << detail::squadName(r.squad) << (r.append ? " append" : "");
        if (r.queue) o << " queue";
        o << "\n";
    }
    return o.str();
}

// ---- building the world ----------------------------------------------------

struct BuildOptions {
    tak::sim::PathfindingMode mode = tak::sim::PathfindingMode::Legion;
    bool serial = true;
    int offset = 0;               // start offset in cells: 0, +1, -1
    const char* data = nullptr;   // retail install (--data); nullptr = asset-free
    // Or an install view the caller has mounted (a test's synthetic features).
    const tak::hpi::Vfs* install = nullptr;
    bool crusades = false;
};

struct Built {
    std::string skipped;                       // non-empty: data-gated, not built
    std::unique_ptr<tak::hpi::Vfs> vfs;
    tak::sim::TypeRegistry registry;
    std::deque<tak::sim::UnitType> types;      // synthetic types (stable addresses)
    std::unique_ptr<tak::sim::World> world;
    std::map<std::string, std::vector<int>> groups;   // unit ids in spawn order
    int width = 0, height = 0;                 // cells
};

namespace detail {
// The roster mix: the retail roster's quartiles (sight 256/320/400, turn rate
// 1000/2500, footprint 2/3, speed 1.6..2.6 px/tick), cycled by member index so
// a group of 60 holds every combination.
inline TypeSpec rosterVariant(const TypeSpec& base, int k) {
    TypeSpec t = base;
    t.kind = TypeSpec::Mover;
    t.sight = std::array<int, 3>{256, 320, 400}[size_t(k % 3)];
    t.turn = (k / 3) % 2 ? 2500 : 1000;
    t.foot = 2 + (k / 6) % 2;
    t.speed = 1.6f + 0.25f * float((k / 12) % 5);
    if (!t.hasGun) { t.hasGun = true; t.gunRange = 160; t.gunDamage = 25; t.gunReload = 1.0f; }
    t.name = base.name + "-r" + std::to_string(k % 60);
    return t;
}
inline tak::sim::UnitType synth(const TypeSpec& t, bool armed) {
    using tak::sim::Fixed;
    using tak::sim::UnitType;
    UnitType u{};
    u.id = u.name = "scn-" + t.name + (armed ? "" : "-unarmed");
    u.maxHp = t.hp;
    u.sight = t.sight;
    u.buildTime = 1;
    u.footX = t.foot;
    u.footZ = t.footZ ? t.footZ : t.foot;
    if (t.kind != TypeSpec::Structure) {
        u.canMove = true;
        u.maxVel = Fixed::fromFloat(t.speed);
        u.accel = u.brake = Fixed::fromFloat(t.accel);
        u.turnRate = t.turn;
        u.turnInPlaceRate = t.turn;
        u.halfCellTicks = 3;
    } else {
        u.maxVel = Fixed();   // isStructure(); UnitType defaults maxVel to 1 (a slow mover)
    }
    switch (t.kind) {
    case TypeSpec::Flyer:
        u.canFly = true;
        u.cruiseAlt = t.cruise;
        u.vtolStandby = true;   // defaultmissiontype=VTOL_standby: an idle flyer lands
        break;
    case TypeSpec::Boat:
        u.domain = UnitType::Domain::Water;
        u.floater = true;
        u.minWaterDepth = t.depth;
        u.maxWaterDepth = 10000;
        u.turnInPlaceRate = 0;   // ships specify only a travel turn rate
        break;
    case TypeSpec::Hover:
        u.domain = UnitType::Domain::Hover;
        u.maxSlope = 10;
        u.maxWaterSlope = 24;
        break;
    default: break;
    }
    if (t.turnInPlace >= 0) u.turnInPlaceRate = t.turnInPlace;
    if (armed) {
        tak::sim::Weapon w;
        w.name = "scn-gun";
        w.range = t.hasGun ? t.gunRange : 120;
        w.damage = t.hasGun ? t.gunDamage : 100;
        w.reload = t.hasGun ? t.gunReload : 0.1f;
        w.aimTol = 32767;
        u.weapon = w;
        u.weapons.push_back(w);
    }
    return u;
}
}  // namespace detail

inline std::unique_ptr<Built> build(const Scenario& s, const BuildOptions& opt) {
    using namespace tak::sim;
    auto b = std::make_unique<Built>();
    if (auto why = s.needsData(); !why.empty() && !opt.data && !opt.install) {
        b->skipped = "needs --data: " + why;
        return b;
    }
    if (opt.install) {
        b->vfs = std::make_unique<tak::hpi::Vfs>(opt.install);   // a local view: map files stay ours
        setupRegistry(b->registry, *opt.install, opt.crusades || s.crusades);
    } else if (opt.data) {
        b->vfs = std::make_unique<tak::hpi::Vfs>(
            tak::hpi::mountRetailRoot(opt.data));
        setupRegistry(b->registry, *b->vfs, opt.crusades || s.crusades);
    } else {
        b->vfs = std::make_unique<tak::hpi::Vfs>();
    }
    b->world = std::make_unique<World>();
    World& w = *b->world;
    if (s.map.kind == MapSpec::Ascii || s.map.kind == MapSpec::Flat) {
        // The legion_world_test Fixture: flat ground at 100, sea level 64,
        // walls as 1x1 blocking map features on the placement plane.
        const int W = s.map.width, H = s.map.height;
        w.setGameSeed(s.seed);
        w.setSerialThreads(opt.serial);
        w.setPathService(true);
        w.setPathfindingMode(opt.mode);
        w.setPlayerCount(s.players);
        std::vector<uint8_t> heights(size_t(W) * H, 100);
        std::vector<uint8_t> walls(size_t(W) * H, 0);
        if (s.map.kind == MapSpec::Ascii)
            for (int z = 0; z < H; ++z)
                for (int x = 0; x < W; ++x) {
                    const char ch = s.map.rows[size_t(z)][size_t(x)];
                    auto& h = heights[size_t(z) * W + x];
                    if (ch == '~') h = 20;        // deep water
                    else if (ch == ',') h = 56;   // shallow shelf
                    else if (ch == '^') h = 200;  // high ground (slope barrier at its edge)
                    else if (ch == '#') walls[size_t(z) * W + x] = 1;
                }
        for (const auto& v : s.map.overlays)
            for (int z = v.z; z < std::min(H, v.z + v.h); ++z)
                for (int x = v.x; x < std::min(W, v.x + v.w); ++x) {
                    if (v.kind == "wall") walls[size_t(z) * W + x] = 1;
                    else heights[size_t(z) * W + x] = uint8_t(v.value);
                }
        w.setTerrain(heights, W, H, 64);
        std::vector<uint16_t> cells(size_t(W) * H, 0xffff);
        for (int z = 0; z < H; ++z)
            for (int x = 0; x < W; ++x)
                if (walls[size_t(z) * W + x]) {
                    cells[size_t(z) * W + x] = 0;
                    w.blockCells(x, z, 1, 1, true);
                }
        w.setMapPlacementFeatures(cells, {{"legion-wall", 1, 1, true, true, false, 0}});
        b->width = W;
        b->height = H;
    } else {
        MatchConfig cfg;
        cfg.vfs = b->vfs.get();
        cfg.loadCrt = false;
        cfg.pathfindingMode = opt.mode;
        cfg.startSeed = s.seed;
        cfg.slots.resize(size_t(s.players));   // unused slots: no monarchs
        if (s.map.kind == MapSpec::Gen1) cfg.mapPath = s.map.recipe;
        else if (s.map.kind == MapSpec::Named) {
            cfg.mapPath = tak::hpi::findMap(*b->vfs, s.map.recipe);
            if (cfg.mapPath.empty()) throw std::runtime_error(s.origin + ": map '" + s.map.recipe + "' is not in the install");
        } else {
            const auto path = std::filesystem::path(s.dir) / s.map.file;
            if (s.map.file.size() > 4 && s.map.file.compare(s.map.file.size() - 4, 4, ".kmp") == 0) {
                auto pkg = tak::net::maps::importSnapshot(*b->vfs, path);
                if (!pkg) throw std::runtime_error("snapshot rejected: " + path.string());
                b->vfs->setMapFiles(pkg->files);
                cfg.mapPath = pkg->mapPath;
            } else {
                std::ifstream in(path, std::ios::binary);
                if (!in) throw std::runtime_error("cannot read snapshot " + path.string());
                auto files = std::make_shared<tak::hpi::Vfs::Files>();
                (*files)["maps/scn-snapshot.tnt"] =
                    std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
                b->vfs->setMapFiles(files);
                cfg.mapPath = "maps/scn-snapshot.tnt";
            }
        }
        setupMatch(w, b->registry, cfg);
        w.setSerialThreads(opt.serial);
        b->width = w.mapW();
        b->height = w.mapH();
    }
    for (int p = 0; p < s.players; ++p) w.setTeam(p, p);
    for (const auto& [p, t] : s.teams) w.setTeam(p, t);

    // Types: one UnitType per (spec, armed) -- roster per (variant, armed).
    std::map<std::string, const UnitType*> made;
    auto typeFor = [&](const TypeSpec& spec, bool armed, int member) -> const UnitType* {
        if (spec.kind == TypeSpec::Fbi) {
            const UnitType* real = b->registry.find(spec.fbi);
            if (!real) throw std::runtime_error(s.origin + ": no unit '" + spec.fbi + "' in the install");
            if (armed) return real;
            // Unarmed copy (as movement_orders_test's Hunters): a type outside
            // the registry plans on its domain's legacy nav grid.
            const std::string key = "fbi:" + spec.fbi;
            if (auto it = made.find(key); it != made.end()) return it->second;
            auto copy = *real;
            copy.weapons.clear();
            copy.weapon.damage = 0;
            if (!s.wanderers) copy.wanders = false;   // the motion cases' wanderer knob
            b->types.push_back(copy);
            return made[key] = &b->types.back();
        }
        const TypeSpec v = spec.kind == TypeSpec::Roster ? detail::rosterVariant(spec, member) : spec;
        const std::string key = v.name + (armed ? "+" : "-");
        if (auto it = made.find(key); it != made.end()) return it->second;
        b->types.push_back(detail::synth(v, armed));
        return made[key] = &b->types.back();
    };
    for (const auto& g : s.groups) {
        const TypeSpec& spec = *s.type(g.type);
        const bool armed = g.weapons >= 0 ? g.weapons == 1 : s.weapons;
        std::vector<std::pair<int, int>> spots = g.cells;
        if (!g.spots.empty()) {
            auto& ids = b->groups[g.name];
            for (const auto& sp : g.spots) {
                // Exact: spawn near the spot, then set the raw fixed-point fields. The group's start offset is
                // whole cells on both axes, as for every other form.
                const int32_t ox = sp.x + opt.offset * 16 * 65536, oz = sp.z + opt.offset * 16 * 65536;
                if (sp.id > 0) w.setNextUnitId(sp.id);
                const int id = w.spawn(typeFor(spec, armed, int(ids.size())), float(ox) / 65536.f, float(oz) / 65536.f,
                                       std::nullopt, g.owner);
                if (id <= 0) throw std::runtime_error(s.origin + ": group '" + g.name + "' spawn failed");
                if (auto* u = w.unit(id)) {
                    u->x.v = ox; u->z.v = oz;
                    u->homeX.v = ox; u->homeZ.v = oz;
                    u->heading = Bam(sp.heading);
                    u->tickStartHeadingBam = uint16_t(sp.heading);
                    if (sp.hp > 0) u->hp.v = sp.hp;
                    if (sp.speed > 0) u->baseSpeed.v = sp.speed;
                    if (sp.vel) u->speed.v = sp.vel;
                    if (sp.stand) {
                        u->standingOrder = uint8_t(sp.stand & 0xff);
                        u->moveState = uint8_t((sp.stand >> 8) & 0xff);
                        u->fireState = uint8_t((sp.stand >> 16) & 0xff);
                        u->stance = int((uint32_t(sp.stand) >> 24) & 0x7f) - 1;
                    }
                }
                ids.push_back(id);
            }
            continue;
        }
        if (g.rect) {
            const UnitType* first = typeFor(spec, armed, 0);
            const int pitch = g.pitch ? g.pitch : std::max(first->footX, first->footZ) + 1;
            for (int z = g.z0; z < g.z1 && int(spots.size()) < g.count; z += pitch)
                for (int x = g.x0; x < g.x1 && int(spots.size()) < g.count; x += pitch) spots.push_back({x, z});
            if (int(spots.size()) < g.count)
                throw std::runtime_error(s.origin + ": group '" + g.name + "' rect holds " +
                                         std::to_string(spots.size()) + " of " + std::to_string(g.count));
        }
        auto& ids = b->groups[g.name];
        for (int k = 0; k < g.count; ++k) {
            const int cx = spots[size_t(k)].first + opt.offset, cz = spots[size_t(k)].second + opt.offset;
            if (cx < 0 || cz < 0 || cx >= b->width || cz >= b->height)
                throw std::runtime_error(s.origin + ": group '" + g.name + "' spawns off the map");
            const int id = w.spawn(typeFor(spec, armed, k), float(cx * 16), float(cz * 16), std::nullopt, g.owner);
            if (id <= 0) throw std::runtime_error(s.origin + ": group '" + g.name + "' spawn failed");
            ids.push_back(id);
        }
    }
    if (!s.exploredRle.empty()) {
        w.updateNavigationExploration();
        auto& known = w.editNavigationExploration();
        size_t at = 0;
        for (const auto& [n, v] : s.exploredRle) {
            if (at + n > known.size()) throw std::runtime_error(s.origin + ": `explored rle` is longer than the map");
            std::fill(known.begin() + ptrdiff_t(at), known.begin() + ptrdiff_t(at + n), v);
            at += n;
        }
        if (at != known.size()) throw std::runtime_error(s.origin + ": `explored rle` covers " + std::to_string(at) +
                                                         " cells, the map has " + std::to_string(known.size()));
    } else if (s.explored) {
        w.updateNavigationExploration();
        auto& known = w.editNavigationExploration();
        std::fill(known.begin(), known.end(), 0xffff);
    }
    if (s.hasClock) w.resumeClocks(s.clockTick, s.clockRng);
    return b;
}

// ---- orders ----------------------------------------------------------------

// Turns the file's orders into the commands of each tick: every order due at
// tick t is clicked (hudCommands, at the World as it stands before tick t),
// then the uplink lands what the client and server would let through. Group
// squad= flags are Ctrl/Alt+N presses at tick 0, ahead of tick-0 orders.
class OrderFeed {
public:
    OrderFeed(const Scenario& s, Built& b, int roundTrip = -2)
        : s_(s), b_(b), link_(roundTrip == -2 ? s.roundTrip : roundTrip) {
        // One press per squad number, selecting every group that names it:
        // separate presses would evict each other (assignSquad replaces).
        for (const auto& g : s.groups) {
            if (!g.squad) continue;
            // Squad numbers are per player: one press per (owner, number).
            auto same = std::find_if(pending_.begin(), pending_.end(), [&](const OrderSpec& o) {
                return o.squad == g.squad && o.verb == Verb::Squad && s.group(o.selection[0])->owner == g.owner;
            });
            if (same != pending_.end()) { same->selection.push_back(g.name); continue; }
            OrderSpec o;
            o.verb = Verb::Squad;
            o.selection = {g.name};
            o.squad = g.squad;
            pending_.push_back(o);
        }
        pending_.insert(pending_.end(), s.orders.begin(), s.orders.end());
    }

    std::vector<int> selection(const OrderSpec& o) const {
        std::vector<int> out;
        auto add = [&](const std::string& n) {
            for (int id : b_.groups.at(n))
                if (const auto* u = b_.world->unit(id); u && u->alive()) out.push_back(id);
        };
        auto addBody = [&](size_t n) {
            const auto [gi, at] = s_.locateBody(n);
            if (gi < 0) return;
            const int id = b_.groups.at(s_.groups[size_t(gi)].name)[at];
            if (const auto* u = b_.world->unit(id); u && u->alive()) out.push_back(id);
        };
        if (o.selection[0] == "all") for (const auto& g : s_.groups) add(g.name);
        else for (const auto& n : o.selection) {
            if (Scenario::isBodyToken(n)) addBody(Scenario::bodyNumber(n));
            else add(n);
        }
        return out;
    }

    std::vector<tak::net::Command> commandsFor(uint32_t tick) {
        const auto& world = *b_.world;
        for (; next_ < pending_.size() && pending_[next_].tick <= tick; ++next_) {
            const auto& o = pending_[next_];
            const auto sel = selection(o);
            if (sel.empty()) continue;
            SelectionOrder so;
            so.verb = o.verb;
            so.queue = o.queue;
            so.squad = o.squad;
            so.append = o.append;
            if (o.point) { so.x = o.x * 16; so.z = o.z * 16; }
            else if (!o.target.empty()) {
                float cx = 0, cz = 0;
                int n = 0, first = 0;
                for (int id : b_.groups.at(o.target))
                    if (const auto* u = world.unit(id); u && u->alive()) {
                        if (!first) first = id;
                        cx += u->x.toFloat(); cz += u->z.toFloat(); ++n;
                    }
                if (!n) continue;   // the target group is gone: the click finds nothing
                so.x = cx / float(n);
                so.z = cz / float(n);
                so.targetId = first;
            }
            const int player = world.unit(sel[0])->player;
            uint32_t& serial = serials_.try_emplace(player, 1u).first->second;
            const auto cmds = hudItems(world, sel, so, player, serial);
            const bool click = !o.queue && !o.append && o.verb != Verb::Stop && o.verb != Verb::Squad;
            // Per unit, the commands issued for it (an explicit order counts once for each member).
            auto count = [&](int id) {
                if (id < 0) return;
                if (size_t(id) >= issued_.size()) issued_.resize(size_t(id) + 1, 0);
                ++issued_[size_t(id)];
                if (click) clicks_.push_back({id, issued_[size_t(id)]});
            };
            for (const auto& c : cmds) {
                if (c.kind == tak::net::Cmd::Order && c.order) for (const int id : c.order->members) count(id);
                else count(c.unitId);
            }
            link_.issue(cmds);
        }
        return link_.land(tick);
    }

    // Apply tick `tick`'s commands; call before World::tick with tick == tickCount().
    size_t apply(uint32_t tick) {
        applyChurn(tick);
        clicks_.clear();
        landed_ = commandsFor(tick);
        landedUnits_.clear();
        const auto start = std::chrono::steady_clock::now();
        for (const auto& c : landed_) tak::sim::applyCommand(*b_.world, b_.registry, c);
        applyMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        for (const auto& c : landed_) {
            if (c.kind == tak::net::Cmd::Order && c.order)
                landedUnits_.insert(landedUnits_.end(), c.order->members.begin(), c.order->members.end());
            else landedUnits_.push_back(c.unitId);
        }
        return landed_.size();
    }
    // Wall time of the last apply()'s applyCommand calls (ms; observation only, stderr reports).
    double lastApplyMs() const { return applyMs_; }
    // The commands (and explicit orders) the last apply() landed, in application order.
    const std::vector<tak::net::Command>& landed() const { return landed_; }
    // The unit of every command the last apply() landed, an explicit order's members each once.
    const std::vector<int>& landedUnits() const { return landedUnits_; }
    // The non-queued clicks the last apply() issued: (unit id, that click's command's position in the unit's
    // command stream). The uplink is one FIFO, so the click is delivered when that many of the unit's commands
    // have landed; a later order of the same tick (a queued leg) comes after it.
    const std::vector<std::pair<int, uint32_t>>& clicks() const { return clicks_; }
    bool done() const { return next_ >= pending_.size() && link_.idle(); }

    // The `churn` events due at `tick`: a blocking feature of the spec's size placed in the static map (an odd
    // event of a toggle lifts the same cell again by re-adding it unblocked).
    void applyChurn(uint32_t tick) {
        for (const auto& ch : s_.churns) {
            if (tick < ch.from || (ch.until && tick >= ch.until) || (tick - ch.from) % uint32_t(ch.every)) continue;
            const int64_t k = (tick - ch.from) / uint32_t(ch.every);
            const int64_t cell = ch.toggle ? k / 2 : k;
            const int64_t row = ch.row ? ch.row : int64_t(1) << 40;
            const int cx = ch.x + ch.dx * int(cell % row);
            const int cz = ch.z + ch.dz * int(cell / row);
            if (cx < 0 || cz < 0 || cx + ch.w > b_.width || cz + ch.h > b_.height) continue;
            b_.world->addFeature(cz * b_.width + cx, float(cx * 16 + ch.w * 8), float(cz * 16 + ch.h * 8), 0.f,
                                 ch.w, ch.h, !ch.toggle || k % 2 == 0, -1, true);
        }
    }

private:
    const Scenario& s_;
    Built& b_;
    Uplink link_;
    std::vector<OrderSpec> pending_;
    size_t next_ = 0;
    std::vector<tak::net::Command> landed_;
    std::vector<int> landedUnits_;
    double applyMs_ = 0;
    std::vector<uint32_t> issued_;   // per unit id
    std::map<int, uint32_t> serials_;   // per player: the next explicit-order serial (from 1)
    std::vector<std::pair<int, uint32_t>> clicks_;
};

// ---- situations: the recording's own positions ------------------------------

// Every body of the file in group then member order (the order `truth` is written in).
inline std::vector<int> allBodies(const Scenario& s, const Built& b) {
    std::vector<int> all;
    for (const auto& g : s.groups)
        for (int id : b.groups.at(g.name)) all.push_back(id);
    return all;
}

inline std::vector<std::pair<int32_t, int32_t>> startPositions(const Scenario& s, const Built& b) {
    std::vector<std::pair<int32_t, int32_t>> out;
    for (int id : allBodies(s, b)) {
        const auto* u = b.world->unit(id);
        out.push_back({u ? u->x.v : 0, u ? u->z.v : 0});
    }
    return out;
}

struct TruthReport {
    int64_t n = 0, within = 0, movedN = 0, movedWithin = 0;   // bodies, and those the recording moved > 2 cells
    int64_t within2Permille() const { return n ? within * 1000 / n : 0; }
    int64_t movedWithin2Permille() const { return movedN ? movedWithin * 1000 / movedN : 1000; }
};

// Compare our bodies with `truth` (call when the world has run `truthTick` ticks): a body matches when it is alive
// and within 2 cells of the recording's position on both axes. Bodies dead in the recording are not counted.
// `shift` cells is the build's start offset (it moved every spawn, so it moves the truth the same way).
inline TruthReport truthReport(const Scenario& s, const Built& b, const Scenario::Truth& truth,
                               const std::vector<std::pair<int32_t, int32_t>>& startPos, int offset,
                               const char* detail = nullptr) {
    TruthReport r;
    const auto all = allBodies(s, b);
    if (truth.pos.size() != all.size()) return r;
    const int64_t two = int64_t(2) * 16 * 65536, shift = int64_t(offset) * 16 * 65536;
    auto far = [&](int64_t a, int64_t t) { return a - t > two || t - a > two; };
    for (size_t k = 0; k < all.size(); ++k) {
        const auto [tx, tz] = truth.pos[k];
        if (tx == INT32_MIN) continue;
        const auto* u = b.world->unit(all[k]);
        const bool in = u && u->alive() && !far(u->x.v, int64_t(tx) + shift) && !far(u->z.v, int64_t(tz) + shift);
        const bool moved = far(startPos[k].first, int64_t(tx) + shift) || far(startPos[k].second, int64_t(tz) + shift);
        ++r.n; r.within += in;
        if (moved) { ++r.movedN; r.movedWithin += in; }
        if (detail && u)
            std::fprintf(stderr, "truth %s id=%d %s start=(%.1f,%.1f) truth=(%.1f,%.1f) ours=(%.1f,%.1f) moved=%d in=%d orders=%zu\n",
                         u->type ? u->type->id.c_str() : "?", all[k], u->alive() ? "alive" : "dead", startPos[k].first / 1048576.0,
                         startPos[k].second / 1048576.0, (int64_t(tx) + shift) / 1048576.0, (int64_t(tz) + shift) / 1048576.0,
                         u->x.v / 1048576.0, u->z.v / 1048576.0, int(moved), int(in), u->orders.size());
    }
    return r;
}

}  // namespace tak::scn
