#pragma once

// The Classic AI level (Difficulty::Classic): a port of the retail TA:Kingdoms
// skirmish AI, behaviour reverse-engineered from the retail binary and described in
// docs/classic-ai-plan.md. It keeps retail's own decisions, retail's AI bugs
// included, and has no difficulty knobs and no income cheat.
//
// Like every AI it runs ONLY on the server: it reads a const World and emits
// net::Commands, so its seat's sim config is exactly a Normal seat's
// (matchConfigFor), and nothing it holds is lockstep or hashed state.
//
// P0 (plumbing): the brain owns its random stream and is wired into
// ai::Controller. P1: it holds retail's unit catalogue and the baked AI profile
// (ai/classicprofile.h). P2: per-type facts (ai/classictypes.h) and retail's
// bookkeeping clock -- the per-tick unit count cache and the dynamic build
// priorities recomputed on rand(30)==0 of each >= 7-tick window. P3: retail's
// AI manager (ai/classicsquads.h) -- the 100 squad slots the constructor builds
// and rolls, the scheduler (the assignment countdown, then every squad whose
// deadline has come), the dying-unit cleanup and the assignment pass: the
// Monarch into base 1 anchored on its start, every other finished unit into the
// nearest base with retail's standing orders (a Stance 0 command), and the
// campaign mass attack. P4a: the base think's member loop (ai/classicsquads.h)
// with retail's factory planner behind it (ai/classicproduction.h): a ready
// factory in a base draws from its menu with the brain's weights and queues one
// unit at a time with a Train command. P4b: a ready mobile builder of a base
// (the Monarch, its builders, Zhon's and Creon's conjurers) goes to retail's
// constructor planner (ai/classicproduction.h) with the Monarch's gates, and
// what it picks is placed by retail's placers (ai/classicplacement.h) on the
// block grids the build manager draws as it is built, and ordered with a Build
// command. P5: the base think's threat test 0x412000 over the World's
// visible-enemy list, and retail's combat-member planner 0x40d370 for every
// other mobile member (ai/classicdefence.h: idle units patrol inside the base
// ring, armed ones under threat are sent at the intruders) and the Monarch's
// retreat 0x40d010, both also reached from the constructor planner, as Move and
// Patrol commands. P6: the rest of the base think (ai/classicwaves.h: quota
// growth, ready counts, the launch of a wave into a strike's backup, a VTOL
// squad or a naval strike) and the army squads' own thinks -- the strike's
// cohesion, dissolve, target pick over every enemy unit, withdrawals and its
// contact / march moves, the backup's move to its strike, the VTOL squad's
// random patrols -- as Move, Patrol and Stop commands. P7: the base think's
// transfers and expansion (ai/classicexpand.h): a builder, ground or air unit
// sent to the nearest base short of them, and a free builder founding a new
// base anchored on the nearest free sacred site in its zone (the zone labels
// retail gives each start and site of a navigator grid at map load), with the
// escort following it.

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ai/classicdefence.h"
#include "ai/classicexpand.h"
#include "ai/classicplacement.h"
#include "ai/classicprofile.h"
#include "ai/classicsquads.h"
#include "ai/classictypes.h"
#include "ai/classicwaves.h"
#include "net/lockstep.h"
#include "sim/retailrng.h"

namespace tak::sim { class World; class TypeRegistry; class NavGrid; struct Unit; }

namespace tak::ai {

class ClassicBrain {
public:
    // `roomSeed` is the seed the server hands every AI of the game; mixing in the
    // player keeps two Classic AIs of one game from mirroring each other.
    // Retail creates the build manager at game start with every type reset to
    // 100 / unlimited (0x4102f0 -> 0x410670); the profile file comes after.
    // Retail builds the AI manager first (0x4f6660: 0x40fed0, its squads and
    // their rolls), then the build manager. `largestEnemyPopulation` is what the
    // ground wave roll reads as E; at a skirmish start every player has at most a
    // Monarch, so any value under 10 gives retail's E = 10.
    ClassicBrain(int player, uint32_t roomSeed, const tak::sim::TypeRegistry& registry,
                 int32_t largestEnemyPopulation = 0);
    ClassicBrain(int player, uint32_t roomSeed);   // an empty catalogue (tests)

    // The player's start location (player+0xec and its record in 0x62dbdc):
    // start record `index` is the 16-pixel cell holding (x, z) pixels. Base 1 is
    // anchored on it by the first assignment pass. Without one base 1 is not
    // anchored and its centre is its members' centroid.
    void setStart(int index, float x, float z);
    // Every start location of the map in start order (retail's ten start
    // records, 0x4e0810: an empty one past the map's own), which the zone labels
    // read; call it before setStart.
    void setStartTable(const std::vector<std::pair<float, float>>& positions);
    // A campaign mission's `massattack` for this player, in ticks (0 = none;
    // classicMassAttackTicks parses the .ota keyword), and the scenario mode the
    // first assignment pass reads (1 = a campaign mission with preplaced units).
    void setMassAttack(uint32_t ticks) { squads_.massAttack = ticks; }
    void setScenarioMode(int32_t mode) { scenarioMode_ = mode; }
    const ClassicSquads& squads() const { return squads_; }
    // A unit's squad slot (retail unit +0xc8): 0 unassigned, -1 dropped while dying.
    int32_t squadOf(int unitId) const {
        const auto it = squadOf_.find(unitId);
        return it == squadOf_.end() ? 0 : it->second;
    }

    // Retail's profile loader (0x4f6860): runs the map's profile script over the
    // reset tables.
    void useProfile(const ClassicProfileScript& script);
    const ClassicCatalogue& catalogue() const { return catalogue_; }
    const ClassicProfile& profile() const { return profile_; }
    const std::vector<ClassicTypeInfo>& typeInfo() const { return info_; }
    // A builder type's menu as retail loads it (0x4c1d35, only for FBI builder
    // types): its canbuild entries in [Menu] Priority order, as catalogue types.
    const std::vector<uint16_t>& menu(size_t type) const {
        static const std::vector<uint16_t> none;
        return type < menu_.size() ? menu_[type] : none;
    }
    // Train commands the factory planner has issued (tests and the debug dump).
    uint32_t factoryQueues() const { return queues_; }
    // Build commands the constructor planner has issued, and how many of them
    // were lodestones (tests).
    uint32_t builds() const { return builds_; }
    uint32_t lodestoneBuilds() const { return lodestoneBuilds_; }
    // Move and Patrol commands the combat planner has issued, and how many
    // times the Monarch's retreat ran (tests).
    uint32_t combatOrders() const { return combatOrders_; }
    uint32_t retreats() const { return retreats_; }
    // The army squads' work (tests): waves launched (ground, VTOL, naval), the
    // moves strikes gave, strikes dissolved, members sent home, target picks
    // and the picks that found one, backup and VTOL group orders.
    struct WaveStats {
        uint32_t groundLaunches = 0, vtolLaunches = 0, navalLaunches = 0;
        uint32_t strikeMoves = 0, contactMoves = 0, dissolves = 0, withdrawals = 0;
        uint32_t picks = 0, targets = 0, backupMoves = 0, vtolPatrols = 0;
        // P7: units moved by the transfers (builders, ground, air) and new bases.
        uint32_t builderTransfers = 0, groundTransfers = 0, airTransfers = 0, expansions = 0;
    };
    const WaveStats& waveStats() const { return waves_; }
    // A unit's first mission as the manager reads it (class U1, flags; tests).
    static std::pair<uint8_t, uint32_t> firstMissionOf(const tak::sim::World& world, const tak::sim::Unit& u);
    // The block grids the build manager drew as it was built (0x4110f0).
    const ClassicBlockGrids& blockGrids() const { return grids_; }
    // Retail's per-type cache (mgr+0x85 / +0x95, recounted every tick by 0x4119d0):
    // the player's live units of each type, and how many of those are finished.
    const std::vector<int16_t>& counts() const { return count_; }
    const std::vector<int16_t>& completed() const { return completed_; }
    // Retail's dynamic build priority per type (mgr+0x69, 0x410810).
    const std::vector<uint8_t>& priorities() const { return priority_; }
    uint32_t priorityRefreshes() const { return refreshes_; }
    // The draw weight of a type for this player (0x412c90): 0 at its limit, else
    // pref * priority / 100.
    int buildWeight(size_t type) const;
    // 0x410810's store fill: stored / capacity, or 0 when the capacity is at most
    // 0.01 (a double compare).
    static float storeFill(float stored, float capacity) {
        return double(capacity) > 0.01 ? stored / capacity : 0.0f;
    }

    // 0x411510 for catalogue type `t` (lodestone class) around `centre` (16.16
    // px) within `radius` px: the nearest sacred site that passes the placement
    // test (siteOpen), as the footprint's top-left cell; none when every site
    // in reach is taken (retail then builds nothing).
    std::optional<std::pair<int16_t, int16_t>> depositSite(const tak::sim::World& world, uint16_t t,
                                                           std::pair<int32_t, int32_t> centre, int32_t radius);

    // Runs every sim tick (the brain keeps retail's own clocks, not a think period).
    void tick(const tak::sim::World& world, uint32_t simTick,
              const std::function<void(const tak::net::Command&)>& sink);

    // Retail's game RNG (Park-Miller, 0x535cc0): a value in [0,n); n < 2 returns 0
    // without a draw. A private, server-local stream; the draw order inside each
    // ported routine follows retail's.
    int rand(int n) { return int(tak::sim::retailRandom(seed_, n)); }
    uint32_t seed() const { return seed_; }
    int player() const { return player_; }

    // A deliberate divergence from retail's seeding (0x535d30), which can hand
    // out a seed that is a multiple of 2^31-1: Park-Miller maps such a seed (and
    // 0) to 0x7fffffff, which then never moves again, so every draw of the game
    // would repeat. retailSeed's |1 already rules out 0; the one odd multiple,
    // 0x7fffffff itself, is moved to a neighbouring odd seed.
    static constexpr uint32_t seedFor(int player, uint32_t roomSeed) {
        const uint32_t s = tak::sim::retailSeed(roomSeed ^ (0x9e3779b9u * uint32_t(player + 1)));
        return s % 0x7fffffffu == 0 ? s ^ 2u : s;
    }
    // The player's units as the manager reads them, in unit order, with their
    // squads; members that left the world (or the player) are dropped first.
    // Public for the tests that check what the manager sees of a unit.
    std::vector<ClassicSquadUnit> squadUnits(const tak::sim::World& world);

private:
    // 0x40fe00 -> 0x40fd80, retail's AI scheduler: the assignment countdown, then
    // each squad whose deadline has come (the shared sim templates).
    void scheduler(const tak::sim::World& world, uint32_t simTick,
                   const std::function<void(const tak::net::Command&)>& sink);
    // Retail's AI squads are the player's group records (unit+0xc8 by set-squad
    // 0x50b4f0, record +8 by set-active 0x50b700), and the Retail movement paces
    // a formation's members (51b890, the Move_Ground and attack group checks).
    // Only a strike squad (while attacking) and a VTOL squad (while it has units)
    // are ever made formations, so the World is told those squads' members and
    // flags, as Cmd::AiGroup, whenever they differ from what it was last told.
    void publishGroups(const tak::sim::World& world, const std::function<void(const tak::net::Command&)>& sink);
    // 0x411a90, retail's per-player bookkeeping: the >= 7-tick window (where the
    // world rebuilds its visibility lists) draws rand(30), and 0 recomputes the
    // priorities from the cache as the previous tick left it; then the recount.
    void bookkeeping(const tak::sim::World& world, uint32_t simTick);
    // 0x40b320's member loop for base `slot` (after its reschedule), and the
    // factory planner 0x40dcb0 for one of its members. Retail's orders take
    // effect at once (0x429810 / 0x4d78a0 put the mission on the unit), so a
    // planner that orders a member updates its first mission in `units`: later
    // members of the same think read it (the idle-factory and busy-deposit tests).
    void baseThink(const tak::sim::World& world, int slot, std::vector<ClassicSquadUnit>& units,
                   const std::function<void(const tak::net::Command&)>& sink);
    void planFactory(const tak::sim::World& world, ClassicSquadUnit& unit,
                     const std::function<void(const tak::net::Command&)>& sink);
    // 0x40c890 for units[i], a ready mobile builder of base `slot`.
    void planConstructor(const tak::sim::World& world, int slot, std::vector<ClassicSquadUnit>& units,
                         size_t i, bool threat, const std::function<void(const tak::net::Command&)>& sink);
    // 0x40d370 (the combat-member planner) for a ready member of base `slot`
    // with the base think's threat answer, or with `retreat` 0x40d010 (the
    // Monarch's retreat); the orders update the member's first mission in the
    // think's snapshot, as retail's do at once.
    void planCombat(const tak::sim::World& world, int slot, const std::vector<ClassicSquadUnit>& units,
                    ClassicSquadUnit& member, bool threat, bool retreat,
                    const std::function<void(const tak::net::Command&)>& sink);
    // The player's visible-enemy list (0x411700's +5) as the AI reads it.
    std::vector<ClassicEnemy> visibleEnemies(const tak::sim::World& world) const;
    // Rebuilds visible_ when the World has just rebuilt its own lists (U16).
    void refreshVisible(const tak::sim::World& world);
    // Any unit as the AI reads an enemy (the lists, the picker, a strike's p4).
    ClassicEnemy enemyView(const tak::sim::World& world, const tak::sim::Unit& e) const;
    // The army squads' thinks (ai/classicwaves.h) for squad `slot`, after the
    // shared clock template's reschedule (and, for a strike, its cohesion pass).
    void strikeThink(const tak::sim::World& world, int slot, tak::sim::RetailAiGroupClock& clock,
                     std::vector<ClassicSquadUnit>& units, uint32_t simTick,
                     const std::function<void(const tak::net::Command&)>& sink);
    void backupThink(const tak::sim::World& world, int slot, std::vector<ClassicSquadUnit>& units,
                     const std::function<void(const tak::net::Command&)>& sink);
    void vtolThink(const tak::sim::World& world, int slot, std::vector<ClassicSquadUnit>& units,
                   const std::function<void(const tak::net::Command&)>& sink);
    // 0x40a200 for `member` around `centre` over every enemy unit
    // (classicPickTarget), and the reachability walk it and the strike's
    // withdrawal test use (0x4e1570 from the member's cell and footprint to
    // range x range cells at the target's cell, over the static grade).
    int pickTarget(const tak::sim::World& world, const ClassicSquadUnit& member, std::pair<int32_t, int32_t> centre);
    bool reachTarget(const tak::sim::World& world, const ClassicSquadUnit& member, const ClassicEnemy& target,
                     int16_t range, bool limited);
    // 0x40b320 after its member loop: the launch (classicBaseLaunch).
    void baseLaunch(const tak::sim::World& world, int slot, std::vector<ClassicSquadUnit>& units,
                    const std::function<void(const tak::net::Command&)>& sink);
    // The largest unit count among the players not allied to this one (the
    // ground wave roll's E, before its floor of 10).
    int32_t largestEnemyPopulation(const tak::sim::World& world) const;
    // A squad member's order (0x4d78a0) as a Move or Patrol command: queue 0
    // replaces its missions, and a Move whose mission +0x4e is a6 > 0 ends
    // within that distance (dropped when the unit is already inside, else aimed
    // at the circle's edge). Updates the member's first mission in the snapshot
    // when it replaces them. Whether a command was sent.
    bool sendOrder(const tak::sim::World& world, ClassicSquadUnit& member, bool patrol, bool queue,
                   const ClassicPoint& at, int32_t a6, const std::function<void(const tak::net::Command&)>& sink);
    // 0x4d6a50(u, 0) as a Stop command; the member's snapshot loses its missions.
    void clearMissions(ClassicSquadUnit& member, const std::function<void(const tak::net::Command&)>& sink);
    // The map's sacred-site records (0x62dbcc) and which feature-plane types are
    // sacred, read from the World once.
    void readSites(const tak::sim::World& world);
    bool sacredCell(const tak::sim::World& world, int x, int z) const;
    // 0x507d10(type, 0, cell, 1, 0) as the AI calls it, for type `t`'s footprint
    // with its top-left on cell (x, z).
    bool siteOpen(const tak::sim::World& world, uint16_t t, int16_t x, int16_t z) const;

    // A sacred-site record's footprint as the zone labelling reads it (its
    // sacred feature's, else 1 x 1).
    std::pair<int16_t, int16_t> siteFootprint(const tak::sim::World& world, const ClassicCell& cell) const;
    // The zone labels (0x4e0a20) of the navigator grid a `footX` x `footZ` mover
    // on `nav` uses, built once per grid over its static bit.
    const ClassicZoneLabels& zoneLabels(const tak::sim::World& world, const tak::sim::NavGrid& nav, int footX, int footZ);
    // A unit's grid labels (unit +8 -> +4), or null for one without a ground
    // navigator (a flyer).
    const ClassicZoneLabels* unitLabels(const tak::sim::World& world, const ClassicSquadUnit& unit);
    // The navigator grid's static bit (what 0x4e1570's mode 1 reads) for a
    // `footX` x `footZ` mover on `nav`: 1 where the grid build set it at load.
    const std::vector<uint8_t>& staticBlocked(const tak::sim::World& world, const tak::sim::NavGrid& nav,
                                              int footX, int footZ);
    // The owner's unit count as retail keeps it (player +0xe8): its live units.
    uint16_t population(const tak::sim::World& world) const;
    void recount(const tak::sim::World& world);
    void recomputePriorities(const tak::sim::World& world);
    // 0x4102f0's pass at the manager's creation (from the registry constructor).
    void creationPass();
#ifndef NDEBUG
    void debugDump(uint32_t simTick);
    bool dumped_ = false;
#endif

    int player_;
    uint32_t seed_;
    ClassicCatalogue catalogue_;
    ClassicProfile profile_;
    std::vector<ClassicTypeInfo> info_;
    std::vector<std::vector<uint16_t>> menu_;
    uint32_t queues_ = 0, builds_ = 0, lodestoneBuilds_ = 0, combatOrders_ = 0, retreats_ = 0;
    WaveStats waves_;
    int32_t ownTotal_ = 0;   // the build manager's +0x7d: live units at the last recount
    ClassicBlockGrids grids_;
    bool sitesRead_ = false;
    std::vector<std::optional<ClassicCell>> sites_;
    std::vector<bool> sacredTypes_;
    std::vector<std::pair<ClassicCell, ClassicCell>> sacredRects_;   // without a feature plane
    // staticBlocked / zoneLabels hand out references into these caches and add
    // to them while one may be held (zoneLabels builds a static plane first), so
    // they are deques: an append never moves an existing entry.
    struct StaticPlane { const tak::sim::NavGrid* nav; int footX, footZ; std::vector<uint8_t> blocked; };
    std::deque<StaticPlane> staticPlanes_;
    struct ZonePlane { const tak::sim::NavGrid* nav; int footX, footZ; ClassicZoneLabels labels; };
    std::deque<ZonePlane> zonePlanes_;
    std::unordered_map<const tak::sim::UnitType*, uint16_t> index_;
    std::vector<int16_t> count_, completed_;
    std::vector<uint8_t> priority_;
    tak::sim::RetailPlayerCacheClock clock_{true, 0};
    uint32_t refreshes_ = 0;
    ClassicSquads squads_;
    // 0x411700's list +5 for this player, as of the World's last rebuild (the
    // World's own list, enemy sites still being built included, U16); id
    // order. visibleStamp_ is that rebuild's tick.
    std::vector<int> visible_;
    uint32_t visibleStamp_ = 0;
    std::unordered_map<int, int32_t> squadOf_;
    // What the World was last told of the squads it paces (publishGroups):
    // each strike or VTOL squad member's squad, and each squad's formation flag.
    std::map<int, int> groupSent_;
    std::array<bool, 100> formationSent_{};
    int32_t startIndex_ = -1;
    int32_t scenarioMode_ = 2;   // retail's skirmish map mode
};

}  // namespace tak::ai
