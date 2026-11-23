// legion_scenario -- run one .scn scenario (tools/scenarios/README.scn-format.txt)
// in Legion and Retail on the same binary and print its observer keys.
//
//   legion_scenario <file.scn | builtin:NAME>... [--mode legion|retail|both]
//       [--offsets 0,1,-1] [--workers|--serial|--both-exec] [--window R]
//       [--data <install>] [--json] [--ticks N] [--no-observer] [--neutral]
//       [--wanderers on|off]   (fbi types keep / lose Standby_wander; default: the file's)
//       [--no-vertex-probe]    (as the game: the W8 vertex probe off, its lanes.vertex_* keys 0;
//                               LegionNavigator::setInstrumentProbes, on by default here)
//       [--budget-div N]       (test: the registration / plane-build / awareness budgets divided by N,
//                               LegionNavigator::setBudgetDivisorForTest; changes behaviour and hashes)
//       [--field-bytes N]      (test: the field byte allowance, LegionNavigator::setFieldBytesForTest -- never
//                               below two whole-map fields; memory pressure on any file; changes behaviour and hashes)
//       [--pause-all]          (test: every first build may pause, LegionNavigator::setPauseAllForTest -- the
//                               stage-5 consumers on paused routes; changes behaviour and hashes)
//       [--sector K]           (test: the routing hierarchy's sector size, LegionNavigator::setSectorSizeForTest --
//                               the stage-6 sector-size experiment; default 32; changes behaviour and hashes)
//       [--no-hier-routes]     (test: every route the window build, LegionNavigator::setHierarchicalRoutesForTest --
//                               the scheduler's load tests; changes behaviour and hashes)
//       [--par-force] [--par-perturb SEED]   (test: stage 9's parallel planning jobs run even with no SimPool workers,
//                               and in a shuffled order with random delays, LegionNavigator::setParallelForTest --
//                               never changes behaviour or hashes; a NAVPAR line on stderr reports the jobs)
//       [--navpar]             (the NAVPAR line without --scale)
//       [--no-route-index]     (test: static changes walk every group instead of the route index,
//                               LegionNavigator::setRouteIndexForTest -- perf follow-up I; never changes behaviour or
//                               hashes)
//       [--routeidx]           (a ROUTEIDX line on stderr per run: LegionNavigator::routeIndexStats)
//       [--check <baseline.json> [--step ID] [--cumulative] [--ratchet "reason"]]
//       [--baseline <baseline.json> --reason "text" [--since ID]]
//       [--exit-table <old.json|run.jsonl>] [--anchor <anchor.json>]
//
// For each mode and each start offset (cells, applied to every spawn) the
// file is built (tools/legion_scn.h), its orders are clicked through
// issueSelection's HUD mirror and uplink (64 commands per tick; with --window
// R, or the file's `uplink R`, at most 512 outstanding over a round trip of R
// ticks), and the world runs `ticks` ticks with the observer
// (tools/legion_observe.h) attached: Observer::sample every tick and the
// NavWork adaptor feeding LegionNavigator::Stats as per-tick work. --both-exec
// (the default) runs serial and workers and fails unless their state hashes,
// checkpoint digests and observer keys are equal.
//
// Output, one line per mode (--json), keys in a fixed order:
//   {"scenario","mode","ticks","units","exec","window","offsets":[..],
//    "hash":{"<offset>":"<16 hex>",..},"serial_eq_workers",
//    "keys":{key: median over offsets},"varying":{key:[value per offset]}}
// "varying" lists only keys whose offsets disagree (the rest equal "keys").
// The median of 3 is the middle value; time keys (t50, t90, done,
// orders_done) read -1 ("never") as the worst value. A work.* key missing
// from an offset counts 0 there. No wall time is printed to stdout, so two
// runs print identical JSON. Wall time goes to stderr, one line per run:
//   NAME MODE offset=O serial|workers MS ms tick_max_ms=M@T tick_p99_ms=P
// (MS the whole run; M the slowest World::tick and T the tick it was, P the
// nearest-rank 99th percentile of the tick times; the observer's own sampling
// is outside them).
//
// Keys beside the observer's:
//   commands             unit commands landed in the sim (an explicit order counts each member)
//   orders               explicit selection orders landed (protocol 267: Legion clicks; 0 in Retail)
//   last_command_tick    tick the last of them landed
//   legion_groups        Legion only: distinct LegionNavigator groups among
//                        the scenario's units with orders, sampled after the
//                        tick the last command landed
//   legion_groups_peak   Legion only: the most such groups on any tick
//   truth.tT.*           a harvested situation (a `truth T ...` directive): truth.tT.n bodies alive in the
//                        recording, truth.tT.within2_permille of them within 2 cells of the recording's
//                        position T ticks in, truth.tT.moved_* the same for the bodies the recording
//                        moved more than 2 cells (the ones that mean something)
//   region.<name>.inside units whose centre cell is in the region at the end
//
// Shapes become observer probes:
//   gate, a segment (x0 == x1 or z0 == z1): a line crossed across it; the
//       probe is an 8-cell strip centred on it (lateral axis = the segment's).
//   gate, a rectangle: a strip travelled along its longer side (the lateral
//       axis is the shorter one).
//   line, vertical or horizontal: a Side at that x (or z); far side >= it.
//   region: region.<name>.inside (above).
// Wall metrics (wall_touch, clearance) need a static mask, so they are
// reported for ascii and flat maps only.
//
// --neutral: instead of keys, checks that the observer cannot change the
// world: per file, mode and offset, serial and workers each with the observer
// attached and detached must give the same hash and digest (and serial and
// workers the same keys). Only --neutral takes several files.
//
// Gate modes (PLAN 3.0 / 3.8; the logic is tools/legion_check.py, kept in one
// place): --check, --baseline, --exit-table and --anchor run the files (several
// allowed, JSON forced) and hand the JSON lines to the checker, which prints
// the verdict; the exit status is the checker's (1 a gate failed, 2 tooling).
// --check compares with a baseline (eq / one-sided bands / bounds / Retail
// floor), --ratchet REASON also writes the improvements into the references
// and a history line, --baseline retakes the base (needs --reason),
// --exit-table lists every key moved over 5% against an older base,
// --anchor prints the drift against the frozen anchor. Mode defaults to both
// (the Retail floor needs Retail beside Legion). --check and --baseline default
// to the gate's eleven offsets 0,+-1..+-5 (lead ruling W3 round 3 (a)): the
// small-count keys -- crossings, wall touch, a t90 of fewer than 10 bodies --
// are gated on all eleven and every other key on the core five 0,+-1,+-2
// (tools/legion_check.py re-reads the run). A file whose spawns the eleven put
// off the map names its own (`gateoffsets`, at least nine). `--offsets gate`
// asks for these gate offsets in any mode (the nightly's JSON run).
//
// Exit: 0 ok, 1 a mismatch (serial != workers, or --neutral), 2 usage or a
// bad file, 77 the file needs --data (ctest SKIP).
#include "legion_observe.h"
#include "legion_scale.h"
#include "legion_scn.h"
#include "client/ordershape.h"
#include "sim/convoy.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <sys/wait.h>
#endif

using tak::sim::PathfindingMode;
namespace obs = tak::legion_observe;
namespace scn = tak::scn;

namespace {

// Built-in fixtures (builtin:NAME), for the ctests: a file of the same text
// behaves identically.
const std::map<std::string, const char*> kBuiltins = {
    // An Alt+N formation of 28 movers and 4 flyers round the end of a wall.
    {"formation", R"(scn 1
name formation
ticks 900
map flat 96 64
wall 44 0 4 40
type m mover 2 2500 10 1.8
type f flyer 2 2500 10 2.5
group A 0 m 28 rect 6 30 27 42 squad=f1
group F 0 f 4 rect 6 46 18 49 squad=f1
gate tip 46 40 46 63
line east 70 0 70 64
at 2 move A,F 80 20
)"},
};

struct Options {
    std::vector<std::string> files;   // several only with --neutral
    std::vector<PathfindingMode> modes{PathfindingMode::Legion, PathfindingMode::Retail};
    std::vector<int> offsets{0, 1, -1};
    bool offsetsGiven = false;
    bool gateOffsets = false;         // --offsets gate (the default of --check / --baseline)
    enum Exec { Serial, Workers, Both } exec = Both;
    int window = -2;        // -2: the file's uplink
    const char* data = nullptr;
    bool json = false, observer = true, neutral = false;
    uint32_t ticks = 0;     // 0: the file's
    uint32_t catchup = 0;   // --catchup N: the path budget is unlimited for the first N ticks (a harvested situation re-issues every
                            // in-flight order at tick 0; the retail request queue would drain that backlog over ~2000 ticks)
    int wanderers = -1;     // --wanderers on|off overrides the file's; -1: the file's
    bool w9 = false;        // --w9: report the W9 instruments for any file (as `probe w9`)
    bool scale = false;     // --scale: report the scale.* keys for any file (as `probe scale`)
    bool vertexProbe = true;   // --no-vertex-probe: the W8 vertex probe off, as in the game (setInstrumentProbes)
    int budgetDiv = 1;         // --budget-div N: setBudgetDivisorForTest (tests of the over-budget paths)
    uint64_t fieldBytes = 0;   // --field-bytes N: setFieldBytesForTest (memory pressure; 0: NavBudget::kFieldBytes)
    bool pauseAll = false;     // --pause-all: setPauseAllForTest (stage 5: the consumers on paused routes)
    int sector = 0;            // --sector K: setSectorSizeForTest (stage 6: the sector-size experiment; 0: the game's 32)
    bool hierRoutes = true;    // --no-hier-routes: setHierarchicalRoutesForTest(false) (every route the window build)
    bool parForce = false;     // --par-force: setParallelForTest (stage 9: the parallel jobs inline with no workers)
    uint64_t parPerturb = 0;   // --par-perturb SEED: ... in a shuffled order with random delays (0 off)
    bool navpar = false;       // --navpar: the NAVPAR line without --scale (it comes with --scale and the --par-* flags)
    bool routeIndex = true;    // --no-route-index: setRouteIndexForTest(false) (perf follow-up I: the walk of every group)
    bool routeIdx = false;     // --routeidx: the ROUTEIDX line (routeIndexStats) on stderr per run
    uint32_t dumpUnits = 0;    // --dump-units N: a DUMP line per live unit on stderr every N ticks (observation only)
    std::vector<int> trace; // --trace IDS: decision trace lines on stderr for these members (debug build)
    std::vector<std::string> traceGroups;   // ... and whole groups, by name (a token that is not a number)
    // Gate modes: the JSON lines go to tools/legion_check.py instead of stdout.
    std::string gate;                  // check | baseline | exit-table | anchor | "" (none)
    std::string gateFile;              // the baseline (check, baseline), old base (exit-table), anchor
    std::vector<std::string> gateArgs; // pass-through options of the checker
};

// JSON lines of a gate-mode run; empty capture = print to stdout.
std::string* g_capture = nullptr;
void emit(const std::string& line) {
    if (g_capture) *g_capture += line + "\n";
    else std::puts(line.c_str());
}

[[noreturn]] void usage(const char* why) {
    std::fprintf(stderr, "legion_scenario: %s\n"
        "usage: legion_scenario <file.scn|builtin:NAME> [--mode legion|retail|both] [--offsets 0,1,-1|gate]\n"
        "       [--workers|--serial|--both-exec] [--window R] [--data <install>] [--json] [--ticks N]\n"
        "       [--no-observer] [--neutral] [--wanderers on|off] [--w9] [--scale] [--trace ID[,ID...]] [--no-vertex-probe]\n"
        "       [--budget-div N] [--field-bytes N] [--pause-all] [--sector K] [--no-hier-routes] [--dump-units N]\n"
        "       [--par-force] [--par-perturb SEED] [--navpar] [--no-route-index] [--routeidx]\n"
        "       [--check B.json [--step ID] [--cumulative] [--ratchet REASON]] [--baseline B.json --reason TEXT\n"
        "       [--since ID]] [--exit-table OLD] [--anchor A.json]   (gate modes: tools/legion_check.py)\n", why);
    std::exit(2);
}

const char* modeName(PathfindingMode m) { return m == PathfindingMode::Retail ? "retail" : "legion"; }

std::string hex(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}

uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

bool timeKey(std::string_view k) {
    for (std::string_view s : {".t50", ".t90", ".done", ".orders_done"})
        if (k.size() >= s.size() && k.substr(k.size() - s.size()) == s) return true;
    return false;
}

int cellLo(float v) { return int(std::floor(v)); }

// A lane line: the segment's cells (a horizontal one is laid along x, its lateral cell is x; a vertical one
// along z). A segment of extent covers floor(lo) .. ceil(hi)-1, a point its own cell.
obs::Line laneLine(const scn::Scenario& s, float x0, float z0, float x1, float z1, int sign) {
    auto span = [](float a, float b) {
        const float lo = std::min(a, b), hi = std::max(a, b);
        return std::pair<int, int>{cellLo(lo), hi > lo ? int(std::ceil(hi)) - 1 : cellLo(lo)};
    };
    (void)s;
    obs::Line l;
    const auto [xa, xb] = span(x0, x1);
    const auto [za, zb] = span(z0, z1);
    l.region = {xa, za, xb, zb};
    l.lateral = z0 == z1 ? 0 : 1;
    l.sign = sign;
    return l;
}

// A group list of a `pair` shape: comma separated, a trailing '*' takes every group with that prefix.
std::string expandGroups(const scn::Scenario& s, const std::string& list) {
    std::string out;
    size_t at = 0;
    while (at <= list.size()) {
        size_t e = list.find(',', at);
        if (e == std::string::npos) e = list.size();
        const std::string item = list.substr(at, e - at);
        at = e + 1;
        if (!item.empty() && item.back() == '*') {
            const std::string prefix = item.substr(0, item.size() - 1);
            for (const auto& g : s.groups)
                if (g.name.compare(0, prefix.size(), prefix) == 0) out += (out.empty() ? "" : ",") + g.name;
        } else out += (out.empty() ? "" : ",") + item;
    }
    return out;
}

// The convoy each directive's click joins (lead ruling W3 round 4 (1)): under A1 (user decision 2) the clicks one
// convoy merges are one order -- wall-4x50's four selections of 50 sent to one point on ticks 1-4 are one command
// -- so the observer keys a command, and sizes a group's arrival disc, by convoy, in both modes. The directives run
// through a ConvoyTable of their own (never the world's: Retail keeps none), in tick order, each as one shared
// order at its click with its selection's owner and class, joined on its directive tick. A queued or appended
// directive, or one without a point (a target group's centroid, Attack, Guard, Stop, Squad), joins none: 0.
std::vector<uint32_t> directiveConvoys(const scn::Scenario& s) {
    std::vector<uint32_t> out(s.orders.size(), 0);
    std::vector<size_t> byTick(s.orders.size());
    for (size_t i = 0; i < byTick.size(); ++i) byTick[i] = i;
    std::stable_sort(byTick.begin(), byTick.end(),
                     [&](size_t a, size_t b) { return s.orders[a].tick < s.orders[b].tick; });
    auto owner = [&](const scn::OrderSpec& o) {
        const std::string& t = o.selection[0];
        if (t == "all") return s.groups.empty() ? 0 : s.groups[0].owner;
        if (scn::Scenario::isBodyToken(t)) {
            const int g = s.groupOfBody(scn::Scenario::bodyNumber(t));
            return g >= 0 ? s.groups[size_t(g)].owner : 0;
        }
        const auto* g = s.group(t);
        return g ? g->owner : 0;
    };
    tak::sim::ConvoyTable table;
    for (size_t i : byTick) {
        const auto& o = s.orders[i];
        if (o.queue || o.append || !o.point || o.selection.empty()) continue;
        tak::sim::ConvoyClass cls;
        switch (o.verb) {
        case scn::Verb::Move: cls = tak::sim::ConvoyClass::Move; break;
        case scn::Verb::Fight: cls = tak::sim::ConvoyClass::Fight; break;
        case scn::Verb::Patrol: cls = tak::sim::ConvoyClass::Patrol; break;
        default: continue;
        }
        const auto raw = [](float cells) { return int32_t(std::lround(double(cells) * 16 * 65536)); };
        out[i] = table.join(owner(o), cls, raw(o.x), raw(o.z), true, o.tick).id;
    }
    return out;
}

// The observer's configuration for one built world.
obs::Config configFor(const scn::Scenario& s, const scn::Built& b) {
    const auto& w = *b.world;
    auto centroid = [&](const std::string& g, bool firstOnly) {
        int64_t x = 0, z = 0, n = 0;
        for (int id : b.groups.at(g)) {
            const auto* u = w.unit(id);
            if (!u) continue;
            x += u->x.v >> 16; z += u->z.v >> 16; ++n;
            if (firstOnly) break;
        }
        return n ? std::pair<int, int>{int(x / n), int(z / n)} : std::pair<int, int>{0, 0};
    };
    obs::Config cfg;
    const auto convoys = directiveConvoys(s);
    std::vector<uint32_t> groupConvoy;   // the convoy of the order that decides each group's click (0: none)
    for (const auto& g : s.groups) {
        obs::Group og;
        og.name = g.name;
        og.ids = b.groups.at(g.name);
        std::tie(og.clickX, og.clickZ) = centroid(g.name, false);
        groupConvoy.push_back(0);
        // The last order that moves this group decides its click.
        for (const auto& o : s.orders) {
            if (!s.selects(o, size_t(&g - s.groups.data()))) continue;
            switch (o.verb) {
            case scn::Verb::Move: case scn::Verb::Fight: case scn::Verb::Patrol:
            case scn::Verb::Attack: case scn::Verb::Guard:
                groupConvoy.back() = convoys[size_t(&o - s.orders.data())];
                break;
            default: break;
            }
            switch (o.verb) {
            case scn::Verb::Move: case scn::Verb::Fight: case scn::Verb::Patrol:
                if (o.point) { og.clickX = int(std::lround(o.x * 16)); og.clickZ = int(std::lround(o.z * 16)); }
                else std::tie(og.clickX, og.clickZ) = centroid(o.target, false);
                break;
            case scn::Verb::Attack: case scn::Verb::Guard:
                std::tie(og.clickX, og.clickZ) = centroid(o.target, true);
                break;
            default: break;
            }
        }
        cfg.groups.push_back(std::move(og));
    }
    // Groups whose clicks one convoy merged share one disc, sized for all of them.
    for (size_t g = 0; g < cfg.groups.size(); ++g) {
        if (!groupConvoy[g]) continue;
        int n = 0;
        for (size_t h = 0; h < cfg.groups.size(); ++h)
            if (groupConvoy[h] == groupConvoy[g]) n += int(cfg.groups[h].ids.size());
        cfg.groups[g].discN = n;
    }
    // Lead ruling W3 final exit (f): one-body groups whose clicks one convoy merged (aware-*, motion-*) are judged on
    // the Retail floor at click level -- the convoy's arrived / t90 / done -- in both modes; the observer reads every
    // group of such a convoy as one click, named by its first group (the per-body keys stay, report-only).
    for (size_t g = 0; g < cfg.groups.size(); ++g) {
        if (!groupConvoy[g] || !cfg.groups[g].click.empty()) continue;
        int groups = 0;
        bool oneBody = false;
        for (size_t h = g; h < cfg.groups.size(); ++h)
            if (groupConvoy[h] == groupConvoy[g]) { ++groups; oneBody |= cfg.groups[h].ids.size() == 1; }
        if (groups < 2 || !oneBody) continue;
        for (size_t h = g; h < cfg.groups.size(); ++h)
            if (groupConvoy[h] == groupConvoy[g]) cfg.groups[h].click = cfg.groups[g].name;
    }
    for (const auto& sh : s.shapes) {
        const float x0 = std::min(sh.x0, sh.x1), x1 = std::max(sh.x0, sh.x1);
        const float z0 = std::min(sh.z0, sh.z1), z1 = std::max(sh.z0, sh.z1);
        if (sh.kind == "gate") {
            obs::Gate g;
            g.name = sh.name;
            if (x0 == x1) {          // crossed west-east: lateral z
                g.lateral = 1;
                g.region = {cellLo(x0) - 4, cellLo(z0), cellLo(x0) + 3, cellLo(z1)};
            } else if (z0 == z1) {   // crossed north-south: lateral x
                g.lateral = 0;
                g.region = {cellLo(x0), cellLo(z0) - 4, cellLo(x1), cellLo(z0) + 3};
            } else {
                g.lateral = (x1 - x0) >= (z1 - z0) ? 1 : 0;
                g.region = {cellLo(x0), cellLo(z0), int(std::ceil(x1)) - 1, int(std::ceil(z1)) - 1};
            }
            if (sh.lateral >= 0) g.lateral = sh.lateral;
            if (sh.band) g.band = sh.band;
            if (sh.minCount) g.minCount = sh.minCount;
            if (sh.edge) g.edge = sh.edge;
            if (sh.pairWindow) g.pairWindow = sh.pairWindow;
            if (sh.flip) g.flipCells = sh.flip;
            cfg.gates.push_back(g);
        } else if (sh.kind == "lane") {
            obs::LaneOrder l;
            l.name = sh.name;
            l.before = laneLine(s, sh.x0, sh.z0, sh.x1, sh.z1, sh.beforeSign);
            l.after = laneLine(s, sh.ax0, sh.az0, sh.ax1, sh.az1, sh.afterSign);
            if (sh.window) l.window = sh.window;
            if (sh.minCells) l.minCells = sh.minCells;
            l.sameGroup = !sh.acrossGroups;
            cfg.laneOrders.push_back(l);
        } else if (sh.kind == "hug") {
            obs::Hug h;
            h.name = sh.name;
            h.first = laneLine(s, sh.x0, sh.z0, sh.x1, sh.z1, 1);
            h.second = laneLine(s, sh.ax0, sh.az0, sh.ax1, sh.az1, 1);
            h.tipFirst = sh.tip1;
            h.tipSecond = sh.tip2;
            if (sh.cells) h.cells = sh.cells;
            cfg.hugs.push_back(h);
        } else if (sh.kind == "pair") {
            obs::Pair p;
            p.name = sh.name;
            p.a = expandGroups(s, sh.a);
            p.b = expandGroups(s, sh.b);
            for (const std::string* list : {&p.a, &p.b}) {
                size_t at = 0;
                while (at <= list->size()) {
                    size_t e = list->find(',', at);
                    if (e == std::string::npos) e = list->size();
                    if (list->empty() || !s.group(list->substr(at, e - at)))
                        throw std::runtime_error(s.origin + ": pair '" + sh.name + "' names no group in '" + *list + "'");
                    at = e + 1;
                }
            }
            if (sh.cells) p.cells = sh.cells;
            cfg.pairs.push_back(p);
        } else if (sh.kind == "line") {
            obs::Side side;
            side.name = sh.name;
            if (x0 == x1) { side.axis = 0; side.at = cellLo(x0); }
            else if (z0 == z1) { side.axis = 1; side.at = cellLo(z0); }
            else throw std::runtime_error(s.origin + ": line '" + sh.name + "' must be vertical or horizontal");
            cfg.sides.push_back(side);
        }
    }
    return cfg;
}

// Wall cells of an ascii or flat map (walls never move with the start offset).
std::vector<uint8_t> wallMask(const scn::Scenario& s, int W, int H) {
    std::vector<uint8_t> m(size_t(W) * size_t(H), 0);
    if (s.map.kind == scn::MapSpec::Ascii)
        for (int z = 0; z < H; ++z)
            for (int x = 0; x < W; ++x)
                if (s.map.rows[size_t(z)][size_t(x)] == '#') m[size_t(z) * W + x] = 1;
    for (const auto& v : s.map.overlays)
        if (v.kind == "wall")
            for (int z = v.z; z < std::min(H, v.z + v.h); ++z)
                for (int x = v.x; x < std::min(W, v.x + v.w); ++x) m[size_t(z) * W + x] = 1;
    return m;
}

// `probe approach` (PLAN W2 step 0, AR-11): per unit, the first tick Legion holds
// it at its approach point (member state 5, Trapped: the nearest reachable spot
// of an unreachable goal) and the first tick its orders are empty. Read-only:
// LegionNavigator::unitState and the unit's order list. Keys (ticks; -1 never):
//   approach.members                    units watched
//   approach.arrived_n / .arrive.t50 .t90 .done      reached the approach point
//   approach.complete_n / .complete.t50 .t90 .done   orders emptied
//   approach.wait.t50 / .wait.done      completion - arrival: median / worst
//                                       (done -1 unless every unit did both)
//   approach.arrive.first / .last       earliest / latest arrival that happened
//   approach.wait.max                   worst completion - arrival among the units
//                                       that did both
//   approach.end_state_N                units whose Legion member state is N (1 moving,
//                                       2 holding, 3 waiting, 4 arrived, 5 at the point)
//                                       after the last tick: what the units that have
//                                       not reached the point are doing (diagnostic;
//                                       not baselined, it depends on --ticks)
//   approach.stopwait_p50 / _p90 / _max completion - the tick the unit last gained on its
//                                       order's point (its distance to it fell 1 px under
//                                       its best so far): how long a unit that stopped
//                                       getting nearer kept its order (both modes; W2
//                                       AR-11 holder rule; -1 none completed)
//   approach.dropped_unarrived_n        orders emptied before the unit ever
//                                       reached its point: a mid-route stop, or
//                                       the 9000-tick walk grace running out
//                                       while the unit still queued in the crowd
//                                       (Legion only; 0 on a lone walk)
struct ApproachProbe {
    std::vector<int> ids;
    std::vector<int> arrive, complete;
    std::vector<int> gain;      // last tick the distance to the order's point fell 1 px under its best
    std::vector<double> best;   // least distance to the order's point so far (px)
    std::vector<char> had;   // the unit has had orders
    int endState[6] = {};    // member states after the last sample (units that still have orders)
    explicit ApproachProbe(const std::vector<int>& all)
        : ids(all), arrive(all.size(), -1), complete(all.size(), -1), gain(all.size(), -1),
          best(all.size(), 1e300), had(all.size(), 0) {}
    void sample(const tak::sim::World& w, const tak::sim::LegionNavigator* nav, int t) {
        std::fill(std::begin(endState), std::end(endState), 0);
        for (size_t i = 0; i < ids.size(); ++i) {
            const auto* u = w.unit(ids[i]);
            if (!u || !u->alive()) continue;
            if (nav && !u->orders.empty()) ++endState[std::clamp(nav->unitState(ids[i]), 0, 5)];
            if (nav && arrive[i] < 0 && nav->unitState(ids[i]) == 5) arrive[i] = t;
            if (!u->orders.empty()) {
                const auto& leg = u->orders[tak::sim::World::currentLeg(u->orders)];
                const auto [tx, tz] = leg.missionTarget.value_or(std::pair{leg.x, leg.z});
                const double d = std::hypot(double(u->x.toFloat() - tx.toFloat()), double(u->z.toFloat() - tz.toFloat()));
                if (gain[i] < 0 || d <= best[i] - 1.0) { best[i] = d; gain[i] = t; }
                had[i] = 1;
            } else if (had[i] && complete[i] < 0) complete[i] = t;
        }
    }
    static void milestones(obs::Keys& k, const std::string& p, std::vector<int> v, size_t n) {
        std::sort(v.begin(), v.end());
        const auto at = [&](size_t need) -> int64_t { return need >= 1 && need <= v.size() ? v[need - 1] : -1; };
        k.emplace_back(p + ".t50", at((n + 1) / 2));
        k.emplace_back(p + ".t90", at((n * 9 + 9) / 10));
        k.emplace_back(p + ".done", at(n));
    }
    void report(obs::Keys& k, bool legion) const {
        const size_t n = ids.size();
        k.emplace_back("approach.members", int64_t(n));
        std::vector<int> a, c, wait, stop;
        int64_t premature = 0;
        for (size_t i = 0; i < n; ++i) {
            if (arrive[i] >= 0) a.push_back(arrive[i]);
            if (complete[i] >= 0) c.push_back(complete[i]);
            if (arrive[i] >= 0 && complete[i] >= 0) wait.push_back(std::max(0, complete[i] - arrive[i]));
            if (legion && arrive[i] < 0 && complete[i] >= 0) ++premature;
            if (gain[i] >= 0 && complete[i] >= 0) stop.push_back(std::max(0, complete[i] - gain[i]));
        }
        if (legion) {
            k.emplace_back("approach.arrived_n", int64_t(a.size()));
            milestones(k, "approach.arrive", a, n);
            k.emplace_back("approach.arrive.first", a.empty() ? -1 : *std::min_element(a.begin(), a.end()));
            k.emplace_back("approach.arrive.last", a.empty() ? -1 : *std::max_element(a.begin(), a.end()));
        }
        k.emplace_back("approach.complete_n", int64_t(c.size()));
        milestones(k, "approach.complete", c, n);
        std::sort(stop.begin(), stop.end());
        k.emplace_back("approach.stopwait_p50", stop.empty() ? -1 : stop[(stop.size() - 1) / 2]);
        k.emplace_back("approach.stopwait_p90", stop.empty() ? -1 : stop[(stop.size() * 9 + 9) / 10 - 1]);
        k.emplace_back("approach.stopwait_max", stop.empty() ? -1 : int64_t(stop.back()));
        if (std::getenv("TAK_APPROACH_STOPWAIT")) {
            std::fprintf(stderr, "approach stopwait (%s):", legion ? "legion" : "retail");
            for (int v : stop) std::fprintf(stderr, " %d", v);
            std::fprintf(stderr, "\n");
        }
        if (legion) {
            std::sort(wait.begin(), wait.end());
            k.emplace_back("approach.wait.t50", wait.empty() ? -1 : wait[(wait.size() - 1) / 2]);
            k.emplace_back("approach.wait.done", wait.size() == n ? int64_t(wait.back()) : -1);
            k.emplace_back("approach.wait.max", wait.empty() ? -1 : int64_t(wait.back()));
            for (int st = 1; st <= 5; ++st) k.emplace_back("approach.end_state_" + std::to_string(st), endState[st]);
            k.emplace_back("approach.dropped_unarrived_n", premature);
        }
    }
};

// `reach NAME ATTACKERS TARGETS [range=PX] [marks=T,T]` (PLAN W5 step 0, AR-06): how many of an attacking
// army ever get a shot at its target, when, and what the army does while it cannot. Read-only. A body is IN
// REACH of a target when its centre is within the reach tickCombat fights at (the weapon's range, plus a
// structure target's footprint half-extent and 24 px; `range=PX` replaces the whole reach, for a Guard's
// "within N cells of the guarded ally"). Keys, prefix reach.NAME. (ticks; -1 never):
//   n                       attackers watched
//   first / t50             first tick any / half of the attackers had been in reach
//   ever_end, ever_tM       attackers that had EVER been in reach by the end / by mark tick M
//   now_end                 attackers in reach at the end
//   out600_end / out600_peak  attackers out of reach whose centre cell had not changed for 600 ticks, at the
//                           end / the most on any tick (the audit's "standing out of range")
//   farthest_held           of those at the end, the farthest from its nearest target (cells; -1 none)
//   hold_out_max            the longest run of ticks one attacker spent out of reach in Legion's Holding state
//   hold0_max               the same while its field potential was 0 (parked ON the goal without a shot: the
//                           wall ring's "Holding at potential 0"); 0 in Retail (no navigator)
//   damage_end, damage_tM   hit points the targets lost to damage (regeneration is never credited back)
//   targets_dead_end        targets destroyed
struct ReachProbe {
    std::string name;
    std::vector<int> att, tgt;
    int range = 0;
    std::vector<int> marks;
    std::vector<int> first, lastMove, cx, cz, holdOut, hold0;
    std::vector<int64_t> tgtMaxHp, prevHp;
    int64_t dealt = 0;
    int out600 = 0, out600Peak = 0, holdOutMax = 0, hold0Max = 0;
    std::vector<std::pair<int, std::pair<int, int64_t>>> atMark;   // (mark, (ever, damage))
    int ever = 0;
    int64_t farthestHeld = -1, nowInReach = 0;
    ReachProbe(std::string n, std::vector<int> a, std::vector<int> t, int r, std::vector<int> m)
        : name(std::move(n)), att(std::move(a)), tgt(std::move(t)), range(r), marks(std::move(m)),
          first(att.size(), -1), lastMove(att.size(), 0), cx(att.size(), INT_MIN), cz(att.size(), INT_MIN),
          holdOut(att.size(), 0), hold0(att.size(), 0), tgtMaxHp(tgt.size(), 0), prevHp(tgt.size(), 0) {}
    // Hit points the targets have lost to damage, summed tick by tick (a target's regeneration is never
    // credited back, a death takes what it had left).
    int64_t damage() const { return dealt >> 16; }
    void sample(const tak::sim::World& w, const tak::sim::LegionNavigator* nav, int t) {
        for (size_t i = 0; i < tgt.size(); ++i) {
            const auto* u = w.unit(tgt[i]);
            if (!tgtMaxHp[i] && u && u->type) { tgtMaxHp[i] = u->type->maxHp; prevHp[i] = int64_t(u->hp.v); }
            const int64_t now = u && u->alive() ? int64_t(u->hp.v) : 0;
            if (now < prevHp[i]) dealt += prevHp[i] - now;
            prevHp[i] = now;
        }
        out600 = 0;
        int64_t held = -1;
        nowInReach = 0;
        for (size_t i = 0; i < att.size(); ++i) {
            const auto* u = w.unit(att[i]);
            if (!u || !u->alive() || !u->type) continue;
            const int ux = int(u->x.v >> 20), uz = int(u->z.v >> 20);
            if (ux != cx[i] || uz != cz[i]) { cx[i] = ux; cz[i] = uz; lastMove[i] = t; }
            bool in = false;
            int64_t nearest = INT64_MAX;
            for (int id : tgt) {
                const auto* v = w.unit(id);
                if (!v || !v->alive() || !v->type) continue;
                const int64_t dx = (int64_t(v->x.v) - u->x.v) >> 12, dz = (int64_t(v->z.v) - u->z.v) >> 12;   // 1/16 px
                const int64_t d2 = dx * dx + dz * dz;
                nearest = std::min(nearest, d2);
                const float pad = v->type->maxVel <= tak::sim::Fixed()
                                      ? 8.0f * float(std::max(v->type->footX, v->type->footZ)) + 24.0f : 0.0f;
                const int64_t reach = int64_t((range ? float(range) : u->type->maxRange() + pad) * 16.0f);
                in |= d2 <= reach * reach;
            }
            if (in) { ++nowInReach; if (first[i] < 0) { first[i] = t; ++ever; } }
            const int st = nav ? nav->unitState(att[i]) : 0;
            holdOut[i] = !in && st == 2 ? holdOut[i] + 1 : 0;
            hold0[i] = holdOut[i] && nav->fieldPotential(att[i], tak::sim::footprintOrigin(u->x, u->type->footX),
                                                         tak::sim::footprintOrigin(u->z, u->type->footZ)) == 0 ? hold0[i] + 1 : 0;
            holdOutMax = std::max(holdOutMax, holdOut[i]);
            hold0Max = std::max(hold0Max, hold0[i]);
            if (!in && t - lastMove[i] >= 600) {
                ++out600;
                if (nearest != INT64_MAX) held = std::max<int64_t>(held, int64_t(tak::sim::isqrt64(uint64_t(nearest)) / 16 / 16));   // 1/16 px -> cells
            }
        }
        out600Peak = std::max(out600Peak, out600);
        farthestHeld = held;
        for (int m : marks)
            if (t + 1 == m) atMark.push_back({m, {ever, damage()}});
    }
    void report(obs::Keys& k, const tak::sim::World& w) const {
        const std::string p = "reach." + name + ".";
        std::vector<int> f;
        for (int v : first) if (v >= 0) f.push_back(v);
        std::sort(f.begin(), f.end());
        k.emplace_back(p + "n", int64_t(att.size()));
        k.emplace_back(p + "first", f.empty() ? -1 : f.front());
        k.emplace_back(p + "t50", f.size() >= (att.size() + 1) / 2 ? f[(att.size() + 1) / 2 - 1] : -1);
        k.emplace_back(p + "ever_end", int64_t(ever));
        for (const auto& [m, v] : atMark) k.emplace_back(p + "ever_t" + std::to_string(m), v.first);
        k.emplace_back(p + "now_end", nowInReach);
        k.emplace_back(p + "out600_end", out600);
        k.emplace_back(p + "out600_peak", out600Peak);
        k.emplace_back(p + "farthest_held", farthestHeld);
        k.emplace_back(p + "hold_out_max", holdOutMax);
        k.emplace_back(p + "hold0_max", hold0Max);
        k.emplace_back(p + "damage_end", damage());
        for (const auto& [m, v] : atMark) k.emplace_back(p + "damage_t" + std::to_string(m), v.second);
        int64_t dead = 0;
        for (int id : tgt) if (const auto* u = w.unit(id); !u || !u->alive()) ++dead;
        k.emplace_back(p + "targets_dead_end", dead);
    }
};

// `probe claims` (PLAN W5 step 0, the claims invariant; Legion only): every 10 ticks the navigator audits
// each member's claimed arrival slot against its point's claimed cells (LegionNavigator::claimsAudit).
//   claims.audits                        audits taken
//   claims.bad_max                       the most overlaps + missing + dangling at any audit: the SAFETY key, 0
//                                        where the invariant holds
//   claims.overlaps_max / .missing_max / .dangling_max / .orphans_max   each class's worst audit
//   claims.orphans_end                   point cells no member's slot covers at the last audit (report only:
//                                        a settled body that left the navigator keeps its cells)
struct ClaimsProbe {
    int audits = 0, bad = 0, overlaps = 0, missing = 0, dangling = 0, orphans = 0, orphansEnd = 0;
    void sample(const tak::sim::LegionNavigator* nav, int t) {
        if (!nav || t % 10) return;
        const auto a = nav->claimsAudit();
        ++audits;
        bad = std::max(bad, a.overlaps + a.missing + a.dangling);
        overlaps = std::max(overlaps, a.overlaps); missing = std::max(missing, a.missing);
        dangling = std::max(dangling, a.dangling); orphans = std::max(orphans, a.orphans);
        orphansEnd = a.orphans;
    }
    void report(obs::Keys& k) const {
        k.emplace_back("claims.audits", audits);
        k.emplace_back("claims.bad_max", bad);
        k.emplace_back("claims.overlaps_max", overlaps);
        k.emplace_back("claims.missing_max", missing);
        k.emplace_back("claims.dangling_max", dangling);
        k.emplace_back("claims.orphans_max", orphans);
        k.emplace_back("claims.orphans_end", orphansEnd);
    }
};

// `creep NAME GROUPS` (PLAN W5 step 0, MV-07): the goal-area creep counter. The ticks the listed groups' ordered
// members spend INSIDE their click's arrival disc (the observer's packed-disc limit, 16*(foot+1)*sqrt(N/pi)*1.25 px
// round the click) stepping at no more than a eighth of their own speed (cap/8 + 1/64 px) without having arrived:
// the slow shuffle that ends the audit's staticblock (members creeping at speed ~21 of 163-197 px/s). Read-only.
//   creep.NAME.ticks         member-ticks inside the disc with orders and a step in (0, cap/8]
//   creep.NAME.inside_ticks  member-ticks inside the disc with orders, stepping or not
struct CreepProbe {
    std::string name;
    struct Body { int id; int64_t cx, cz, r2; int32_t px = INT32_MIN, pz = INT32_MIN; };
    std::vector<Body> bodies;
    int64_t creep = 0, inside = 0;
    void sample(const tak::sim::World& w) {
        for (auto& b : bodies) {
            const auto* u = w.unit(b.id);
            if (!u || !u->alive() || !u->type) continue;
            const int64_t dx = (int64_t(u->x.v) >> 16) - b.cx, dz = (int64_t(u->z.v) >> 16) - b.cz;
            const bool in = dx * dx + dz * dz <= b.r2;
            if (in && !u->orders.empty()) {
                ++inside;
                if (b.px != INT32_MIN) {
                    const int64_t sx = int64_t(u->x.v) - b.px, sz = int64_t(u->z.v) - b.pz;
                    const int64_t cap = int64_t(u->baseSpeed.v) / 8 + 65536 / 64;
                    const int64_t step2 = sx * sx + sz * sz;
                    if (step2 > 0 && step2 <= cap * cap) ++creep;
                }
            }
            b.px = u->x.v; b.pz = u->z.v;
        }
    }
    void report(obs::Keys& k) const {
        k.emplace_back("creep." + name + ".ticks", creep);
        k.emplace_back("creep." + name + ".inside_ticks", inside);
    }
};

struct Run {
    std::string skipped;
    uint64_t hash = 0, digest = 0;
    obs::Keys keys;
    double ms = 0;
    // Wall time of World::tick alone (the observer's sampling left out), per tick: the slowest tick, the
    // tick it was, and the nearest-rank 99th percentile. stderr only, like `ms`: never in the JSON.
    double tickMaxMs = 0, tickP99Ms = 0;
    uint32_t tickMaxAt = 0;
};

// The tick-time line printed after each run's whole-run time (stderr): tick_max_ms=M@T tick_p99_ms=P.
std::string tickTimes(const Run& r) {
    char b[96];
    std::snprintf(b, sizeof b, "tick_max_ms=%.3f@%u tick_p99_ms=%.3f", r.tickMaxMs, r.tickMaxAt, r.tickP99Ms);
    return b;
}

Run runOnce(const scn::Scenario& s, PathfindingMode mode, int offset, bool serial, bool observe,
            const Options& opt) {
    Run r;
    scn::BuildOptions bo;
    bo.mode = mode;
    bo.serial = serial;
    bo.offset = offset;
    bo.data = opt.data;
    auto b = scn::build(s, bo);
    if (!b->skipped.empty()) { r.skipped = b->skipped; return r; }
    auto& w = *b->world;
    if (!opt.traceGroups.empty()) {   // --trace with group names: the ids exist only now
        std::vector<int> ids = opt.trace;
        for (const auto& name : opt.traceGroups)
            if (const auto g = b->groups.find(name); g != b->groups.end()) ids.insert(ids.end(), g->second.begin(), g->second.end());
        tak::sim::LegionNavigator::setTrace(ids);
    }
    scn::OrderFeed feed(s, *b, opt.window);
    std::optional<obs::Observer> o;
    obs::NavWork navWork;
    std::vector<int> all;
    for (const auto& g : s.groups)
        for (int id : b->groups.at(g.name)) all.push_back(id);
    const auto convoys = directiveConvoys(s);
    if (observe) {
        o.emplace(configFor(s, *b));
        if (s.map.kind == scn::MapSpec::Ascii || s.map.kind == scn::MapSpec::Flat)
            o->setStaticBlocked(wallMask(s, b->width, b->height), b->width, b->height);
    }
    // The navigator is made on the world's first Legion tick: re-read it.
    const tak::sim::LegionNavigator* nav = nullptr;
    std::optional<ApproachProbe> approach;
    if (observe && s.probeApproach) approach.emplace(all);
    // `probe scale` / --scale (tools/legion_scale.h): each order's packed disc (the observer's, sized for every
    // body of the convoy the click joined, at the selection's largest footprint) and the bodies it selects.
    std::optional<tak::legion_scale::ScaleProbe> scale;
    std::vector<int64_t> scaleR2(s.orders.size(), 0);
    if (observe && (s.probeScale || opt.scale)) {
        scale.emplace(all);
        std::map<uint32_t, int64_t> convoyN;
        std::vector<int> foot(s.orders.size(), 1), count(s.orders.size(), 0);
        for (size_t i = 0; i < s.orders.size(); ++i) {
            for (int id : feed.selection(s.orders[i]))
                if (const auto* u = w.unit(id); u && u->type) {
                    foot[i] = std::max({foot[i], u->type->footX, u->type->footZ});
                    ++count[i];
                }
            if (convoys[i]) convoyN[convoys[i]] += count[i];
        }
        for (size_t i = 0; i < s.orders.size(); ++i)
            scaleR2[i] = obs::detail::discRadius2(foot[i], int(std::max<int64_t>(1, convoys[i] ? convoyN[convoys[i]] : count[i])));
    }
    std::vector<CreepProbe> creeps;
    if (observe)
        for (const auto& sh : s.shapes) {
            if (sh.kind != "creep") continue;
            CreepProbe cp;
            cp.name = sh.name;
            const auto cfg = configFor(s, *b);
            for (const auto& g : cfg.groups) {
                size_t at = 0;
                bool listed = false;
                const std::string names = expandGroups(s, sh.a);
                while (at <= names.size()) {
                    size_t e = names.find(',', at);
                    if (e == std::string::npos) e = names.size();
                    listed |= names.substr(at, e - at) == g.name;
                    at = e + 1;
                }
                if (!listed) continue;
                int foot = 1;
                for (int id : g.ids) if (const auto* u = w.unit(id); u && u->type) foot = std::max({foot, u->type->footX, u->type->footZ});
                const int n = g.discN ? g.discN : int(g.ids.size());
                const int64_t r2 = obs::detail::discRadius2(foot, n);
                for (int id : g.ids) cp.bodies.push_back({id, g.clickX, g.clickZ, r2});
            }
            if (cp.bodies.empty()) throw std::runtime_error(s.origin + ": creep '" + sh.name + "' names no group in '" + sh.a + "'");
            creeps.push_back(std::move(cp));
        }
    std::vector<obs::BehindProbe> behinds;
    if (observe)
        for (const auto& sh : s.shapes) {
            if (sh.kind != "behind") continue;
            obs::BehindProbe bp;
            bp.name = sh.name;
            if (sh.cells) bp.edge = int64_t(sh.cells) * 16;
            const auto cfg = configFor(s, *b);
            auto listed = [&](const std::string& list, const std::string& group) {
                size_t at = 0;
                const std::string names = expandGroups(s, list);
                while (at <= names.size()) {
                    size_t e = names.find(',', at);
                    if (e == std::string::npos) e = names.size();
                    if (names.substr(at, e - at) == group) return true;
                    at = e + 1;
                }
                return false;
            };
            for (const auto& g : cfg.groups) {
                if (listed(sh.a, g.name)) for (int id : g.ids) bp.late.push_back({id, g.clickX, g.clickZ});
                if (listed(sh.b, g.name)) bp.early.insert(bp.early.end(), g.ids.begin(), g.ids.end());
            }
            if (bp.late.empty() || bp.early.empty())
                throw std::runtime_error(s.origin + ": behind '" + sh.name + "' names no group in '" + sh.a + "' or '" + sh.b + "'");
            behinds.push_back(std::move(bp));
        }
    std::vector<ReachProbe> reach;
    std::optional<ClaimsProbe> claims;
    if (observe && s.probeClaims) claims.emplace();
    if (observe)
        for (const auto& sh : s.shapes) {
            if (sh.kind != "reach") continue;
            auto idsOf = [&](const std::string& list) {
                std::vector<int> out;
                const std::string names = expandGroups(s, list);
                size_t at = 0;
                while (at <= names.size()) {
                    size_t e = names.find(',', at);
                    if (e == std::string::npos) e = names.size();
                    const std::string item = names.substr(at, e - at);
                    at = e + 1;
                    if (item.empty() || !s.group(item))
                        throw std::runtime_error(s.origin + ": reach '" + sh.name + "' names no group in '" + list + "'");
                    const auto& g = b->groups.at(item);
                    out.insert(out.end(), g.begin(), g.end());
                }
                return out;
            };
            reach.emplace_back(sh.name, idsOf(sh.a), idsOf(sh.b), sh.range, sh.marks);
        }
    int64_t commands = 0, orders = 0, lastCommand = -1, groupsAfter = -1, groupsPeak = 0;
    double applyMaxMs = 0;   // the longest tick of applyCommand calls (membership application; stderr only)
    uint32_t applyMaxAt = 0;
    std::vector<int> ids;
    auto legionGroups = [&]() -> int64_t {
        ids.clear();
        for (int id : all)
            if (const auto* u = w.unit(id); u && u->alive() && !u->orders.empty())
                if (const int g = nav->unitGroup(id); g) ids.push_back(g);
        std::sort(ids.begin(), ids.end());
        return std::unique(ids.begin(), ids.end()) - ids.begin();
    };
    // A harvested situation (`truth`): the recording's body positions TICK ticks in, against ours.
    const auto startPos = s.truths.empty() ? std::vector<std::pair<int32_t, int32_t>>{} : scn::startPositions(s, *b);
    std::vector<std::pair<uint32_t, scn::TruthReport>> truthNow;   // (tick, report) per `truth` line the run reaches
    // TAK_ORDER_STATS=N: log the movers' order-queue shape every N ticks (src/client/ordershape.h; compare a harvested situation with its recording).
    const uint32_t orderStatsEvery = std::getenv("TAK_ORDER_STATS") ? uint32_t(std::strtoul(std::getenv("TAK_ORDER_STATS"), nullptr, 10)) : 0;
    std::vector<double> tickMs, wholeMs;
    tickMs.reserve(s.ticks);
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t t = 0; t < s.ticks; ++t) {
        // contact_settled's command identity (W3-1, ruling W3 round 4 (1)): one convoy is one command,
        // whatever the uplink splits it into and however many selections it merged; a shift-queued or
        // appended order keeps its units' command.
        if (o)
            for (const auto& od : s.orders)
                if (od.tick == t && !od.queue && !od.append)
                    o->noteSelection(feed.selection(od), convoys[size_t(&od - s.orders.data())]);
        if (scale)
            for (const auto& od : s.orders) {
                if (od.tick != t || od.queue || od.append) continue;
                if (od.verb == scn::Verb::Stop) {
                    for (int id : feed.selection(od)) scale->cancel(id);
                    continue;
                }
                const bool pointVerb = od.verb == scn::Verb::Move || od.verb == scn::Verb::Fight || od.verb == scn::Verb::Patrol;
                if (!pointVerb && od.verb != scn::Verb::Attack && od.verb != scn::Verb::Guard) continue;
                int64_t px = int64_t(std::lround(double(od.x) * 16)), pz = int64_t(std::lround(double(od.z) * 16));
                bool point = pointVerb && od.point;
                if (pointVerb && !od.point && !od.target.empty()) {   // @GROUP: its centroid now, as the feed clicks it
                    int64_t sx = 0, sz = 0, n = 0;
                    for (int id : b->groups.at(od.target))
                        if (const auto* u = w.unit(id); u && u->alive()) { sx += u->x.v >> 16; sz += u->z.v >> 16; ++n; }
                    if (n) { px = sx / n; pz = sz / n; point = true; }
                }
                const int64_t r2 = scaleR2[size_t(&od - s.orders.data())];
                for (int id : feed.selection(od)) scale->click(w, id, int(t), point, px, pz, r2);
            }
        const size_t landed = feed.apply(t);
        if (feed.lastApplyMs() > applyMaxMs) { applyMaxMs = feed.lastApplyMs(); applyMaxAt = t; }
        if (landed) {
            commands += int64_t(feed.landedUnits().size());   // unit commands: an explicit order's members each
            for (const auto& c : feed.landed()) orders += c.kind == tak::net::Cmd::Order;
            lastCommand = t;
        }
        if (scale) {
            // A click is delivered when its own command lands (the uplink is one FIFO: an earlier order or a
            // squad press still draining lands before it, a queued leg of the same tick after it).
            for (const auto& [id, n] : feed.clicks()) scale->expect(id, n);
            for (const int id : feed.landedUnits()) scale->landed(w, id, int(t));
        }
        if (opt.catchup) {
            if (t == 0) w.setPathBudget(1 << 28);
            else if (t == opt.catchup) w.setPathBudget(tak::sim::kPathBudgetDefault);
        }
        const auto tickStart = std::chrono::steady_clock::now();
        w.tick(1.f / 30);
        tickMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tickStart).count());
        wholeMs.push_back(tickMs.back() + feed.lastApplyMs());   // the whole sim tick: its commands applied, then World::tick
        nav = w.legionNavigator();
        if (opt.dumpUnits && t % opt.dumpUnits == opt.dumpUnits - 1)
            // --dump-units: every live unit's position (px), footprint, Legion state and group, steering site and
            // first order's point (px, -1 none), for rendering a tick (observation only; reads, never writes).
            for (const auto& u : w.units()) {
                if (!u.alive() || !u.type) continue;
                const bool any = !u.orders.empty();
                int32_t sx = -1, sz = -1;   // the member's goal footprint centre (its slot), raw Fixed
                if (!nav || !nav->goalPoint(u.id, sx, sz)) sx = sz = -(1 << 16);
                std::fprintf(stderr, "DUMP t=%u id=%d p=%d x=%d z=%d f=%d st=%d g=%d site=%d gx=%d gz=%d sx=%d sz=%d\n", t, u.id,
                             u.player, int(u.x.v >> 16), int(u.z.v >> 16), u.type->footX, nav ? nav->unitState(u.id) : 0,
                             nav ? nav->unitGroup(u.id) : 0, nav ? nav->steerSite(u.id) : -1,
                             any ? int(u.orders.front().x.v >> 16) : -1, any ? int(u.orders.front().z.v >> 16) : -1, sx >> 16, sz >> 16);
            }
        if (o) {
            navWork.sample(*o, w);
            o->sample(w, t);
            if (approach) approach->sample(w, nav, int(t));
            if (scale) scale->sample(w, nav, int(t));
            for (auto& rp : reach) rp.sample(w, nav, int(t));
            for (auto& cp : creeps) cp.sample(w);
            for (auto& bp : behinds) bp.sample(w);
            if (claims) claims->sample(nav, int(t));
            if (nav) {
                const int64_t n = legionGroups();
                groupsPeak = std::max(groupsPeak, n);
                if (lastCommand == int64_t(t)) groupsAfter = n;
            }
        }
        for (const auto& tr : s.truths)
            if (t + 1 == tr.tick && tr.pos.size() == all.size())
                truthNow.push_back({tr.tick, scn::truthReport(s, *b, tr, startPos, offset,
                                                              std::getenv("LEGION_TRUTH_DETAIL") ? "" : nullptr)});
        if (orderStatsEvery && (t + 1) % orderStatsEvery == 0)
            std::fprintf(stderr, "order shape at +%u: %s\n", t + 1, tak::situation::orderShape(w).c_str());
        if ((t + 1) % 100 == 0) r.digest = mix(r.digest, w.stateHash());
    }
    r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (!tickMs.empty()) {
        const auto top = std::max_element(tickMs.begin(), tickMs.end());
        r.tickMaxMs = *top;
        r.tickMaxAt = uint32_t(top - tickMs.begin());
        const size_t at = (tickMs.size() * 99 + 99) / 100 - 1;   // nearest rank, as the observer's work p99
        std::nth_element(tickMs.begin(), tickMs.begin() + ptrdiff_t(at), tickMs.end());
        r.tickP99Ms = tickMs[at];
        if (scale) {   // the scale suite's wall line (stderr only, never in the JSON; tools/legion_scale.py reads it)
            double sum = 0;
            int over = 0;
            for (double v : tickMs) { sum += v; over += v > 1000.0 / 30; }
            const size_t mid = (tickMs.size() + 1) / 2 - 1;
            std::nth_element(tickMs.begin(), tickMs.begin() + ptrdiff_t(mid), tickMs.end());
            // The whole sim tick (apply + World::tick): its p99, max and ticks over 33.3 ms. Legion's
            // deterministic NavBytes and its capacity estimate (Stats::bytes) at the end, beside the
            // process heap, so the allocator's overhead can be read off (stage 4).
            const auto wtop = std::max_element(wholeMs.begin(), wholeMs.end());
            const double wholeMax = *wtop;
            const uint32_t wholeAt = uint32_t(wtop - wholeMs.begin());
            int wholeOver = 0;
            for (double v : wholeMs) wholeOver += v > 1000.0 / 30;
            std::nth_element(wholeMs.begin(), wholeMs.begin() + ptrdiff_t(at), wholeMs.end());
            const double wholeP99 = wholeMs[at];
            long long navDet = -1, navCap = -1;
            if (nav) { navDet = (long long)nav->accounting().bytes.total(); navCap = (long long)nav->stats().bytes; }
            std::fprintf(stderr, "SCALE_WALL %s %s offset=%d %s ticks=%zu tick_mean_ms=%.3f tick_p50_ms=%.3f "
                         "tick_p99_ms=%.3f tick_max_ms=%.3f@%u over_budget=%d rss_peak_kb=%lld heap_kb=%lld "
                         "apply_max_ms=%.3f@%u whole_p99_ms=%.3f whole_max_ms=%.3f@%u whole_over_budget=%d "
                         "navbytes=%lld nav_capacity_bytes=%lld\n",
                         s.name.c_str(), mode == PathfindingMode::Legion ? "legion" : "retail", offset,
                         serial ? "serial" : "workers", tickMs.size(), sum / double(tickMs.size()), tickMs[mid],
                         r.tickP99Ms, r.tickMaxMs, r.tickMaxAt, over, tak::legion_scale::peakRssKb(),
                         tak::legion_scale::heapKb(), applyMaxMs, applyMaxAt, wholeP99, wholeMax, wholeAt, wholeOver,
                         navDet, navCap);
        }
    }
    r.hash = w.stateHash();
    r.digest = mix(r.digest, r.hash);
    if (nav && (scale || opt.navpar || opt.parForce || opt.parPerturb)) {   // stage 9's parallel jobs (stderr only: not the same on every worker count)
        const auto p = nav->parallelStats();
        std::fprintf(stderr, "NAVPAR %s offset=%d %s picked=%llu staged=%llu forks=%llu jobs=%llu used=%llu unused=%llu "
                     "used_work=%llu unused_work=%llu keys_reused=%llu serial_work=%llu waits=%llu staged_bytes_peak=%llu "
                     "plane_rows=%llu worker_jobs=%llu fork_ms=%.1f wait_ms=%.1f join_ms=%.1f\n", s.name.c_str(), offset,
                     serial ? "serial" : "workers", (unsigned long long)p.picked, (unsigned long long)p.staged,
                     (unsigned long long)p.forks, (unsigned long long)p.jobs, (unsigned long long)p.used,
                     (unsigned long long)p.unused, (unsigned long long)p.usedWork, (unsigned long long)p.unusedWork,
                     (unsigned long long)p.keysReused, (unsigned long long)p.serialWork, (unsigned long long)p.waits,
                     (unsigned long long)p.stagedBytesPeak, (unsigned long long)p.planeRows, (unsigned long long)p.workerJobs,
                     double(p.forkNs) / 1e6, double(p.waitNs) / 1e6, double(p.joinNs) / 1e6);
    }
    if (nav && opt.routeIdx) {   // perf follow-up I: how static changes found their routes (stderr only, observation)
        const auto x = nav->routeIndexStats();
        const auto a = nav->accounting();
        std::fprintf(stderr, "ROUTEIDX %s offset=%d %s index=%d calls=%llu whole_calls=%llu indexed=%llu walk_groups=%llu "
                     "candidates=%llu touched=%llu decisions=%016llx routes_closed=%llu routes_opened=%llu group_loop_iters=%llu "
                     "lists=%llu unlists=%llu entries_added=%llu queries=%llu bins_scanned=%llu entries_scanned=%llu "
                     "stale_dropped=%llu compactions=%llu compacted=%llu entries_peak=%llu live_entries=%llu bytes=%llu\n",
                     s.name.c_str(), offset, serial ? "serial" : "workers", int(opt.routeIndex), (unsigned long long)x.calls,
                     (unsigned long long)x.wholeCalls, (unsigned long long)x.indexed, (unsigned long long)x.walkGroups,
                     (unsigned long long)x.candidates, (unsigned long long)x.touched, (unsigned long long)x.decisions,
                     (unsigned long long)a.routesClosed, (unsigned long long)a.routesOpened,
                     (unsigned long long)nav->stats().groupLoopIters, (unsigned long long)x.lists, (unsigned long long)x.unlists,
                     (unsigned long long)x.entriesAdded, (unsigned long long)x.queries, (unsigned long long)x.binsScanned,
                     (unsigned long long)x.entriesScanned, (unsigned long long)x.staleDropped, (unsigned long long)x.compactions,
                     (unsigned long long)x.compacted, (unsigned long long)x.entriesPeak, (unsigned long long)x.liveEntries,
                     (unsigned long long)x.bytes);
    }
    if (o) {
        r.keys = o->report();
        if (approach) approach->report(r.keys, mode == PathfindingMode::Legion);
        if (scale && nav) scale->account(*nav);
        if (scale) scale->report(r.keys, w, mode == PathfindingMode::Legion);
        for (const auto& rp : reach) rp.report(r.keys, w);
        for (const auto& cp : creeps) cp.report(r.keys);
        for (const auto& bp : behinds) bp.report(r.keys);
        if (claims && mode == PathfindingMode::Legion) claims->report(r.keys);
        r.keys.emplace_back("commands", commands);
        r.keys.emplace_back("orders", orders);
        r.keys.emplace_back("last_command_tick", lastCommand);
        // The convoy lookups' declared per-order bounds (PLAN 3.0: p99 <= 16,
        // max <= 64 tests per order), which per-tick work.convoy_tests cannot
        // show: the most for one order, orders over 16 (per mille, rounded
        // up) and orders over 64. Legion only (Retail keeps no convoys).
        if (mode == PathfindingMode::Legion) {
            const auto c = w.convoyStats();
            r.keys.emplace_back("convoy.orders", int64_t(c.orders));
            r.keys.emplace_back("convoy.tests_max", int64_t(c.testsMax));
            r.keys.emplace_back("convoy.over16_permille",
                                c.orders ? int64_t((c.over16 * 1000 + c.orders - 1) / c.orders) : 0);
            r.keys.emplace_back("convoy.over64", int64_t(c.over64));
        }
        for (const auto& [tick, tr] : truthNow) {
            const std::string pre = "truth.t" + std::to_string(tick) + ".";
            r.keys.emplace_back(pre + "n", tr.n);
            r.keys.emplace_back(pre + "within2_permille", tr.within2Permille());
            r.keys.emplace_back(pre + "moved_n", tr.movedN);
            r.keys.emplace_back(pre + "moved_within2_permille", tr.movedWithin2Permille());
        }
        if (nav) {
            // W4 step 0 instruments: the two running-maximum gauges and total Legion work per
            // 1500-tick bin (churn flatness, B1's gate).
            r.keys.emplace_back("gauge.still_per_residue_max", int64_t(navWork.stillPerResidueMax()));
            r.keys.emplace_back("gauge.quota_peg_run_max", int64_t(navWork.quotaPegRunMax()));
            // W5 step 0: the bodies tickCombat braked in reach (the ones W5's Engaged flag will mark).
            r.keys.emplace_back("gauge.engaged_max", int64_t(navWork.engagedMax()));
            r.keys.emplace_back("engaged.member_ticks", int64_t(navWork.engagedMemberTicks()));
            for (size_t i = 0; i < navWork.churnBins().size(); ++i)
                r.keys.emplace_back("churn.bin" + std::to_string(i), int64_t(navWork.churnBins()[i]));
            if (s.probeAware) {   // W7 step 0: the awareness / parting / give-way counters (Stats, never hashed)
                for (const auto& [name, v] : navWork.w7())   // aware_builds -> aware.builds, parts -> aware.parts
                    r.keys.emplace_back("aware." + (name.rfind("aware_", 0) == 0 ? name.substr(6) : name), int64_t(v));
                r.keys.emplace_back("aware.work_max", int64_t(navWork.awareWorkMax()));
            }
            if (s.probeW9 || opt.w9) {   // W9 step 0: the mechanism counters (0 until their step) and the debug census
                for (const auto& [name, v] : navWork.w9())
                    r.keys.emplace_back("w9." + name, int64_t(v));
                if (const auto* ln = const_cast<tak::sim::World&>(w).legionNavigator())
                    for (const auto& [name, v] : ln->census())
                        r.keys.emplace_back("census." + name, int64_t(v));
                if (o) {   // the tails (members not arrived at the end), so a second run can trace them
                    for (size_t g = 0; g < o->groupCount(); ++g) {
                        const auto ids = o->leftBehind(w, g);
                        if (ids.empty()) continue;
                        std::string list;
                        for (int id : ids) list += (list.empty() ? "" : ",") + std::to_string(id);
                        std::fprintf(stderr, "TAIL %s group %s offset=%d: %zu not arrived: %s\n", s.name.c_str(),
                                     o->groupName(g).c_str(), offset, ids.size(), list.c_str());
                    }
                }
            }
            if (s.probeLanes) {   // W8 step 0: pivot_work is the base steering work, lane_work / lane_clipped stay 0 until S1
                for (const auto& [name, v] : navWork.w8())   // pivot_work -> lanes.pivot_work
                    r.keys.emplace_back("lanes." + name, int64_t(v));
            }
            r.keys.emplace_back("legion_groups", groupsAfter);
            r.keys.emplace_back("legion_groups_peak", groupsPeak);
        }
        for (const auto& sh : s.shapes) {
            if (sh.kind != "region") continue;
            const obs::Rect box{cellLo(sh.x0), cellLo(sh.z0), int(std::ceil(sh.x1)) - 1, int(std::ceil(sh.z1)) - 1};
            int64_t inside = 0;
            for (int id : all)
                if (const auto* u = w.unit(id); u && u->alive())
                    inside += box.contains(int(u->x.v >> 20), int(u->z.v >> 20));
            r.keys.emplace_back("region." + sh.name + ".inside", inside);
        }
    }
    return r;
}

std::string execName(Options::Exec e) {
    return e == Options::Serial ? "serial" : e == Options::Workers ? "workers" : "both";
}

std::vector<int> parseOffsets(const std::string& list) {
    std::vector<int> out;
    size_t at = 0;
    while (at <= list.size()) {
        const size_t comma = std::min(list.find(',', at), list.size());
        const std::string item = list.substr(at, comma - at);
        char* end = nullptr;
        const long v = std::strtol(item.c_str(), &end, 10);
        if (item.empty() || *end || v < -64 || v > 64) usage(("bad offset '" + item + "'").c_str());
        out.push_back(int(v));
        at = comma + 1;
    }
    return out;
}

int runFile(const std::string& file, Options opt);

// Hand the captured JSON lines to tools/legion_check.py (the one place the
// gate logic lives) and return its exit status.
int runChecker(const Options& opt, const std::string& lines) {
#ifdef _WIN32
    (void)opt; (void)lines;
    std::fprintf(stderr, "legion_scenario: gate modes need python3 and a POSIX shell\n");
    return 2;
#else
    namespace fs = std::filesystem;
    fs::path script = fs::path(__FILE__).parent_path() / "legion_check.py";
    if (!fs::exists(script)) script = fs::path("tools") / "legion_check.py";
    if (!fs::exists(script)) {
        std::fprintf(stderr, "legion_scenario: tools/legion_check.py not found\n");
        return 2;
    }
    fs::path tmp = fs::temp_directory_path() /
                   ("legion_check_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                    ".jsonl");
    {
        std::ofstream f(tmp);
        f << lines;
        if (!f) {
            std::fprintf(stderr, "legion_scenario: cannot write %s\n", tmp.c_str());
            return 2;
        }
    }
    auto q = [](const std::string& x) {
        std::string o = "'";
        for (char c : x) o += c == '\'' ? std::string("'\\''") : std::string(1, c);
        return o + "'";
    };
    std::string cmd = "python3 " + q(script.string()) + " " + opt.gate;
    if (opt.gate == "exit-table") cmd += " --base " + q(opt.gateFile);
    else if (opt.gate == "anchor") cmd += " --anchor " + q(opt.gateFile);
    else cmd += " --baseline " + q(opt.gateFile);
    cmd += " --results " + q(tmp.string());
    for (const auto& a : opt.gateArgs) cmd += " " + q(a);
    std::fflush(stdout);
    const int rc = std::system(cmd.c_str());
    std::error_code ec;
    fs::remove(tmp, ec);
    if (rc == -1) return 2;
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : 2;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) usage((a + " needs a value").c_str());
            return argv[++i];
        };
        if (a == "--mode") {
            const auto v = value();
            if (v == "legion") opt.modes = {PathfindingMode::Legion};
            else if (v == "retail") opt.modes = {PathfindingMode::Retail};
            else if (v == "both") opt.modes = {PathfindingMode::Legion, PathfindingMode::Retail};
            else usage("--mode legion|retail|both");
        } else if (a == "--offsets") {
            const auto v = value();
            opt.offsetsGiven = true;
            opt.gateOffsets = v == "gate";
            if (!opt.gateOffsets) opt.offsets = parseOffsets(v);
        }
        else if (a == "--workers") opt.exec = Options::Workers;
        else if (a == "--serial") opt.exec = Options::Serial;
        else if (a == "--both-exec") opt.exec = Options::Both;
        else if (a == "--window") {
            const auto v = value();
            opt.window = std::atoi(v.c_str());
            if (opt.window < 1 || opt.window > 600) usage("--window R takes 1..600 ticks");
        } else if (a == "--data") { static std::string d; d = value(); opt.data = d.c_str(); }
        else if (a == "--json") opt.json = true;
        else if (a == "--scale") opt.scale = true;
        else if (a == "--no-vertex-probe") opt.vertexProbe = false;
        else if (a == "--budget-div") opt.budgetDiv = std::max(1, std::atoi(value().c_str()));
        else if (a == "--field-bytes") opt.fieldBytes = std::strtoull(value().c_str(), nullptr, 10);
        else if (a == "--pause-all") opt.pauseAll = true;
        else if (a == "--sector") opt.sector = std::atoi(value().c_str());
        else if (a == "--no-hier-routes") opt.hierRoutes = false;
        else if (a == "--par-force") opt.parForce = true;
        else if (a == "--navpar") opt.navpar = true;
        else if (a == "--no-route-index") opt.routeIndex = false;
        else if (a == "--routeidx") opt.routeIdx = true;
        else if (a == "--par-perturb") opt.parPerturb = std::strtoull(value().c_str(), nullptr, 10);
        else if (a == "--dump-units") opt.dumpUnits = uint32_t(std::max(0L, std::atol(value().c_str())));
        else if (a == "--ticks") {
            const long v = std::atol(value().c_str());
            if (v < 1 || v > 10'000'000) usage("--ticks N takes 1..10000000");
            opt.ticks = uint32_t(v);
        } else if (a == "--wanderers") {
            const auto v = value();
            if (v != "on" && v != "off") usage("--wanderers on|off");
            opt.wanderers = v == "on" ? 1 : 0;
        } else if (a == "--check" || a == "--baseline" || a == "--exit-table" || a == "--anchor") {
            if (!opt.gate.empty()) usage("one of --check, --baseline, --exit-table, --anchor");
            opt.gate = a == "--check" ? "check" : a == "--baseline" ? "baseline" : a == "--exit-table" ? "exit-table" : "anchor";
            opt.gateFile = value();
        } else if (a == "--step" || a == "--reason" || a == "--since" || a == "--ratchet" || a == "--intended" ||
                   a == "--intended-file" || a == "--cluster" || a == "--keys") {
            opt.gateArgs.push_back(a);
            opt.gateArgs.push_back(value());
        } else if (a == "--cumulative" || a == "--require-all" || a == "--hashes") {
            opt.gateArgs.push_back(a);
        } else if (a == "--catchup") {
            const long v = std::atol(value().c_str());
            if (v < 0 || v > 100000) usage("--catchup N takes 0..100000 ticks");
            opt.catchup = uint32_t(v);
        } else if (a == "--w9") opt.w9 = true;
        else if (a == "--trace") {
            const auto v = value();
            for (size_t pos = 0; pos < v.size();) {
                const size_t comma = v.find(',', pos);
                const std::string tok = v.substr(pos, comma == std::string::npos ? comma : comma - pos);
                if (tok == "all") opt.trace.push_back(-1);
                else if (!tok.empty() && std::isdigit(static_cast<unsigned char>(tok[0]))) opt.trace.push_back(std::atoi(tok.c_str()));
                else if (!tok.empty()) opt.traceGroups.push_back(tok);
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        } else if (a == "--no-observer") opt.observer = false;
        else if (a == "--neutral") opt.neutral = true;
        else if (a == "-h" || a == "--help") usage("help");
        else if (!a.empty() && a[0] == '-') usage(("unknown option " + a).c_str());
        else opt.files.push_back(a);
    }
    if (opt.files.empty()) usage("no scenario file");
    const bool gate = !opt.gate.empty();
    if (opt.files.size() > 1 && !opt.neutral && !gate) usage("one scenario file (several only with --neutral or a gate mode)");
    if (!opt.trace.empty() && opt.traceGroups.empty()) tak::sim::LegionNavigator::setTrace(opt.trace);   // debug builds; a no-op in release
    tak::sim::LegionNavigator::setInstrumentProbes(opt.vertexProbe);   // the lanes.vertex_* keys (Stats only)
    tak::sim::LegionNavigator::setBudgetDivisorForTest(opt.budgetDiv);
    tak::sim::LegionNavigator::setPauseAllForTest(opt.pauseAll);
    if (opt.sector) tak::sim::LegionNavigator::setSectorSizeForTest(opt.sector);
    tak::sim::LegionNavigator::setHierarchicalRoutesForTest(opt.hierRoutes);
    tak::sim::LegionNavigator::setRouteIndexForTest(opt.routeIndex);
    if (opt.parForce || opt.parPerturb) tak::sim::LegionNavigator::setParallelForTest(opt.parForce, opt.parPerturb);
    if (gate && opt.neutral) usage("--neutral and a gate mode do not combine");
    for (size_t i = 0; i + 1 < opt.gateArgs.size(); ++i)
        if (opt.gateArgs[i] == "--ratchet" && opt.gate != "check") usage("--ratchet goes with --check");
    std::string capture;
    if (gate) { opt.json = true; g_capture = &capture; }
    if ((opt.gate == "check" || opt.gate == "baseline") && !opt.offsetsGiven) opt.gateOffsets = true;
    if (opt.gateOffsets) opt.offsets = {0, 1, -1, 2, -2, 3, -3, 4, -4, 5, -5};   // legion_check.py WIDE_OFFSETS
    int status = 0, skipped = 0;
    for (const auto& file : opt.files) {
        const int one = runFile(file, opt);
        if (one == 77) { ++skipped; continue; }   // data-gated: the others still run
        if (one) status = status ? status : one;
    }
    if (gate) {
        g_capture = nullptr;
        if (status == 2) return 2;
        const int verdict = runChecker(opt, capture);
        return verdict ? verdict : status;
    }
    return status ? status : skipped == int(opt.files.size()) ? 77 : 0;
}

namespace {
int runFile(const std::string& file, Options opt) {
    scn::Scenario s;
    try {
        if (file.rfind("builtin:", 0) == 0) {
            const auto it = kBuiltins.find(file.substr(8));
            if (it == kBuiltins.end()) usage(("no " + file).c_str());
            s = scn::parse(it->second, file);
        } else {
            s = scn::load(file);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "legion_scenario: %s\n", e.what());
        return 2;
    }
    if (opt.ticks) s.ticks = opt.ticks;
    if (opt.gateOffsets && !s.gateOffsets.empty()) opt.offsets = s.gateOffsets;   // the eleven leave its map
    if (opt.wanderers >= 0) s.wanderers = opt.wanderers == 1;
    // The field byte allowance: --field-bytes, else the file's `navbytes`, else the game's (per file:
    // a gate run loads several files in one process).
    tak::sim::LegionNavigator::setFieldBytesForTest(opt.fieldBytes ? opt.fieldBytes : s.navBytes);
    const std::string name = !s.name.empty() ? s.name : std::filesystem::path(file).stem().string();
    if (const auto why = s.needsData(); !why.empty() && !opt.data) {
        if (opt.json) emit("{\"scenario\":\"" + name + "\",\"skipped\":\"needs --data: " + why + "\"}");
        else std::printf("%s: SKIPPED (needs --data: %s)\n", name.c_str(), why.c_str());
        return 77;
    }
    const int window = opt.window != -2 ? opt.window : s.roundTrip;
    opt.window = window;
    int units = 0;
    for (const auto& g : s.groups) units += g.count;

    int status = 0;
    try {
        if (opt.neutral) {
            for (auto mode : opt.modes)
                for (int off : opt.offsets) {
                    struct V { bool serial, observe; Run r; };
                    std::vector<V> v{{true, true, {}}, {true, false, {}}, {false, true, {}}, {false, false, {}}};
                    for (auto& x : v) x.r = runOnce(s, mode, off, x.serial, x.observe, opt);
                    bool same = true;
                    for (const auto& x : v) same &= x.r.hash == v[0].r.hash && x.r.digest == v[0].r.digest;
                    same &= v[0].r.keys == v[2].r.keys;   // observer keys: serial == workers
                    std::printf("observer_neutral %s %s offset=%d hash=%s digest=%s %s\n", name.c_str(),
                                modeName(mode), off, hex(v[0].r.hash).c_str(), hex(v[0].r.digest).c_str(),
                                same ? "OK" : "MISMATCH");
                    if (!same) {
                        for (const auto& x : v)
                            std::printf("  %s observer=%s hash=%s digest=%s\n", x.serial ? "serial " : "workers",
                                        x.observe ? "on " : "off", hex(x.r.hash).c_str(), hex(x.r.digest).c_str());
                        status = 1;
                    }
                }
            return status;
        }

        for (auto mode : opt.modes) {
            std::vector<Run> runs;
            bool eq = true;
            for (int off : opt.offsets) {
                Run main = runOnce(s, mode, off, opt.exec != Options::Workers, opt.observer, opt);
                std::fprintf(stderr, "%s %s offset=%d %s %.0f ms %s\n", name.c_str(), modeName(mode), off,
                             opt.exec == Options::Workers ? "workers" : "serial", main.ms, tickTimes(main).c_str());
                if (opt.exec == Options::Both) {
                    const Run other = runOnce(s, mode, off, false, opt.observer, opt);
                    std::fprintf(stderr, "%s %s offset=%d workers %.0f ms %s\n", name.c_str(), modeName(mode), off,
                                 other.ms, tickTimes(other).c_str());
                    if (other.hash != main.hash || other.digest != main.digest || other.keys != main.keys) {
                        std::fprintf(stderr, "%s %s offset=%d: serial != workers (hash %s vs %s, digest %s vs %s, keys %s)\n",
                                     name.c_str(), modeName(mode), off, hex(main.hash).c_str(),
                                     hex(other.hash).c_str(), hex(main.digest).c_str(), hex(other.digest).c_str(),
                                     other.keys == main.keys ? "equal" : "differ");
                        eq = false;
                        status = 1;
                    }
                }
                runs.push_back(std::move(main));
            }
            // Keys in first-seen order over the offsets; missing counts 0.
            std::vector<std::string> order;
            std::set<std::string> seen;
            for (const auto& r : runs)
                for (const auto& [k, v] : r.keys)
                    if (seen.insert(k).second) order.push_back(k);
            std::vector<std::pair<std::string, int64_t>> medians;
            std::vector<std::pair<std::string, std::vector<int64_t>>> varying;
            for (const auto& k : order) {
                std::vector<int64_t> vals;
                for (const auto& r : runs) vals.push_back(obs::Observer::get(r.keys, k, 0));
                const bool never = timeKey(k);
                auto sorted = vals;
                for (auto& x : sorted) if (never && x < 0) x = INT64_MAX;
                std::sort(sorted.begin(), sorted.end());
                int64_t med = sorted[(sorted.size() - 1) / 2];
                if (med == INT64_MAX) med = -1;
                medians.emplace_back(k, med);
                if (std::adjacent_find(vals.begin(), vals.end(), std::not_equal_to<>()) != vals.end())
                    varying.emplace_back(k, vals);
            }
            if (opt.json) {
                std::string j = "{\"scenario\":\"" + name + "\",\"mode\":\"" + modeName(mode) +
                                "\",\"ticks\":" + std::to_string(s.ticks) + ",\"units\":" + std::to_string(units) +
                                ",\"exec\":\"" + execName(opt.exec) + "\",\"window\":" + std::to_string(window) +
                                ",\"offsets\":[";
                for (size_t i = 0; i < opt.offsets.size(); ++i) j += (i ? "," : "") + std::to_string(opt.offsets[i]);
                j += "],\"hash\":{";
                for (size_t i = 0; i < runs.size(); ++i)
                    j += (i ? ",\"" : "\"") + std::to_string(opt.offsets[i]) + "\":\"" + hex(runs[i].hash) + "\"";
                j += "},\"serial_eq_workers\":";
                j += opt.exec == Options::Both ? (eq ? "true" : "false") : "null";
                j += ",\"keys\":" + obs::Observer::json(medians) + ",\"varying\":{";
                for (size_t i = 0; i < varying.size(); ++i) {
                    j += (i ? ",\"" : "\"") + varying[i].first + "\":[";
                    for (size_t k = 0; k < varying[i].second.size(); ++k)
                        j += (k ? "," : "") + std::to_string(varying[i].second[k]);
                    j += "]";
                }
                j += "}}";
                emit(j);
            } else {
                std::printf("%s %s: %d units, %u ticks, exec %s, window %d\n", name.c_str(), modeName(mode), units,
                            s.ticks, execName(opt.exec).c_str(), window);
                for (size_t i = 0; i < runs.size(); ++i)
                    std::printf("  offset %+d hash %s\n", opt.offsets[i], hex(runs[i].hash).c_str());
                if (opt.exec == Options::Both) std::printf("  serial == workers: %s\n", eq ? "yes" : "NO");
                for (const auto& [k, v] : medians) {
                    std::printf("  %-40s %lld", k.c_str(), (long long)v);
                    for (const auto& [vk, vals] : varying)
                        if (vk == k) {
                            std::printf("   (");
                            for (size_t i = 0; i < vals.size(); ++i) std::printf("%s%lld", i ? " " : "", (long long)vals[i]);
                            std::printf(")");
                        }
                    std::printf("\n");
                }
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "legion_scenario: %s\n", e.what());
        return 2;
    }
    return status;
}
}  // namespace
