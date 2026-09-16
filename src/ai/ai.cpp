#include "ai/ai.h"

#include <cstdlib>

#include "hpi/hpi.h"
#include "sim/detmath.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <vector>

namespace ta::ai {

// Parse a decimal weight into HUNDREDTHS without going through floating point:
// the profile is read on the server, whose picks drive commands every peer then
// applies, so the text must land on the same integer everywhere. Accepts "2",
// "0.2", ".1", "1.25"; extra decimals are truncated. Returns false for a token
// that is not a number at all (retail ships `Limit ARMSABO` with no value).
static bool parseHundredths(const std::string& s, int& out) {
    size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    int whole = 0, frac = 0, fracDigits = 0;
    bool anyDigit = false;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
        anyDigit = true;
        if (whole < 1000000) whole = whole * 10 + (s[i] - '0');
    }
    if (i < s.size() && s[i] == '.') {
        ++i;
        for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
            anyDigit = true;
            if (fracDigits < 2) { frac = frac * 10 + (s[i] - '0'); ++fracDigits; }
        }
    }
    if (!anyDigit || i != s.size()) return false;
    while (fracDigits < 2) { frac *= 10; ++fracDigits; }
    out = whole * 100 + frac;
    if (neg) out = -out;
    return true;
}

const Plan& Profile::forDifficulty(Difficulty d) const {
    // Five difficulties over three shipped plans. Passive is Easy that never
    // attacks and Absurd is Hard with an income cheat, so both share their
    // sibling's build plan -- the difference between them lives in DiffParams.
    switch (d) {
        case Difficulty::Passive:
        case Difficulty::Easy:   return easy;
        case Difficulty::Hard:
        case Difficulty::Absurd: return hard;
        case Difficulty::Normal:
        default:                 return medium;
    }
}

Profile loadProfile(const ta::hpi::Vfs& vfs, const std::string& name) {
    // Campaign missions and maps name their own build profile (the .ota's
    // `aiprofile=`), and retail ships a dozen of them (ai/Metal.txt,
    // ai/SeaBattle.TXT...) tuned for that map's shape. Fall back to DEFAULT.TXT
    // when the named one is absent.
    std::string path = "ai/" + name + ".txt";
    if (name.empty() || !vfs.has(path)) path = "ai/default.txt";
    if (!vfs.has(path)) return Profile{};
    auto b = vfs.read(path);
    return parseProfile(std::string(b.begin(), b.end()));
}

Profile parseProfile(const std::string& text) {
    Profile prof;
    std::istringstream f(text);

    // Lines before the first `plan` apply to EVERY plan: the base game's
    // DEFAULT.TXT states its six global weight tweaks and `limit special 10`
    // that way, then narrows per difficulty below.
    Plan* targets[3] = {&prof.easy, &prof.medium, &prof.hard};
    int nTargets = 3;

    for (std::string line; std::getline(f, line);) {
        // Strip the comment tail and any stray CR before tokenising, so a line
        // written `weight armrl 2 // encourage rockets` still parses.
        if (size_t c = line.find("//"); c != std::string::npos) line.resize(c);
        std::istringstream ss(line);
        std::string kw, unit, val;
        if (!(ss >> kw)) continue;
        std::transform(kw.begin(), kw.end(), kw.begin(), ::tolower);
        if (kw == "plan") {
            if (!(ss >> unit)) continue;
            std::transform(unit.begin(), unit.end(), unit.begin(), ::tolower);
            if (unit == "easy")        { targets[0] = &prof.easy;   nTargets = 1; }
            else if (unit == "medium") { targets[0] = &prof.medium; nTargets = 1; }
            else if (unit == "hard")   { targets[0] = &prof.hard;   nTargets = 1; }
            continue;
        }
        if (kw != "weight" && kw != "limit") continue;
        if (!(ss >> unit >> val)) continue;   // `Limit ARMSABO` (no value) ships in MISSIONS.TXT
        std::transform(unit.begin(), unit.end(), unit.begin(), ::tolower);
        int v = 0;
        if (!parseHundredths(val, v)) continue;
        for (int i = 0; i < nTargets; ++i) {
            if (kw == "weight") targets[i]->weight[unit] = v;
            // A limit is a whole number of units; the file never writes a
            // fractional one, so drop the hundredths scaling here.
            else                targets[i]->limit[unit] = v / 100;
        }
    }
    return prof;
}

// Resolve `t` against `plan`, trying its unit id first and then each of its FBI
// category tags. A unit-id entry is the author being specific about this exact
// unit, so it wins outright rather than compounding with its categories.
int profileWeight(const Plan& plan, const ta::sim::UnitType& t) {
    if (auto it = plan.weight.find(t.id); it != plan.weight.end()) return it->second;
    // Categories compound: ARMCK is `arm` (0.2, a blanket damper retail applies to
    // both sides) and `constr` (3, the role boost), and retail's behaviour -- a
    // steady trickle of construction units rather than none or a swarm -- only
    // falls out if those multiply to 0.6 rather than either one winning alone.
    long w = kWeightOne;
    for (const auto& c : t.categories) {
        auto it = plan.weight.find(c);
        if (it != plan.weight.end()) w = w * it->second / kWeightOne;
    }
    return int(std::clamp<long>(w, 0, 1000000));
}

int profileLimit(const Plan& plan, const ta::sim::UnitType& t) {
    if (auto it = plan.limit.find(t.id); it != plan.limit.end()) return it->second;
    int lim = -1;
    for (const auto& c : t.categories) {
        auto it = plan.limit.find(c);
        if (it == plan.limit.end()) continue;
        lim = lim < 0 ? it->second : std::min(lim, it->second);
    }
    return lim;
}

DiffParams paramsFor(Difficulty d) {
    switch (d) {
        // Turtle: Easy's build-up, but never sends an attack wave -- it only defends
        // (idle units still auto-fire on anything that walks into range).
        case Difficulty::Passive: return {60, 4, 1, 70,  false, false, 0};
        // Sluggish: reacts slowly, builds up slowly, and only commits once it has
        // gathered a sizeable group -- so it's passive and beatable. No raiding.
        case Difficulty::Easy:   return {60, 4, 1, 70,  false, true,  0};
        // Fast, army-heavy, and aggressive: reacts often, musters a large army before
        // the big push, harasses with raids meanwhile, and pushes bigger unit limits.
        case Difficulty::Hard:   return {20, 6, 8, 150, true,  true,  4};
        // Hard's behaviour, plus a 2x income cheat applied to the sim (incomeMultFor).
        case Difficulty::Absurd: return {20, 6, 8, 150, true,  true,  4};
        case Difficulty::Normal:
        default:                 return {30, 5, 3, 100, true,  true,  3};
    }
}

Controller::Controller(int player, const ta::sim::TypeRegistry& registry,
                       const Profile& profile, uint32_t seed, Difficulty difficulty,
                       std::vector<std::pair<float, float>> enemyStarts)
    : player_(player), registry_(registry), profile_(profile),
      // The seed is taken as-is: the CALLER hands distinct seeds to distinct players
      // when it wants them to diverge from tick one (the lobby does, per game+player).
      // Same-seed controllers still diverge within a few ticks, because every RNG draw
      // is gated on that player's own unit counts / income / producers, which differ
      // by map position. The AI runs server-side only, so this RNG exists just to make
      // a --mpai run repeatable, not for lockstep.
      rng_(seed), diff_(difficulty), dp_(paramsFor(difficulty)),
      enemyStarts_(std::move(enemyStarts)) {}

void Controller::emit(const CommandSink& sink, ta::net::Cmd kind, int unitId,
                      const std::string& type, float x, float z) const {
    ta::net::Command c;
    c.kind = kind;
    c.player = uint8_t(player_);
    c.unitId = unitId;
    c.x = x;
    c.z = z;
    std::snprintf(c.type, sizeof c.type, "%s", type.c_str());
    sink(c);
}

// What is this unit FOR? Derived purely from its stats, so it works for any side
// and any mod:
//   Economy  - a structure that makes or holds either resource: solar, wind,
//              tidal, metal extractors, metal makers, storage
//   Factory  - a structure that builds units, OR a mobile producer
//   Defense  - any other structure (towers, walls, radar)
//   Builder  - a mobile unit that builds and expands (the commander, construction
//              units)
//   Army     - any other mobile unit
BuildCat Controller::categoryOf(const ta::sim::UnitType* t) const {
    if (t->isStructure()) {
        if (!registry_.buildable(t->id).empty()) return BuildCat::Factory;
        // Every way a TA building can contribute to the economy. This used to
        // test the Kingdoms mogrium fields (income/storage), which are zero on
        // every TA unit -- so solar collectors, wind farms, extractors and storage
        // all classified as DEFENSE and the AI never built an economy at all.
        if (t->metalMake > 0 || t->energyMake > 0 ||
            t->metalStorage > 0 || t->energyStorage > 0 ||
            t->extractsMetal > 0 || t->makesMetal > 0 ||
            t->windGenerator > 0 || t->tidalGenerator > 0)
            return BuildCat::Economy;
        return BuildCat::Defense;
    }
    if (t->isBuilder) {
        // A mobile builder whose menu is DOMINATED by mobile combat units is really
        // a factory, not an economy/expansion builder. A mobile CONSTRUCTOR (mostly
        // buildings) or the commander stays a Builder.
        if (!t->commander) {
            int combat = 0, structs = 0;
            for (const auto& id : registry_.buildable(t->id)) {
                const auto* b = registry_.find(id);
                if (!b) continue;
                if (b->isStructure()) ++structs;
                else if (!b->isBuilder) ++combat;
            }
            if (combat > structs && combat > 0) return BuildCat::Factory;
        }
        return BuildCat::Builder;
    }
    return BuildCat::Army;
}

// Count the empire by category and set the targets the planner steers toward.
Needs Controller::assessNeeds(const ta::sim::World& world) const {
    Needs n;
    const auto& me = world.player(player_);
    n.income = me.metal.income / std::max(me.incomeMult, 1.0f);   // ignore an Absurd AI's cheat
    for (const auto& u : world.units()) {
        if (!u.alive() || u.player != player_ || !u.type) continue;
        ++n.counts[u.type];
        switch (categoryOf(u.type)) {
            case BuildCat::Economy:  ++n.economy;   break;
            case BuildCat::Factory:  ++n.factories; break;
            case BuildCat::Builder:  ++n.builders;  break;
            case BuildCat::Army:     ++n.army;      break;
            case BuildCat::Defense:  break;
        }
    }
    // One factory per ~40 income so production can actually spend what we earn; a couple
    // of mobile builders is plenty (more just spiral the economy). Hard/Absurd run hotter.
    n.desiredFactories = std::clamp(int(n.income / 40.0f) + 1, 1, 8);
    n.builderCap = (diff_ == Difficulty::Hard || diff_ == Difficulty::Absurd) ? 3 : 2;
    return n;
}

// Priority of building one more of a category, given the empire's needs. Higher wins;
// 0 means "have enough, don't". The ladder: guarantee production, floor the economy,
// keep a few builders, grow economy/factories to match income, then army as the sink.
int Controller::desire(BuildCat c, const Needs& n) const {
    switch (c) {
        case BuildCat::Economy:
            // Bootstrap income BEFORE the pricey first factory -- building a 1700-mana
            // keep out of the opening treasury with no income starves everything after.
            if (n.income < 20.0f) return 95;
            return n.income < 25.0f + 20.0f * n.factories ? 60 : 0; // sustain the factories
        case BuildCat::Factory:
            if (n.factories == 0) return 90;                        // then: some production
            return n.factories < n.desiredFactories ? 70 : 0;       // scale with income
        case BuildCat::Builder:
            return n.builders < n.builderCap ? 65 : 0;              // a handful, then stop
        case BuildCat::Army:
            return 50;                                              // the default sink
        case BuildCat::Defense:
            return 0;                                               // (profile walls weight 0)
    }
    return 0;
}

// Pick what a producer should build: the most-needed category its menu can supply
// (desire()), then a weighted-random draw WITHIN that category (profile weights, for
// variety + retail flavour). A candidate must be affordable to finish and under its
// difficulty-scaled limit. Returns nullptr if nothing worth building is affordable now.
const ta::sim::UnitType* Controller::weightedPick(const ta::sim::World& world,
                                                   const ta::sim::Unit& producer,
                                                   const Needs& needs, int excludeCats) {
    const auto& menu = registry_.buildable(producer.type->id);
    const auto& me = world.player(player_);
    float income = me.metal.income / std::max(me.incomeMult, 1.0f);   // plan against base income
    // A menu entry the AI may build right now: has a positive weight, is under its
    // limit, and savings + income over its build time cover the cost (so a builder
    // never traps itself on a site the mana runs dry beneath).
    auto usable = [&](const ta::sim::UnitType* ut) -> int {
        if (!ut) return 0;
        const Plan& plan = profile_.forDifficulty(diff_);
        int w = profileWeight(plan, *ut);
        if (w <= 0) return 0;
        int lim = profileLimit(plan, *ut);
        if (lim == 0) return 0;      // an explicit `limit <x> 0` means never build x
        if (lim > 0) {
            auto ci = needs.counts.find(ut);
            if ((ci == needs.counts.end() ? 0 : ci->second) >=
                std::max(1, lim * dp_.limitScale / 100))
                return 0;
        }
        if (ut->buildTime > 0) {
            float secs = ut->buildTime / std::max(producer.type->workerTime, 1.0f);
            if (me.metal.cur + income * secs < ut->buildCostMetal) return 0;
        }
        return w;
    };
    // Pass 1: the highest desire among categories this producer can actually build now.
    int best = 0;
    for (const auto& id : menu) {
        const auto* ut = registry_.find(id);
        if (usable(ut) <= 0) continue;
        if (ut && (excludeCats & (1 << int(categoryOf(ut))))) continue;
        best = std::max(best, desire(categoryOf(ut), needs));
    }
    // TA_AI_PICK: why a producer chose nothing. A stalled economy is almost always
    // "every menu entry scored 0", and this says which gate did it.
    static const bool kPickLog = std::getenv("TA_AI_PICK") != nullptr;
    if (kPickLog && best <= 0) {
        std::fprintf(stderr, "    pick %s: nothing usable (metal=%.0f income=%.0f)\n",
                     producer.type->id.c_str(), world.player(player_).metal.cur, income);
        for (const auto& id : menu) {
            const auto* ut = registry_.find(id);
            if (!ut) { std::fprintf(stderr, "      %-9s MISSING from registry\n", id.c_str()); continue; }
            const Plan& plan = profile_.forDifficulty(diff_);
            const int w = profileWeight(plan, *ut);
            auto ci = needs.counts.find(ut);
            const int have = ci == needs.counts.end() ? 0 : ci->second;
            const int lim = profileLimit(plan, *ut);
            const float secs = ut->buildTime / std::max(producer.type->workerTime, 1.0f);
            std::fprintf(stderr, "      %-9s w=%d lim=%d have=%d cost=%.0f btime=%.0f "
                                 "afford=%s cat=%d usable=%d\n",
                         ut->id.c_str(), w, lim, have, ut->buildCostMetal, ut->buildTime,
                         (world.player(player_).metal.cur + income * secs >= ut->buildCostMetal) ? "Y" : "N",
                         int(categoryOf(ut)), usable(ut));
        }
    }
    if (best <= 0) return nullptr;   // nothing needed is affordable -> wait (no spiral)
    // Pass 2: weighted-random among the usable entries in that top category.
    const ta::sim::UnitType* chosen = nullptr;
    int total = 0;
    for (const auto& id : menu) {
        const auto* ut = registry_.find(id);
        int w = usable(ut);
        if (w <= 0 || desire(categoryOf(ut), needs) != best) continue;
        if (ut && (excludeCats & (1 << int(categoryOf(ut))))) continue;
        total += w;
        if (rand(total) < w) chosen = ut;   // reservoir sample
    }
    return chosen;
}

// Turn a pick into a command: a factory (keep/castle) trains a mobile unit; a
// mobile builder places a structure or conjures a mobile unit near itself. The
// placement spot is probed against the (const) world, then issued as a Build.
bool Controller::produce(const ta::sim::World& world, const ta::sim::Unit& p,
                         const ta::sim::UnitType* pick, const CommandSink& sink) {
    if (pick->isStructure()) {                  // structure
        if (!p.type->isStructure() && p.type->isBuilder) {
            // The Commander builds relative to HOME, not wherever it has drifted -- so a
            // lodestone goes on the nearest deposit to the BASE and other structures
            // ring the base, keeping the Commander near home instead of trekking across
            // the map (where losing it can lose the game). Other builders build where
            // they stand.
            float ox = p.x, oz = p.z;
            if (p.type->commander) { auto h = homeOf(world); ox = h.first; oz = h.second; }
            float x, z;
            if (placeSite(world, pick, ox, oz, x, z)) {
                emit(sink, ta::net::Cmd::Build, p.id, pick->id, x, z);
                return true;
            } else {
                static const bool kPickLog = std::getenv("TA_AI_PICK") != nullptr;
                if (kPickLog)
                    std::fprintf(stderr, "    NO SITE: %s#%d cannot site %s near (%.0f,%.0f)\n",
                                 p.type->id.c_str(), p.id, pick->id.c_str(), ox, oz);
            }
        }
        return false;
    } else if (p.type->isStructure()) {         // factory trains mobile
        emit(sink, ta::net::Cmd::Train, p.id, pick->id, 0, 0);
        return true;
    } else if (p.type->isBuilder) {             // mobile builder conjures mobile
        for (float r = 40; r < 170; r += 20)
            for (float a = 0; a < 6.28f; a += 0.6f) {
                float x = p.x + detmath::cos(a) * r, z = p.z + detmath::sin(a) * r;
                if (world.canPlace(pick, x, z)) {
                    emit(sink, ta::net::Cmd::Build, p.id, pick->id, x, z);
                    return true;
                }
            }
        static const bool kPickLog = std::getenv("TA_AI_PICK") != nullptr;
        if (kPickLog)
            std::fprintf(stderr, "    NO SPOT: %s#%d at (%.0f,%.0f) cannot place %s "
                                 "anywhere in r=40..170\n",
                         p.type->id.c_str(), p.id, p.x, p.z, pick->id.c_str());
    }
    return false;
}

// Find a build site for the AI: lodestones go on the nearest free mana deposit
// (when the map has any), everything else probes outward from the builder.
// Returns true and the chosen (outX,outZ) if a spot was found.
bool Controller::placeSite(const ta::sim::World& world, const ta::sim::UnitType* t,
                           float nx, float nz, float& outX, float& outZ) const {
    if (!t) return false;
    if (t->onMana && world.hasManaSpots()) {
        float bestD = 1e18f;
        bool found = false;
        for (const auto& [sx, sz] : world.manaSpots()) {
            if (!world.canPlace(t, sx, sz)) continue;   // taken or blocked
            float dx = sx - nx, dz = sz - nz, d = dx * dx + dz * dz;
            if (d < bestD) { bestD = d; outX = sx; outZ = sz; found = true; }
        }
        return found;
    }
    for (float r = 70; r < 340; r += 30)
        for (float a = 0; a < 6.28f; a += 0.5f) {
            float x = nx + detmath::cos(a) * r, z = nz + detmath::sin(a) * r;
            if (world.canPlace(t, x, z)) { outX = x; outZ = z; return true; }
        }
    return false;
}

// Nearest enemy the group at (cx,cz) can SEE (some AI unit within sight/radar of it)
// AND can actually REACH (flow connectivity), scanning closest-first. Fog: the AI
// never targets a unit it hasn't spotted; reachability keeps it off walled-in foes.
bool Controller::nearestVisibleEnemy(const ta::sim::World& world, float cx, float cz,
                                     const ta::sim::UnitType* atype,
                                     float& tx, float& tz) const {
    // My eyes: each own unit reveals a radius of max(sight, radar) around itself.
    struct Eye { float x, z, r2; };
    std::vector<Eye> eyes;
    for (auto& u : world.units())
        if (u.alive() && u.player == player_ && u.type && !u.embarked() &&
            !u.underConstruction) {   // a half-built unit has no eyes yet
            float s = std::max(u.type->sight, u.type->radar);
            eyes.push_back({u.x, u.z, s * s});
        }
    if (eyes.empty()) return false;
    std::vector<std::pair<float, std::pair<float, float>>> vis;
    for (auto& e : world.units()) {
        if (!e.alive() || e.embarked() || world.allied(e.player, player_) || !e.type)
            continue;
        bool seen = false;
        for (const Eye& eye : eyes) {
            float dx = e.x - eye.x, dz = e.z - eye.z;
            if (dx * dx + dz * dz <= eye.r2) { seen = true; break; }
        }
        if (!seen) continue;   // fogged: we haven't spotted this one
        float dx = e.x - cx, dz = e.z - cz;
        vis.push_back({dx * dx + dz * dz, {e.x, e.z}});
    }
    if (vis.empty()) return false;
    std::sort(vis.begin(), vis.end());
    int checked = 0;
    for (auto& e : vis) {
        if (++checked > 16) break;   // bound the reachability probes (flow builds)
        if (!atype || world.pathExists(atype, e.second.first, e.second.second, cx, cz)) {
            tx = e.second.first; tz = e.second.second;
            return true;
        }
    }
    return false;
}

// The nearest KNOWN enemy start position -- where the AI marches when fog hides the
// enemy, so the army pushes into a base (and spots its defenders) instead of idling.
bool Controller::nearestEnemyStart(float cx, float cz, float& tx, float& tz) const {
    float best = 1e18f;
    bool found = false;
    for (const auto& [x, z] : enemyStarts_) {
        float dx = x - cx, dz = z - cz, d = dx * dx + dz * dz;
        if (d < best) { best = d; tx = x; tz = z; found = true; }
    }
    return found;
}

// Pool idle (non-builder) fighters; once a strike force has gathered (waveSize, per
// difficulty), attack-move the whole group at ONE target so the wave arrives together
// rather than trickling in: the nearest enemy it can SEE, else the nearest enemy start
// (marching on the base). The original reason given here was that one target let them
// share a flow field; the flow fields are gone and paths are per-unit now, so arriving
// as a group is the whole of it.
// Also sends one early scout so the AI reveals + commits rather than turtling forever.
std::pair<float, float> Controller::homeOf(const ta::sim::World& world) const {
    double sx = 0, sz = 0; int n = 0;
    float kx = 0, kz = 0; bool haveKing = false;
    for (const auto& u : world.units()) {
        if (!u.alive() || u.player != player_ || !u.type) continue;
        if (u.type->isStructure()) { sx += u.x; sz += u.z; ++n; }
        if (u.type->commander && !haveKing) { kx = u.x; kz = u.z; haveKing = true; }
    }
    if (n) return {float(sx / n), float(sz / n)};   // centroid of my buildings
    if (haveKing) return {kx, kz};                  // no buildings yet: anchor on the Commander
    return {0.0f, 0.0f};
}

void Controller::sendWaves(const ta::sim::World& world, uint32_t simTick,
                           const CommandSink& sink) {
    if (!dp_.attack) return;   // Passive: never marches out; units defend in place.
    std::vector<int> idle;
    double sx = 0, sz = 0;
    const ta::sim::UnitType* atype = nullptr;
    for (auto& u : world.units())
        if (u.alive() && u.player == player_ && u.type && !u.type->isStructure() &&
            !u.type->isBuilder && waveFree(u)) {
            idle.push_back(u.id);
            sx += u.x; sz += u.z;
            if (!atype && !u.type->canFly) atype = u.type;
        }
    if (idle.empty()) return;
    float cx = float(sx / idle.size()), cz = float(sz / idle.size());

    // Objective: the nearest enemy we can actually SEE, else march on the nearest
    // known enemy base (which draws us forward and into its defenders).
    float tx = 0, tz = 0;
    if (!nearestVisibleEnemy(world, cx, cz, atype, tx, tz) &&
        !nearestEnemyStart(cx, cz, tx, tz))
        return;   // nothing seen and no known base to march on -> hold

    // The "big push" army scales with mana INCOME: a rich economy masses a large army
    // before it commits, a lean one strikes with less. So a strong AI stops trickling
    // its units into the enemy and instead builds an overwhelming force. A tapped-out
    // economy (little metal, little income) attacks with what it has rather than turtle.
    const auto& me = world.player(player_);
    float income = me.metal.income / std::max(me.incomeMult, 1.0f);   // ignore an Absurd cheat
    int bigPush = std::clamp(dp_.waveSize + int(income * 0.25f), dp_.waveSize, 60);
    bool tapped = me.metal.cur < 200.0f && me.metal.income < 40.0f;

    if (int(idle.size()) >= bigPush || tapped) {
        // Commit the army -- but cap commands per think so a huge force (a near-cap
        // game, or a stress test with thousands of units) doesn't emit one giant tick
        // bundle that blows the wire frame limit. The rest stay idle and deploy over
        // the next few thinks; all march to the same goal, so they still share a flow
        // field. kMaxWaveCmds keeps even several coincident AIs well under the cap.
        constexpr int kMaxWaveCmds = 256;
        int n = std::min(int(idle.size()), kMaxWaveCmds);
        for (int i = 0; i < n; ++i)
            emit(sink, ta::net::Cmd::AttackMove, idle[size_t(i)], "", tx, tz);
        lastRaidTick_ = simTick;      // let the freshly-built stragglers regroup, don't raid next
        scouted_ = true;
        return;
    }

    // Still mustering the big army -- but keep HARASSING so the enemy is pressured and
    // revealed instead of the AI turtling behind a wall. Peel off a small raiding party
    // (leaving a home core to keep growing toward the big push), rate-limited so the
    // army isn't bled away piecemeal. The very first raid doubles as the early scout.
    constexpr uint32_t kRaidCooldown = 25 * 30;   // ~25s between raids
    int homeCore = std::max(dp_.waveSize, bigPush / 3);   // never raid below this reserve
    bool firstProbe = dp_.scout && !scouted_ && int(idle.size()) >= 1;
    bool canRaid = dp_.raidSize > 0 &&
                   int(idle.size()) >= homeCore + dp_.raidSize &&
                   simTick - lastRaidTick_ >= kRaidCooldown;
    if (firstProbe || canRaid) {
        int party = firstProbe && !canRaid ? 1 : dp_.raidSize;   // opening scout is a lone unit
        for (int i = 0; i < party && i < int(idle.size()); ++i)
            emit(sink, ta::net::Cmd::AttackMove, idle[size_t(i)], "", tx, tz);
        scouted_ = true;
        lastRaidTick_ = simTick;
    }
}

void Controller::tick(const ta::sim::World& world, uint32_t simTick,
                      const CommandSink& sink) {
    // Think cadence (per difficulty), staggered by player so several AIs don't all
    // fire on the same tick. Sim-tick driven so a --mpai run is repeatable. Hard
    // reacts more often (thinkPeriod=20) than Easy (60).
    if ((simTick % uint32_t(dp_.thinkPeriod)) != uint32_t(player_ % dp_.thinkPeriod)) return;
    if (world.player(player_).defeated) return;

    const Needs needs = assessNeeds(world);   // one empire assessment drives every producer
    // Snapshot the idle producers, then act on up to producersPerThink of them -- the
    // per-think cap is what paces the economy across difficulties (Easy builds one
    // thing per think, Hard many).
    std::vector<int> producers;
    for (auto& u : world.units()) {
        if (!u.alive() || u.player != player_ || !u.type) continue;
        // A building can carry canMove=1 in its FBI (the Keep, the Taros Hell), so
        // classify by isStructure() (maxVel), not canMove -- otherwise a canMove
        // building is mistaken for a mobile builder and issued Build orders it can't
        // honour. And never drive a producer that is itself still under construction.
        if (!u.type->isStructure() && u.type->isBuilder && !u.underConstruction &&
            u.orders.empty() && u.buildSiteId == 0)
            producers.push_back(u.id);                        // idle mobile builder
        else if (u.type->isStructure() && !u.underConstruction && u.buildQueue.empty() &&
                 !registry_.buildable(u.type->id).empty())
            producers.push_back(u.id);                        // idle factory
    }
    // Round-robin who acts when the per-think cap is smaller than the producer count
    // (Easy caps at 1): otherwise the lowest-id producer -- the Commander -- takes the
    // only slot every think, so it keeps building economy and the factories never get
    // to train an army. Rotating by the think index gives each producer its turn.
    if (!producers.empty() && dp_.producersPerThink < int(producers.size())) {
        uint32_t rr = simTick / uint32_t(std::max(dp_.thinkPeriod, 1));
        std::rotate(producers.begin(),
                    producers.begin() + rr % uint32_t(producers.size()), producers.end());
    }
    // Over-commit throttle: while broke with construction already in progress, don't
    // start ANOTHER build. Concurrent builds share the one mana pool, so piling on more
    // while the treasury is empty starves them all and leaves half-built units that
    // never finish -- the "produces units then gets stuck" failure. Let the in-flight
    // ones finish (income flows into them) before starting the next.
    bool throttle = false;
    if (world.player(player_).metal.cur < 50.0f)
        for (const auto& u : world.units())
            if (u.player == player_ && u.alive() && u.underConstruction) { throttle = true; break; }
    int acted = 0;
    bool commanderActed = false;
    if (!throttle)
        for (int pid : producers) {
            if (acted >= dp_.producersPerThink) break;
            const auto* p = world.unit(pid);
            if (!p || !p->alive()) continue;
            // Retry in the next-best category when a pick cannot be acted on.
            // Without this a producer whose top category is unproducible burns its
            // turn silently, every think, for ever: measured on Zhon, all three
            // producers picked a lodestone 153 times in 240s with every mana
            // deposit already taken, emitted nothing, and banked 6875 mana while
            // building no army at all. One category per attempt, so the worst case
            // is one pass over the five.
            int exclude = 0;
            for (int attempt = 0; attempt < 5; ++attempt) {
                const auto* pick = weightedPick(world, *p, needs, exclude);
                if (!pick) break;
                if (produce(world, *p, pick, sink)) {
                    if (p->type && p->type->commander) commanderActed = true;
                    ++acted;
                    break;
                }
                exclude |= 1 << int(categoryOf(pick));
            }
        }
    // Keep the Commander safe: when it's idle (no build this think, no order, no site)
    // and has strayed beyond a leash of home, walk it back to the base. Losing the
    // Commander can lose the game (Commander Expendable), so it must not sit exposed out
    // in the field. A plain Move (not AttackMove) -- it retreats, it doesn't hunt.
    for (const auto& u : world.units()) {
        if (!u.alive() || u.player != player_ || !u.type || !u.type->commander) continue;
        if (commanderActed || !u.orders.empty() || u.buildSiteId != 0 || u.underConstruction)
            break;
        auto h = homeOf(world);
        float dx = u.x - h.first, dz = u.z - h.second;
        constexpr float kHomeLeash = 520.0f;   // ~a third of a small map's span
        if (dx * dx + dz * dz > kHomeLeash * kHomeLeash)
            emit(sink, ta::net::Cmd::Move, u.id, "", h.first, h.second);
        break;
    }
    sendWaves(world, simTick, sink);
}

}  // namespace ta::ai
