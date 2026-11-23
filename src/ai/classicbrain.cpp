#include "ai/classicbrain.h"

#include "ai/classicdefence.h"
#include "ai/classicproduction.h"
#include "sim/footprint.h"
#include "sim/retailai.h"
#include "sim/retailreach.h"
#include "sim/sim.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>

#ifndef NDEBUG
#include <cctype>
#include <cstdio>
#endif

namespace ta::ai {

namespace {
// Mission kinds as the brain's 0x4de530 numbers them: the army squads compare
// a member's first mission with the move they would give (equal or not is all
// that matters). Every other mission is 0x40 | its class.
constexpr uint8_t kKindMoveGround = 0x31, kKindMoveVtol = 0x32, kKindPatrol = 0x33;
constexpr uint8_t otherKind(uint8_t cls) { return uint8_t(0x40u | cls); }
uint8_t moveKind(bool canFly) { return canFly ? kKindMoveVtol : kKindMoveGround; }

// A unit's first mission (retail unit +0x60) as the AI reads it, from its orders
// (docs/classic-ai-plan.md U1): the class byte of the mission's descriptor
// (descriptor +0x10: 1 the attack kinds, 2 AirStrike, 0 MobileBuild, 6 HelpBuild
// and RepairUnit, 19 BuildingBuild ...), the flags it copies from the
// descriptor's +0x11 (8 on MobileBuild, HelpBuild and BuildingBuild; 0x200 a
// target unit, 0x400 a goal point), its target and its goal. Run-time flag
// 0x2000 marks the type's default mission, which the mission stepper (0x4d8597
// -> 0x4d83b0) pushes onto an AI or human unit left with none, and any mission
// later pushed in front of it (0x4d7709 copies the bit): our empty queue, and a
// queue holding only the World's own auto-attack (and its return leg) on top of
// one.
struct FirstMission {
    bool any = false;
    uint8_t cls = 0;
    uint8_t kind = 0;
    uint32_t flags = 0;
    bool buildingBuild = false;
    const ta::sim::UnitType* builds = nullptr;   // what its build mission makes
    int target = 0;
    int32_t goalX = 0, goalZ = 0;
};
const ta::sim::Weapon* currentWeapon(const ta::sim::Unit& u);
FirstMission firstMission(const ta::sim::World& world, const ta::sim::Unit& u) {
    FirstMission m;
    if (u.type->isStructure()) {
        // A factory at work has a BuildingBuild mission per queued unit.
        if (!u.buildQueue.empty()) { m.any = true; m.cls = 19; m.kind = otherKind(19); m.flags = 0x8010c; m.buildingBuild = true; m.builds = u.buildQueue.front(); }
        return m;
    }
    // A flying builder's step-out (VTOL_StepOutNoFormation, its job's air
    // traffic) leads the list ahead of the job it stopped: it keeps its site
    // (buildSiteId) but its first mission is the step-out, read below.
    if (u.buildSiteId && !(!u.orders.empty() && u.orders.front().stepOut)) {
        const auto* site = world.unit(u.buildSiteId);
        m.any = true; m.cls = 0; m.kind = otherKind(0); m.flags = 0x80508; m.builds = site ? site->type : nullptr;
        if (site) { m.goalX = site->x.v; m.goalZ = site->z.v; }
        return m;
    }
    if (u.orders.empty()) return m;
    // A campaign script's order at the head (sim/mission.cpp): retail's Wait,
    // WaitForAttack, AttackUType or SelfDestructFG mission, descriptor class
    // 0x13 with its own flags (4; WaitForAttack 0x204, its watched unit the
    // target; SelfDestructFG 0), no goal point and never the default
    // mission's 0x2000: to the AI that unit is busy, as in retail.
    if (ta::sim::World::scriptHold(u)) {
        const auto& head = u.orders.front();
        const auto* s = world.scriptedOrder(head);
        m.any = true; m.cls = 19; m.kind = otherKind(19);
        m.flags = head.waitAttack ? 0x204u : s && s->kind == ta::sim::ScriptedOrder::Kind::SelfDestruct ? 0u : 4u;
        if (head.waitAttack && s) m.target = s->watched;
        return m;
    }
    // The current order is the first issued goal: path waypoints the sim splices
    // in ahead of it belong to it (retail's mission navigates on its own).
    size_t leg = u.orders.size() - 1;
    for (size_t k = 0; k < u.orders.size(); ++k)
        if (u.orders[k].goal) { leg = k; break; }
    const auto& o = u.orders[leg];
    m.any = true;
    m.goalX = o.x.v; m.goalZ = o.z.v;
    if (o.buildType) { m.cls = 0; m.flags = 0x80508; m.builds = o.buildType; m.goalX = o.buildX.v; m.goalZ = o.buildZ.v; }
    else if (o.repairTarget) { m.cls = 6; m.flags = 0x80200; m.target = o.repairTarget; }
    else if (o.reclaimFeat || o.reclaimArea) { m.cls = 11; m.flags = 0x800; }
    else if (o.load) { m.cls = 12; m.flags = 0x1000200; m.target = o.targetId; }
    else if (o.unload) { m.cls = 13; m.flags = 0x1000400; }
    // Follow_Ground (0x5eb89f: 0x3000200); a flyer's is VTOL_Follow (0x5ebd37:
    // 0x1000200), both with the ward.
    else if (o.guard) { m.cls = 5; m.flags = u.type->canFly ? 0x1000200 : 0x3000200; m.target = o.targetId; }
    else if (o.targetId) {
        m.cls = 1; m.flags = 0x1000280; m.target = o.targetId;   // Attack_Chase / Attack_Melee (0x5eb8d1 / 0x5eb8ea)
        // A flyer's attack is one of the air rows (the resolver's unit branch,
        // 4de8b0..4de9ea): AirToAir at a flyer (0x5ebd82), AirStrike on a
        // selected `dropped` weapon (0x5ebd69: class 2, 0x1000600), else
        // AirToGroundHover / AirToGround (0x5ebdb4 / 0x5ebd9b); 0x1000200 each
        // with a live target. AirStrike keeps its 0x400 with the point an
        // order passes (521a66..521abc: the target's position), not an
        // automatic one's (4d7de0 passes none: 4d6d6f clears it); its +0x22
        // follows the target (tickAirStrike).
        // tools/re/check_air_point_mission.py.
        const auto* t = world.unit(o.targetId);
        if (u.type->canFly && t && t->type) {
            const auto* w = currentWeapon(u);
            const bool strike = w && w->cursorAirstrike && !t->type->canFly;
            m.cls = strike ? 2 : 1;
            m.flags = strike && !o.autoTarget ? 0x1000600 : 0x1000200;
            if (strike) { m.goalX = o.attackPointX.v; m.goalZ = o.attackPointZ.v; }
        }
    }
    // VTOL_SEEKATTACK (row 0x5ebe7c: 0x1000600), a flyer's attack after its
    // target is lost. World's seek never carries the target, so the mission
    // ctor (0x4d6d66) has cleared the target bit 0x200, as for a dead one.
    else if (o.seekAttack) { m.cls = 19; m.flags = 0x1000400; }
    // VTOL_SEEKGUARD (row 0x5ebe95: 0x1000600), a flyer's guard after its
    // guarded unit is lost: World's seek carries none, so 0x200 is clear.
    else if (o.seekGuard) { m.cls = 19; m.flags = 0x1000400; }
    // Patrol (0x5eb94e: 0x3000410); a flyer's is VTOL_PATROL (0x5ebd50), whose
    // flags World's leg carries (kVtolPatrolFlags, 0x4000 once anchored).
    else if (o.patrol) { m.cls = 7; m.flags = u.type->canFly ? ta::sim::kVtolPatrolFlags | (o.mission.flags & 0x4000u) : 0x3000410; }
    // A factory output's GetBuilt PARK: definition class 14, flags 0x200.
    else if (o.park) { m.cls = 14; m.flags = o.mission.flags; }
    // A flyer's step-out (retail's air traffic) ahead of its mission: VTOL_StepOut,
    // VTOL_StepOutStandby or VTOL_StepOutNoFormation (rows 0x5ebee0 / 0x5ebef9 /
    // 0x5ebf12: class 14, flags 0x1000000 / 0x5000000 / 0), not a move's 0x3000400.
    else if (o.stepOut) { m.cls = 14; m.flags = o.mission.flags; }
    // SUPPRESS, fire at a point (row 0x5eb8b8: class 1, 0x1000410). Its stage-4
    // approach is retail's controller on that mission, a leg ahead of it in ours.
    else if (o.suppress || o.suppressApproach) {
        m.cls = 1; m.flags = 0x1000410; m.goalX = o.attackPointX.v; m.goalZ = o.attackPointZ.v;
    }
    // A flyer's attack at a point: AirStrike (row 0x5ebd69: class 2, 0x1000600),
    // else AirToGroundHover / AirToGround (0x5ebdb4 / 0x5ebd9b: class 1,
    // 0x1000200), less the target bit 0x200 the ctor clears with no target (4d6d66).
    else if (o.airPoint) {
        const bool strike = o.airPoint == ta::sim::Order::kAirStrike;
        m.cls = strike ? 2 : 1; m.flags = strike ? 0x1000400 : 0x1000000;
        m.goalX = o.attackPointX.v; m.goalZ = o.attackPointZ.v;
    }
    // A move: Move_Ground (0x5eb86d: 0x3000400), a flyer's VTOL_Move (0x5ebcd3:
    // 0x1000402), each with its point.
    else { m.cls = 14; m.flags = u.type->canFly ? ta::sim::kVtolMoveFlags : 0x3000400; }
    m.kind = m.cls == 14 ? moveKind(u.type->canFly) : m.cls == 7 ? kKindPatrol : otherKind(m.cls);
    // Only the World's auto-attacks (and the return move behind a leashed one):
    // they sit on the default mission, so they carry its 0x2000.
    bool automatic = true, leash = false;
    for (const auto& q : u.orders) {
        if (!q.goal) continue;
        // A flyer's auto attack has the flag only when it went in ahead of the
        // default mission (aheadOfDefault), not ahead of VTOL_SeekAttack.
        if (q.autoTarget && (q.aheadOfDefault || !u.type->canFly)) { leash = q.chaseLeashed; continue; }
        if (leash && !q.targetId && !q.patrol && !q.buildType && !q.guard && !q.load && !q.unload) { leash = false; continue; }
        automatic = false;
        break;
    }
    if (automatic) m.flags |= 0x2000;
    return m;
}
const ta::sim::Weapon* currentWeapon(const ta::sim::Unit& u) {
    const auto& w = u.type->weapons;
    if (w.empty()) return nullptr;
    const int slot = u.type->weaponSwitching ? std::clamp(u.weaponSlot, 0, int(std::min<size_t>(3, w.size())) - 1) : 0;
    return &w[size_t(slot)];
}
}  // namespace

std::pair<uint8_t, uint32_t> ClassicBrain::firstMissionOf(const ta::sim::World& world, const ta::sim::Unit& u) {
    const FirstMission m = firstMission(world, u);
    return {m.cls, m.flags};
}

ClassicBrain::ClassicBrain(int player, uint32_t roomSeed)
    : player_(player), seed_(seedFor(player, roomSeed)), profile_(catalogue_.size()),
      info_(catalogue_.size()), menu_(catalogue_.size()), count_(catalogue_.size()),
      completed_(catalogue_.size()), priority_(catalogue_.size()) {
    classicConstructSquads(squads_, 0, [&](int n) { return rand(n); });
    grids_ = classicDrawBlockGrids([&](int n) { return rand(n); });
}

ClassicBrain::ClassicBrain(int player, uint32_t roomSeed, const ta::sim::TypeRegistry& registry,
                           int32_t largestEnemyPopulation)
    : player_(player), seed_(seedFor(player, roomSeed)),
      catalogue_(ClassicCatalogue::from(registry)), profile_(catalogue_.size()),
      info_(catalogue_.size()), menu_(catalogue_.size()), count_(catalogue_.size()),
      completed_(catalogue_.size()), priority_(catalogue_.size()) {
    // 0x4f6660: the AI manager (0x40fed0) before the build manager (0x4121e0),
    // whose constructor draws the block grids (0x4110f0) before its creation pass.
    classicConstructSquads(squads_, largestEnemyPopulation, [&](int n) { return rand(n); });
    grids_ = classicDrawBlockGrids([&](int n) { return rand(n); });
    for (size_t t = 1; t < catalogue_.size(); ++t) {
        info_[t] = ClassicTypeInfo::from(*catalogue_.types[t], &registry);
        index_.emplace(catalogue_.types[t], uint16_t(t));
    }
    // 0x4c1d35: an FBI builder type's menu is CanBuild\<unitname>\*.tdf, each file
    // name looked up as a unit name (unknown ones dropped), sorted by [Menu]
    // Priority. The registry's build tree is already in that order, ties by name
    // (retail keeps ties in its archive listing's order, assumed to be by name;
    // the order only maps draws to types, not the odds).
    for (size_t t = 1; t < catalogue_.size(); ++t) {
        const auto* type = catalogue_.types[t];
        if (!type || !info_[t].builder) continue;
        const std::string& name = type->classicFbi.unitName.empty() ? type->id : type->classicFbi.unitName;
        for (const auto& entry : registry.buildable(name))
            if (const auto it = catalogue_.byName.find(entry); it != catalogue_.byName.end())
                menu_[t].push_back(it->second);
    }
    creationPass();
}

void ClassicBrain::creationPass() {
    // The build manager computes its priorities once as it is created at game
    // start (0x4102f0 -> 0x410810), before the profile file is read and before
    // the player has units or a store: every limit is still -1, every count and
    // the population 0, the store empty (fill 0) with an empty income history,
    // which reads as a shortfall. Reproduced exactly against a captured retail
    // PrintWeights table (tools/re/check_ai_printweights.py, Taros at tick 16).
    const ta::sim::RetailResourceHistory none;
    const bool shortfall = none.shortfall(0.0f);
    for (size_t t = 1; t < catalogue_.size(); ++t)
        priority_[t] = ta::sim::retailBuildPriority(info_[t].priorityInputs(0, 0, -1), 0, storeFill(0, 0),
                                                     shortfall, [&](int n) { return rand(n); });
    ++refreshes_;
}

void ClassicBrain::useProfile(const ClassicProfileScript& script) {
    profile_.reset(catalogue_.size());   // as retail's ReloadAIProfiles does first
    profile_.execute(script.text, catalogue_);
    profile_.path = script.path;
}

int ClassicBrain::buildWeight(size_t t) const {
    if (t == 0 || t >= priority_.size()) return 0;
    // An AI seat always has its limits on (retail player+0xe3 = 1 in skirmish).
    return ta::sim::retailAiBuildWeight(true, profile_.limit[t], count_[t], profile_.pref[t],
                                         int8_t(priority_[t]));
}

void ClassicBrain::recount(const ta::sim::World& world) {
    std::fill(count_.begin(), count_.end(), int16_t(0));
    std::fill(completed_.begin(), completed_.end(), int16_t(0));
    ownTotal_ = 0;
    for (const auto& u : world.units()) {
        if (u.player != player_ || !u.alive() || !u.type) continue;
        ++ownTotal_;
        const auto it = index_.find(u.type);
        if (it == index_.end()) continue;
        ++count_[it->second];
        if (!u.underConstruction) ++completed_[it->second];
    }
}

void ClassicBrain::recomputePriorities(const ta::sim::World& world) {
    // The owner's live unit count (player+0xe8) and its store: fill = stored /
    // capacity (0 under 0.01 capacity), shortfall from the 29-of-30-tick income
    // and REQUEST history the World keeps for every player (displayResources):
    // 410928/410938 subtract 401350, the requested total, not mana paid.
    unsigned population = 0;
    for (const auto& u : world.units())
        if (u.player == player_ && u.alive() && u.type) ++population;
    const auto& me = world.player(player_);
    const float stored = float(me.mana);
    const float capacity = me.manaCapacity();
    const float ratio = storeFill(stored, capacity);
    const bool shortfall = me.displayResources.shortfall(stored);
    for (size_t t = 1; t < catalogue_.size(); ++t) {
        const auto in = info_[t].priorityInputs(count_[t], completed_[t], profile_.limit[t]);
        priority_[t] = ta::sim::retailBuildPriority(in, uint16_t(population), ratio, shortfall,
                                                     [&](int n) { return rand(n); });
    }
    ++refreshes_;
}

void ClassicBrain::bookkeeping(const ta::sim::World& world, uint32_t simTick) {
    if (ta::sim::retailTickPlayerCache(clock_, simTick, [&](int n) { return rand(n); }))
        recomputePriorities(world);
    recount(world);
}

namespace {
// 0x4e0810: a start's 16.16 position shifted down to its 16-pixel cell.
int16_t startCell(float v) { return int16_t(uint32_t(int32_t(std::lround(double(v) * 65536.0))) >> 20); }
}

void ClassicBrain::setStartTable(const std::vector<std::pair<float, float>>& positions) {
    // Retail always holds ten start records; those past the map's own are empty.
    squads_.starts.assign(std::max<size_t>(positions.size(), 10), std::nullopt);
    for (size_t k = 0; k < positions.size(); ++k)
        squads_.starts[k] = ClassicCell{startCell(positions[k].first), startCell(positions[k].second)};
    zonePlanes_.clear();
}

void ClassicBrain::setStart(int index, float x, float z) {
    if (index < 0) return;
    auto cell = startCell;
    if (squads_.starts.size() <= size_t(index)) squads_.starts.resize(size_t(index) + 1);
    squads_.starts[size_t(index)] = ClassicCell{cell(x), cell(z)};
    startIndex_ = index;
    zonePlanes_.clear();
}

std::vector<ClassicSquadUnit> ClassicBrain::squadUnits(const ta::sim::World& world) {
    std::vector<ClassicSquadUnit> units;
    std::unordered_set<int> own;
    for (const auto& u : world.units()) {
        if (u.player != player_ || !u.type) continue;
        own.insert(u.id);
        const auto* t = u.type;
        const auto it = index_.find(t);
        const ClassicTypeInfo* info = it == index_.end() ? nullptr : &info_[it->second];
        ClassicSquadUnit s;
        s.id = u.id;
        const bool structure = t->isStructure();
        const bool armed = info ? info->armed : !t->weapons.empty();
        // The port's readings of the unit bits (docs/classic-ai-plan.md U6): a unit
        // in the World is a live slot, a dead one still listed is dying; 0x20 is
        // the World's selectable bit (made with every unit, 0x511990, U16; cleared
        // while a mission script runs the unit, so the AI leaves it alone).
        s.flags = ClassicSquadUnit::kLive | (u.alive() ? 0u : ClassicSquadUnit::kDying) |
                  (u.selectable ? ClassicSquadUnit::kSelectable : 0u) |
                  (structure ? ClassicSquadUnit::kStructure : 0u) | (armed ? ClassicSquadUnit::kArmed : 0u);
        s.commander = info ? info->commander : t->commander;
        s.sacredSite = info && info->manaSite;
        s.builder = info && info->builder;
        s.finished = !u.underConstruction;
        s.carried = u.embarked();
        s.mover = !structure;
        s.type = it == index_.end() ? 0 : it->second;
        s.menu = int32_t(menu(s.type).size());
        // A factory's BuildingBuild mission is our production queue. The first
        // mission's class (U1) matters only to the constructor planner (P4b).
        s.producing = !u.buildQueue.empty();
        s.x = u.x.v; s.z = u.z.v;
        s.footX = int16_t(t->footX); s.footZ = int16_t(t->footZ);
        s.squad = squadOf(u.id);
        s.typeFlags = (info && info->builder ? 0x100u : 0u) | (t->canFly ? 0x800u : 0u) |
                      (armed ? 0x10000u : 0u) | (t->floater ? 0x80000u : 0u);
        s.secondaryFlags = (s.commander ? 0x40000u : 0u) | (s.sacredSite ? 0x80000000u : 0u);
        s.minWaterDepth = info ? info->minWaterDepth : int16_t(-10000);
        const FirstMission m = firstMission(world, u);
        s.mission = m.any; s.missionClass = m.cls; s.missionFlags = m.flags; s.buildingBuild = m.buildingBuild;
        s.missionTarget = m.target; s.goalX = m.goalX; s.goalZ = m.goalZ;
        if (m.builds && (m.flags & 8))
            if (const auto bt = index_.find(m.builds); bt != index_.end()) s.buildsManaSite = info_[bt->second].manaSite;
        if (const auto* w = currentWeapon(u)) {
            // 5307e9 stores the reload as a 16-bit tick count.
            s.weaponDamage = uint16_t(w->damage);
            s.weaponReload = uint16_t(int32_t(double(w->reload) * 30.0));
            s.range = w->range;
        }
        s.missionKind = m.kind;
        s.hp = int16_t(u.hp.floorInt());
        s.maxDamage = uint32_t(t->maxHp);
        s.sight = int16_t(world.sightDistance(*t));
        s.navigator = !structure && !t->canFly;
        s.naval = info && info->floater && info->minWaterDepth > 0;
        s.cellX = int16_t(ta::sim::footprintOrigin(u.x, t->footX));
        s.cellZ = int16_t(ta::sim::footprintOrigin(u.z, t->footZ));
        units.push_back(s);
    }
    // A member that is no longer the player's unit (removed from the World, or
    // given away) leaves its squad the way set-squad removes one.
    for (auto& squad : squads_.squads)
        for (size_t k = 0; k < squad.members.size();)
            if (!own.count(squad.members[k])) { squad.members[k] = squad.members.back(); squad.members.pop_back(); }
            else ++k;
    std::erase_if(squadOf_, [&](const auto& e) { return !own.count(e.first); });
    return units;
}

void ClassicBrain::scheduler(const ta::sim::World& world, uint32_t simTick,
                             const std::function<void(const ta::net::Command&)>& sink) {
    std::vector<ClassicSquadUnit> units;
    bool snapshot = false;
    auto random = [&](int n) { return rand(n); };
    auto command = [&](ta::net::Cmd kind, int unitId) {
        ta::net::Command c;
        c.kind = kind; c.player = uint8_t(player_); c.unitId = unitId; c.targetId = 0;
        sink(c);
    };
    ta::sim::retailTickAiSchedule(squads_.schedule, simTick, random,
        [&] {   // 0x40fe80
            units = squadUnits(world); snapshot = true;
            classicDropDying(squads_, units);
        },
        [&] {   // 0x40eeb0
            const ClassicAssignContext ctx{simTick, scenarioMode_, startIndex_};
            classicAssign(squads_, units, ctx,
                [&](size_t i) { command(ta::net::Cmd::Stance, units[i].id); },   // 0x5198a0(u, 2)
                [&](size_t i) { command(ta::net::Cmd::Stop, units[i].id); });    // 0x4d6a50(u, 0)
        },
        [&](unsigned slot, auto& clock) {
            // Each kind's think reschedules itself (a strike then runs its
            // cohesion pass 0x40abd0, and an empty strike or VTOL squad goes
            // inactive); an occupied squad runs its own think.
            using K = ta::sim::RetailAiSquadKind;
            ta::sim::retailTickAiSquad(squads_.squads[slot], clock, simTick, random,
                [&](auto&) { classicStrikeRecruit(squads_, units, int(slot)); },
                [&](auto& squad) {
                    switch (squad.kind) {
                    case K::Base: baseThink(world, int(slot), units, sink); break;
                    case K::Strike: strikeThink(world, int(slot), clock, units, simTick, sink); break;
                    case K::Backup: backupThink(world, int(slot), units, sink); break;
                    case K::Vtol: vtolThink(world, int(slot), units, sink); break;
                    case K::Reserve: break;
                    }
                });
        });
    publishGroups(world, sink);
    if (!snapshot) return;
    for (const auto& u : units) {
        if (u.squad) squadOf_[u.id] = u.squad;
        else squadOf_.erase(u.id);
    }
}

void ClassicBrain::publishGroups(const ta::sim::World& world,
                                 const std::function<void(const ta::net::Command&)>& sink) {
    using K = ta::sim::RetailAiSquadKind;
    auto send = [&](int unitId, int slot, bool formation) {
        ta::net::Command c;
        c.kind = ta::net::Cmd::AiGroup; c.player = uint8_t(player_);
        c.unitId = unitId; c.targetId = slot; c.queue = formation ? 1 : 0;
        sink(c);
    };
    std::map<int, int> want;
    for (int slot = 1; slot < 100; ++slot) {
        const auto& squad = squads_.squads[size_t(slot)];
        const bool paced = squad.kind == K::Strike || squad.kind == K::Vtol;
        const bool formation = paced && squad.active != 0;
        if (formation != formationSent_[size_t(slot)]) {
            send(0, slot, formation);
            formationSent_[size_t(slot)] = formation;
        }
        if (paced) for (const int id : squad.members) want[id] = slot;
    }
    for (auto it = groupSent_.begin(); it != groupSent_.end();) {
        if (want.count(it->first)) { ++it; continue; }
        // Left its squad: told so while it is still this player's unit.
        if (const auto* u = world.unit(it->first); u && u->alive() && u->player == player_) send(it->first, 0, false);
        it = groupSent_.erase(it);
    }
    for (const auto& [id, slot] : want) {
        auto& sent = groupSent_[id];
        if (sent != slot) { send(id, slot, false); sent = slot; }
    }
}

uint16_t ClassicBrain::population(const ta::sim::World& world) const {
    unsigned n = 0;
    for (const auto& u : world.units())
        if (u.player == player_ && u.alive() && u.type) ++n;
    return uint16_t(n);
}

void ClassicBrain::baseThink(const ta::sim::World& world, int slot, std::vector<ClassicSquadUnit>& units,
                             const std::function<void(const ta::net::Command&)>& sink) {
    const bool planned = classicBaseMembers(squads_, units, slot,
        // 0x412000: a unit of the visible-enemy list inside the ring-0 radius.
        [&](const std::pair<int32_t, int32_t>& centre, int32_t radius) {
            return classicThreat(visibleEnemies(world), centre, radius);
        },
        [&](size_t i) { planFactory(world, units[i], sink); },
        [&](size_t i, bool danger) { planConstructor(world, slot, units, i, danger, sink); },
        [&](size_t i, bool danger) { planCombat(world, slot, units, units[i], danger, false, sink); });
    if (planned) baseLaunch(world, slot, units, sink);
}

void ClassicBrain::planFactory(const ta::sim::World& world, ClassicSquadUnit& unit,
                               const std::function<void(const ta::net::Command&)>& sink) {
    auto random = [&](int n) { return rand(n); };
    const auto* builder = unit.type ? catalogue_.types[unit.type] : nullptr;
    if (!builder) return;
    // Our World's per-player construction allocation is retail's: the store over
    // the tick's full-rate construction demand, capped at 1 (sim.cpp, 0x51d837).
    const float allocation = float(world.player(player_).constructionAllocation);
    classicPlanFactory(allocation, unit.producing, population(world), menu(unit.type),
        [&](uint16_t t) { return buildWeight(t); },
        [&](uint16_t t) { return info_[t].lodestoneClass; },
        [&](uint16_t t) { return catalogue_.types[t] && catalogue_.types[t]->side == builder->side; },
        [&](uint16_t t) { return info_[t].cost; }, random,
        [&](uint16_t t) {
            ta::net::Command c;
            c.kind = ta::net::Cmd::Train; c.player = uint8_t(player_);
            c.unitId = unit.id; c.targetId = 1;
            std::strncpy(c.type, catalogue_.types[t]->id.c_str(), sizeof c.type - 1);
            sink(c);
            ++queues_;
            // Its first mission is now that BuildingBuild (firstMission's reading).
            unit.producing = unit.mission = unit.buildingBuild = true;
            unit.missionClass = 19; unit.missionKind = otherKind(19); unit.missionFlags = 0x8010c;
            unit.buildsManaSite = info_[t].manaSite;
        });
}

void ClassicBrain::readSites(const ta::sim::World& world) {
    sitesRead_ = true;
    zonePlanes_.clear();
    // 0x4e0810's sacred-site table: one record per sacred feature, its anchor
    // cell, in the map's scan order (the World's list is built the same way).
    // Retail clears a record when its feature is deleted (0x496380); the World's
    // sacred features are never removed, so every record stays.
    const auto& cells = world.mapPlacementCells();
    const bool plane = !cells.empty() && cells.size() == size_t(world.mapW()) * size_t(world.mapH());
    sacredTypes_.assign(plane ? world.mapPlacementTypes().size() : 0, false);
    // (Kingdoms sacred sites: none in TA.)
    // The expansion bases' anchors (p0 = -(k+1)) index the same table.
    squads_.sites = sites_;
}

std::pair<int16_t, int16_t> ClassicBrain::siteFootprint(const ta::sim::World& world, const ClassicCell& cell) const {
    const auto& cells = world.mapPlacementCells();
    const int w = world.mapW(), h = world.mapH();
    if (cells.empty() || cells.size() != size_t(w) * size_t(h)) {
        for (const auto& [at, foot] : sacredRects_)
            if (at == cell) return foot;
        return {1, 1};
    }
    const auto& types = world.mapPlacementTypes();
    const auto f = classicSacredFeature(cell.first, cell.second, uint32_t(types.size()),
        [&](int cx, int cz) {
            if (cx < 0 || cz < 0 || cx >= w || cz >= h) return ClassicFeatureCell{};
            const auto& c = cells[size_t(cz) * size_t(w) + size_t(cx)];
            return ClassicFeatureCell{c.feature, c.backX, c.backZ};
        },
        [&](uint16_t t) { return t < sacredTypes_.size() && sacredTypes_[t]; });
    if (!f || *f >= types.size()) return {1, 1};
    return {int16_t(types[*f].footX), int16_t(types[*f].footZ)};
}

const ClassicZoneLabels& ClassicBrain::zoneLabels(const ta::sim::World& world, const ta::sim::NavGrid& nav,
                                                  int footX, int footZ) {
    for (const auto& p : zonePlanes_)
        if (p.nav == &nav && p.footX == footX && p.footZ == footZ) return p.labels;
    if (!sitesRead_) readSites(world);
    // 0x4e0a20 at map load, over the grid's static bit (mode 1, unbounded).
    const auto& blocked = staticBlocked(world, nav, footX, footZ);
    auto labels = classicZoneLabels(squads_.starts, squads_.sites,
        [&](const ClassicCell& c) { return siteFootprint(world, c); },
        [&](int sx, int sz, int sw, int sh, int tx, int tz, int tw, int th) {
            return ta::sim::retailRectangleReachable(nav.width(), nav.height(), sx, sz, sw, sh, tx, tz, tw, th,
                true, false, [&](int x, int z) { return blocked[size_t(z) * size_t(nav.width()) + size_t(x)] ? 8 : 0; });
        });
    zonePlanes_.push_back({&nav, footX, footZ, std::move(labels)});
    return zonePlanes_.back().labels;
}

const ClassicZoneLabels* ClassicBrain::unitLabels(const ta::sim::World& world, const ClassicSquadUnit& unit) {
    const auto* u = world.unit(unit.id);
    if (!u || !u->type || u->type->isStructure() || u->type->canFly) return nullptr;
    return &zoneLabels(world, world.navFor(u->type), u->type->footX, u->type->footZ);
}

bool ClassicBrain::siteOpen(const ta::sim::World& world, uint16_t t, int16_t x, int16_t z) const {
    // 0x507d10(type, 0, cell, 1, 0): the World's site test at the
    // footprint's centre (canPlaceSite: a mobile type by its footprint cells
    // too, as the builder will place it). Retail's 0x507400 refuses a site
    // any live entity stands on, and has no in-place upgrade; the World lets
    // a Divine Lodestone replace the player's own finished lodestone (a
    // human's order), which the Classic AI must not do: it takes the next
    // free site.
    const auto* type = t < catalogue_.types.size() ? catalogue_.types[t] : nullptr;
    if (!type) return false;
    const ClassicTypeInfo& f = info_[t];
    const auto c = classicSiteCentre({x, z}, int16_t(f.footX), int16_t(f.footZ));
    const float px = float(c.first) / 65536.0f, pz = float(c.second) / 65536.0f;
    return world.canPlaceSite(type, px, pz, player_) && !world.lodestoneUpgradeSource(type, px, pz, player_);
}

std::optional<std::pair<int16_t, int16_t>> ClassicBrain::depositSite(const ta::sim::World& world, uint16_t t,
                                                                     std::pair<int32_t, int32_t> centre,
                                                                     int32_t radius) {
    if (!sitesRead_) readSites(world);
    const ClassicTypeInfo& f = info_[t];
    return classicDepositSite(sites_, centre, radius, int16_t(f.footX), int16_t(f.footZ),
        [&](int16_t x, int16_t z) { return sacredCell(world, x, z); },
        [&](int16_t x, int16_t z) { return siteOpen(world, t, x, z); });
}

bool ClassicBrain::sacredCell(const ta::sim::World& world, int x, int z) const {
    const auto& cells = world.mapPlacementCells();
    const int w = world.mapW(), h = world.mapH();
    if (cells.empty() || cells.size() != size_t(w) * size_t(h)) {
        // A World without a feature plane (synthetic tests): the sites' footprints.
        for (const auto& [at, foot] : sacredRects_)
            if (x >= at.first && z >= at.second && x < at.first + foot.first && z < at.second + foot.second) return true;
        return false;
    }
    return classicSacredCell(x, z, uint32_t(world.mapPlacementTypes().size()),
        [&](int cx, int cz) {
            if (cx < 0 || cz < 0 || cx >= w || cz >= h) return ClassicFeatureCell{};
            const auto& c = cells[size_t(cz) * size_t(w) + size_t(cx)];
            return ClassicFeatureCell{c.feature, c.backX, c.backZ};
        },
        [&](uint16_t f) { return f < sacredTypes_.size() && sacredTypes_[f]; });
}

const std::vector<uint8_t>& ClassicBrain::staticBlocked(const ta::sim::World& world, const ta::sim::NavGrid& nav,
                                                        int footX, int footZ) {
    for (const auto& p : staticPlanes_)
        if (p.nav == &nav && p.footX == footX && p.footZ == footZ) return p.blocked;
    // The grid's static bit is set at map load only (classicStaticPlane), so the
    // buildings and units that come later never count. A cell's own rating is
    // the World's search grade without bodies (searchCellGrade: the map feature,
    // then the terrain); without a feature plane (synthetic Worlds) the terrain
    // grade counts the obstacle overlay, as the World's does.
    const auto& cells = world.mapPlacementCells();
    const bool features = !cells.empty() && cells.size() == size_t(nav.width()) * size_t(nav.height());
    staticPlanes_.push_back({&nav, footX, footZ,
        classicStaticPlane(nav.width(), nav.height(), footX, footZ, [&](int x, int z) {
            return features ? world.mapFeatureGrade(x, z) == 0 || !nav.terrainWalkable(x, z) : !nav.walkable(x, z);
        })});
    return staticPlanes_.back().blocked;
}

void ClassicBrain::planConstructor(const ta::sim::World& world, int slot, std::vector<ClassicSquadUnit>& units,
                                   size_t i, bool threat, const std::function<void(const ta::net::Command&)>& sink) {
    ClassicSquadUnit& member = units[i];
    const auto* unit = world.unit(member.id);
    const auto* builder = member.type ? catalogue_.types[member.type] : nullptr;
    if (!unit || !unit->type || !builder) return;
    if (!sitesRead_) readSites(world);
    const detail::ClassicUnitIndex index(units);
    auto random = [&](int n) { return rand(n); };
    const auto centre = classicBaseCentre(squads_, units, index, slot);
    if (!centre) return;   // the base think stops before its members without one

    ClassicConstructorUnit u;
    u.commander = member.commander;
    u.hp = int16_t(unit->hp.floorInt());
    u.maxDamage = int32_t(unit->type->maxHp);
    // TA units carry no personal mana pool: shots are paid from stored E/M.
    u.mana = 0;
    u.maxMana = 0;
    if (currentWeapon(*unit)) {
        u.weapon = true;
        u.shotCost = 0;
    }
    u.mission = member.mission;
    u.missionClass = member.missionClass;
    u.building = member.mission && (member.missionFlags & 8);
    u.navigator = !unit->type->canFly;

    auto siteType = [&](uint16_t t) {
        const ClassicTypeInfo& f = info_[t];
        ClassicSiteType s;
        s.footX = int16_t(f.footX); s.footZ = int16_t(f.footZ); s.minWaterDepth = f.minWaterDepth;
        s.tower = classicSiteTower(uint8_t(f.bmcode), f.armed, f.firstWeaponRange, int32_t(menu(t).size()));
        s.lodestone = f.lodestoneClass;
        return s;
    };
    struct Host {
        ClassicBrain& b;
        const ta::sim::World& world;
        const std::vector<ClassicSquadUnit>& units;
        const detail::ClassicUnitIndex& index;
        int slot;
        ClassicSquadUnit& member;
        const ta::sim::Unit& unit;
        const ta::sim::UnitType& builder;
        std::pair<int32_t, int32_t> centre;
        decltype(random)& rnd;
        decltype(siteType)& placeType;
        const std::function<void(const ta::net::Command&)>& sink;

        float allocation() const { return float(world.player(b.player_).constructionAllocation); }
        int32_t firepower() const { return classicBaseFirepower(b.squads_, units, index, slot, 4); }
        int32_t builders() const { return classicBaseCount(b.squads_, units, index, slot, 2); }
        int32_t desiredBuilders() const { return b.squads_.squads[size_t(slot)].parameters[1]; }
        uint32_t tick() const { return world.tickCount(); }
        uint32_t alarm() const { return world.player(b.player_).aiThreatUntil; }
        uint16_t draw(bool special) {
            return classicMenuDraw(b.menu(member.type), special,
                [&](uint16_t t) { return b.buildWeight(t); },
                [&](uint16_t t) { return b.info_[t].lodestoneClass; },
                [&](uint16_t t) { return b.catalogue_.types[t] && b.catalogue_.types[t]->side == builder.side; },
                rnd);
        }
        ClassicConstructorType type(uint16_t t) const {
            const ClassicTypeInfo& f = b.info_[t];
            return {uint8_t(f.bmcode), int32_t(b.menu(t).size()), f.armed};
        }
        bool idleFactory(uint16_t t) const { return classicIdleFactory(b.squads_, units, index, slot, t); }
        int32_t typeCount(uint16_t t) const { return t >= 1 && t < b.count_.size() ? b.count_[t] : 0; }
        std::optional<int32_t> radius(unsigned ring) const {
            return classicBaseRadius(b.squads_, units, index, slot, ring);
        }
        std::optional<std::pair<int32_t, int32_t>> site(uint16_t t, int32_t r) {
            const ClassicSiteType s = placeType(t);
            return classicPlaceSite(s,
                [&] { return b.depositSite(world, t, centre, r); },
                [&] {
                    return classicScatterSite(b.grids_, centre, r, s, rnd,
                        [&](int16_t x, int16_t z) { return b.siteOpen(world, t, x, z); },
                        [&](int x, int z) { return b.sacredCell(world, x, z); });
                });
        }
        bool reachable(uint16_t t, std::pair<int32_t, int32_t> at) const {
            // 0x4e1570 from the builder's cell (1 x 1) to the site's footprint grown
            // by its builddistance, over the grid's static bit (mode 1).
            const ClassicTypeInfo& f = b.info_[t];
            const ClassicReachRect r = classicReachRect(at, int16_t(f.footX), int16_t(f.footZ),
                                                        uint16_t(b.info_[member.type].buildDistance));
            const auto& nav = world.navFor(unit.type);
            const auto& blocked = b.staticBlocked(world, nav, unit.type->footX, unit.type->footZ);
            return ta::sim::retailRectangleReachable(nav.width(), nav.height(),
                ta::sim::footprintOrigin(unit.x, unit.type->footX), ta::sim::footprintOrigin(unit.z, unit.type->footZ),
                1, 1, r.x, r.z, r.w, r.h, true, false,
                [&](int x, int z) { return blocked[size_t(z) * size_t(nav.width()) + size_t(x)] ? 8 : 0; });
        }
        bool lodestoneBusy() const { return classicLodestoneBusy(b.squads_, units, index, slot); }
        void build(uint16_t t, std::pair<int32_t, int32_t> at) {
            ta::net::Command c;
            c.kind = ta::net::Cmd::Build; c.player = uint8_t(b.player_); c.unitId = member.id;
            c.x = float(at.first) / 65536.0f; c.z = float(at.second) / 65536.0f;
            std::strncpy(c.type, b.catalogue_.types[t]->id.c_str(), sizeof c.type - 1);
            sink(c);
            ++b.builds_;
            if (b.info_[t].lodestoneClass) ++b.lodestoneBuilds_;
            // 0x4d78a0 clears its missions and puts the build first: MobileBuild.
            member.mission = true; member.missionClass = 0; member.missionKind = otherKind(0); member.missionFlags = 0x80508;
            member.buildingBuild = false; member.buildsManaSite = b.info_[t].manaSite;
            member.missionTarget = 0; member.goalX = at.first; member.goalZ = at.second;
        }
        void cancel() { b.clearMissions(member, sink); }
        void retreat() { b.planCombat(world, slot, units, member, threat, true, sink); }
        void combat() { b.planCombat(world, slot, units, member, threat, false, sink); }
        bool threat;
    } host{*this, world, units, index, slot, member, *unit, *builder, *centre, random, siteType, sink, threat};
    classicPlanConstructor(host, u, threat);
}

ClassicEnemy ClassicBrain::enemyView(const ta::sim::World&, const ta::sim::Unit& e) const {
    // Unit +0x130 as the AI tests it: a unit still in the World is a live slot,
    // a dead one dying; its low bits 1 on the ground, 2 in the air, 0 carried;
    // 0x8000000 armed. 0x2000 is never set (U16): retail sets it only on a
    // scenario placement whose record asks for it (0x514c92..0x514cb3) until a
    // MakeSelectable mission clears it; a unit or site being built is created
    // with 0x20 and without 0x2000 (0x512060 -> 0x511990), so it is listed,
    // threatens, and is picked with the unfinished divisor (+0x108 != 0).
    const auto it = index_.find(e.type);
    const ClassicTypeInfo* info = it == index_.end() ? nullptr : &info_[it->second];
    ClassicEnemy c;
    c.id = e.id;
    c.flags = ClassicEnemy::kLive | (e.alive() && e.hp > ta::sim::Fixed() ? 0u : ClassicEnemy::kDying) |
              (e.embarked() ? 0u : e.flightGroundMode==2 ? 2u : 1u) |
              ((info ? info->armed : !e.type->weapons.empty()) ? ClassicSquadUnit::kArmed : 0u);
    c.x = e.x.v; c.y = (e.type->canFly ? e.flightY : e.groundY).v; c.z = e.z.v;
    c.cloaked = e.cloaked;
    c.cellX = int16_t(ta::sim::footprintOrigin(e.x, e.type->footX));
    c.cellZ = int16_t(ta::sim::footprintOrigin(e.z, e.type->footZ));
    c.unfinished = e.underConstruction;
    c.manaSite = info && info->manaSite;
    return c;
}

std::vector<ClassicEnemy> ClassicBrain::visibleEnemies(const ta::sim::World& world) const {
    // 411700's list +5 as of the last rebuild (it re-checks nothing, so a unit
    // that has since died is still listed, as dying).
    std::vector<ClassicEnemy> out;
    for (const int id : visible_)
        if (const auto* e = world.unit(id); e && e->type) out.push_back(enemyView(world, *e));
    return out;
}

void ClassicBrain::refreshVisible(const ta::sim::World& world) {
    // The World rebuilds its lists on the bookkeeping clock (every >= 7 ticks)
    // as retail's 0x411700 does: list +5 drops only 0x2000 (U10), which a site
    // still being built does not carry (U16), so the World's list holds such
    // sites and the brain takes it as it is when the stamp moves.
    if (player_ < 0 || player_ >= world.numPlayers()) return;
    const auto& owner = world.player(player_);
    const uint32_t stamp = owner.cacheClock.enabled ? owner.cacheClock.lastRefresh : owner.visibleRefresh;
    if (stamp == visibleStamp_) return;
    visibleStamp_ = stamp;
    const auto listed = world.visibleEnemies(player_);
    visible_.assign(listed.begin(), listed.end());
}

bool ClassicBrain::sendOrder(const ta::sim::World& world, ClassicSquadUnit& member, bool patrol, bool queue,
                             const ClassicPoint& at, int32_t a6, const std::function<void(const ta::net::Command&)>& sink) {
    const auto* unit = world.unit(member.id);
    if (!unit || !unit->type) return false;
    // A retail MOVE with mission +0x4e = a6 > 0 ends as soon as the unit is
    // within a6 of its point (the centre leg's r, the retreat's 16, a member
    // sent home): a unit already that close skips the leg, one farther off
    // heads for the circle's edge on its way in.
    float x = float(at[0]) / 65536.0f, z = float(at[2]) / 65536.0f;
    if (!patrol && a6 > 0) {
        const float ux = unit->x.toFloat(), uz = unit->z.toFloat();
        const float dx = ux - x, dz = uz - z, d = std::sqrt(dx * dx + dz * dz);
        if (d <= float(a6)) return false;
        x += dx * float(a6) / d; z += dz * float(a6) / d;
    }
    ta::net::Command c;
    c.kind = patrol ? ta::net::Cmd::Patrol : ta::net::Cmd::Move;
    c.player = uint8_t(player_); c.unitId = member.id;
    c.x = x; c.z = z;
    c.queue = queue ? 1 : 0;
    sink(c);
    // Its first mission is now that move (or patrol); a build is gone.
    if (!queue) {
        member.mission = true; member.missionClass = patrol ? 7 : 14;
        member.missionKind = patrol ? kKindPatrol : moveKind(unit->type->canFly);
        const bool flyer = unit->type->canFly;
        member.missionFlags = patrol ? (flyer ? ta::sim::kVtolPatrolFlags : 0x3000410u)
                                     : (flyer ? ta::sim::kVtolMoveFlags : 0x3000400u);
        member.buildingBuild = member.buildsManaSite = false; member.missionTarget = 0;
        member.goalX = at[0]; member.goalZ = at[2];
    }
    return true;
}

void ClassicBrain::clearMissions(ClassicSquadUnit& member, const std::function<void(const ta::net::Command&)>& sink) {
    ta::net::Command c;
    c.kind = ta::net::Cmd::Stop; c.player = uint8_t(player_); c.unitId = member.id;
    sink(c);
    // 0x4d6a50(unit, 0): its missions are gone (our Stop keeps none).
    member.mission = member.buildingBuild = member.buildsManaSite = false;
    member.missionClass = 0; member.missionKind = 0; member.missionFlags = 0; member.missionTarget = 0;
}

int32_t ClassicBrain::largestEnemyPopulation(const ta::sim::World& world) const {
    // 0x40c240's E: each active record of kind 1-3 with an index that is not
    // allied to this player (a player is allied to itself), by +0xe8.
    std::vector<int32_t> count(size_t(std::max(world.numPlayers(), 0)), 0);
    for (const auto& u : world.units())
        if (u.alive() && u.type && u.player >= 0 && u.player < int(count.size())) ++count[size_t(u.player)];
    int32_t most = 0;
    for (int p = 0; p < int(count.size()); ++p)
        if (!world.isNeutralPlayer(p) && !world.allied(p, player_)) most = std::max(most, int32_t(uint16_t(count[size_t(p)])));
    return most;
}

bool ClassicBrain::reachTarget(const ta::sim::World& world, const ClassicSquadUnit& member, const ClassicEnemy& target,
                               int16_t range, bool limited) {
    const auto* unit = world.unit(member.id);
    if (!unit || !unit->type) return false;
    // 0x4e1570(member +0x74, member +0x78, target +0x74, (range, range), 1, flag):
    // from the member's footprint to a range x range block at the target's
    // cell, over the grid's static bit (mode 1), as the constructor's walk.
    const auto& nav = world.navFor(unit->type);
    const auto& blocked = staticBlocked(world, nav, unit->type->footX, unit->type->footZ);
    return ta::sim::retailRectangleReachable(nav.width(), nav.height(), member.cellX, member.cellZ,
        unit->type->footX, unit->type->footZ, target.cellX, target.cellZ, range, range, true, limited,
        [&](int x, int z) { return blocked[size_t(z) * size_t(nav.width()) + size_t(x)] ? 8 : 0; });
}

int ClassicBrain::pickTarget(const ta::sim::World& world, const ClassicSquadUnit& member,
                             std::pair<int32_t, int32_t> centre) {
    const auto* self = world.unit(member.id);
    if (!self || !self->type) return 0;
    const ta::sim::Weapon* weapon = currentWeapon(*self);
    // 0x40a200's player loop: every player not allied to this one (neutral
    // owners are not players of kind 1-3), its units in the World's order.
    std::vector<ClassicPickPlayer> players(size_t(std::max(world.numPlayers(), 0)));
    std::vector<bool> listed(players.size(), false);
    for (int p = 0; p < int(players.size()); ++p) listed[size_t(p)] = !world.isNeutralPlayer(p) && !world.allied(p, player_);
    std::unordered_map<int, const ta::sim::Unit*> byId;
    for (const auto& u : world.units()) {
        if (!u.type || u.player < 0 || u.player >= int(players.size()) || !listed[size_t(u.player)]) continue;
        auto& owner = players[size_t(u.player)];
        if (u.alive()) owner.population = uint16_t(owner.population + 1);
        owner.units.push_back(enemyView(world, u));
        byId.emplace(u.id, &u);
    }
    std::erase_if(players, [](const ClassicPickPlayer& p) { return p.units.empty(); });
    struct Host {
        ClassicBrain& b;
        const ta::sim::World& world;
        const ta::sim::Weapon* weapon;
        const std::vector<ClassicPickPlayer>& players;
        const std::unordered_map<int, const ta::sim::Unit*>& byId;
        const std::vector<ClassicPickPlayer>& pickPlayers() const { return players; }
        // U12: the lobby's cap; an uncapped World reads as retail's options default.
        uint16_t unitCap() const { return uint16_t(world.unitCap() > 0 ? world.unitCap() : 250); }
        int random(int n) { return b.rand(n); }
        int32_t damage(const ClassicSquadUnit&, const ClassicEnemy& e) const {
            const auto it = byId.find(e.id);
            return weapon && it != byId.end() ? int32_t(weapon->damageVs(it->second->type)) : 0;
        }
        bool visible(const ClassicEnemy& e) const {
            const auto it = byId.find(e.id);
            return it != byId.end() && world.playerSees(b.player_, *it->second);
        }
        bool reachable(const ClassicSquadUnit& m, const ClassicEnemy& t, int16_t range, bool naval) {
            return b.reachTarget(world, m, t, range, naval);
        }
    } host{*this, world, weapon, players, byId};
    return classicPickTarget(host, member, centre);
}

namespace {
// The army squads' host (ai/classicwaves.h) over the brain and the World.
struct WaveHost {
    ClassicBrain& b;
    const ta::sim::World& world;
    std::vector<ClassicSquadUnit>& units;
    const std::function<void(const ta::net::Command&)>& sink;
    std::function<int(int)> rnd{};
    std::function<bool(ClassicSquadUnit&, bool, const ClassicPoint&, int32_t)> send{};
    std::function<void(ClassicSquadUnit&)> clearer{};
    std::function<int(const ClassicPoint&, size_t)> picker{};
    std::function<ClassicEnemy(const ta::sim::Unit&)> view{};
    std::function<bool(std::pair<int32_t, int32_t>, int32_t)> threatTest{};
    std::function<bool(const ClassicSquadUnit&, const ClassicEnemy&, int16_t, bool)> reach{};
    std::unordered_map<int, ClassicEnemy> seen{};
    uint32_t orders = 0, patrols = 0;

    int random(int n) { return rnd(n); }
    int pick(const ClassicPoint& c, size_t i) { return picker(c, i); }
    const ClassicEnemy* target(int id) {
        // 0x40ddd0: a unit the World still holds (live slot; dead ones read as dying).
        if (!id) return nullptr;
        const auto* u = world.unit(id);
        if (!u || !u->type) return nullptr;
        return &(seen[id] = view(*u));
    }
    bool threat(std::pair<int32_t, int32_t> c, int32_t r) { return threatTest(c, r); }
    bool reachable(const ClassicSquadUnit& m, const ClassicEnemy& t, int16_t range, bool limited) {
        return reach(m, t, range, limited);
    }
    uint8_t resolve(int command, size_t i, const ClassicPoint&) const {
        // 0x4de530 for a mobile unit: MOVE_GROUND / VTOL_MOVE, or a patrol.
        const auto* u = world.unit(units[i].id);
        if (!u || !u->type || u->type->isStructure()) return 0;
        return command == kClassicSquadPatrol ? kKindPatrol : moveKind(u->type->canFly);
    }
    void order(uint8_t kind, bool, size_t i, const ClassicPoint& at, int32_t a6, int32_t) {
        // The army squads' orders are all unqueued (queue 0).
        if (!send(units[i], kind == kKindPatrol, at, a6)) return;
        ++(kind == kKindPatrol ? patrols : orders);
    }
    void clear(size_t i) { clearer(units[i]); }
    int width() const { return world.mapW() * 16; }
    int height() const { return world.mapH() * 16; }
    // The P7 routines the base think calls (ai/classicexpand.h, set by baseLaunch).
    std::function<int(unsigned, int)> shortBase{};
    std::function<void(int, int, int, int)> transferer{};
    std::function<bool(int, int32_t)> expander{};
    int builderBase() { return shortBase ? shortBase(2, 1) : 0; }
    int groundBase() { return shortBase ? shortBase(4, 2) : 0; }
    int airBase() { return shortBase ? shortBase(5, 3) : 0; }
    void transfer(int to, int n, int category, int clear) { if (transferer) transferer(to, n, category, clear); }
    bool expand(int at, int32_t escort) { return expander && expander(at, escort); }
};
}  // namespace

void ClassicBrain::strikeThink(const ta::sim::World& world, int slot, ta::sim::RetailAiGroupClock& clock,
                               std::vector<ClassicSquadUnit>& units, uint32_t simTick,
                               const std::function<void(const ta::net::Command&)>& sink) {
    auto& squad = squads_.squads[size_t(slot)];
    const int backup = squad.parameters[0];
    const size_t reserve = backup >= 1 && backup <= classic_slot::kMassBackup ? squads_.squads[size_t(backup)].members.size() : 0;
    // The think dissolves the strike when it has a centroid and the strike and
    // its backup are down to p2 members (counted for the tests only).
    const bool dissolving = classicSquadCentroid(squads_, units, detail::ClassicUnitIndex(units), slot) &&
                            int32_t(squad.members.size() + reserve) <= squad.parameters[2];
    WaveHost h{*this, world, units, sink};
    h.rnd = [&](int n) { return rand(n); };
    h.send = [&](ClassicSquadUnit& m, bool patrol, const ClassicPoint& at, int32_t a6) {
        return sendOrder(world, m, patrol, false, at, a6, sink);
    };
    uint32_t cleared = 0;
    h.clearer = [&](ClassicSquadUnit& m) { ++cleared; clearMissions(m, sink); };
    h.picker = [&](const ClassicPoint& c, size_t i) {
        ++waves_.picks;
        const int t = pickTarget(world, units[i], {c[0], c[2]});
        if (t) ++waves_.targets;
        return t;
    };
    h.view = [&](const ta::sim::Unit& u) { return enemyView(world, u); };
    h.threatTest = [&](std::pair<int32_t, int32_t> c, int32_t r) { return classicThreat(visibleEnemies(world), c, r); };
    h.reach = [&](const ClassicSquadUnit& m, const ClassicEnemy& t, int16_t range, bool limited) {
        return reachTarget(world, m, t, range, limited);
    };
    classicStrikeThink(squads_, units, slot, clock, simTick, h);
    if (dissolving) ++waves_.dissolves;
    else waves_.withdrawals += cleared;
    waves_.strikeMoves += squad.parameters[5] == 2 ? h.orders : 0;
    waves_.contactMoves += squad.parameters[5] == 1 ? h.orders : 0;
}

void ClassicBrain::backupThink(const ta::sim::World& world, int slot, std::vector<ClassicSquadUnit>& units,
                               const std::function<void(const ta::net::Command&)>& sink) {
    WaveHost h{*this, world, units, sink};
    h.send = [&](ClassicSquadUnit& m, bool patrol, const ClassicPoint& at, int32_t a6) {
        return sendOrder(world, m, patrol, false, at, a6, sink);
    };
    classicBackupThink(squads_, units, slot, h);
    waves_.backupMoves += h.orders;
}

void ClassicBrain::vtolThink(const ta::sim::World& world, int slot, std::vector<ClassicSquadUnit>& units,
                             const std::function<void(const ta::net::Command&)>& sink) {
    WaveHost h{*this, world, units, sink};
    h.rnd = [&](int n) { return rand(n); };
    h.send = [&](ClassicSquadUnit& m, bool patrol, const ClassicPoint& at, int32_t a6) {
        return sendOrder(world, m, patrol, false, at, a6, sink);
    };
    h.clearer = [&](ClassicSquadUnit& m) { ++waves_.withdrawals; clearMissions(m, sink); };
    classicVtolThink(squads_, units, slot, h);
    waves_.vtolPatrols += h.patrols;
}

void ClassicBrain::baseLaunch(const ta::sim::World& world, int slot, std::vector<ClassicSquadUnit>& units,
                              const std::function<void(const ta::net::Command&)>& sink) {
    WaveHost h{*this, world, units, sink};
    h.rnd = [&](int n) { return rand(n); };
    h.clearer = [&](ClassicSquadUnit& m) { clearMissions(m, sink); };
    // The transfers' and the expansion's host: the founder's or a mover's grid
    // labels, and the enemy query 0x411ae0 over the visible-enemy list.
    struct MoveHost {
        ClassicBrain& b;
        const ta::sim::World& world;
        std::vector<ClassicSquadUnit>& units;
        const std::function<void(const ta::net::Command&)>& sink;
        int random(int n) { return b.rand(n); }
        void clear(size_t i) { b.clearMissions(units[i], sink); }
        const ClassicZoneLabels* labels(size_t i) { return b.unitLabels(world, units[i]); }
        size_t enemies(std::pair<int32_t, int32_t> at, int32_t r) {
            return classicEnemiesNear(b.visibleEnemies(world), at, r).size();
        }
    } move{*this, world, units, sink};
    h.shortBase = [&](unsigned category, int quota) { return classicShortBase(squads_, units, slot, category, quota); };
    h.transferer = [&](int to, int n, int category, int clear) {
        const size_t before = squads_.squads[size_t(to)].members.size();
        classicTransfer(squads_, units, slot, to, n, unsigned(category), clear != 0, move);
        const uint32_t moved = uint32_t(squads_.squads[size_t(to)].members.size() - before);
        (category == 2 ? waves_.builderTransfers : category == 4 ? waves_.groundTransfers : waves_.airTransfers) += moved;
    };
    h.expander = [&](int at, int32_t escort) {
        const bool founded = classicExpand(squads_, units, slot, at, escort, move);
        waves_.expansions += founded ? 1u : 0u;
        return founded;
    };
    // A launch fills a free squad of its family (counted for the tests).
    std::array<bool, 100> empty{};
    for (size_t k = 0; k < empty.size(); ++k) empty[k] = squads_.squads[k].members.empty();
    classicBaseLaunch(squads_, units, slot, ownTotal_, largestEnemyPopulation(world), h);
    auto gained = [&](int first, int last, int step) {
        int n = 0;
        for (int k = first; k <= last; k += step) n += empty[size_t(k)] && !squads_.squads[size_t(k)].members.empty();
        return n;
    };
    waves_.groundLaunches += uint32_t(gained(classic_slot::kFirstStrike + 1, classic_slot::kFirstNavalStrike - 1, 2));
    waves_.navalLaunches += uint32_t(gained(classic_slot::kFirstNavalStrike + 1, classic_slot::kFirstVtol - 1, 2));
    waves_.vtolLaunches += uint32_t(gained(classic_slot::kFirstVtol, classic_slot::kLastVtol, 1));
}


void ClassicBrain::planCombat(const ta::sim::World& world, int slot, const std::vector<ClassicSquadUnit>& units,
                              ClassicSquadUnit& member, bool threat, bool retreat,
                              const std::function<void(const ta::net::Command&)>& sink) {
    const auto* unit = world.unit(member.id);
    if (!unit || !unit->type) return;
    const auto* type = unit->type;
    const detail::ClassicUnitIndex index(units);
    ClassicCombatUnit u;
    u.armed = member.flags & ClassicSquadUnit::kArmed;
    u.commander = member.commander;
    u.builder = member.typeFlags & 0x100u;
    u.canFly = type->canFly;
    u.hp = int16_t(unit->hp.floorInt());
    u.maxDamage = uint32_t(type->maxHp);
    u.at = {unit->x.v, (type->canFly ? unit->flightY : unit->groundY).v, unit->z.v};
    u.sight = int16_t(world.sightDistance(*type));
    const auto* weapon = currentWeapon(*unit);
    u.range = weapon ? weapon->range : 0;
    u.melee = weapon && weapon->melee;
    u.mission = member.mission;
    u.missionFlags = member.missionFlags;
    u.missionTarget = member.missionTarget;
    u.goalX = member.goalX; u.goalZ = member.goalZ;
    const std::vector<ClassicEnemy> visible = visibleEnemies(world);
    struct Host {
        ClassicBrain& b;
        const ta::sim::World& world;
        const std::vector<ClassicSquadUnit>& units;
        const detail::ClassicUnitIndex& index;
        int slot;
        ClassicSquadUnit& member;
        const ta::sim::Unit& unit;
        const ta::sim::Weapon* weapon;
        const std::vector<ClassicEnemy>& visible;
        const std::function<void(const ta::net::Command&)>& sink;
        bool replaced = false;

        std::optional<ClassicPoint> centre() const {
            // The base think plans members only with a centre (0x412c10, the
            // profile home it would fall back on, is never reached from it).
            const auto c = classicBaseCentre(b.squads_, units, index, slot);
            if (!c) return std::nullopt;
            return ClassicPoint{c->first, 0, c->second};
        }
        std::optional<int32_t> radius(unsigned ring) const { return classicBaseRadius(b.squads_, units, index, slot, ring); }
        std::vector<ClassicEnemy> enemies(std::pair<int32_t, int32_t> at, int32_t r) const {
            return classicEnemiesNear(visible, at, r);
        }
        int32_t damage(const ClassicEnemy& e) const {
            const auto* t = world.unit(e.id);
            return weapon && t && t->type ? int32_t(weapon->damageVs(t->type)) : 0;
        }
        int random(int n) { return b.rand(n); }
        bool placeable(int32_t x, int32_t z) const {
            // 0x507d10(type, self, cell, 1, 0): its own footprint, top-left cell
            // of a footprint centred there, by the mobile placement rule.
            const int cx = classicFootprintCell(x, int16_t(unit.type->footX));
            const int cz = classicFootprintCell(z, int16_t(unit.type->footZ));
            const auto& cells = world.mapPlacementCells();
            if (!cells.empty()) return world.mobilePlacement(unit, cx, cz, false);
            // A World without map feature data (synthetic tests): the building test.
            return world.canPlace(unit.type, float(cx * 16 + unit.type->footX * 8), float(cz * 16 + unit.type->footZ * 8), b.player_);
        }
        int width() const { return world.mapW() * 16; }
        int height() const { return world.mapH() * 16; }
        uint8_t resolve(int command, const ClassicPoint&) const { return uint8_t(command); }
        void order(uint8_t kind, bool queue, const ClassicPoint& at, int32_t a6, int32_t) {
            // queue 0 clears its missions first (0x4d6a50); a skipped first leg
            // must not let a queued one append to the old missions.
            if (b.sendOrder(world, member, kind == kClassicPatrol, queue && replaced, at,
                            kind == kClassicMove ? a6 : 0, sink)) {
                replaced = true;
                ++b.combatOrders_;
            }
        }
    } host{*this, world, units, index, slot, member, *unit, weapon, visible, sink};
    if (retreat) { ++retreats_; classicRetreat(host, u); }
    else classicPlanCombat(host, u, threat);
}

void ClassicBrain::tick(const ta::sim::World& world, uint32_t simTick,
                        const std::function<void(const ta::net::Command&)>& sink) {
    // Retail's player loop: the AI scheduler (0x40fe00 -> 0x40fd80), then the
    // bookkeeping (0x4121c0 -> 0x411a90). The sacred-site table is the map's,
    // read once (retail builds it at load, 0x4e0810).
    if (!sitesRead_) readSites(world);
    refreshVisible(world);
    scheduler(world, simTick, sink);
    bookkeeping(world, simTick);
#ifndef NDEBUG
    debugDump(simTick);
#endif
}

#ifndef NDEBUG
// Debug-only harness hook, for comparing with retail's PrintWeights dumps
// (tools/re/retail_ai_harness.py --printweights): TA_CLASSIC_WEIGHTS names an
// output path and TA_CLASSIC_WEIGHTS_AT a sim tick (default 600). At the first
// tick at or past it, each Classic brain writes <path>.p<player> once, one row
// per catalogue type in retail's column layout:
//     <limit, or n/a> - <priority> = <preference> - '<unit>\t\t:<name>'
// A release taserver compiles none of this and reads neither variable. It only
// writes a file, so --mpai hashes are the same with it on.
void ClassicBrain::debugDump(uint32_t simTick) {
    static const char* path = std::getenv("TA_CLASSIC_WEIGHTS");
    static const uint32_t at = [] {
        const char* e = std::getenv("TA_CLASSIC_WEIGHTS_AT");
        const long v = e ? std::strtol(e, nullptr, 10) : 600;
        return uint32_t(v > 0 ? v : 600);
    }();
    if (dumped_ || !path || !*path || simTick < at) return;
    dumped_ = true;
    const std::string out = std::string(path) + ".p" + std::to_string(player_);
    std::FILE* f = std::fopen(out.c_str(), "w");
    if (!f) { std::fprintf(stderr, "TA_CLASSIC_WEIGHTS: cannot write %s\n", out.c_str()); return; }
    const unsigned secs = simTick / 30;
    std::fprintf(f, "gametime: '%02u:%02u:%02u'\nplayer:   'player %d' num: %d\ncontroller: AI (Classic)\n"
                    "profile:    '%s'\nsim tick: %u, priority refreshes: %u, units: %d\n"
                    "------------------------------------------------\n"
                    "<limit> - <dynamic priority> = <profile preference> - <unit>\n",
                 secs / 3600, secs / 60 % 60, secs % 60, player_, player_, profile_.path.c_str(), simTick,
                 refreshes_, [&] { int n = 0; for (auto c : count_) n += c; return n; }());
    for (size_t t = 1; t < catalogue_.size(); ++t) {
        char limit[16];
        if (profile_.limit[t] == -1) std::snprintf(limit, sizeof limit, "n/a ");
        else std::snprintf(limit, sizeof limit, "%4d", profile_.limit[t]);
        std::string name = catalogue_.names[t];
        for (char& c : name) c = char(std::toupper(static_cast<unsigned char>(c)));
        std::fprintf(f, "%s - %3d = %3d - '%s\t\t:%s'\n", limit, int(int8_t(priority_[t])),
                     int(profile_.pref[t]), name.c_str(),
                     catalogue_.types[t] ? catalogue_.types[t]->name.c_str() : "");
    }
    std::fclose(f);
}
#endif

uint32_t classicMassAttackTicks(const std::string& line) {
    // strstr(line, "massattack"), then sscanf(at, "massattack %i"): the format's
    // space takes any whitespace, %i a sign and a 0x / 0 prefix.
    const size_t at = line.find("massattack");
    if (at == std::string::npos) return 0;
    const char* p = line.c_str() + at + 10;
    char* end = nullptr;
    const long v = std::strtol(p, &end, 0);
    if (end == p) return 0;
    return uint32_t(v) * 30u;   // the stored dword: n x 30, wrapping
}

}  // namespace ta::ai
