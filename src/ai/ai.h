#pragma once

// The skirmish AI, extracted from the SDL viewer so it can also run headless on
// the multiplayer server. A Controller reads a const World and EMITS net::Commands
// through a sink instead of mutating the world directly -- so the same AI drives
// a player whether the commands are applied in-process (single-player) or fed into
// the server's command sequencer (server-hosted AI). See docs/multiplayer-design.md.
//
// The AI runs ONLY on the server (the referee), never on clients: its *decisions*
// are server-local, and only the net::Commands it emits are relayed and applied by
// every peer -- so lockstep never depends on the AI being reproducible. We still
// keep it RNG-driven so a --mpai run is repeatable for testing.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "net/lockstep.h"
#include "sim/sim.h"

namespace ta::ai {

class ClassicBrain;   // ai/classicbrain.h: the Classic level's brain
struct ClassicProfileScript;   // ai/classicprofile.h

// Retail AI profile weights and policy limits. Normal/Easy retain scaled limits;
// sustained-growth modes raise them to their force/production targets. A zero
// limit remains disabled; -1 is unlimited. Actual UnitType limits always apply.
// Missing weight => the AI never builds that unit.
struct Profile {
    std::unordered_map<std::string, int> weight, limit;
};

// Parse a game's ai/<name>.txt (else ai/default.txt) from `vfs` in the
// builtinprofile.h line format. One file covers every faction. Never throws; a
// missing file yields an empty profile (the AI then builds nothing). Our levels
// never read these files -- they play their built-in tables (builtinProfile) --
// and the Classic AI bakes them its own way (classicprofile.h), so this serves only
// the check that the built-in tables still reproduce the shipped default profile.
Profile loadProfile(const ta::hpi::Vfs& vfs, const std::string& name = "default");

// Opponent skill, loosely modelled on retail's easy/normal/hard. It scales HOW the
// AI plays (economy pace, army size before it commits, aggression, reaction rate)
// rather than cheating its economy -- so all three respect the same rules the human
// does. See paramsFor().
// The value is the wire aiLevel (SlotInfo.aiLevel); 0-4 run weakest -> strongest.
// Defensive builds a home force and intercepts nearby
// threats, scouting or attacking enemy bases only when far ahead, and keeps
// building in step with its income with no AI-imposed limit on how much. Absurd = Hard with a
// mana-income cheat (see incomeMultFor). AbsurdlyDefensive (5) = Defensive with
// Absurd's cheat. Classic (6) is a
// port of the retail game's own AI (docs/classic-ai-plan.md): a separate brain
// (ClassicBrain) with no difficulty knobs and no income cheat, so the sim gives its
// seat exactly a Normal seat's config. Passive (7) plays
// Easy in every way but two: nothing of its goes looking for the enemy, scout, army
// or fleet, so it attacks only what it has seen, and its builders stay within 900 px
// of home (Controller::stayHome_). Its seat's sim config is exactly an Easy seat's.
// Renumbering is versioned by kNetVersion.
enum class Difficulty : uint8_t {
    Defensive = 0, Easy = 1, Normal = 2, Hard = 3, Absurd = 4, AbsurdlyDefensive = 5, Classic = 6, Passive = 7,
    // Total Annihilation's own computer player (ai/taretail.h) at retail Easy and
    // Hard; Classic is the same brain at retail Medium.
    RetailEasy = 8, RetailHard = 9
};
inline constexpr uint8_t kAiLevels = 10;
// The TA retail brain's levels and their retail difficulty index (0 Easy, 1 Medium, 2 Hard).
inline constexpr bool retailLevel(Difficulty d) {
    return d == Difficulty::Classic || d == Difficulty::RetailEasy || d == Difficulty::RetailHard;
}
inline constexpr int retailDifficulty(Difficulty d) {
    return d == Difficulty::RetailEasy ? 0 : d == Difficulty::RetailHard ? 2 : 1;
}

// Map a wire aiLevel to a Difficulty, clamping anything out of range to Normal.
inline Difficulty difficultyFromLevel(uint8_t lvl) {
    return lvl < kAiLevels ? Difficulty(lvl) : Difficulty::Normal;
}

// The defensive levels: no offensive waves, a non-pursuing stance from birth
// (MatchSlot::defensiveAi), and production that follows income without caps.
inline constexpr bool defensiveLevel(Difficulty d) {
    return d == Difficulty::Defensive || d == Difficulty::AbsurdlyDefensive;
}

// Per-player mana-income multiplier for a difficulty. This is the ONE economy cheat:
// Absurd and Absurdly Defensive earn double from every income source. It scales
// HASHED sim state (mana), so every peer must apply the same factor to the same
// player -- it is derived from the broadcast aiLevel and set on the sim player at
// match setup, never AI-local.
inline constexpr float incomeMultFor(Difficulty d) {
    // TotalA.exe scales every resource credit to a computer's units by its
    // difficulty: Easy 0.5, Medium 0.7, Hard 1.0 (docs/retail-ai.md section 2).
    if (d == Difficulty::RetailEasy) return 0.5f;
    if (d == Difficulty::Classic) return 0.7f;
    if (d == Difficulty::RetailHard) return 1.0f;
    return d == Difficulty::Absurd || d == Difficulty::AbsurdlyDefensive ? 2.0f : 1.0f;
}

// When a level upgrades its own lodestones to Divine Lodestones (the faction's
// "mana" building, World::lodestoneUpgradeBase): not before `earliestMin` minutes
// of game time, only where the upgrade -- its cost, the old lodestone's income
// lost while it builds, and a priest's cost when one must be trained first --
// pays for itself out of the extra income within `horizonMin` minutes, at most
// `limit` of them standing or under way (-1 no limit, 0 never), saving
// `saveFraction` of the next purchase's cost before buying it. Controller::planUpgrade.
struct UpgradeLevel { int earliestMin, horizonMin, limit; float saveFraction; };
inline constexpr UpgradeLevel upgradeLevelFor(Difficulty d) {
    switch (d) {
        case Difficulty::Defensive:           return {15, 15, 5, 0.6f};
        case Difficulty::Normal:            return {10, 15, 2, 0.6f};
        case Difficulty::Hard:              return {8, 15, 6, 0.6f};
        case Difficulty::Absurd:            return {6, 15, -1, 0.6f};
        case Difficulty::AbsurdlyDefensive: return {8, 20, -1, 0.6f};
        default:                            return {0, 0, 0, 0.0f};   // Easy, Passive, Classic: never
    }
}

// How a level scouts (Controller::sendScouts): its first scouting party not
// before `firstMin` minutes of game time, then one every `everySec` seconds of
// game time while fewer than `maxScouts` are out; with `keep` a scout goes on to
// the next unexplored spot when it reaches one, else it rejoins the army. No new
// party goes while `maxLosses` scouts have been killed in the last five minutes.
// The defensive levels scout only while far ahead of the enemy units they know
// of (StrikeBoard::knownAhead).
struct ExploreLevel { int firstMin, everySec, maxScouts; bool keep; int maxLosses; };
inline constexpr ExploreLevel exploreLevelFor(Difficulty d) {
    switch (d) {
        case Difficulty::Defensive:           return {5, 60, 1, false, 1};
        case Difficulty::AbsurdlyDefensive: return {5, 60, 1, false, 1};
        case Difficulty::Easy:              return {4, 150, 1, false, 1};
        case Difficulty::Normal:            return {0, 60, 1, false, 1};
        case Difficulty::Hard:              return {0, 40, 2, true, 2};
        case Difficulty::Absurd:            return {0, 20, 3, true, 3};
        default:                            return {0, 0, 0, false, 0};   // Passive: never; Classic: its own brain
    }
}

// A start location an enemy may have opened at, as a player in that seat knows
// it: with FIXED or BY SLOT start locations the room shows whose start each is
// (`owner`, a player index); with RANDOM ones only the map's start spots, less
// the seat's own and its allies' (`owner` -1: some enemy's, or nobody's).
struct StartSpot { float x = 0, z = 0; int owner = -1; };

// The start spots seat `slot`'s player knows an enemy may have opened at: under
// FIXED or BY SLOT start locations each seated enemy's own (slotPos[j], (0,0) for
// a seat that opened nowhere), under RANDOM ones (`randomTable`: every spot a seat
// could have opened at, empty otherwise) each spot but the seat's own and its
// allies', which its screen shows from the start. `seated[j]`: slot j holds a
// player; `allied(i, j)`: the sim's alliance. The server's AI seats take these.
std::vector<StartSpot> enemyStartSpots(int slot, const std::vector<std::pair<float, float>>& slotPos,
                                       const std::vector<uint8_t>& seated,
                                       const std::function<bool(int, int)>& allied,
                                       const std::vector<std::pair<float, float>>& randomTable);

// Behaviour knobs derived from Difficulty (paramsFor).
struct DiffParams {
    int  thinkPeriod;      // sim ticks between decisions (30 = 1 Hz); lower = faster reactions
    int  waveSize;         // base army size to gather before an attack; the actual "big
                           // push" threshold scales UP with mana income (a rich AI masses
                           // a larger army, a poor one strikes with what it has)
    int  producersPerThink;// how many idle producers act each think (economy/APM pace)
    int  limitScale;       // percent applied to the profile's unit limits (100 = as shipped)
    bool attack;           // commit offensive waves (Defensive only intercepts local threats)
    int  raidSize;         // fighters peeled off for a small harassing raid while the main
                           // army musters (0 = no raiding)
    bool unbounded;        // no AI-imposed caps: the planner's targets scale with income
                           // without ceilings, profile limits do not apply, and spending
                           // is paced by mana alone (the defensive levels)
};
DiffParams paramsFor(Difficulty d);

// What a buildable unit is FOR, derived from its UnitType (faction-agnostic) so the
// planner can balance an army instead of drawing types blindly. See Controller::categoryOf.
enum class BuildCat { Economy, Factory, Builder, Army, Defense };

// The empire's current shape, assessed once per think. The planner compares these to
// simple targets to decide which category a producer should build next -- so the AI
// bootstraps an economy, adds factories to spend its income, keeps only a few builders,
// and then pours the rest into army, rather than letting a weighted-random draw spiral
// into all-economy / all-builders / no-soldiers (the old seed-fragile failure).
struct Needs {
    float income = 0;          // actual spendable income, including difficulty bonus
    // Own live units per type, filled in the same assessNeeds pass -- weightedPick's
    // limit checks read this instead of re-scanning all units per menu entry.
    std::unordered_map<const ta::sim::UnitType*, int> counts;
    std::unordered_map<const ta::sim::UnitType*, int> unfinished; // of counts, still under construction
    int   economyProjects = 0; // producers committed to income/storage construction
    int   lodestoneProjects = 0; // builders whose job is a lodestone (onMana), placed or queued
    int   population = 0;      // includes construction, and every unit ordered but not yet
                               // begun (a factory's queue, a builder's build order)
    int   lodestones = 0;      // income buildings (lodestones, Divine ones too), standing or ordered
    int   economy = 0;         // count: income/storage structures
    int   factories = 0;       // count: static or mobile production units
    int   navalFactories = 0;  // dedicated shipyards, including construction
    int   navalProducers = 0;  // shipyards or mobile conjurers with a water-unit menu
    int   navalArmy = 0;
    int   builders = 0;        // count: mobile builders (incl. the Monarch)
    int   defenses = 0;
    int   desiredArmy = 0;
    int   desiredDefenses = 1;
    int   army = 0;            // count: mobile combatants
    int   builderCap = 2;      // stop making builders past this, the Monarch included
    int   desiredFactories = 1;// how many factories the current income wants to feed;
                               // a side with no factory buildings adds its builder allowance
    int   mobileBuilders = 0;  // every builder=1 mobile unit of either category, the
                               // Monarch, those unfinished and those queued or ordered
    int   mobileBuilderCap = 2;// builderCap; a side with no factory buildings adds its factories
    bool  storeFull = false;   // mana at 90% of capacity or more: income goes unspent
    int   upgrades = 0;        // Divine Lodestones standing (finished or not) or ordered
    int   upgradeProjects = 0; // of upgrades, those unfinished or still only ordered
    int   priests = 0;         // units that build Divine Lodestones: standing, trained or ordered
    float reserve = 0;         // mana planUpgrade is saving; other builds wait below it
    float committed = 0;       // unbounded levels: build cost ordered this think, held
                               // against mana by the affordability gate
};

// Sink for the commands a Controller decides to issue this tick. Offline this
// applies them immediately; on the server it queues them into the tick sequencer.
using CommandSink = std::function<void(const ta::net::Command&)>;

// One AI brain, driving a single player.
class Controller {
public:
    // enemyStarts: the start positions of the players this AI is NOT allied with, so
    // it can march on a base under fog before it has actually spotted enemy units
    // (owners unknown: setStartSpots gives them).
    Controller(int player, const ta::sim::TypeRegistry& registry,
               const Profile& profile, uint32_t seed,
               Difficulty difficulty = Difficulty::Normal,
               std::vector<std::pair<float, float>> enemyStarts = {});
    Controller(Controller&&) noexcept;
    ~Controller();

    // Evaluate the AI for sim tick `simTick`. Does nothing off its think cadence.
    // A Classic controller hands every tick to its ClassicBrain instead.
    // Reads `world` (never mutates it) and emits any orders through `sink`.
    void tick(const ta::sim::World& world, uint32_t simTick, const CommandSink& sink);

    // Difficulty::Classic only (a no-op otherwise): the AI profile script the
    // ClassicBrain bakes its per-type preferences and limits from -- the map's
    // aiprofile file, chosen by the server (ai/classicprofile.h). Without one the
    // brain keeps retail's defaults (every type 100 / unlimited).
    void setClassicProfile(const ClassicProfileScript& script);
    // Difficulty::Classic only (a no-op otherwise): this seat's start location,
    // start number `index` at (x, z) pixels, which the brain anchors its home
    // base on (ClassicBrain::setStart).
    void setClassicStart(int index, float x, float z);
    // Difficulty::Classic only: every start location of the map, in start order
    // (ClassicBrain::setStartTable); call it before setClassicStart.
    void setClassicStarts(const std::vector<std::pair<float, float>>& positions);
    // Difficulty::Classic only: this player is a campaign mission's computer
    // player (retail's scenario mode 1: the first assignment pass's pre-placed
    // rules, base 1 not anchored on a start); `otaPlayer` is its .ota Player<N>
    // value, whose `massattack <n>` keyword (if any) starts the mass attack at
    // n x 30 ticks (classicMassAttackTicks).
    void setClassicCampaign(const std::string& otaPlayer);
    const ClassicBrain* classicBrain() const { return classic_.get(); }
    const class TaRetailBrain* retailBrain() const { return retail_.get(); }

    // The start spots an enemy may have opened at (StartSpot), in place of the
    // constructor's enemyStarts. Call before the first tick.
    void setStartSpots(std::vector<StartSpot> spots);

    // Read-only identity, for the debug roster dump (server/rosterdump.h).
    int player() const { return player_; }
    Difficulty difficulty() const { return stayHome_ ? Difficulty::Passive : diff_; }

private:
    friend struct PlacementProbe;
    std::vector<std::pair<float,float>> plannedManaSites_; // reservations until emitted orders reach the world
    // Unbounded levels: building sites already spoken for -- ordered this think
    // (plannedSites_), or ordered earlier and refused when the builder got there
    // (refusedSites_, held for kRefusedSiteTicks; issuedBuilds_ watches each order).
    struct PlannedSite { const ta::sim::UnitType* type; float x, z; };
    std::vector<PlannedSite> plannedSites_;
    struct IssuedBuild { int unit; const ta::sim::UnitType* type; float x, z; uint32_t tick; };
    std::vector<IssuedBuild> issuedBuilds_;
    struct RefusedSite { float x, z; uint32_t until; };
    std::vector<RefusedSite> refusedSites_;
    void updateIssuedBuilds(const ta::sim::World&, uint32_t simTick);
    // --- deterministic RNG (retail-style LCG) --------------------------------
    int rand(int n) {
        rng_ = rng_ * 1103515245u + 12345u;
        return n > 0 ? int((rng_ >> 16) % uint32_t(n)) : 0;
    }

    // --- decision helpers (all read-only over the world) ---------------------
    // Needs-based build planner: assess the empire, score each category against its
    // target, and let a producer build the most-needed thing its menu offers.
    Needs assessNeeds(const ta::sim::World&) const;
    BuildCat categoryOf(const ta::sim::UnitType*) const;
    // True for a side with no factory buildings (Zhon): its mobile builders do the
    // production other sides' factories do as well as the construction.
    bool mobileProduction(const std::string& side) const;
    mutable std::unordered_map<std::string,bool> mobileProduction_;
    bool navalFactory(const ta::sim::UnitType*) const;
    bool navalProducer(const ta::sim::UnitType*) const;
    const std::vector<std::pair<float,float>>& navalLaunchOffsets(const ta::sim::UnitType*) const;
    // A ship's water body at (x,z) holds at least `cells` positions it fits in.
    static constexpr int kMinNavalBodyCells = 256;
    bool navalBodyAtLeast(const ta::sim::World&, const ta::sim::UnitType* ship, float x, float z, int cells) const;
    bool navalLaunchClear(const ta::sim::World&,const ta::sim::UnitType* factory,
                          const ta::sim::UnitType* ship,float x,float z,float dx,float dz) const;
    // Open water out of a yard's door (placeSite prefers it; never a veto): the
    // moves a ship launched at (x+dx,z+dz) needs, beyond a straight run out of the
    // door, to get navalDoorReach cells clear of the yard's rectangle by
    // navalLaunchClear's cell rules -- 0 for open water straight out -- or -1 when
    // it cannot within a short budget: the door opens onto a shore pocket and the
    // way out runs round the yard.
    int navalDoorSteps(const ta::sim::World&,const ta::sim::UnitType* factory,
                       const ta::sim::UnitType* ship,float x,float z,float dx,float dz) const;
    // The clear depth in cells: the PARK ring's inner radius for that ship (its
    // smallest padding), less the yard's half-depth and the ship's half-width.
    static int navalDoorReach(const ta::sim::UnitType* factory,const ta::sim::UnitType* ship);
    // Our levels' base layout -- not retail, whose block-grid streets know no
    // doors. A production building's door is each yardmap edge holding 'c'/'C'
    // cells, the side its output leaves by. In front of it lies a strip -- the
    // door's span widened a cell each side, doorDepth deep -- kept clear of new
    // buildings, of their builders' approach and of parked units. Rects are world
    // px, half-open; a type's strips are offsets from its footprint origin.
    struct Rect {
        float x0, z0, x1, z1;
        bool overlaps(const Rect& o) const { return x0<o.x1 && o.x0<x1 && z0<o.z1 && o.z0<z1; }
    };
    const std::vector<Rect>& doorStrips(const ta::sim::UnitType*) const;
    mutable std::unordered_map<const ta::sim::UnitType*,std::vector<Rect>> doorStrips_;
    static Rect footprintRect(const ta::sim::UnitType*, float x, float z);
    void addDoorStrips(const ta::sim::UnitType*, float x, float z, std::vector<Rect>& out) const;
    // Every own structure's footprint (yard cells included) and every own
    // production building's door strip, gathered once per use.
    struct BaseLayout {
        std::vector<Rect> rects, doors;
        bool blocks(const Rect& r) const {
            const auto hit=[&](const Rect& o){return o.overlaps(r);};
            return std::any_of(rects.begin(),rects.end(),hit) || std::any_of(doors.begin(),doors.end(),hit);
        }
    };
    BaseLayout baseLayout(const ta::sim::World&) const;
    mutable std::map<std::tuple<const ta::sim::UnitType*,const ta::sim::UnitType*,int,int,int,int>,int>
        navalDoorSteps_;   // per yard type, ship and site, for one think
    // Divine Lodestone upgrades (planUpgrade). `target` is the own finished
    // lodestone the plan names this think (0: none), at deposit (x,z); `priest`
    // is the type a factory trains when no unit that builds `divine` exists.
    // `stage` 1 saves for the priest, 2 waits for it to be trained, 3 saves for
    // the Divine Lodestone; `saveStart` is when the current stage began, and
    // after a save that timed out nothing is planned before `retryAt`.
    struct UpgradePlan {
        int target = 0;
        float x = 0, z = 0;
        const ta::sim::UnitType* divine = nullptr;
        const ta::sim::UnitType* priest = nullptr;
        int stage = 0;
        uint32_t saveStart = 0, retryAt = 0;
    };
    UpgradePlan upgrade_;
    void planUpgrade(const ta::sim::World&, Needs&);
    // The Divine Lodestone in `type`'s menu, else nullptr (cached).
    const ta::sim::UnitType* divineIn(const ta::sim::UnitType* type) const;
    mutable std::unordered_map<const ta::sim::UnitType*,const ta::sim::UnitType*> divineIn_;
    // A mobile builder with a Divine Lodestone in its menu (Taros's Priest, Aramon's...).
    bool upgrader(const ta::sim::UnitType* type) const;
    // A spotted, armed enemy within `r` of (x,z).
    bool armedEnemyNear(const ta::sim::World&, float x, float z, float r) const;
    int   desire(BuildCat, const Needs&) const;
    // Retail's lodestone trigger, at every level: short of mana, with no
    // lodestone under way.
    bool  manaShort(const ta::sim::World&, const Needs&) const;
    // Retail's base radius around home for that lodestone's deposit (icd 0x40cf30).
    float retailLodestoneRadius(const ta::sim::World&, const ta::sim::Unit& builder) const;
    // `excludeCats` is a bitmask of 1<<int(BuildCat): categories already tried and
    // found unproducible this think, so the pick falls through to the next best.
    // `lodestoneOnly` limits the pick to the lodestone manaShort() asks for.
    // `siteLimit`, when given, receives the radius from home the pick's site must
    // lie within (retail's base radius for the trigger's lodestone), else 0.
    const ta::sim::UnitType* weightedPick(const ta::sim::World&,
                                           const ta::sim::Unit& producer, const Needs&,
                                           int excludeCats = 0, bool lodestoneOnly = false,
                                           float* siteLimit = nullptr);
    // True if an order was actually emitted. False means the pick could not be
    // acted on (no build site, nowhere to conjure) -- the caller then retries in
    // another category rather than burning the producer's turn on it.
    bool produce(const ta::sim::World&, const ta::sim::Unit& producer,
                 const ta::sim::UnitType* pick, const CommandSink&, float siteLimit = 0);
    // `limit` > 0 narrows the radius from home a site may lie within.
    bool placeSite(const ta::sim::World&, const ta::sim::UnitType*, const ta::sim::Unit& builder, float nx, float nz,
                   float& outX, float& outZ, float limit = 0) const;
    // Offensive targets, from what this AI knows (knowledge_): the enemy units
    // its own and its allies' sight shows it now (World::playerSees), the units
    // hitting its own, and what it saw before -- a structure where it last stood
    // until that spot is in sight again without it, a mobile unit for
    // kMobileMemoryTicks. Units are gathered on a kSiteCell grid into sites; a
    // strike goes for a site's most valuable unit (its anchor) and fights
    // whatever stands around it.
    struct StrikeSite {
        int anchor = 0;               // 0: an exploration point (no unit known)
        const ta::sim::UnitType* anchorType = nullptr;
        bool anchorUnfinished = false;
        float x = 0, z = 0;           // the anchor's position, last seen
        float value = 0;              // what destroying the site's units is worth
        float defence = 0;            // armed strength there and in the cells around it
        bool seen = false;            // this AI sees some unit of it now
        bool visible = false;         // ... its anchor among them
    };
    struct StrikeBoard {
        std::vector<StrikeSite> sites;
        float ours = 0;               // this AI's fighters (strength())
        float theirs = 0;             // every enemy's armed units and defences, as this AI reckons them
        bool ahead = false;           // ours outweighs theirs by this level's margin
        bool knownAhead = false;      // ours outweighs the enemy units known by that margin
        bool winning = false;         // ahead with a site known: clean up even at the defensive levels
    };
    static constexpr float kSiteCell = 400;
    StrikeBoard scanTargets(const ta::sim::World&) const;
    // A unit's fighting strength: its cost, scaled by the health it has left.
    static float strength(const ta::sim::Unit&);
    static bool armed(const ta::sim::UnitType*);
    // Some weapon of `a` can hurt `t` (flyers need a weapon that is not noair).
    static bool canHurt(const ta::sim::UnitType* a, const ta::sim::UnitType* t);
    // `u`'s attack weapon (a switcher's selected slot, else the first) can
    // reach `t` where it is now: a melee one never reaches a unit in the air,
    // and the sim drops such an attack order at once (Attack_Melee's head).
    static bool reachesNow(const ta::sim::Unit& u, const ta::sim::Unit& t);
    // Some weapon of `u` can do something to `t` as it stands, by the sim's own
    // test of an attack order (World::canAttackTarget for a computer's unit):
    // the damage table, the layer it is on, and what a charm, a stone or a
    // freeze can take -- a mobile unit that is finished, no Monarch and not
    // immune, and for a charm an owner below the unit cap. canHurt asks only
    // the damage table, by type.
    bool canTake(const ta::sim::World&, const ta::sim::Unit& u, const ta::sim::Unit& t) const;
    // The best site for a force of `force` strength at (x,z) whose units move as
    // `type`, by value (economy, production, the Monarch, finishing a player off),
    // threat (armed units, nearer our base) and practicality (distance, whether the
    // force outweighs the defence there, whether it can get there at all). `taken`
    // is the strength already sent to each site this think. -1 when nothing is.
    int pickSite(const ta::sim::World&, const StrikeBoard&, float x, float z, float force,
                 const ta::sim::UnitType* type, bool raid, const std::vector<float>& taken) const;
    // The strength a site needs sent at it (its defence, with this level's margin).
    float siteNeed(const StrikeSite&) const;
    // Where unit `u` goes for `site`: an Attack on the anchor once it is close (or
    // the anchor is a site still building, which auto-acquisition takes only
    // behind finished targets), else an attack-move to the nearest point it can
    // reach. False when it cannot get there.
    bool strikeOrder(const ta::sim::World&, const ta::sim::Unit& u, const StrikeSite& site,
                     uint32_t simTick, const CommandSink&);
    // What this AI knows of an enemy unit, by id: its type, owner and position
    // when last seen (cx,cz: its projected 32 px view cell, World::viewCount),
    // the think tick it was, and its strength then; `sighted` when that was by
    // sight, not only heard (a unit hitting ours from out of sight).
    struct Known {
        const ta::sim::UnitType* type = nullptr;
        int player = -1;
        float x = 0, z = 0, strength = 0;
        int cx = 0, cz = 0;
        uint32_t seenAt = 0;
        bool unfinished = false, sighted = false;
    };
    std::map<int, Known> knowledge_;
    static constexpr uint32_t kMobileMemoryTicks = 25 * 30;
    // The players whose sight this AI shares: itself and its allies but a
    // passive neutral (World::viewShareMask).
    std::vector<int> team_;
    bool teamSees(const ta::sim::World&, const ta::sim::Unit&) const;
    bool teamViews(const ta::sim::World&, int cx, int cz) const;
    // What this side knows, as a human player of it is shown it under the room's
    // FOG OF WAR rule. Our client draws the ground (terrain, features, mana
    // deposits) on every explored cell, and enemy units only on cells in view now:
    // its units' and allies' sight, and their radar out to radardistance through
    // terrain. NOT EXPLORED starts with nothing explored, EXPLORED with the whole
    // map's ground (no enemy unit shown), FULL VISION shows every unit. Mirrored
    // from simulation state alone, never the display fog: sight is the sim's view
    // counters (World::playerSees), explored its Mapping plane (World::exploredAt)
    // or the room's reveal (World::mapRevealedFor), radar the team's radar discs.
    // FULL VISION is World::roomLosOff; radar_ is refreshed once per think
    // (updateKnowledge, updateRadar), by the discs that changed since the last.
    static constexpr int kRadarCell = 64;
    int radarW_ = 0, radarH_ = 0;
    std::vector<uint16_t> radar_;     // per kRadarCell cell, the team's radar discs over it now
    std::vector<uint8_t> radarSeen_;  // ... and whether one ever was
    std::vector<std::tuple<int, int, int>> radarSources_;   // the discs counted: 32 px spot, range
    void stampRadar(int qx, int qz, int r, int delta);
    void updateRadar(const ta::sim::World&);
    bool radarAt(float x, float z) const;
    // The ground at (x,z) -- terrain, features, a mana deposit -- is known.
    bool knowsGround(const ta::sim::World&, float x, float z) const;
    // The ground point (x,z) is in view now.
    bool inView(const ta::sim::World&, float x, float z) const;
    // An enemy unit is in view now (never cloaked or carried).
    bool sees(const ta::sim::World&, const ta::sim::Unit&) const;
    // Mana deposits (World::manaSpots() index) this AI holds taken by an enemy: a
    // lodestone of one seen there, or remembered there (knowledge_), or learned
    // from a refused site (learnSiteBlockers). Refreshed per think; mutable so a
    // search can add what it learns at once.
    mutable std::vector<uint8_t> depositTaken_;
    // Deposits under a lodestone of ours or an ally's, this think: never free for
    // a new one, so the search passes them before the real placement test.
    std::vector<uint8_t> depositAllied_;
    // Deposits bucketed on a kDepositCell grid (they never move), built once.
    static constexpr int kDepositCell = 128;
    std::vector<std::pair<int64_t, int>> depositGrid_;
    template <class F> void forDepositsNear(const ta::sim::World&, float x, float z, float r, F&& fn) const;
    // Enemy lodestones out of view this think: what a lodestone site's real
    // placement test (World::canPlace) can be refused by unseen.
    struct HiddenLode { int id; float x, z; };
    std::vector<HiddenLode> hiddenLodes_;
    // A lodestone site refused by the real test: the order-time answer a player
    // gets. An enemy lodestone out of view on that deposit is learned -- noted
    // as known (heard, not sighted) at the next think, the deposit taken from now.
    // Other sites refused are only passed over, as before.
    mutable std::vector<int> learned_;
    void learnSiteBlockers(const ta::sim::World&, const ta::sim::UnitType*, float x, float z) const;
    // Refresh knowledge_ and the exploration state from this think's sight.
    void updateKnowledge(const ta::sim::World&);
    // Exploration: the start spots an enemy may hold (starts_), then a coarse
    // grid of map sectors, each with the think tick + 1 it was last in sight (0
    // never) and the tick until which a party sent there holds it. A start is
    // `gone` once it can hold no living enemy's base: its owner beaten, or (owner
    // unknown) seen with no enemy building near it, or every enemy found at
    // another; one whose owner is unknown takes the owner of the enemy buildings
    // known near it. enemyStarts_ holds the spots not gone, for the AI's other
    // judgements of where the enemy is. `startBase_`: an enemy building is known
    // near the start (scouts look elsewhere; armies still go and see it).
    // exploreTarget picks, for a unit of `type` at (x,z), an unchecked start (or
    // a beaten player's base out of sight a while) nearest first, else the
    // sector longest out of sight for its distance, weighed by what may stand
    // there (deposits, starts), and the point of it
    // that unit can reach; `-1 - start` or the sector's index comes back in
    // `slot`. With `scout` it is a scouting party's pick: past starts whose base
    // is found, targets backed off after scouts died going there (losses_), and
    // routes past the spots they died at.
    // starts_ from allyStartsFrom_ on are our allies' (updateKnowledge adds
    // them, gone from the start): followed only once their owner is beaten.
    std::vector<StartSpot> starts_;
    size_t allyStartsFrom_ = 0;
    bool startsSized_ = false;   // the start state below is sized to starts_
    std::vector<uint8_t> startGone_, startBase_;
    // Per start, the think tick + 1 its owner (an enemy or an ally) was found
    // beaten (0 never): a base left empty, which exploreTarget takes as a start
    // again once out of sight five minutes, since someone else may move in --
    // unless an enemy building is known near it (startBase_), a target for the
    // strikes instead.
    std::vector<uint32_t> startVacated_;
    std::vector<uint32_t> startSeen_, sectorSeen_, sectorClaim_, startClaim_;
    struct Probe { int radar, cx, cz; };   // a sector's five points: radar_ index (-1 off it, -2 no sector), view cell
    std::vector<Probe> sectorProbes_;
    std::vector<float> sectorLure_;   // per sector, 1 + what may draw an enemy there (deposits)
    int sectorsX_ = 0, sectorsZ_ = 0;
    float sectorSize_ = 0;
    std::map<std::pair<int, const ta::sim::UnitType*>, uint32_t> unreachable_;   // slot, type -> until
    bool exploreTarget(const ta::sim::World&, const ta::sim::UnitType* type, float x, float z,
                       float& tx, float& tz, int& slot, bool scout = false);
    void claimSlot(int slot, uint32_t ticks);
    bool sectorCentre(int sector, float mapX, float mapZ, float& x, float& z) const;
    int sectorOf(float x, float z) const;
    // Explore with `ids` from where the first of each kind of mover stands
    // (ground first): every unit that can reach the point picked attack-moves
    // there. Returns the units ordered; with `scout`, a scouting party's pick,
    // whose slot comes back in `*slotOut`.
    int explore(const ta::sim::World&, const std::vector<int>& ids, const CommandSink&,
                bool scout = false, int* slotOut = nullptr);
    // Scouting parties (exploreLevelFor): the scouts out, by unit id, each with
    // the tick its trip began, the slot it was sent to, where it was last seen
    // and its party (the id of the party's first scout); the tick the next one
    // may go. A scout killed on its trip is a loss:
    // where it fell, when, and per slot how many (slotLosses_), which backs that
    // slot off for kBackoffTicks, doubling with each loss up to four times, and
    // sends a party one fighter larger there the next time.
    struct ScoutTrip { uint32_t since; int slot; float x, z; int party; };
    std::map<int, ScoutTrip> scouts_;
    struct Loss { float x, z; uint32_t tick; };
    std::vector<Loss> losses_;
    struct SlotLosses { int count = 0; uint32_t last = 0; };
    std::map<int, SlotLosses> slotLosses_;
    bool backedOff(int slot) const;
    // Losses within kLossRouteRadius of the way from (x,z) to (tx,tz).
    int lossesOnRoute(float x, float z, float tx, float tz) const;
    uint32_t nextScoutTick_ = 0;
    int scoutsSent_ = 0, explorations_ = 0;   // dispatch counts, for the TA_AI_EXPLORE log
    int scoutsLost_ = 0;                       // scouts killed on a trip, for the same log
    int vacatedTrips_ = 0, vacatedLost_ = 0;   // scouts sent to / lost going to a beaten player's base
    uint32_t loggedMinute_ = 0;                // minute + 1 last logged
    bool loggedWin_ = false;
    void logExploration(const ta::sim::World&, uint32_t simTick);
    struct Fighter { int id; float x, z, s; };   // a fighter free for sendWaves, its strength s
    // Bring scouts home to the army, send the next one on, and, when one is due,
    // take the pool's fastest fighter for a new scouting party.
    void sendScouts(const ta::sim::World&, uint32_t simTick, const CommandSink&,
                    const StrikeBoard&, std::vector<Fighter>& pool);
    // (x,z) lies nearer some enemy start than home (hx,hz): the enemy's side of the map.
    bool enemySide(float x, float z, float hx, float hz) const;
    // `pad` pushes the approach rings out, so a unit stands clear of a body at
    // (tx,tz) rather than in it (sitePad).
    bool reachablePoint(const ta::sim::World&, const ta::sim::UnitType*,
                        float fx, float fz, float tx, float tz, float& x, float& z, float pad = 0) const;
    // That pad for a unit of `type` going for `site`'s anchor.
    static float sitePad(const StrikeSite& site, const ta::sim::UnitType* type);
    int terrainWeight(const ta::sim::World&, const ta::sim::Unit&,
                      const ta::sim::UnitType*, int weight) const;
    void sendWaves(const ta::sim::World&, uint32_t simTick, const CommandSink&,
                   const std::vector<int>& assigned);
    void sendDefenders(const ta::sim::World&, const CommandSink&);
    // Unbounded levels: walk idle fighters out of the base to a ring around it.
    void stationArmy(const ta::sim::World&, std::vector<int>& assigned, const CommandSink&);
    // The initial base anchor stays fixed as expansion adds remote buildings.
    // The Monarch and defensive AI use it to remain near home.
    std::pair<float, float> homeOf(const ta::sim::World&) const;

    // Idle, or only holding a factory's PARK release: retail parks every factory
    // output with no rally (GetBuilt 0x402220) and that PARK lasts until the next
    // order, so a parked newborn is as free to command as one with no orders.
    // So is a flyer only LANDING (VTOL_LandIfCan, which every flyer runs when its job ends --
    // a flying builder after each building): it is on its standby, and a new order
    // replaces the landing. Waiting for the touchdown cost a flying builder (the Zhon
    // Monarch) about four seconds after every building.
    static bool idle(const ta::sim::Unit& u) {
        const auto first=u.orders.begin();
        return std::all_of(first,u.orders.end(),[](const ta::sim::Order& o) {
            return o.park || o.landing || (!o.goal && !o.groundMission && !o.buildType && !o.targetId && !o.reclaimFeat &&
                !bool(o.reclaimArea) && !o.repairTarget && !o.load && !o.unload && !o.patrol &&
                !o.attackMove && !o.guard);
        }) && (first==u.orders.end() || std::any_of(first,u.orders.end(),
            [](const ta::sim::Order& o){return o.park || o.landing;}));
    }
    // Only raising a body (a RESURRECT leg, which reviveIdle gives): as free to
    // produce as an idle builder -- a build order simply replaces it.
    static bool raising(const ta::sim::Unit& u) {
        return !u.orders.empty() && u.orders.back().reviveKind &&
               std::none_of(u.orders.begin(),u.orders.end(),[](const ta::sim::Order& o) {
                   return o.goal && !o.reviveKind;
               });
    }
    // Our levels raise the dead by order now that nothing in the sim raises a
    // body on its own: each idle caster with a body it can raise within the
    // old idle search's 56 px is sent to raise the nearest (Cmd::Resurrect).
    void reviveIdle(const ta::sim::World&, std::vector<int>& assigned, const CommandSink&) const;
    // A fighter is free to be committed to a wave when it's idle or only doing a plain
    // move -- NOT while it's already fight-moving or attacking (so re-commanding it each
    // think doesn't reset its march and thrash it in place).
    // The whole queue is read, not its head: the sim puts legs of its own in front
    // of the order a unit was given -- a flyer's step-out (retail's air traffic),
    // the walk out of a weapon's minimum range or to a clear shot
    // (World::repositionForShot) -- and the order behind them is still what the
    // unit is doing. Read by its head alone, a Catapult backing off was free on
    // every think: the Attack was sent again, its queue replaced, the walk begun
    // again until the sim's three were spent, and it never fired.
    static bool waveFree(const ta::sim::Unit& u) {
        return std::none_of(u.orders.begin(), u.orders.end(), [](const ta::sim::Order& o) {
            return o.targetId != 0 || o.attackMove || o.buildType || o.load || o.unload || o.guard;
        });
    }
    // Each fighter of ours, by unit id: where it stood when it last made headway
    // (came kHeadway from there), the think tick it last did, fought, or was not
    // on a march (it holds no attack-move), and the stops this AI has given it
    // since for making none (updateStrikeProgress).
    struct StrikeProgress { float x, z; uint32_t since; uint8_t stops; };
    std::unordered_map<int, StrikeProgress> strikeProgress_;
    uint32_t strikeProgressAt_ = 0;   // the think strikeProgress_ was last brought up to
    void updateStrikeProgress(const ta::sim::World&, uint32_t simTick, const CommandSink&);
    void emit(const CommandSink& sink, ta::net::Cmd kind, int unitId,
              const std::string& type, float x, float z) const;

    int player_;
    const ta::sim::TypeRegistry& registry_;
    const Profile& profile_;
    uint32_t rng_;
    Difficulty diff_;     // Passive runs as Easy: every Easy branch is its own
    bool stayHome_;       // Passive: never explores, builders within 900 px of home
    DiffParams dp_;
    std::vector<std::pair<float, float>> enemyStarts_;   // starts_ not gone, refreshed each think
    std::optional<std::pair<float,float>> home_;
    // The seat's faction (a UnitType::side), fixed at the first think like home_:
    // its Monarch's side, else -- the Monarch already dead when this AI took the
    // seat -- the side of the god the match setup resolved from that Monarch
    // (Player::godType). Empty when neither tells (a constructed world): then
    // assessNeeds reads the side off the seat's mobile builders, as it once
    // always did after the Monarch died -- and a captured builder with a lower
    // unit id than every own one turned a Zhon seat into "Creon" for good.
    std::string side_;
    // Allied players' home anchors, fixed at the first think like home_ (homeOf's
    // rule applied to each ally). Expansion leaves a living ally's home deposits
    // to that ally while it can still claim them, then to the nearest other ally
    // (updateAllyClaims).
    struct AllyHome { int player; float x, z; };
    std::vector<AllyHome> allyHomes_;
    bool allyHomesKnown_ = false;
    // Per mana deposit (World::manaSpots() index): the last think tick it was
    // claimed -- a lodestone on it or a builder bound for it -- and whether this
    // think leaves it to an ally.
    std::vector<uint32_t> depositClaimedAt_;
    std::vector<uint8_t> allyHeldDeposits_;
    void updateAllyClaims(const ta::sim::World&, uint32_t simTick);
    // Each allied builder whose orders hold a deposit, by unit id: the point of
    // the task it is on now (its build site, or its leg's goal), the nearest it
    // has come to it, and the think tick it last came 64 px nearer than that or
    // was at work. `stalled` once it has not for a minute: it then holds no
    // deposit, and this AI stops its own (updateBuilderProgress) and never sends
    // that builder back to them.
    struct BuilderProgress { float siteX, siteZ, best; uint32_t gainedAt; bool stalled; };
    std::unordered_map<int, BuilderProgress> builderProgress_;
    struct StalledSite { int unit; float x, z; };
    std::vector<StalledSite> stalledSites_;
    void updateBuilderProgress(const ta::sim::World&, uint32_t simTick, const CommandSink&);
    bool stalledBuilder(int unitId) const;
    // Hard's and Absurd's builders reach the whole map for deposits (placeSite).
    // Ours, not retail, whose AI builds only inside its bases. A deposit is not
    // safe for them, and is passed over:
    //  - within kEnemyBase of a start an enemy may still hold (enemyStarts_),
    //    when nearer that start than home (placeSite tests this one);
    //  - within reach of an armed enemy this side knows of (knowledge_: in sight
    //    now, or remembered): its weapon range and 64 px, kThreatRadius at least;
    //  - for kLostSiteTicks after a lodestone site of ours on it was destroyed
    //    unfinished, or the builder bound for it was killed beside it or on its
    //    way there, within kTripReach of it (lostSites_, updateLostSites).
    // unsafeDeposit answers the last two; the known armed enemies are bucketed
    // once per think, on first use.
    static constexpr float kEnemyBase = 1000, kThreatRadius = 400, kTripReach = 1000, kTripGain = 64;
    static constexpr uint32_t kLostSiteTicks = 5 * 60 * 30;
    static constexpr int kThreatCell = 512;
    struct Threat { float x, z, r; };
    mutable std::vector<std::pair<int64_t, Threat>> threats_;
    mutable uint32_t threatsAt_ = UINT32_MAX;   // the think threats_ was gathered for
    bool unsafeDeposit(float x, float z) const;
    struct LostSite { float x, z; uint32_t until; };
    std::vector<LostSite> lostSites_;
    // Own unfinished lodestone sites as of the last think: unit id -> the deposit.
    std::map<int, std::pair<float, float>> lodestoneSites_;
    // Own builders with a lodestone order as of the last think, by unit id: the
    // deposit, how far from it the builder was when first seen with that order,
    // and whether its death now would make the deposit unsafe (updateLostSites).
    struct LodestoneTrip { float x, z, from; bool unsafe; };
    std::map<int, LodestoneTrip> lodestoneTrips_;
    void updateLostSites(const ta::sim::World&);
    bool buildsLodestone(const ta::sim::UnitType*) const;
    mutable std::unordered_map<const ta::sim::UnitType*,bool> lodestoneBuilders_;
    struct DefenseResponse {
        float homeX, homeZ, goalX, goalZ;
        bool returning = false;
        int targetId = 0;   // melee defenders chase one attacker with a direct Attack
        bool ownAttacker = false;   // targetId is the unit that was hitting this one
    };
    // Server-local assignments, not simulation state. Commands still use the
    // ordinary sequencer and movement controllers on every peer.
    std::unordered_map<int, DefenseResponse> defenders_;
    mutable std::unordered_map<const ta::sim::UnitType*,std::vector<std::pair<float,float>>> navalOffsets_;
    // Unbounded levels: a factory whose queue head has waited kQueueStallTicks
    // without a site (its output pad blocked, say) has it unqueued, and that factory
    // trains anything but that type for kRefusedTicks (refused_).
    struct QueueWait { const ta::sim::UnitType* type; uint32_t since; };
    std::unordered_map<int, QueueWait> queueWait_;
    std::unordered_map<int, QueueWait> refused_;   // since = the tick it may try again
    void unqueueStalled(const ta::sim::World&, uint32_t simTick, const CommandSink&);
    // Difficulty::Classic only: the retail-AI port that drives this player in place
    // of everything above (which then stays idle).
    std::unique_ptr<ClassicBrain> classic_;
    std::unique_ptr<class TaRetailBrain> retail_;   // TA retail levels (ai/taretail.h)
    uint32_t now_ = 0;
    // Fighters sent on a wave or raid (sendWaves). Once one is idle again it goes
    // straight for the next site rather than back into the pool for the next push.
    std::set<int> strikers_;
    // A site's anchor that strikers standing at it have been sent at again and
    // again without hurting it (out of reach on a cliff, say): passed over until
    // `until` so the army does not stand there re-ordered for ever.
    struct Pursuit { int tries = 0; float hp = 0; uint32_t last = 0, until = 0; };   // last: think tick + 1
    std::map<int, Pursuit> pursuits_;
    int raidersSincePush_ = 0;
    bool scouted_ = false;        // one-shot early scout sent
    bool firstThinkWaits_ = false;   // the tick-0 think waits a tick for the first view pass (tick)
    uint32_t lastRaidTick_ = 0;   // last tick a harassing raid was sent (raid cooldown)
};

}  // namespace ta::ai
