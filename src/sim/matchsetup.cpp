#include "util/virtualpath.h"
#include "sim/matchsetup.h"
#include "sim/sides.h"

#include "sim/detmath.h"
#include "gaf/featureburntiming.h"
#include "gaf/gaf.h"
#include "sim/footprint.h"
#include "sim/retailflyerspread.h"
#include "sim/retailpiecepose.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

#include "hpi/hpi.h"
#include "sim/mission.h"
#include "sim/scenario.h"
#include "tnt/ota.h"
#include <stdexcept>
#include "tdf/tdf.h"

#include <cstdio>
#include <memory>
#include "tdf/tdf.h"
#include "tnt/tnt.h"
#include "tnt/mapgen.h"
#include "sim/groupspots.h"
#include "sim/taweapons.h"

namespace ta::sim {

static_assert(ta::crt::kNeutralOwner == kNeutralPlayer && ta::crt::kMaxOwners == kMaxPlayers,
              "the .crt owner numbering is the sim's player numbering");
static_assert(ta::mapgen::kMaxPlayers == kMaxPlayers, "a random map has a start per player");


// One explicit selection order (Cmd::Order): every member gets
// the template command, in increasing id order, inside one World::beginOrder
// scope, so under Legion all their legs carry the order's identity and the order
// joins its arrival area once. Ownership and existence are checked per member by
// the ordinary command path (a dead, gone or given-away member is skipped), so
// the sim itself refuses what the server cannot know. The server and the strict
// replay reader validate the order; an invalid one is ignored here as well.
static void applySelectionOrder(World& world, const TypeRegistry& reg, const ta::net::Command& c) {
    using ta::net::OrderShape;
    if (!c.order || !ta::net::validOrder(*c.order)) return;
    const auto& o = *c.order;
    ta::net::Command m = o.tmpl;
    m.player = c.player;
    const Fixed cx0 = Fixed::fromFloat(o.tmpl.x), cz0 = Fixed::fromFloat(o.tmpl.z);
    auto owned = [&](int id) -> const Unit* {
        const Unit* u = world.unit(id);
        return u && u->alive() && u->type && u->player == int(c.player) ? u : nullptr;
    };
    // Both offset shapes: the click goes in the template and the members' goals
    // are recomputed here from their positions at activation.
    const bool spread = o.shape == OrderShape::Offsets || o.shape == OrderShape::FlyerOffsets;
    const bool legion = isLegionPathfinding(world.pathfindingMode());
    // The verbs whose template carries the click: their members share an area there.
    const bool point = o.tmpl.kind == ta::net::Cmd::Move || o.tmpl.kind == ta::net::Cmd::AttackMove ||
                       o.tmpl.kind == ta::net::Cmd::Patrol;
    // Every member of a move, patrol or fight-move gets its OWN spot (an engine
    // feature, docs/ta-port-plan.md "Known deviations"; TotalA.exe 0x48cf30 lets
    // stragglers share the click) except Legion's planned surface movers, which
    // Legion slots on the shared point itself (and buildings: theirs is a rally).
    std::vector<GroupSpotMember> spotMembers;
    std::vector<int32_t> spotIds;
    if (spread && point)
        for (const int32_t id : o.members)
            if (const Unit* u = owned(id)) {
                const bool legionPlans = legion && legionSharedClick(u->type->canFly, u->type->footX, u->type->footZ);
                spotMembers.push_back({u->x.v, u->z.v, u->type->footX, u->type->footZ,
                                       !legionPlans && !u->type->isStructure(), legionPlans});
                spotIds.push_back(id);
            }
    const auto spots = groupSpots(spotMembers, cx0.v, cz0.v, world.mapW() * 16, world.mapH() * 16);
    world.beginOrder(o.serial, int(c.player), point, cx0, cz0);
    for (const int32_t id : o.members) {
        m.unitId = id;
        world.orderMember(id);
        if (spread) {
            const Unit* u = owned(id);
            if (!u) continue;
            const auto at = std::lower_bound(spotIds.begin(), spotIds.end(), id);
            if (at != spotIds.end() && *at == id) {
                const auto& g = spots[size_t(at - spotIds.begin())];
                m.x = Fixed::raw(g[0]).toFloat();
                m.z = Fixed::raw(g[1]).toFloat();
            } else {
                m.x = o.tmpl.x;
                m.z = o.tmpl.z;
            }
        }
        applyCommand(world, reg, m);
    }
    world.endOrder();
}

bool stockpileButton(const char* type) {
    // 0x419b3c / 0x419b4e: _stricmp against "MAKENUKE" and "MAKEANTI".
    auto same = [&](const char* name) {
        for (size_t i = 0;; ++i) {
            const char a = char(std::tolower(static_cast<unsigned char>(type[i]))), b = name[i];
            if (a != b) return false;
            if (!a) return true;
        }
    };
    return same("makenuke") || same("makeanti");
}

void applyCommand(World& world, const TypeRegistry& reg, const ta::net::Command& c) {
    using ta::net::Cmd;
    if (c.kind == Cmd::Order) { applySelectionOrder(world, reg, c); return; }
    // An infinitely producing mobile builder stays on that job until Stop.
    // Move/patrol/attack/fight-move describe its output rally, without
    // redirecting construction.
    // Gate before redirect(): rejected orders must not cancel its construction.
    // Test repeatType rather than queue size so item transitions, resource stalls
    // and blocked output positions do not briefly unlock the producer.
    // The classic AI's squads: no unit order, so no producer gate below.
    if (c.kind == Cmd::AiGroup) { world.setAiGroup(int(c.player), c.unitId, c.targetId, c.queue != 0); return; }
    if (c.kind == Cmd::BenchClock) { world.benchmarkClock(uint32_t(std::max(0, c.targetId)), c.queue != 0); return; }
    const auto* producer=world.unit(c.unitId);
    const bool repeatingMobile = producer && producer->type &&
        !producer->type->isStructure() && producer->repeatType;
    // Assist is the exception: retail's HelpBuild has no repeat check, so a
    // repeating conjurer told to help an unfinished unit helps (its own output
    // waits meanwhile; see tickProduction).
    // Stance sets the stance its output starts with (World::setStance's outputStance).
    if (repeatingMobile && c.kind!=Cmd::Stop && c.kind!=Cmd::Assist && c.kind!=Cmd::Move && c.kind!=Cmd::Patrol && c.kind!=Cmd::Attack && c.kind!=Cmd::AttackMove && c.kind!=Cmd::GiveUnit && c.kind!=Cmd::ShareResources && c.kind!=Cmd::Disco && c.kind!=Cmd::Headbang && c.kind!=Cmd::PowerCode && c.kind!=Cmd::Stance) return;
    auto owns = [&](int id) {
        const auto* u = world.unit(id);
        return u && u->player == int(c.player);
    };
    // Whether the unit may take this as a new order: what each verb checks
    // before anything is cancelled. The front insert below asks first too, as
    // it rewinds the unit's list -- and a Load's carrier's, which may be any
    // player's unit -- before it issues the order.
    auto admits = [&] {
        switch (c.kind) {
            case Cmd::Load: return world.canLoadInto(c.unitId, c.targetId);
            case Cmd::Train: case Cmd::RepeatTrain: case Cmd::Build:
                return producer && world.canProduce(*producer, reg.find(c.type),
                                                    c.kind == Cmd::Build);
            case Cmd::Assist: {
                // HelpBuild's own eligibility (World::canHelpBuild -- same owner,
                // a mobile builder, the build menu when builderlimited).
                const Unit* site = world.unit(c.targetId);
                return producer && site && !producer->underConstruction && world.canHelpBuild(*producer, *site);
            }
            case Cmd::Resurrect: return world.reviveKind(c.unitId, c.targetId) != 0;
            case Cmd::ReclaimUnit: {
                // What World::reclaimUnit takes: a mobile reclaimer and a live
                // unit other than itself (0x489960 is the mission's stage 0).
                const Unit* target = world.unit(c.targetId);
                return producer && producer->alive() && producer->type && producer->type->canMove &&
                       !producer->type->isStructure() && producer->type->canReclaim &&
                       target && target->alive() && target->type && target->id != producer->id;
            }
            default: return true;
        }
    };
    // Retail's queue modifiers (net/lockstep.h kQueueFront, kQueueToggle): a
    // player's Shift/Ctrl order on a queued one of its kind takes it back, and
    // Ctrl puts the order at the head of the queue -- for a producer's rally
    // verbs, of its rally list; for everyone else, a tower included, of its own.
    // Armed ATTACK at a point: a unit the resolver gives nothing (4de530 kind
    // 3) neither toggles nor front-inserts, and its work is left alone.
    if (c.kind == Cmd::AttackGround && (!owns(c.unitId) || !world.suppressAccepts(c.unitId, c.x, c.z))) return;
    {
        using ta::net::kQueueFront; using ta::net::kQueuePlace; using ta::net::kQueueToggle;
        const bool front = (c.queue & kQueuePlace) == kQueueFront;
        const bool toggle = (c.queue & kQueueToggle) && (c.queue & kQueuePlace);
        const bool rallyVerb = c.kind == Cmd::Move || c.kind == Cmd::AttackMove || c.kind == Cmd::Patrol ||
                               c.kind == Cmd::Attack;
        const bool orderVerb = rallyVerb || c.kind == Cmd::Build ||
            c.kind == Cmd::Assist || c.kind == Cmd::Guard || c.kind == Cmd::Load || c.kind == Cmd::Unload ||
            c.kind == Cmd::Reclaim || c.kind == Cmd::ReclaimArea || c.kind == Cmd::Resurrect || c.kind == Cmd::Repair ||
            c.kind == Cmd::AttackGround || c.kind == Cmd::ReclaimUnit;
        // A Load is the carrier's order (TotalA.exe's LOAD gives the carrier
        // GROUND_PICKUP / VTOL_PICKUP, 0x43f701; World::loadInto): its list is
        // the one Shift and Ctrl work on, the passenger its target.
        const bool load = c.kind == Cmd::Load;
        const int holder = load ? c.targetId : c.unitId;
        const auto* holderUnit = load ? world.unit(holder) : producer;
        if (orderVerb && (front || toggle) && owns(c.unitId) && producer->type && holderUnit) {
            const bool rally = rallyVerb && (producer->repeatType ||
                (c.kind == Cmd::Attack ? producer->type->producesUnits() : producer->type->isStructure()));
            if (toggle) {
                World::QueuedMatch m;
                m.x = Fixed::fromFloat(c.x); m.z = Fixed::fromFloat(c.z);
                m.target = load ? c.unitId : c.targetId;
                switch (c.kind) {
                    case Cmd::Move:       m.kind = World::QueuedKind::Move; break;
                    case Cmd::AttackMove: m.kind = World::QueuedKind::AttackMove; break;
                    case Cmd::Patrol:     m.kind = World::QueuedKind::Patrol; break;
                    case Cmd::Attack:     m.kind = World::QueuedKind::Attack; break;
                    case Cmd::Guard:      m.kind = World::QueuedKind::Guard; break;
                    case Cmd::Load:       m.kind = World::QueuedKind::Load; break;
                    case Cmd::Unload:     m.kind = World::QueuedKind::Unload; break;
                    case Cmd::Build:      m.kind = World::QueuedKind::Build; m.type = reg.find(c.type); break;
                    case Cmd::Reclaim:    m.kind = World::QueuedKind::Reclaim; break;
                    case Cmd::ReclaimUnit: m.kind = World::QueuedKind::ReclaimUnit; break;
                    case Cmd::Repair:     m.kind = World::QueuedKind::Repair; break;
                    case Cmd::Assist:     m.kind = World::QueuedKind::Assist; break;
                    case Cmd::AttackGround: m.kind = World::QueuedKind::AttackGround; break;
                    default: break;
                }
                if (world.removeQueuedMatch(holder, rally, m)) return;
            }
            // Taking a queued order back (above) is not a new one; a new one the
            // unit cannot take stops here, before anything is rewound.
            if (!admits()) return;
            const bool idle = rally ? producer->rally.empty() : holderUnit->orders.empty();
            // Nothing can come ahead of a front unload to fill the hold: refused,
            // as unloadAt refuses a plain one.
            if (front && c.kind == Cmd::Unload && producer->cargo.empty()) return;
            if (front && !idle) {
                ta::net::Command append = c;
                append.queue = ta::net::kQueueAppend;
                const auto issue = [&] { applyCommand(world, reg, append); };
                world.insertOrderFront(holder, rally, issue);
                return;
            }
            // At the head of an empty list is the plain order, given as one (a
            // Patrol then gets its two-leg loop, not an open leg).
            if (front) {
                ta::net::Command plain = c;
                plain.queue = ta::net::kQueueReplace;
                applyCommand(world, reg, plain);
                return;
            }
        }
    }
    auto redirect = [&] { if (!c.queue && !repeatingMobile) world.cancelBuilds(c.unitId); };
    switch (c.kind) {
        case Cmd::ShareResources:
            world.setResourceSharing(int(c.player), c.targetId, (c.queue & 1) != 0);
            if (c.queue & 2) world.setShareThreshold(int(c.player), c.targetId, c.x);
            break;
        case Cmd::GiveUnit:
            world.giveUnit(c.unitId, int(c.player), c.targetId);
            break;
        case Cmd::Move:
            if (owns(c.unitId)) { redirect(); world.order(c.unitId, c.x, c.z, c.queue); }
            break;
        case Cmd::Attack:
            if (owns(c.unitId)) { redirect(); world.attack(c.unitId, c.targetId, c.queue); }
            break;
        case Cmd::DGun:
            if (owns(c.unitId)) { redirect(); world.dgun(c.unitId, c.targetId, c.queue); }
            break;
        case Cmd::Land:
            // Shift (append) and Ctrl (front) both queue it behind the work.
            if (owns(c.unitId)) {
                const bool queued = (c.queue & ta::net::kQueuePlace) != ta::net::kQueueReplace;
                if (!queued) redirect();
                world.landOnAirBase(c.unitId, c.targetId, queued);
            }
            break;
        case Cmd::AttackMove:
            if (owns(c.unitId)) { redirect(); world.attackMove(c.unitId, c.x, c.z, c.queue); }
            break;
        case Cmd::AttackGround:
            // A refused point (an anti-air weapon, a melee one off a clearable
            // blocker) returned above, leaving the unit's work alone.
            redirect();
            world.suppress(c.unitId, c.x, c.z, c.queue != 0);
            break;
        case Cmd::Patrol:
            // Shift-patrol ADDS a waypoint to the route. patrol() builds a fresh
            // two-point loop and was the only thing wired up, so a queued patrol
            // silently replaced the route instead of extending it -- patrolTo has
            // existed for this the whole time, used only by mission scripts.
            if (owns(c.unitId)) {
                if (!repeatingMobile) world.cancelBuilds(c.unitId);
                if (c.queue) world.queuePatrol(c.unitId, c.x, c.z);
                else world.patrol(c.unitId, c.x, c.z);
            }
            break;
        case Cmd::Stop:
            if (owns(c.unitId)) { world.cancelBuilds(c.unitId); world.stop(c.unitId); }
            break;
        case Cmd::Train:
            // The build menu's stockpile button (TotalA.exe 0x419b00: MAKENUKE
            // or MAKEANTI -> BUILDWEAPON slot 0): `type` carries its name.
            if (owns(c.unitId) && stockpileButton(c.type)) { world.orderBuildWeapon(c.unitId, 0, std::max(1, c.targetId)); break; }
            if (owns(c.unitId) && admits()) world.train(c.unitId, reg.find(c.type), c.targetId);
            break;
        case Cmd::Unqueue:
            if (owns(c.unitId) && stockpileButton(c.type)) { world.orderBuildWeapon(c.unitId, 0, -std::max(1, c.targetId)); break; }
            if (owns(c.unitId)) world.dequeue(c.unitId, reg.find(c.type), std::max(1, c.targetId));
            break;
        case Cmd::Build:
            if (owns(c.unitId) && admits()) world.queueBuild(c.unitId, reg.find(c.type), c.x, c.z, c.queue);
            break;
        case Cmd::Assist:
            // Help/resume an unfinished unit, checked (admits) before anything
            // is cancelled so a refused order leaves the builder's work alone.
            if (owns(c.unitId) && admits()) {
                redirect();
                world.assist(c.unitId, c.targetId, c.queue);
            }
            break;
        case Cmd::Guard:
            if (owns(c.unitId)) world.guard(c.unitId, c.targetId, c.queue);
            break;
        case Cmd::Load:
            if (owns(c.unitId)) world.loadInto(c.unitId, c.targetId, c.queue != 0);
            break;
        case Cmd::Unload:
            if (owns(c.unitId)) world.unloadAt(c.unitId, c.x, c.z, Fixed::raw(c.targetId), c.queue != 0);
            break;

        case Cmd::RepeatTrain:
            // The type it repeats again ends the repeat: taking work back, not a new order.
            if (owns(c.unitId)) {
                const auto* type = reg.find(c.type);
                if (type && (producer->repeatType == type || admits())) world.setRepeat(c.unitId, type);
            }
            break;
        case Cmd::SetWeapon: case Cmd::RetiredAreaBuild:   // retired Kingdoms verbs: never applied
            break;
        case Cmd::Destroy:
            if (owns(c.unitId)) world.destroy(c.unitId);
            break;
        case Cmd::Disco:
        case Cmd::Headbang: {
            bool append = c.queue != 0;
            auto emote = [&](int id) {
                if (c.kind == Cmd::Disco) world.startDisco(int(c.player), id, append);
                else world.startHeadbang(int(c.player), id, append);
                append = true;
            };
            if (c.unitId != 0) emote(c.unitId); // World checks ownership and life
            else { // Empty selection: living monarch only.
                for (const auto& u : world.units()) {
                    if (!u.alive() || u.player != int(c.player) || !u.type) continue;
                    if (ta::sides::isCommander(u.type->id))
                        emote(u.id);
                }
            }
            break;
        }
        case Cmd::Reclaim:
            if (owns(c.unitId)) { redirect(); world.reclaim(c.unitId, c.targetId, c.queue); }
            break;
        case Cmd::ReclaimUnit:
            // Checked before anything is cancelled: a unit that cannot take
            // the order keeps its work.
            if (owns(c.unitId) && admits()) { redirect(); world.reclaimUnit(c.unitId, c.targetId, c.queue); }
            break;
        case Cmd::ReclaimArea:
            if (owns(c.unitId)) { redirect(); world.reclaimArea(c.unitId, c.x, c.z, c.x2, c.z2, c.queue); }
            break;
        case Cmd::Resurrect:
            // Checked before anything is cancelled, so a caster that cannot
            // raise that body keeps its work.
            if (owns(c.unitId) && admits()) {
                redirect();
                world.resurrect(c.unitId, c.targetId, c.queue);
            }
            break;
        case Cmd::Stance:
            if (owns(c.unitId)) world.setStance(c.unitId, c.targetId);
            break;
        case Cmd::StandingOrder:
            if (owns(c.unitId)) world.setStandingOrder(c.unitId, c.queue != 0, c.targetId);
            break;
        case Cmd::Cloak:
            if (owns(c.unitId)) world.setCloak(c.unitId, c.targetId != 0);
            break;
        case Cmd::SetActive:
            if (owns(c.unitId)) world.setActive(c.unitId, c.targetId != 0);
            break;
        case Cmd::Repair:
            if (owns(c.unitId)) { redirect(); world.repair(c.unitId, c.targetId, c.queue); }
            break;
        case Cmd::SetSquad:
            if (owns(c.unitId)) world.setSquad(c.unitId, c.targetId);
            break;
        case Cmd::Order:   // applied by applySelectionOrder() above
        case Cmd::AiGroup: // applied above
        case Cmd::BenchClock:
            break;
        case Cmd::PowerCode:   // the World checks the room's option and ownership
            if (simPowerCode(c.targetId) && c.queue <= uint8_t(PowerArg::Invalid))
                world.applyPowerCode(int(c.player), PowerCode(c.targetId), PowerArg(c.queue), c.unitId);
            break;
    }
}

void applyEvent(World& world, const ta::net::Event& e) {
    int p = e.player;
    if (p < 0 || p >= world.numPlayers()) return;
    // A server AI takes over a seat whose player stopped communicating: the empire
    // plays on with its orders intact, and from this tick it is computer-owned, as an
    // AI seat is from the start (MatchSlot::automaticGates: gates open themselves, no
    // human death sights).
    if (e.kind == ta::net::Event::Kind::AiTakeover) { world.player(p).automaticGates = true; return; }
    if (e.kind != ta::net::Event::Kind::Forfeit && e.kind != ta::net::Event::Kind::Leave) return;
    if (world.hasScenarioOutcomes()) world.forceDefeat(p);
    for (auto& u : world.units())
        if (u.alive() && u.player == p) world.stop(u.id);
}

void setupRegistry(TypeRegistry& reg, const hpi::Vfs& input) {
    // Map sprite banks must not change unit weapon/nimbus timing in one room.
    hpi::Vfs vfs(&input, true);
    reg.loadMoveInfo(vfs, "gamedata/moveinfo.tdf");
    // The roster is the units/ directory. The VFS already merges base + expansion +
    // community units into one namespace, resolved by retail newest-date precedence.
    reg.loadDir(vfs, "units");
    // Build menus come from the canbuild/ tree, filtered by that roster (retail 0x4c1950).
    reg.loadBuildTree(vfs, "canbuild");
}

std::string scenarioTypeMissingText(const ScenarioTypeMissing& missing) {
    auto upper = missing.type;
    for (char& c : upper) c = char(std::toupper(static_cast<unsigned char>(c)));
    return "this scenario places " + upper + ", a unit this game's data does not have";
}

bool balanceUnitFile(const hpi::Vfs& vfs, const std::string& name) {
    return vfs.has("units/" + name + ".fbi");
}

std::vector<std::pair<float, float>> parseStartPositions(const hpi::Vfs& vfs,
                                                         const std::string& mapPath) {
    std::vector<std::pair<float, float>> out;
    std::string otaPath = ta::vpath::replaceExtension(mapPath,".ota");
    if (!vfs.has(otaPath)) return out;
    try {
        auto b = vfs.read(otaPath);
        auto root = ta::tdf::parseText(std::string(b.begin(), b.end()), otaPath);
        const auto* gh = root.child("globalheader");
        // Total Annihilation: [Schema N]\[specials], XPos/ZPos in PIXELS (0x436860;
        // e.g. Ashap Plateau StartPos1 at 3504,304 on a 4128 px map). The first
        // "Network" schema serves skirmish, else Schema 0. TAK-era engine maps keep
        // [Map Data]\[specials] in cells.
        const ta::tdf::Node* schema = nullptr;
        if (gh)
            for (int i = 0; i < 16 && !schema; ++i)
                if (const auto* sc = gh->child("schema " + std::to_string(i))) {
                    std::string type = sc->valueOr("type", "");
                    for (char& c : type) c = char(std::tolower(static_cast<unsigned char>(c)));
                    if (type.rfind("network", 0) == 0) schema = sc;
                }
        if (gh && !schema) schema = gh->child("schema 0");
        const bool pixels = schema && schema->child("specials");
        const auto* md = gh ? gh->child("map data") : nullptr;
        const auto* sp = pixels ? schema->child("specials") : (md ? md->child("specials") : nullptr);
        if (!sp) return out;
        std::map<int, std::pair<float, float>> byIndex;
        for (const auto& name : sp->childOrder) {
            const auto* s = sp->child(name);
            if (!s) continue;
            std::string what = s->valueOr("specialwhat", "");
            if (what.rfind("StartPos", 0) != 0 && what.rfind("startpos", 0) != 0) continue;
            int n = std::atoi(what.c_str() + 8);
            if (n <= 0) continue;
            const float k = pixels ? 1.0f : 16.0f;
            byIndex[n] = {float(s->numberOr("xpos", 0) * k), float(s->numberOr("zpos", 0) * k)};
        }
        for (auto& [n, pos] : byIndex) out.push_back(pos);
        // The map's own player count caps its starts. Cavedog's editor pads most
        // multiplayer maps to ten StartPos specials (Sherwood: numplayers=2, 3 with
        // StartPos8..10 stacked in the top-left corner), and retail reads all ten
        // (0x436a9f..0x436b0e counts every StartPos* special in the schema). Here the
        // largest numplayers entry bounds the starts, so the preview markers, the
        // room's seats (capacityFor / seatsFor) and the Random shuffle all use the
        // starts the map says it has. Not retail: retail seats past numplayers.
        if (gh) {
            int most = 0;
            const std::string list = gh->valueOr("numplayers", "");
            for (size_t i = 0; i < list.size();) {
                if (std::isdigit(static_cast<unsigned char>(list[i]))) {
                    size_t used = 0;
                    most = std::max(most, std::stoi(list.substr(i), &used));
                    i += used;
                } else ++i;
            }
            if (most > 0 && size_t(most) < out.size()) out.resize(size_t(most));
        }
    } catch (const std::exception&) {}
    return out;
}

std::vector<int> startSpotsFor(const std::vector<MatchSlot>& slots, size_t authored, StartMode mode) {
    std::vector<int> spot(slots.size(), -1);
    size_t used = 0;
    for (size_t i = 0; i < slots.size(); ++i)
        if (slots[i].used) {
            // Fixed and Random: seated order (Random has shuffled the list itself).
            if (mode != StartMode::BySlot) spot[i] = int(used);
            ++used;
        }
    if (mode != StartMode::BySlot) return spot;
    // BySlot: every seat the map has a start for claims its own first, so a later
    // fallback never takes a start that belongs to a seated slot's number.
    std::vector<uint8_t> claimed(std::max(authored, used), 0);
    for (size_t i = 0; i < slots.size() && i < authored; ++i)
        if (slots[i].used) { spot[i] = int(i); claimed[i] = 1; }
    // The rest in slot order, lowest free entry first. There are at least as many
    // entries as seats, so the scan never runs past the end.
    size_t next = 0;
    for (size_t i = 0; i < slots.size(); ++i)
        if (slots[i].used && spot[i] < 0) {
            while (claimed[next]) ++next;
            spot[i] = int(next);
            claimed[next] = 1;
        }
    return spot;
}

namespace {
// Feature definition fields the sim cares about: is it a metal deposit, and its
// footprint (for nav blocking). Loaded from the feature TDFs.
struct FeatDef { bool deposit = false; int blocking = 0; int fx = 1, fz = 1;
                 int reclaimable = 0; bool autoReclaimable = true; float energy = 0, metal = 0;
                 uint8_t projectileHeight = 0;
                 // Burning chain (see World::tickBurning).
                 bool flamable = false; bool hasBurnAnim = false; uint32_t burnTicks = 0;
                 int spreadChance = 0; int sparkTicks = 0; std::string burnt;
                 uint32_t dieTicks = 0;   // seqnamedie playback (World::damageFeature)
                 // Corpse lifecycle (features/corpses).
                 int decomposeTicks = 0; bool resurrectable = false;

                 bool indestructible = false;
                 float hp = 0; std::string dead;
                 std::string object;
                 bool treeOrRock = false; };

std::unordered_map<std::string, FeatDef> loadFeatureDefs(const hpi::Vfs& vfs) {
    std::unordered_map<std::string, FeatDef> defs;
    gaf::FeatureBurnTiming burnTiming(vfs);
    try {
        for (const std::string& path : vfs.list("features")) {
            if (ta::vpath::extension(path) != ".tdf") continue;
            try {
                auto fb = vfs.read(path);
                auto root = ta::tdf::parseText(std::string(fb.begin(), fb.end()), path);
                for (const auto& n : root.childOrder) {
                    std::string k = n;
                    std::transform(k.begin(), k.end(), k.begin(), ::tolower);
                    const auto& node = root.children.at(n);
                    FeatDef d;
                    std::string cat = node.valueOr("category", "");
                    std::transform(cat.begin(), cat.end(), cat.begin(), ::tolower);
                    // Lone scenery Legion's pinwheel ignores (RetailMapFeatureType::treeOrRock).
                    d.treeOrRock = cat == "trees" || cat == "plant" || cat == "plants" || cat == "rocks";
                    d.blocking = int(node.numberOr("blocking", 0));
                    d.fx = int(node.numberOr("footprintx", 1));
                    d.fz = int(node.numberOr("footprintz", 1));
                    d.reclaimable = int(node.numberOr("reclaimable", 0));
                    d.autoReclaimable = (int32_t(node.numberOr("autoreclaimable", 1)) & 1) != 0;
                    d.projectileHeight = uint8_t(int(node.numberOr("height", 0)));
                    // TA: metal/energy read as int & 0xffff, then float (0x422e40).
                    d.energy = float(int32_t(node.numberOr("energy", 0)) & 0xffff);
                    d.metal = float(int32_t(node.numberOr("metal", 0)) & 0xffff);
                    d.indestructible = (int32_t(node.numberOr("indestructible", 0)) & 1) != 0;
                    // TA: the "deposit" features are the ore patches -- indestructible
                    // features carrying metal (docs/retail-ai.md: the AI sends extractors
                    // to them; docs/retail-economy.md section 6: they set the cell metal).
                    d.deposit = d.indestructible && d.metal > 0;
                    d.flamable = node.numberOr("flamable", 0) != 0;
                    d.burnTicks = burnTiming.duration(node);
                    d.hasBurnAnim = d.burnTicks != 0;
                    d.dieTicks = burnTiming.dieDuration(node);
                    d.spreadChance = int(node.numberOr("spreadchance", 0));
                    d.sparkTicks = int(node.numberOr("sparktime", 0) * 30);
                    d.burnt = node.valueOr("featureburnt", "");
                    std::transform(d.burnt.begin(), d.burnt.end(), d.burnt.begin(), ::tolower);
                    // 494337..494353: _ftol(decomposetime * 30.0), its low 16 bits.
                    d.decomposeTicks = int(node.numberOr("decomposetime", 0) * 30) & 0xffff;
                    d.resurrectable = node.numberOr("resurrectable", 0) != 0;

                    d.object = node.valueOr("object", "");
                    std::transform(d.object.begin(), d.object.end(), d.object.begin(), ::tolower);

                    d.indestructible = node.numberOr("indestructible", 0) != 0;
                    d.hp = float(node.numberOr("damage", 0));
                    d.dead = node.valueOr("featuredead", "");
                    std::transform(d.dead.begin(), d.dead.end(), d.dead.begin(), ::tolower);
                    defs[k] = d;
                }
            } catch (const std::exception&) {}
        }
    } catch (const std::exception&) {}
    return defs;
}
// TotalA.exe RESURRECT stage 3 (0x404f36..0x404f60): the feature's name
// (lstrcpyn, 64 chars) up to its first '_' names the unit it raises (0x488b10).
void linkRaisedUnits(std::vector<ta::sim::FeatType>& table, const TypeRegistry& reg) {
    for (auto& t : table) {
        std::string name = t.name.substr(0, std::min<size_t>(t.name.size(), 63));
        if (const size_t cut = name.find('_'); cut != std::string::npos) name.resize(cut);
        t.raises = name.empty() ? nullptr : reg.find(name);
    }
}
struct FeatTypeInterner {
    const std::unordered_map<std::string, FeatDef>& defs;
    std::vector<ta::sim::FeatType> table;
    std::unordered_map<std::string, int> byName;
    explicit FeatTypeInterner(const std::unordered_map<std::string, FeatDef>& d) : defs(d) {}
    int intern(const std::string& nm) {
        if (nm.empty()) return -1;
        if (auto it = byName.find(nm); it != byName.end()) return it->second;
        auto di = defs.find(nm);
        if (di == defs.end()) return -1;
        int idx = int(table.size());
        byName[nm] = idx;                  // reserve BEFORE recursing (cycle guard)
        ta::sim::FeatType t;
        t.name = nm;
        t.flamable = di->second.flamable;
        t.hasBurnAnim = di->second.hasBurnAnim;
        t.burnTicks = di->second.burnTicks;
        t.dieTicks = di->second.dieTicks;
        t.spreadChance = di->second.spreadChance;
        t.sparkTicks = di->second.sparkTicks;
        t.energy = di->second.energy;
        t.metal = di->second.metal;
        t.fx = di->second.fx; t.fz = di->second.fz;
        t.blocking = di->second.blocking != 0;
        t.projectileHeight = di->second.projectileHeight;
        t.treeOrRock = di->second.treeOrRock;
        t.decomposeTicks = di->second.decomposeTicks;
        t.resurrectable = di->second.resurrectable;

        t.reclaimable = di->second.reclaimable != 0;
        t.autoReclaimable = di->second.autoReclaimable;

        t.indestructible = di->second.indestructible;
        t.hp = di->second.hp;
        t.object = di->second.object;
        table.push_back(std::move(t));
        table[size_t(idx)].burntType = intern(di->second.burnt);
        table[size_t(idx)].deadType = intern(di->second.dead);
        return idx;
    }
};
}  // namespace


// ONE feature-plane walk, shared by setupMatch and registerMapFeatures.
//
// These were two near-identical loops, and the duplication is exactly how they
// drifted: setupMatch blocked features into the nav overlay, registerMapFeatures
// did not, and the paths that use the latter (campaign missions, CRT scenarios,
// the local harness) leaned on the VIEWER to block instead -- from
// GameView::loadFeatures, which wrote to nav_.cells_. The referee never runs the
// viewer, so its ground grid lacked those blockers and losBetween disagreed,
// which desynced auto-acquisition. Blocking belongs here, in the sim, where both
// peers run it; having one function makes it impossible for the two to diverge
// again.
static void scanFeaturePlane(World& world, const ta::tnt::Map& map,
                             const std::unordered_map<std::string, FeatDef>& defs,
                             FeatTypeInterner& types,
                             std::vector<std::pair<float, float>>& rawDeposits) {
    std::vector<RetailMapFeatureType> placementTypes;
    for (auto name:map.featureNames) {
        std::transform(name.begin(),name.end(),name.begin(),::tolower);
        RetailMapFeatureType type;
        if (auto it=defs.find(name);it!=defs.end()) {
            type={name,it->second.fx,it->second.fz,it->second.blocking!=0,it->second.indestructible};
            type.projectileHeight=it->second.projectileHeight;
            type.treeOrRock=it->second.treeOrRock;
            // 4945bb: grade-1 blockers must remain removable through every
            // dead/burnt replacement, including cycles and shared successors.
            type.clearable=!type.indestructible;
            std::vector<std::string> pending{name};
            std::set<std::string> seen;
            while (type.clearable && !pending.empty()) {
                const auto current=std::move(pending.back());pending.pop_back();
                if (!seen.insert(current).second) continue;
                const auto found=defs.find(current);
                if (found==defs.end()) continue;
                const auto& def=found->second;
                if (def.blocking && def.indestructible) { type.clearable=false;break; }
                if (!def.dead.empty()) pending.push_back(def.dead);
                if (!def.burnt.empty()) pending.push_back(def.burnt);
            }
        }
        placementTypes.push_back(std::move(type));
    }
    world.setMapPlacementFeatures(map.features,std::move(placementTypes));
    if (map.featureNames.empty()) return;
    // Only the features that survived install (495360 removes the earlier of
    // two overlapping ones): retail has no record of the others, so they make
    // no deposit, no blocker and no target.
    const auto& installed = world.installedMapFeatures();
    for (int cz = 0; cz < map.height; ++cz)
        for (int cx = 0; cx < map.width; ++cx) {
            uint16_t v = installed[size_t(cz) * map.width + cx];
            if (v >= map.featureNames.size()) continue;
            std::string key = map.featureNames[v];
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            auto di = defs.find(key);
            if (di == defs.end()) continue;
            // Metal patches: an indestructible feature with metal overwrites the
            // cells under it, in cell order (docs/retail-economy.md section 6).
            if (di->second.indestructible && di->second.metal != 0)
                world.stampFeatureMetal(cx, cz, di->second.fx, di->second.fz, int(di->second.metal),
                                        map.width, map.height);
            // Retail feature records name the footprint origin (0x4931e0).
            float x = float(cx * 16 + di->second.fx * 8);
            float z = float(cz * 16 + di->second.fz * 8);
            if (di->second.deposit) rawDeposits.push_back({x, z});
            // Retail movement treats the feature definition's blocking flag as
            // authoritative. Decorative waves and other nonblocking map art must
            // not become route obstacles.
            if (di->second.blocking != 0) {
                int fx = di->second.fx, fz = di->second.fz;
                world.blockCells(cx, cz, fx, fz, true);
            }
            // Track anything that can be reclaimed, burned OR damaged. Some
            // authored walls have hit points but permit neither reclaim nor fire.
            // Their damage state still belongs in every peer's simulation.
            const bool destructible = di->second.hp > 0 && !di->second.indestructible;
            if ((di->second.reclaimable || di->second.flamable || destructible) && !di->second.deposit) {
                world.addFeature(cz * map.width + cx, x, z, di->second.energy,
                                 di->second.fx, di->second.fz, di->second.blocking != 0,
                                 types.intern(key),false);
            }
        }
}

// Cluster the ore-patch features into deposits and publish them (shared by both
// feature-registration paths, so they agree).
static void installDepositSpots(World& world, const std::vector<std::pair<float, float>>& raw) {
    std::vector<int> par(raw.size());
    for (size_t i = 0; i < par.size(); ++i) par[i] = int(i);
    std::function<int(int)> find = [&](int a) {
        while (par[size_t(a)] != a) { par[size_t(a)] = par[size_t(par[size_t(a)])]; a = par[size_t(a)]; }
        return a;
    };
    const float link2 = 60.0f * 60.0f;
    for (size_t i = 0; i < raw.size(); ++i)
        for (size_t j = i + 1; j < raw.size(); ++j) {
            float dx = raw[i].first - raw[j].first, dz = raw[i].second - raw[j].second;
            if (dx * dx + dz * dz < link2) par[size_t(find(int(i)))] = find(int(j));
        }
    std::map<int, std::pair<std::pair<double, double>, int>> acc;
    for (size_t i = 0; i < raw.size(); ++i) {
        auto& a = acc[find(int(i))];
        a.first.first += raw[i].first; a.first.second += raw[i].second; ++a.second;
    }
    std::vector<std::pair<float, float>> spots;
    for (auto& [root, a] : acc)
        spots.push_back({float(a.first.first / a.second), float(a.first.second / a.second)});
    // TA: the clustered ore patches -- where the AI sends metal extractors.
    world.setDepositSpots(spots);
}

// Register the map's obstacle features into the sim world (reclaim + burning),
// exactly as setupMatch's placement loop does -- for the client's LOCAL harness
// and mission paths, which build their worlds without setupMatch. Nav blocking
// is NOT done here (those paths already block via their own feature placement).
void registerMapFeatures(World& world, const ta::tnt::Map& map, const hpi::Vfs& vfs,
                         const TypeRegistry* reg) {
    world.clearFeatureTypes();
    auto defs = loadFeatureDefs(vfs);
    FeatTypeInterner types(defs);
    std::vector<std::pair<float, float>> rawDeposits;
    scanFeaturePlane(world, map, defs, types, rawDeposits);
    installDepositSpots(world, rawDeposits);
    if (reg)
        for (const auto& [tid, ut] : reg->types()) {
            if (!ut.corpse.empty()) {
                int ci = types.intern(ut.corpse);
                if (ci >= 0) world.mapCorpse(&ut, ci);
            }

        }
    if (reg) linkRaisedUnits(types.table, *reg);
    world.setFeatureTypes(std::move(types.table));
}

// Every type the units on the map can bring into play (themselves, what they
// build, what that builds, ...), by name so the walk is the same on every peer,
// and the reachability labelling (World::prebuildComponents) for each of their
// movement grids and footprints. Built lazily, the first factory output of each
// movement class flood-filled the whole map inside the tick it left the
// factory: 82-110 ms on a 2048-cell map, four times in the first three minutes,
// on every peer. A cache: the simulation is unchanged.
// anims\vismasks.gaf `vismask` (TotalA.exe 0x429b9e): the ten Circular sight
// masks, transparent index 9 (retailexploration.h). Missing or short: the
// built-in discs stay.
static void loadVisMasks(const hpi::Vfs& vfs) {
    RetailVisMasks masks = retailDefaultVisMasks();
    const std::string path = "anims/vismasks.gaf";
    if (vfs.has(path)) try {
        gaf::Palette pal{};   // a distinct colour per index: the loader keys index 9 by colour
        for (int i = 0; i < 256; ++i) { pal.rgba[i][0] = pal.rgba[i][1] = pal.rgba[i][2] = uint8_t(i); pal.rgba[i][3] = 255; }
        for (const auto& seq : gaf::load(vfs.read(path), pal, 9, path)) {
            if (ta::hpi::MountSet::key(seq.name) != ta::hpi::MountSet::key("vismask")) continue;
            for (size_t k = 0; k < seq.frames.size() && k < masks.size(); ++k) {
                const auto& f = seq.frames[k];
                if (f.width <= 0 || f.height <= 0) continue;
                RetailVisMask& m = masks[k];
                m.width = int16_t(f.width); m.height = int16_t(f.height);
                m.xoff = int16_t(f.xoff); m.yoff = int16_t(f.yoff);
                m.on.assign(size_t(f.width) * size_t(f.height), 0);
                for (size_t i = 0; i < m.on.size(); ++i) m.on[i] = f.rgba[i * 4 + 3] != 0;
            }
        }
    } catch (const std::exception&) {}
    setRetailVisMasks(masks);
}

// gamedata\los.tdf (TotalA.exe 0x433130 / 0x433380 / 0x4336f0): True sight's
// ray tables (retailexploration.h). [TABLEINFO] numtables tables, [TABLEn]
// numlines lines "count, a, b, a, b, ...", each stored four times as the
// quarter turns (a, -b), (b, a), (-a, b), (-b, -a), ray q*numlines + i.
static void loadLosTables(const hpi::Vfs& vfs) {
    std::vector<RetailLosTable> tables;
    const std::string path = "gamedata/los.tdf";
    if (vfs.has(path)) try {
        const auto data = vfs.read(path);
        const auto root = tdf::parseText(std::string(data.begin(), data.end()), path);
        const tdf::Node* info = root.child("tableinfo");
        const int count = info ? std::max(0, int(info->numberOr("numtables", 0))) : 0;
        tables.resize(size_t(count));
        for (int t = 0; t < count; ++t) {
            const tdf::Node* node = root.child("table" + std::to_string(t + 1));
            if (!node) continue;
            const int n = std::max(0, int(node->numberOr("numlines", 0)));
            RetailLosTable& table = tables[size_t(t)];
            table.assign(size_t(n) * 4, {});
            for (int i = 0; i < n; ++i) {
                std::vector<int> v;
                const std::string text = node->valueOr("line" + std::to_string(i + 1), "");
                size_t at = 0;
                while (at < text.size()) {   // strtok on "," then atoi
                    const size_t end = std::min(text.find(',', at), text.size());
                    v.push_back(std::atoi(text.substr(at, end - at).c_str()));
                    at = end + 1;
                }
                const int cells = v.empty() ? 0 : std::max(0, v[0]);
                for (int q = 0; q < 4; ++q) {
                    RetailLosRay& ray = table[size_t(q * n + i)];
                    for (int k = 0; k < cells; ++k) {
                        const int a = size_t(1 + 2 * k) < v.size() ? v[size_t(1 + 2 * k)] : 0;
                        const int b = size_t(2 + 2 * k) < v.size() ? v[size_t(2 + 2 * k)] : 0;
                        const std::array<int, 2> c = q == 0 ? std::array<int, 2>{a, -b} : q == 1 ? std::array<int, 2>{b, a}
                                                   : q == 2 ? std::array<int, 2>{-a, b} : std::array<int, 2>{-b, -a};
                        ray.push_back({int8_t(c[0]), int8_t(c[1])});
                    }
                }
            }
        }
    } catch (const std::exception&) { tables.clear(); }
    setRetailLosTables(std::move(tables));
}

// The schema's meteor storm (TotalA.exe 0x436732..0x436813; docs/retail-campaign.md
// section 2.3): a MeteorWeapon name (31 chars at most) arms it; if any of
// MeteorRadius, Density, Duration or Interval is 0 it still does, with every
// number (the weapon too) from gamedata/meteor.tdf [Default] (0x438320). No name
// leaves it disarmed, with that default storm for +Meteor to start.
static void setupMeteorStorm(World& world, const TypeRegistry& reg, const hpi::Vfs& vfs,
                             const tdf::Node* schema) {
    std::string weapon = schema ? schema->valueOr("meteorweapon", "") : std::string();
    if (weapon.size() > 31) weapon.resize(31);
    World::MeteorConfig c;
    c.armed = !weapon.empty();
    if (c.armed) {
        c.radius = int32_t(schema->numberOr("meteorradius", 0));
        c.density = float(schema->numberOr("meteordensity", 0));
        c.duration = float(schema->numberOr("meteorduration", 0));
        c.interval = float(schema->numberOr("meteorinterval", 0));
    }
    if (!c.armed || !c.radius || c.density == 0 || c.duration == 0 || c.interval == 0) {
        weapon.clear(); c.radius = 0; c.density = c.duration = c.interval = 0;
        const std::string path = "gamedata/meteor.tdf";
        if (vfs.has(path)) {
            const auto data = vfs.read(path);
            const auto root = tdf::parseText(std::string(data.begin(), data.end()), path);
            if (const tdf::Node* d = root.child("default")) {
                weapon = d->valueOr("meteorweapon", "");
                if (weapon.size() > 31) weapon.resize(31);
                c.radius = int32_t(d->numberOr("meteorradius", 0));
                c.density = float(d->numberOr("meteordensity", 0));
                c.duration = float(d->numberOr("meteorduration", 0));
                c.interval = float(d->numberOr("meteorinterval", 0));
            }
        }
    }
    // 0x437ce9: the weapon by name, else the table's first record (game +0x2cf3).
    if (const TaWeaponTable* table = reg.taWeapons(); table && !table->empty() && !weapon.empty())
        c.weapon = &table->resolve(weapon);
    world.setMeteorStorm(c);
}

static void prebuildReachability(World& world, const TypeRegistry& reg) {
    std::set<std::string> seen;
    std::vector<const UnitType*> open, types;
    auto add = [&](const UnitType* t) {
        if (t && seen.insert(t->id).second) { open.push_back(t); types.push_back(t); }
    };
    for (const auto& u : world.units())
        if (u.alive()) add(u.type);
    while (!open.empty()) {
        const UnitType* t = open.back(); open.pop_back();
        for (const auto& name : reg.buildable(t->id)) add(reg.find(name));
    }
    world.prebuildComponents(types);
}

std::vector<std::pair<float, float>> setupMatch(World& world, const TypeRegistry& reg,
                                                const MatchConfig& cfg,
                                                std::vector<std::pair<float, float>>* startTable) {
    std::unique_ptr<ScenarioScript> pendingScenario;
    hpi::Vfs generatedData(cfg.vfs, true);
    const hpi::Vfs& vfs = ta::mapgen::isGeneratedMapId(cfg.mapPath) ? generatedData : *cfg.vfs;
    world.setGameSeed(cfg.startSeed);
    loadVisMasks(*cfg.vfs);
    loadLosTables(*cfg.vfs);
    // A "~gen1~" mapPath is a random-map recipe: generate it in memory (identically
    // on client and referee -- the params ride the mapId, generation is integer-only).
    const bool generated = ta::mapgen::isGeneratedMapId(cfg.mapPath);
    int windMin = 100, windMax = 2000;
    double mapGravity = 112.0;
    bool noSeaLevelTrigger = false, authoredPlaytest = false, scenarioOn = false, meteorSet = false;
    world.setBuildRestrictions({},false);
    if (!generated) {
        auto ota = ta::vpath::replaceExtension(cfg.mapPath,".ota");
        if (vfs.has(ota)) {
            const auto bytes = vfs.read(ota);
            const auto root = tdf::parseText(std::string(bytes.begin(), bytes.end()), ota);
            if(const auto* test=root.child("taplaytest"))authoredPlaytest=test->numberOr("authoredscenario",0)!=0;
            const auto* gh = root.child("globalheader");
            const bool declared = gh && gh->numberOr("hasscenario", 0) != 0;
            // Scenario content runs for a Test Map copy (its opt-in) or when the room
            // turned MAP SCENARIO on for a map that declares a scenario.
            scenarioOn = cfg.loadCrt && !cfg.stressTest && !cfg.benchmark &&
                         (authoredPlaytest || (cfg.mapScenario && declared));
            if (gh) {
                // TA economy inputs: tidalstrength (default 0.5) and the schema's
                // SurfaceMetal. Skirmish/MP use the "Network" schema whose start
                // count best matches the players (docs/retail-ai.md section 1).
                world.setTidalStrength(float(gh->numberOr("tidalstrength", 0.5)));
                world.setScoreWeights(float(gh->numberOr("killmul", 0)), float(gh->numberOr("timemul", 0)));
                {
                    int players = 0;
                    for (const auto& sl : cfg.slots) if (sl.used) ++players;
                    const tdf::Node* best = gh->child("Schema 0");
                    int bestDiff = 1 << 30;
                    for (int i = 0; i < 16; ++i) {
                        const tdf::Node* sc = gh->child("Schema " + std::to_string(i));
                        if (!sc) continue;
                        std::string type = sc->valueOr("type", "");
                        for (char& ch : type) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                        if (type.rfind("network", 0) != 0) continue;
                        int starts = 0;
                        if (const tdf::Node* sp = sc->child("specials"))
                            for (const auto& [n, node] : sp->children) {
                                std::string what = node.valueOr("specialwhat", "");
                                for (char& ch : what) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                                if (what.rfind("startpos", 0) == 0) ++starts;
                            }
                        const int diff = std::abs(starts - players);
                        if (diff < bestDiff) { bestDiff = diff; best = sc; }
                    }
                    world.setSurfaceMetal(best ? int(best->numberOr("surfacemetal", 0)) : 0);
                    setupMeteorStorm(world, reg, vfs, best);
                    meteorSet = true;
                }
                windMin = int(gh->numberOr("minwindspeed", 100));
                windMax = int(gh->numberOr("maxwindspeed", 2000));
                mapGravity = gh->numberOr("gravity", 112.0);
                noSeaLevelTrigger = gh->numberOr("nosealeveltrigger", 0) != 0;
                const auto restriction = gh->valueOr("useonlyunits", "");
                if (scenarioOn && !restriction.empty()) {
                    // Map companion names are virtual archive paths, never native paths.
                    // Authored packages carry their restriction file beside the OTA.
                    if (ta::vpath::filename(restriction) != restriction ||
                        restriction == "." || restriction == "..")
                        throw std::runtime_error("Authored Use Only file must be a map companion: " + restriction);
                    const auto slash = ota.find_last_of('/');
                    const auto path = (slash == std::string::npos ? std::string{} : ota.substr(0,slash+1)) + restriction;
                    if (hpi::MountSet::key(path) != hpi::MountSet::key(ta::vpath::replaceExtension(ota,".tdf")))
                        throw std::runtime_error("Authored Use Only file must match the map name: " + restriction);
                    if (!vfs.has(path)) throw std::runtime_error("Missing authored Use Only file: " + path);
                    const auto data = vfs.read(path);
                    const auto whitelist = tdf::parseText(std::string(data.begin(),data.end()),path);
                    std::vector<std::string> allowed;
                    for (const auto& name : whitelist.childOrder) {
                        if (!reg.find(name)) {
                            // MAP SCENARIO plays maps made for the original game, whose lists
                            // can name units no install has (NeverGlory: 44 of its 242). A name
                            // no unit answers to permits nothing, so it is left out. A Test Map
                            // copy still refuses it: there it is the author's own typo,
                            // unless a unit file of that name exists.
                            if (!authoredPlaytest || balanceUnitFile(vfs, name)) continue;
                            throw std::runtime_error("Unknown authored Use Only unit: " + name);
                        }
                        allowed.push_back(name);
                    }
                    world.setBuildRestrictions(std::move(allowed));
                }
            }
        }
    }
    if (!meteorSet) setupMeteorStorm(world, reg, vfs, nullptr);   // no OTA: meteor.tdf's, disarmed
    world.setWindRange(windMin, windMax);
    world.setBallisticGravityRaw(retailBallisticGravityRaw(mapGravity));
    world.setMapGravity(int32_t(mapGravity));
    std::vector<std::pair<float, float>> genStarts;
    ta::tnt::Map map;
    if (generated) {
        auto g = ta::mapgen::generate(ta::mapgen::parseMapId(cfg.mapPath), vfs);
        map = std::move(g.map);
        for (auto& [scx, scz] : g.starts)   // cell -> px, matching parseStartPositions
            genStarts.push_back({float(scx * 16), float(scz * 16)});
    } else {
        map = ta::tnt::Map::load(vfs.read(cfg.mapPath), cfg.mapPath);
    }
    // Validate the incoming mode before replacing terrain. A reused World may
    // still carry the previous replay's mode; it must not constrain the new match.
    if(!validPathfindingMode(uint8_t(cfg.pathfindingMode)))
        throw std::invalid_argument("invalid pathfinding mode");
    world.setPathfindingMode(PathfindingMode::Retail);
    world.setTerrain(map.heights, map.width, map.height, map.seaLevel, &map.features);
    world.setNoSeaLevelTrigger(noSeaLevelTrigger);
    // One nav grid per distinct movement-limit tuple, as retail bakes one per class.
    // After setTerrain (it needs the heights) and before anything blocks a cell.
    world.buildNavClasses(reg);
    // Retail's background pathfinder. Every match gets it; see
    // docs/retail-engine.md for what it is and what it still cannot do.
    world.setPathService(true);
    world.setPathfindingMode(cfg.pathfindingMode);
    world.setPatrolRepairs(cfg.patrolRepairs);
    // Route delivery timing affects steering and RNG order. Use retail's
    // default work budget (0x41617b), not the former performance shortcut.
    world.setPathBudget(kPathBudgetDefault);
    world.clearFeatures();   // authoritative rebuild (the client ctor pre-registers)

    // Features: block nav footprints, and gather deposit positions. Iterate
    // in the same (row-major) order the client does so clustering is identical.
    auto defs = loadFeatureDefs(vfs);
    // Intern feature names into the world's FeatType table, resolving the
    // featureburnt chain recursively (AraTree01 -> AraTree01a -> Arasmudge01).
    // First-seen order over the row-major cell walk = identical on every peer.
    FeatTypeInterner types(defs);
    auto featTypeIdx = [&](const std::string& nm) { return types.intern(nm); };
    std::vector<std::pair<float, float>> rawDeposits;
    scanFeaturePlane(world, map, defs, types, rawDeposits);
    // Corpse defs: every unit type's FBI corpse= feature (and its chains) joins
    // the table so death can mint corpse records without art/def lookups later.
    for (const auto& [tid, ut] : reg.types()) {
        if (!ut.corpse.empty()) {
            int ci = featTypeIdx(ut.corpse);
            if (ci >= 0) world.mapCorpse(&ut, ci);
        }

    }
    linkRaisedUnits(types.table, reg);
    world.setFeatureTypes(std::move(types.table));
    installDepositSpots(world, rawDeposits);

    // Players + teams.
    world.setPlayerCount(int(cfg.slots.size()));
    // 0x4f6379 puts every initialized player in retail's 5x path-budget class;
    // only a campaign mission (setupMission) narrows it afterwards.
    world.setPathBudgetClasses(~0u);
    for (int i = 0; i < int(cfg.slots.size()); ++i) {
        world.setTeam(i, cfg.slots[i].team);
        world.player(i).cacheClock.enabled=true;
        world.player(i).books.mult = cfg.slots[i].incomeMult;  // TA: AI income x0.5/0.7/1.0 (retail)
        world.player(i).automaticGates = cfg.slots[i].automaticGates;
        world.player(i).defensiveAi = cfg.slots[i].defensiveAi;
    }
    // GODS DO NOT APPEAR IN MULTIPLAYER. This is a deliberate divergence, decided
    // 2026-09-16 -- do not "restore fidelity" here without asking.
    //
    // Retail's routine at 0x519420 reads GameData\\Gods.tdf [TIMING] and rolls a
    // GameChance (shipped: 0.1) for whether the game is god-based at all, then draws
    // an appear time uniformly in AppearTimeMin..Max (shipped: 30..60 minutes, stored
    // as a 16-bit second count at 0x64117a). A missing file falls back to the same
    world.setUnitCap(cfg.unitCap);
    world.setMonarchExpendable(cfg.monarchExpendable);
    world.setDoubleSight(cfg.doubleSight);
    world.setPowerCodes(cfg.powerCodes, cfg.powerMapRevealed);
    world.setRoomVision(cfg.roomMapRevealed, cfg.roomLosOff);
    world.setTrueLos(cfg.trueLos);

    // Cartographer scenarios use authored placements instead of skirmish monarchs.
    // Read the same map-local CRT on every peer, before any default units spawn.
    if (scenarioOn && !generated) {
        const auto otaPath = ta::vpath::replaceExtension(cfg.mapPath, ".ota");
        const auto otaBytes = vfs.tryRead(otaPath);
        if (otaBytes && ta::tnt::Scenario::parse(std::string(otaBytes->begin(), otaBytes->end())).hasScenario) {
            const auto crtPath = ta::vpath::replaceExtension(cfg.mapPath, ".crt");
            if (!vfs.has(crtPath)) throw std::runtime_error("Scenario map is missing its CRT file");
            auto scenario = ta::crt::parse(vfs.read(crtPath));
            if (scenario.version == 0) throw std::runtime_error("Scenario CRT file is damaged");
            auto requirePlayer = [&](int player) {
                if (player == kNeutralPlayer) { world.enableScenarioNeutralPlayer(); return; }
                if (player < 0 || player >= kMaxPlayers)
                    throw std::runtime_error("Scenario owner is outside the supported player range");
                if (player >= int(cfg.slots.size()) || !cfg.slots[size_t(player)].used)
                    throw std::runtime_error("Seat player " + std::to_string(player + 1) + " in the lobby to play this scenario");
            };
            for (const auto& type : scenario.customTypes) {
                auto name = type.name;
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                // Retail skips a custom type no unit answers to, silently: its resolve loop
                // (0x4cd243) stores the lookup's 0 for an unknown name with no test and no
                // message, and 0x4cd590 then matches no unit (type ids start at 1).
                if (!reg.find(name)) continue;
                // stat[3]: the type's starting kill count (an old rank file's rank x 5, crt::parse).
                world.setScenarioTypeStats(name, std::clamp(type.stat[1],0,1000),
                    std::clamp(type.stat[2],0,1000), std::clamp(type.stat[3],0,65535));
            }
            // Banks 1..10 belong to owners 0..9 (crt.h Owners: parse renumbers an older
            // eight-player file). A bank whose groups are all empty does
            // nothing, so it needs no seat (259; it used to demand one). A bank past the
            // players' is not Cartographer output and keeps the old rule (the neutral owner,
            // or a refusal), so no file sets up differently except by starting at all.
            for (size_t p = 1; p < scenario.players.size(); ++p)
                if (p > size_t(kMaxPlayers) ? !scenario.players[p].empty()
                                            : std::any_of(scenario.players[p].begin(), scenario.players[p].end(), ta::crt::hasRules))
                    requirePlayer(int(p)-1);
            for (const auto& placed : scenario.units) {
                requirePlayer(placed.player);
                auto name = placed.objectName;
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                if (!reg.find(name)) throw ScenarioTypeMissing(placed.objectName);
                if (placed.x < 0 || placed.z < 0 || placed.x >= map.width || placed.z >= map.height)
                    throw std::runtime_error("Scenario unit is outside the map: " + placed.objectName);
            }
            std::vector<std::pair<float,float>> centers(cfg.slots.size(), {0,0});
            std::vector<int> counts(cfg.slots.size(), 0);
            for (const auto& placed : scenario.units) {
                auto name = placed.objectName;
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                const auto* type = reg.find(name);
                const float x = float(placed.x) * 16 + type->footX * 8, z = float(placed.z) * 16 + type->footZ * 8;
                const int id = world.spawn(type, x, z, float(placed.angle) * 3.14159265f / 180.0f, placed.player);
                if (auto* unit = world.unit(id)) {
                    // Native 51f4c0 keeps at most 31 bytes; this is a display label.
                    unit->scenarioName = placed.uniqueName.substr(0,31);
                    // The CRT vertical field is not read by native 4cc8e6..4cc953.
                    // Terrain/flight initialization supplies height, not placed.y.
                    // Native 4cd32d..4cd3e2: the placement's kills replace the type default;
                    // placement armor/weapon multiply the type defaults. Health is current
                    // HP percentage, truncated and clamped to [0, type maximum], not max HP.
                    const float health = float(double(std::clamp(placed.health,0,100)) * double(0.01f) * double(unit->hp.floorInt()));
                    unit->hp = Fixed::fromInt(std::clamp(int(health),0,std::max(0,type->maxHp)));
                    unit->scenarioArmor = float(double(unit->scenarioArmor) * double(std::clamp(placed.armor,0,1000)) * double(0.01f));
                    unit->scenarioWeapon = float(double(unit->scenarioWeapon) * double(std::clamp(placed.weapon,0,1000)) * double(0.01f));
                    // TA's veterancy: its kill count (an old rank file's rank x 5, crt::parse).
                    world.setKills(*unit, placed.kills);
                    if (type->isStructure()) world.blockFoot(*type, x, z, true);
                    world.finishAsFeature(id);   // 51230c: a placed wall becomes its feature
                } else throw std::runtime_error("Could not place scenario unit: " + placed.objectName);
                if (!world.isNeutralPlayer(placed.player)) {
                    centers[size_t(placed.player)].first += x;
                    centers[size_t(placed.player)].second += z;
                    ++counts[size_t(placed.player)];
                }
            }
            // Each seat's position (the opening camera and the AI's first targets): the
            // centre of its placed units, else its own slot's start (slot N at StartPos N,
            // as BY SLOT does), else the map centre. START LOCATIONS does not apply here
            // in any mode: CRT owners are slot numbers and no Monarch is spawned. A
            // scenario of rules alone falls through to the assignment below, which does.
            std::vector<std::pair<float,float>> assigned;
            const auto starts = parseStartPositions(vfs, cfg.mapPath);
            for (size_t p = 0; p < cfg.slots.size(); ++p) if (cfg.slots[p].used) {
                if (counts[p]) assigned.push_back({centers[p].first / counts[p], centers[p].second / counts[p]});
                else assigned.push_back(p < starts.size() ? starts[p] : std::pair<float,float>{float(map.width)*8, float(map.height)*8});
            }
            uint32_t participants = 0;
            for (size_t p=0; p<cfg.slots.size(); ++p) if (cfg.slots[p].used) participants |= uint32_t(1)<<p;
            auto runner = std::make_unique<ScenarioScript>(scenario, reg, cfg.scenarioViewPlayer,
                                                           world.numPlayers(), map.width, map.height, participants);
            runner->setTraceSink(cfg.scenarioTrace);
            pendingScenario = std::move(runner);
            // Rules/regions without placements extend an ordinary skirmish;
            // keep its authored start positions and default monarchs.
            if (!scenario.units.empty()) {
                world.setScenario(std::move(pendingScenario));
                prebuildReachability(world, reg);
                return assigned;
            }
        }
    }

    // Assign the used slots to start positions (ring fallback if the map has too few).
    // FIXED: the k-th USED slot takes spot k, so an empty seat takes no start: a human
    // in slot 1 and an AI in slot 8 open at StartPos1 and StartPos2. That is retail's
    // rule, not an accident -- the skirmish spawn (0x4d2e70) and the multiplayer
    // host's start table (0x4ea950) both hand the k-th occupied battle-room row the
    // k-th start (docs/retail-engine.md, "Start positions"; start_positions_test).
    // BY SLOT, which is not retail, gives slot N spot N instead (startSpotsFor).
    int used = 0;
    for (auto& s : cfg.slots) if (s.used) ++used;
    auto starts = generated ? genStarts : parseStartPositions(vfs, cfg.mapPath);
    float cx = map.blocksX * 16.0f, cz = map.blocksY * 16.0f;
    std::vector<std::pair<float, float>> spots = starts;
    StartMode startMode = cfg.startMode;
    // Benchmark: ignore the map's (few) start positions and spread all factions evenly
    // on a big ring around the map centre, so their large armies don't pile onto each
    // other. Radius is kept well inside the map edge (each army block is ~2840px wide).
    // The ring has a spot per seated player, not per slot, so it is always filled in
    // seated order.
    if (cfg.benchmark) {
        spots.clear();
        float radius = std::min(cx, cz) * 0.62f;
        for (int k = 0; k < used; ++k) {
            float a = float(k) / float(std::max(used, 1)) * 6.2831853f;
            spots.push_back({cx + detmath::cos(a) * radius, cz + detmath::sin(a) * radius});
        }
        if (startMode == StartMode::BySlot) startMode = StartMode::Fixed;
    }
    // Random Start Locations: shuffle the spot list before slots are assigned to it.
    // A Fisher-Yates on a small seeded LCG -- integer-only and order-stable, so every
    // peer and the referee land on the same arrangement.
    if (startMode == StartMode::Random && spots.size() > 1) {
        uint32_t rs = cfg.startSeed ? cfg.startSeed : 0x54414B53u;
        for (size_t i = spots.size() - 1; i > 0; --i) {
            rs = rs * 1103515245u + 12345u;
            size_t j = size_t((rs >> 16) % uint32_t(i + 1));
            std::swap(spots[i], spots[j]);
        }
    }
    while (int(spots.size()) < used) {
        float a = float(spots.size()) / float(std::max(used, 1)) * 6.2831853f;
        spots.push_back({cx + detmath::cos(a) * 300, cz + detmath::sin(a) * 300});
    }
    if (startTable) *startTable = spots;
    // Which spot each slot opens at. Every index is below max(starts, used), which
    // the ring above has filled. Fixed and Random index in seated order, exactly as
    // the running counter here did before BY SLOT.
    const std::vector<int> slotSpot = startSpotsFor(cfg.slots, starts.size(), startMode);
    std::vector<std::pair<float, float>> assigned;
    // Benchmark: a gradual ramp -- each faction spawns 1 unit every 1/spawnsPerSec seconds
    // for 60 real seconds. The intensity sets the count N; the stages fire in order as the
    // server's Cmd::BenchClock says they are due (stage k at (k+1)/N of the run), so every
    // peer and the replay agree whatever the game speed. Units filled below.
    // Both block layouts below are BLIND grids -- a fixed lattice centred on the start
    // spot -- and World::spawn does no terrain check at all, so a unit whose lattice
    // point lands in a lake, on a cliff face or inside a blocked footprint is simply
    // placed there. Measured on Ulasem with 8 players: 4.5% of benchmark units at
    // intensity Low, 7.2% at Extra Absurd, and 20.5% of a stress-test fill -- one unit
    // in five starting somewhere it cannot stand. They cannot walk out (a blocked cell
    // is blocked in both directions), so they sit there for the whole run, which both
    // looks wrong and quietly makes the benchmark measure a different thing than it
    // claims to: a fifth of the army is inert.
    //
    // So each lattice point gets snapped to the nearest cell the unit can actually
    // occupy. `claimed` stops the snap from stacking bodies: without it, everything
    // that lands in the same lake converges on the one shoreline cell nearest it, and
    // bodies are solid with nothing to pull an overlap apart. Deterministic -- a fixed
    // ring scan over the nav grid, and a claim set filled in a fixed order -- so every
    // peer lays out the identical army and lockstep holds.
    // Cells already spoken for by a placed body. A flat bitmap over the nav grid, not
    // a std::set: the ring scan below can touch tens of thousands of cells per unit on
    // a crowded map, and a set lookup per cell made setup take minutes.
    const int claimW = std::max(world.nav().width(), 1);
    const int claimH = std::max(world.nav().height(), 1);
    std::vector<uint8_t> claimBits(size_t(claimW) * size_t(claimH), 0);
    // Floor division by 16: a body can extend left of x=0, where C's truncation would
    // round the wrong way and claim the wrong cell.
    auto floorDiv16 = [](int v) { return v >= 0 ? v / 16 : -((-v + 15) / 16); };
    auto claimedAt = [&](int x, int z) {
        if (x < 0 || z < 0 || x >= claimW || z >= claimH) return true;   // off-map = taken
        return claimBits[size_t(z) * size_t(claimW) + size_t(x)] != 0;
    };
    // Reserve a whole FOOTPRINT, never a centre cell. fits(x,z,foot) asks about the
    // square [x,x+foot) x [z,z+foot), so that is what a body occupies and that is what
    // has to be claimed. Reserving only {nx,nz} was not overlap protection at all: two
    // 2x2 units could take adjacent cells 16px apart while needing 32, which is exactly
    // where snapping packs them -- along a shoreline, where the free cells are a thin
    // line. Measured on Inner Circle with 8 players: 19059 overlapping pairs with
    // centre-cell claims.
    auto claimFoot = [&](int nx, int nz, int foot) {
        // CENTRED, like everything else that reasons about a footprint: NavGrid::fits
        // and World::cellScore both take (cx,cz) as the body's centre and work from
        // cx - foot/2. Claiming [nx, nx+foot) instead offset every reservation by half
        // a footprint, so with MIXED sizes two disjoint claims could still describe
        // overlapping bodies -- a size-2 and a size-4 two cells apart passed the check
        // while actually overlapping.
        // Claim the cells the BODY ACTUALLY COVERS, not a foot x foot block.
        //
        // A snapped unit sits at the CENTRE of cell nx (ux = nx*16 + 8) with half-width
        // foot*8, so an EVEN footprint straddles half a cell on each side and touches
        // foot+1 cells. Claiming only foot of them let two disjoint claims describe
        // bodies that overlap: a size-2 and a size-3 could end up 32px apart needing 40.
        // Every overlapping pair in the benchmark fill was mixed-size for exactly this
        // reason -- 857 of them over 3140 units, invisible until the test that was
        // supposed to catch them stopped halving its own threshold.
        const int hs = foot * 8;
        const int lox = floorDiv16(nx * 16 + 8 - hs), hix = floorDiv16(nx * 16 + 8 + hs - 1);
        const int loz = floorDiv16(nz * 16 + 8 - hs), hiz = floorDiv16(nz * 16 + 8 + hs - 1);
        for (int z = loz; z <= hiz; ++z)
            for (int x = lox; x <= hix; ++x)
                if (claimedAt(x, z)) return false;
        for (int z = loz; z <= hiz; ++z)
            for (int x = lox; x <= hix; ++x)
                if (x >= 0 && z >= 0 && x < claimW && z < claimH)
                    claimBits[size_t(z) * size_t(claimW) + size_t(x)] = 1;
        return true;
    };
    // Fallback scan position, PER (footprint, nav grid) -- not one shared cursor.
    //
    // The cursor is forward-only so the whole fill costs one pass over the grid rather
    // than a rescan per unit. Sharing it across sizes made that correctness-visible: a
    // 4x4 body advances the cursor past every gap only a 2x2 would fit in, and a later
    // 2x2 whose local search fails cannot go back for them -- so its player's fill
    // stopped early on a map that still had room. One cursor per size and domain keeps
    // the single-pass cost (there are only a handful of distinct sizes) without one
    // roster entry consuming another's space.
    //
    // Grids are identified by FIRST-SEEN ORDER, not by pointer value: snapSpawn is
    // called in a deterministic order on every peer, so the indices match, whereas
    // pointer values would not.
    std::vector<const NavGrid*> sweepGrids;
    std::map<std::pair<int, int>, int> sweepCursors;
    auto sweepGridIdx = [&](const NavGrid* g) {
        for (size_t i = 0; i < sweepGrids.size(); ++i)
            if (sweepGrids[i] == g) return int(i);
        sweepGrids.push_back(g);
        return int(sweepGrids.size() - 1);
    };
    int mapCapacity = -1;  // bodies this map can hold, computed once (see capacityFor)
    // How many bodies of `roster`'s typical size this map can hold, minus 5% slack so
    // the snap is never scraping the last few free cells. Walkable AREA divided by what
    // a roster unit actually occupies -- footprints here run 2x2 to 4x4, so counting
    // cells alone would overstate capacity several-fold.
    auto capacityFor = [&](const std::vector<const UnitType*>& roster) {
        if (mapCapacity >= 0 || roster.empty()) return mapCapacity;
        long walkable = 0;
        const NavGrid& g0 = world.navFor(roster.front());
        for (int z = 0; z < g0.height(); ++z)
            for (int x = 0; x < g0.width(); ++x)
                if (g0.fits(x, z, 1)) ++walkable;
        long footArea = 0;
        for (const UnitType* t : roster) {
            const int fo = std::clamp(std::max(t->footX, t->footZ), 1, 15);
            footArea += long(fo) * long(fo);
        }
        const long meanFoot = std::max<long>(1, footArea / long(roster.size()));
        mapCapacity = int((walkable / meanFoot) * 95 / 100);
        return mapCapacity;
    };
    // How far a snap may look, in cells. Generous on purpose: `claimed` makes units
    // compete for cells, so the radius has to hold the whole block, not just clear the
    // nearest lake. A radius-r disc holds about pi*r^2 cells before terrain takes its
    // cut, and the first attempt at 24 (~1810) quietly ran out against a 1900-unit
    // stress fill, leaving 2.4% of the army in terrain anyway -- the exhaustion looked
    // exactly like a terrain limit, and was not. 96 leaves 1 unit of 15208 misplaced.
    //
    // It costs nothing to be generous here: the ring scan returns the instant it finds
    // a cell, so a wide bound is only ever walked by the units that genuinely need it.
    // Measured over the whole 8-player setup, 24 vs 96 is 3.98s vs 4.08s -- noise. A
    // formula that sized the radius to the unit count was written first and deleted:
    // it was more code, and slower to converge on the right answer than the constant.
    constexpr int kSnapCells = 96;
    // The class grid alone is not enough: the mover also holds each type to its own maxSlope (World::passable), so
    // a body snapped onto a slope its class allows but its type does not could never take a step (2026-11-19:
    // watch_end's all-AI stress game, 128 of 399 bodies spawned on such cells; the winner's reserve stood at home
    // and the game never ended). Every snap requires the footprint passable for the type itself.
    auto snapSpawn = [&](const UnitType* t, float& ux, float& uz) {
        if (!t) return true;
        if (t->canFly) {
            // Flyers may cross any terrain, but the stress lattice must not
            // start them outside the map (they can later enter a landed state).
            const float halfX=float(t->footX)*8, halfZ=float(t->footZ)*8;
            ux=std::clamp(ux,halfX,std::max(halfX,float(claimW)*16-halfX));
            uz=std::clamp(uz,halfZ,std::max(halfZ,float(claimH)*16-halfZ));
            return true;
        }
        const NavGrid& g = world.navFor(t);
        if (g.empty()) return true;
        const int foot = std::clamp(std::max(t->footX, t->footZ), 1, 15);
        const int cx = footprintCell(ux,foot), cz = footprintCell(uz,foot);
        for (int r = 0; r <= kSnapCells; ++r)
            for (int j = -r; j <= r; ++j)
                for (int i = -r; i <= r; i += (j == -r || j == r) ? 1 : 2*r) {
                    // Same perimeter and row order; skip the unused interior
                    // directly so dense stress fills do not exhaust the load timeout.
                    const int nx = cx + i, nz = cz + j;
                    if (!g.fits(nx, nz, foot) || !world.footprintPassable(t, nx, nz, foot)) continue;
                    if (!claimFoot(nx, nz, foot)) continue;                  // taken
                    ux = footprintWaypoint(nx,foot).toFloat();
                    uz = footprintWaypoint(nz,foot).toFloat();
                    return true;
                }
        // Nothing free within kSnapCells of where this unit wanted to be. Sweep a
        // GLOBAL cursor for the next free spot anywhere rather than giving up or
        // dropping the body on top of someone. The cursor only moves forward, so the
        // whole fill costs one pass over the grid however many units ask.
        int& sweepCursor = sweepCursors[{foot, sweepGridIdx(&g)}];
        for (; sweepCursor < claimW * claimH; ++sweepCursor) {
            const int nx = sweepCursor % claimW, nz = sweepCursor / claimW;
            if (!g.fits(nx, nz, foot) || !world.footprintPassable(t, nx, nz, foot)) continue;
            if (!claimFoot(nx, nz, foot)) continue;
            ux = footprintWaypoint(nx,foot).toFloat();
            uz = footprintWaypoint(nz,foot).toFloat();
            return true;
        }
        return false;                             // the map really is full
    };
    // The Monarch needs the same treatment as everything else, and used only to have its
    // square RESERVED so a snapped unit would not stand on it -- its own position was
    // never checked. Every other body gets snapped to ground it can occupy; the Monarch
    // was spawned at the raw start spot whatever was there.
    //
    // That matters most exactly where it is least visible. A real map's start positions
    // are authored and fine, but the benchmark ignores them and spreads factions around a
    // computed ring (cx + cos(a)*radius), and any game with more slots than the map has
    // starts fills the shortfall from a second computed ring. Neither consults the
    // terrain, so a Monarch could open the match standing in a lake or on a cliff face --
    // and being unable to move, it stayed there.
    //
    // Keep the authored spot when the Monarch actually fits it, so valid maps place
    // exactly as before; snap only when it does not.
    auto placeMonarch = [&](const UnitType* t, float& ux, float& uz) {
        if (!t || t->canFly) return;
        const int foot = std::clamp(std::max(t->footX, t->footZ), 1, 15);
        const NavGrid& g = world.navFor(t);
        const int cx = footprintCell(ux,foot), cz = footprintCell(uz,foot);
        if (!g.empty() && g.fits(cx, cz, foot) && claimFoot(cx, cz, foot))
            return;                       // the spot is good: leave it untouched
        snapSpawn(t, ux, uz);             // claims the cell it settles on
    };

    const int kBenchSpawns = benchmarkSpawns(cfg.benchmark);   // 0 when off
    // Reserve every monarch before an earlier player's stress army can consume
    // the remaining ground cells on a cramped map.
    std::vector<std::pair<float,float>> monarchPositions(cfg.slots.size());
    for (size_t i=0;i<cfg.slots.size();++i) if (cfg.slots[i].used) {
        auto [x,z]=spots[size_t(slotSpot[i])];
        placeMonarch(reg.find(ta::sides::commander(cfg.slots[i].faction)),x,z);
        monarchPositions[i]={x,z};
    }
    std::vector<BenchStage> benchPlan;
    if (kBenchSpawns > 0) {
        benchPlan.resize(size_t(kBenchSpawns));
        for (int s = 0; s < kBenchSpawns; ++s)
            // Nominal only (where it falls at NORMAL speed): BenchClock fires it.
            benchPlan[size_t(s)].tick = uint32_t((uint64_t(s + 1) * 1800) / uint64_t(kBenchSpawns));
    }
    // Build the process-wide ground-support rotation table now (retailRotationCoefficients:
    // 65536 Q60 Taylor evaluations, 20-50 ms, a pure function of the heading). Its first
    // user is the first spawn of a non-upright ground-support unit; that used to be the
    // stress fill's first catapult here at setup, but a staged fill or a benchmark ramp
    // spawns it inside a tick, where it was the whole opening hitch of a staged run.
    // Hash-neutral: the table is immutable and identical whenever it is built.
    (void)retailRotationCoefficients(0);
    // Staged stress fill (see MatchConfig::stressImmediate): stage k spawns at tick k+1.
    const bool stressStaged = cfg.stressTest && !cfg.stressImmediate;
    std::vector<BenchStage> stressPlan;
    if (stressStaged) {
        stressPlan.resize(size_t(kStressStageTicks));
        for (int s = 0; s < kStressStageTicks; ++s) stressPlan[size_t(s)].tick = uint32_t(s + 1);
    }
    for (int i = 0; i < int(cfg.slots.size()); ++i) {
        if (!cfg.slots[i].used) continue;
        const UnitType* monarch = reg.find(ta::sides::commander(cfg.slots[i].faction));
        auto [mx,mz]=monarchPositions[size_t(i)];
        // AFTER the snap, not before: this list is what the caller opens the camera on
        // (and what the server records as the slot's position), so recording the raw
        // spot would point the opening view at the lake the Monarch was just moved out
        // of. Unchanged whenever the spot was already good, which is every real map.
        assigned.push_back({mx, mz});
        // 4d3200's 512060 passes 1: the Monarch's mogriumstorage (5000 for all
        // five) is the pool a skirmish or multiplayer game opens with.
        world.setStartResources(i, cfg.startEnergy, cfg.startMetal);
        world.spawn(monarch, mx, mz, 0, i);
#ifndef NDEBUG
        // Harness fixture (TA_START_FLYERS=N, at most 16): every player also
        // starts with N finished flying builders of its side (every other one at
        // half health), round-robin by type (Aramon arafly, Taros tarprie2/
        // tarpries; Zhon's, zonhunt, is its Monarch and so skipped, and Veruna
        // and Creon have none), in rows just north of its Monarch, so
        // autoplay runs put flying guards to work (VTOL_Follow's help, repair
        // and orbit) from the first minute. A shared input to the lockstep
        // setup, as TA_GODS: set it for EVERY peer in the run, the server too.
        if (const char* e = std::getenv("TA_START_FLYERS"); e && monarch) {
            std::vector<const UnitType*> flyers;
            for (const auto& [id, t] : reg.types())
                if (t.side == monarch->side && t.canFly && t.isBuilder && !t.isStructure() && !t.commander)
                    flyers.push_back(&t);
            const int n = std::clamp(std::atoi(e), 0, 16);
            const float right = float(map.width) * 16.0f - 48.0f, bottom = float(map.height) * 16.0f - 48.0f;
            for (int k = 0; k < n && !flyers.empty(); ++k) {
                const float fx = std::clamp(mx + float(k % 4) * 48.0f - 72.0f, 48.0f, std::max(48.0f, right));
                const float fz = std::clamp(mz - 96.0f - float(k / 4) * 48.0f, 48.0f, std::max(48.0f, bottom));
                const int id = world.spawn(flyers[size_t(k) % flyers.size()], fx, fz, 0, i);
                // Every other one starts at half health: a repair job for a flying guard.
                if (Unit* f = world.unit(id); f && (k & 1))
                    f->hp = Fixed::fromInt(std::max(1, int(f->type->maxHp) / 2));
            }
        }
#endif
        // Reserve per-type slots for both immediate stress spawns and the future
        // benchmark plan. Continue round-robin with eligible types so unique
        // units do not either multiply or shrink the requested workload.
        std::map<const UnitType*,int> allocated;
        for (const auto& u:world.units())
            if (u.alive() && u.player==i) ++allocated[u.type];
        auto nextType=[&](const std::vector<const UnitType*>& roster,size_t& cursor) -> const UnitType* {
            for (size_t attempt=0;attempt<roster.size();++attempt) {
                const auto* type=roster[cursor++ % roster.size()];
                if (type->totalAllowed>0 && allocated[type]>=type->totalAllowed) continue;
                ++allocated[type];
                return type;
            }
            return nullptr;
        };

        // Stress test: fill this player to ~95% of the unit cap with its faction's
        // combat units right now, so an all-AI game starts under a heavy sim load.
        // Deterministic: fixed roster (name-sorted), round-robin, grid placement --
        // every peer builds the identical army, so lockstep holds.
        if (cfg.stressTest && monarch) {
            // Land+air combat units only (exclude Water-domain so no boats spawn on land).
            std::vector<const UnitType*> roster;
            for (const UnitType* t : reg.combatUnits(monarch->side))
                if (t->domain != UnitType::Domain::Water) roster.push_back(t);
            int cap = cfg.unitCap > 0 ? cfg.unitCap : 1000;   // unlimited -> a sane default
            int target = (cap * 95) / 100;
            // ...but never more than the MAP can hold. The fill used to ask for 95% of
            // the cap per player regardless -- 15200 bodies at cap 2000 with 8 players,
            // on an Inner Circle that holds about 12000 -- and the excess had nowhere
            // legal to go: it ended up either in terrain (permanently stuck, because a
            // body cannot walk out of a cell it could not walk into) or inside another
            // body. There is no placement that satisfies both once it is
            // over-subscribed, so do not over-subscribe: take 95% of real capacity and
            // leave the last 5% as slack, so the snap is never scraping the last few
            // cells on the map.
            //
            // Capacity is the walkable area divided by what a roster unit actually
            // occupies -- footprints here run 2x2 to 4x4, so counting cells alone would
            // overstate it several-fold.
            mapCapacity = capacityFor(roster);
            // Divide by the players who ACTUALLY SPAWN, not the slot-vector length.
            // Both client and server size that vector to maxSlot + 1, so a lobby with
            // gaps (say players in slots 0 and 7) counts 8 and hands each of the two a
            // quarter of what the map can hold -- reserving room for six armies that
            // never arrive, and quietly shrinking every stress and benchmark run on a
            // capacity-limited map.
            const int players = std::max(1, used);
            target = std::min(target, std::max(1, mapCapacity / players));
            if (!roster.empty() && target > 0) {
                int cols = 1;                                 // integer ceil(sqrt(target))
                while (cols * cols < target) ++cols;
                // Space units out so they don't start piled inside one another. This was
                // sized for the separation pass (~1.5 per 32px nav cell left it a handful
                // of neighbours to push, against the pathological ~4/cell a tight 16px
                // grid produced -- the whole initial separation spike, a pure spawn
                // artifact). That pass is gone, and the spacing matters as much now for a
                // different reason: bodies are solid and nothing pulls an overlap apart,
                // so units spawned inside each other would simply stay that way.
                const float spacing = 24.0f;
                float x0 = mx - float(cols) * spacing * 0.5f; // centre the block on the start
                float z0 = mz + spacing;                      // just south of the Monarch
                size_t cursor=0;
                for (int k = 0; k < target; ++k) {
                    const UnitType* t = nextType(roster,cursor);
                    if (!t) break;
                    float ux = x0 + float(k % cols) * spacing;
                    float uz = z0 + float(k / cols) * spacing;
                    // The lattice ignores terrain; this does not. False means the map
                    // is genuinely full -- stop rather than stack the remainder.
                    if (!snapSpawn(t, ux, uz)) break;
                    // Staged: unit k of this player's army appears at tick
                    // 1 + k*kStressStageTicks/target, so every army grows evenly. The
                    // snap above already claimed its footprint, exactly as before.
                    if (stressStaged)
                        stressPlan[size_t((int64_t(k) * kStressStageTicks) / target)]
                            .units.push_back({t, ux, uz, i});
                    else
                        world.spawn(t, ux, uz, 0, i);
                }
            }
        }
        // Benchmark: append this faction's staged units (land+air combat only -- Water-
        // domain excluded, so no boats on land) to the plan, on a deterministic grid
        // centred on the start. Same roster/order/grid on every peer -> lockstep.
        if (cfg.benchmark && monarch) {
            std::vector<const UnitType*> roster;
            for (const UnitType* t : reg.combatUnits(monarch->side))
                if (t->domain != UnitType::Domain::Water) roster.push_back(t);
            if (!roster.empty()) {
                // Cap the plan at what the map can hold, exactly as the stress fill
                // does. Intensity 5 asks for 960 units per player -- 7680 across 8 --
                // and Inner Circle holds about 3100 bodies of this roster's footprint,
                // so most of the top intensities were queueing units that had nowhere
                // legal to stand. The RAMP is what the benchmark measures, so this
                // shortens it rather than thinning it: the run still spawns at the
                // chosen rate, it just stops once the map is full.
                mapCapacity = capacityFor(roster);
                const int players = std::max(1, int(cfg.slots.size()));
                const int myShare = std::max(1, mapCapacity / players);
                const int plan = std::min(kBenchSpawns, myShare);
                int cols = 1; while (cols * cols < plan) ++cols;   // ceil(sqrt(plan))
                const float spacing = 40.0f;
                float x0 = mx - float(cols) * spacing * 0.5f;   // centre the block on the start
                float z0 = mz - float(cols) * spacing * 0.5f;
                // One unit per stage (each 0.25s tick), cycling the roster, on a grid cell.
                // Snapped at PLAN time, not spawn time: the plan is built identically on
                // every peer here, whereas snapping during the run would have to agree
                // about a world that has moved on.
                size_t cursor=0;
                for (int s = 0; s < plan; ++s) {
                    const UnitType* t = nextType(roster,cursor);
                    if (!t) break;
                    float ux = x0 + float(s % cols) * spacing;
                    float uz = z0 + float(s / cols) * spacing;
                    if (!snapSpawn(t, ux, uz)) break;   // map full: stop, do not stack
                    benchPlan[size_t(s)].units.push_back({t, ux, uz, i});
                }
            }
        }
    }
    // Two executors: the stress fill by tick, the benchmark ramp by the server's BenchClock.
    if (stressStaged) world.setStagedSpawns(std::move(stressPlan));
    if (cfg.benchmark) world.setBenchmarkPlan(std::move(benchPlan));
    // Block the (structure) footprints just spawned. Monarchs move, so this is a
    // no-op today, but it mirrors the client and covers any non-mover spawns.
    for (auto& u : world.units()) {
        // isStructure (maxVel <= 0), not canMove: see the note in World::startBuild --
        // two of the four walls set canmove=1 and would otherwise block nothing.
        if (!u.type || !u.type->isStructure()) continue;
        world.blockFoot(*u.type, u.x.toFloat(), u.z.toFloat(), true);
    }
    // Initialization rules must see the default units too.
    if (pendingScenario) world.setScenario(std::move(pendingScenario));
    prebuildReachability(world, reg);
    return assigned;
}

// Total Annihilation campaign mission (docs/retail-campaign.md sections 2, 5.3):
// maps/<file>.ota + .tnt, the [Schema N] picked by difficulty with retail's
// fallback order, its [units] placed fully built (Player=1 the human, index 0;
// Player=2 the computer, index 1, the only "enemy"), HumanMetal/ComputerMetal...
// as start resources, and the GlobalHeader conditions run by MissionScript's
// TA rules (once a second, the 5 s end countdown, default DestroyAllUnits).
// `spec` is "<missionfile>[:<difficulty 0..2>]".
static bool setupTaMission(World& world, const TypeRegistry& reg, const hpi::Vfs& vfs,
                           const std::string& spec, int& humanOut, MissionSetup* out) {
    std::string file = spec;
    int difficulty = 1;
    if (const auto colon = spec.rfind(':'); colon != std::string::npos) {
        difficulty = std::clamp(std::atoi(spec.c_str() + colon + 1), 0, 2);
        file = spec.substr(0, colon);
    }
    std::string stem = file;
    if (stem.size() > 4 && vpath::extension(stem) == ".ota") stem.resize(stem.size() - 4);
    const std::string base = "maps/" + stem;
    if (!vfs.has(base + ".ota") || !vfs.has(base + ".tnt")) {
        std::fprintf(stderr, "setupTaMission: '%s' not found\n", base.c_str());
        return false;
    }
    const auto otaBytes = vfs.read(base + ".ota");
    const tdf::Node root = tdf::parseText(std::string(otaBytes.begin(), otaBytes.end()), base + ".ota");
    const tdf::Node* gh = root.child("globalheader");
    if (!gh) return false;
    // 0x436c1c: the requested difficulty's schema, else the fallback order.
    static const char* order[3][3] = {{"easy", "medium", "hard"}, {"medium", "easy", "hard"},
                                      {"hard", "medium", "easy"}};
    const tdf::Node* schema = nullptr;
    for (const char* want : order[difficulty]) {
        for (int i = 0; i < 16 && !schema; ++i)
            if (const tdf::Node* sc = gh->child("schema " + std::to_string(i))) {
                std::string type = sc->valueOr("type", "");
                std::transform(type.begin(), type.end(), type.begin(), ::tolower);
                if (type == want) schema = sc;
            }
        if (schema) break;
    }
    if (!schema) { std::fprintf(stderr, "setupTaMission: %s has no schema\n", base.c_str()); return false; }
    world.setBallisticGravityRaw(retailBallisticGravityRaw(gh->numberOr("gravity", 112.0)));
    world.setMapGravity(int32_t(gh->numberOr("gravity", 112.0)));
    world.setPowerCodes(false);
    // TotalA.exe 0x4363d9 / 0x497468: mapping=1 is the shroud ON, lineofsight=0 LOS off.
    world.setRoomVision(gh->numberOr("mapping", 1) == 0, gh->numberOr("lineofsight", 1) == 0);
    const tdf::Node* units = schema->child("units");
    // A side per player from its placed units' name prefixes (ARM / COR).
    int sideOf[2] = {0, sides::count() > 1 ? 1 : 0};
    if (units) {
        int votes[2][8] = {};
        for (const auto& key : units->childOrder) {
            const tdf::Node& u = units->children.at(key);
            int p = int(u.numberOr("player", 1)); if (p == 0) p = 1;
            if (p < 1 || p > 2) continue;
            std::string name = u.valueOr("unitname", "");
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            for (int s = 0; s < std::min(8, sides::count()); ++s)
                if (name.rfind(sides::lowerPrefix(s), 0) == 0) ++votes[p - 1][s];
        }
        for (int p = 0; p < 2; ++p) {
            int best = -1;
            for (int s = 0; s < std::min(8, sides::count()); ++s)
                if (votes[p][s] > 0 && (best < 0 || votes[p][s] > votes[p][best])) best = s;
            if (best >= 0) sideOf[p] = best;
        }
    }
    std::vector<MatchSlot> slots(2);
    slots[0].faction = sideOf[0]; slots[0].team = 0;
    slots[1].faction = sideOf[1]; slots[1].team = 1; slots[1].automaticGates = true;
    MatchConfig cfg;
    cfg.vfs = &vfs;
    cfg.mapPath = base + ".tnt";
    cfg.pathfindingMode = PathfindingMode::Retail;
    cfg.loadCrt = false;
    cfg.slots = slots;
    cfg.unitCap = int(gh->numberOr("maxunits", 200));
    cfg.trueLos = true;   // TotalA.exe 0x4363d9: a campaign plays True line of sight
    setupMatch(world, reg, cfg);
    world.setSurfaceMetal(int(schema->numberOr("surfacemetal", 0)));
    setupMeteorStorm(world, reg, vfs, schema);
    world.setStartResources(0, float(schema->numberOr("humanenergy", 1000)), float(schema->numberOr("humanmetal", 1000)));
    world.setStartResources(1, float(schema->numberOr("computerenergy", 1000)), float(schema->numberOr("computermetal", 1000)));
    if (gh->numberOr("waterdoesdamage", 0) != 0) world.setWaterDamage(int32_t(gh->numberOr("waterdamage", 0)));
    // The computer's income by difficulty, as in a skirmish: TotalA.exe scales every
    // resource credit to a type-2 player's units Easy x0.5, Medium x0.7, Hard x1.0
    // (constants 0x4fc480/0x4fc488, production loop 0x401360; docs/retail-ai.md
    // section 2). The difficulty is in the spec every peer sets up from.
    static constexpr float kIncome[3] = {0.5f, 0.7f, 1.0f};
    world.player(1).books.mult = kIncome[difficulty];
    // UseOnlyUnits (TotalA.exe 0x431740 from game start 0x4974de; docs/retail-campaign.md
    // section 2.5): camps\useonly\<file>.TDF clears the "available" type flag on every
    // type, then sets it again for each top-level section naming a type. One global
    // list, so it binds the human and the computer alike. No key or no file: no limit.
    int useOnly = -1;   // buildable types, for the log; -1: no list
    if (const std::string file = gh->valueOr("useonlyunits", ""); !file.empty()) {
        std::string path = "camps/useonly/" + file;
        if (vpath::extension(path) != ".tdf") path += ".tdf";
        if (vfs.has(path)) {
            const auto data = vfs.read(path);
            const auto list = tdf::parseText(std::string(data.begin(), data.end()), path);
            std::vector<std::string> allowed;
            for (const auto& name : list.childOrder)
                if (reg.find(name)) allowed.push_back(name);
            useOnly = int(allowed.size());
            world.setBuildRestrictions(std::move(allowed));
        }
    }

    int spawned = 0;
    std::vector<std::pair<int, std::string>> initialOrders;
    std::vector<MissionScript::Placement> placements;
    if (units)
        for (const auto& key : units->childOrder) {
            const tdf::Node& u = units->children.at(key);
            std::string name = u.valueOr("unitname", "");
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            placements.push_back({u.valueOr("ident", ""), name, 0});
            const UnitType* t = reg.find(name);
            if (!t) continue;   // 0x488a50: unknown type, slot stays null
            int p = int(u.numberOr("player", 1)); if (p == 0) p = 1;
            const int player = std::clamp(p - 1, 0, 1);
            // 0x436c30: XPos/ZPos are pixels; the unit is created fully built there.
            const float x = float(u.numberOr("xpos", 0)), z = float(u.numberOr("zpos", 0));
            const int id = world.spawn(t, x, z, std::nullopt, player);
            if (id < 0) continue;
            ++spawned;
            placements.back().unitId = id;
            if (auto* su = world.unit(id)) {
                // Angle: degrees * 65536 / 360 (0x436c30).
                const uint16_t angle = uint16_t(int32_t(u.numberOr("angle", 0) * 65536.0 / 360.0));
                su->heading = retailHeadingToPort(angle);
                su->tickStartHeadingBam = uint16_t(su->heading.v);
                if (t->canFly) su->flightNavigation.heading = angle;
                const int16_t health = int16_t(u.numberOr("healthpercentage", 100));
                su->hp = Fixed::fromInt(MissionScript::percentHp(*t, health));
                world.finishAsFeature(id);
            }
            if (const std::string* im = u.value("initialmission"))
                if (!im->empty()) initialOrders.push_back({id, *im});
        }
    if (out) {
        out->aiSlots = {1};
        out->difficulty = difficulty;
        out->aiProfile = schema->valueOr("aiprofile", "");
        std::transform(out->aiProfile.begin(), out->aiProfile.end(), out->aiProfile.begin(), ::tolower);
        out->playerDefs = {"", "strategic opponent"};
        out->fullVision = gh->numberOr("lineofsight", 1) == 0;   // LOS off (Permanent)
        out->preMapped = gh->numberOr("mapping", 1) == 0;        // the shroud off: map revealed
        out->slotPos.assign(size_t(kMaxPlayers), {0.0f, 0.0f});
        std::vector<int> n(size_t(kMaxPlayers), 0);
        for (const auto& u : world.units()) {
            if (!u.alive() || !u.type || u.player < 0 || u.player >= kMaxPlayers) continue;
            out->slotPos[size_t(u.player)].first += u.x.toFloat();
            out->slotPos[size_t(u.player)].second += u.z.toFloat();
            ++n[size_t(u.player)];
        }
        for (size_t i = 0; i < out->slotPos.size(); ++i)
            if (n[i]) { out->slotPos[i].first /= float(n[i]); out->slotPos[i].second /= float(n[i]); }
    }
    auto ms = std::make_unique<MissionScript>(std::vector<uint8_t>{}, *gh, reg, 0, stem,
                                              std::vector<int>{-1, 0, 1}, /*taRules=*/true);
    ms->setPlayerCommanders({reg.find(sides::commander(sideOf[0])), reg.find(sides::commander(sideOf[1]))});
    ms->setInitialOrders(std::move(initialOrders));
    ms->setPlacements(std::move(placements));
    if (spawned == 0) ms->disableConditions();   // 0x488541: no [units], no outcome
    world.setMission(std::move(ms));
    prebuildReachability(world, reg);
    humanOut = 0;
    std::fprintf(stderr, "setupTaMission: %s (%s) -- %d placed units, %s %d types\n", stem.c_str(),
                 order[difficulty][0], spawned, useOnly < 0 ? "no UseOnly list," : "UseOnly", std::max(useOnly, 0));
    return true;
}

static std::string taMissionStem(const std::string& spec) {
    std::string file = spec.substr(0, spec.rfind(':'));
    if (vpath::extension(file) == ".ota") file.resize(file.size() - 4);
    return file;
}

bool isTaMission(const hpi::Vfs& vfs, const std::string& spec) {
    const std::string file = taMissionStem(spec);
    return !vfs.has("missions/" + file + ".ota") && vfs.has("maps/" + file + ".ota");
}

std::string missionMapPath(const hpi::Vfs& vfs, const std::string& spec) {
    return isTaMission(vfs, spec) ? "maps/" + taMissionStem(spec) + ".tnt" : "missions/" + spec + ".tnt";
}

bool setupMission(World& world, const TypeRegistry& reg, const hpi::Vfs& vfs,
                  const std::string& stem, int& humanOut, MissionSetup* out) {
    // TA campaign missions live in maps/ (camps/*.tdf missionfile=); Kingdoms' in missions/.
    if (isTaMission(vfs, stem)) return setupTaMission(world, reg, vfs, stem, humanOut, out);
    const std::string base = "missions/" + stem;
    // The .cob is OPTIONAL: 11 of the 74 shipped missions (takmission05_dh --
    // chapter 5 of Book of Darien! -- 07/23/28/40_ph/dh and takx07/10/12/14/15/17)
    // are pure DATA missions with no god script at all; they run entirely on the
    // .ota win/lose conditions and the placed units' InitialMission order queues,
    // both of which we implement. Requiring a cob dead-ended the campaign at
    // chapter 5.
    if (!vfs.has(base + ".tnt") || !vfs.has(base + ".ota")) {
        std::fprintf(stderr, "setupMission: '%s' not found\n", stem.c_str());
        return false;
    }
    auto otaBytes = vfs.read(base + ".ota");
    tdf::Node root = tdf::parseText(std::string(otaBytes.begin(), otaBytes.end()), base + ".ota");
    const tdf::Node* gh = root.child("globalheader");
    if (!gh) { std::fprintf(stderr, "setupMission: %s no [GlobalHeader]\n", stem.c_str()); return false; }
    world.setBallisticGravityRaw(retailBallisticGravityRaw(gh->numberOr("gravity",112.0)));
    world.setMapGravity(int32_t(gh->numberOr("gravity",112.0)));
    world.setPowerCodes(false);   // TotalA.exe 0x497462: never in a campaign
    // The mission's own vision (TotalA.exe 0x4363d9 / 0x497468; docs/retail-fog.md
    // section 6): mapping=1 plays with the shroud on, 0 with the map revealed;
    // lineofsight=0 plays with LOS off (Permanent: explored cells stay in view).
    world.setRoomVision(gh->numberOr("mapping", 1) == 0, gh->numberOr("lineofsight", 1) == 0);
    world.setTrueLos(true);   // 0x4363d9: LOSType forced True

    // Map the .ota's sparse, 1-based Player<N> ids to compact 0-based World slots in
    // ascending N order, so gaps (e.g. Player1,2,9,10) don't collide when clamped into
    // kMaxPlayers slots. The SAME map applies to the placed units and to the script's
    // Create player refs (passed to MissionScript). Diplomacy from each player's role:
    // opponents fight the human (team 1); the human, allies and neutrals share team 0,
    // so the human never auto-attacks them.
    //
    // The human is ALWAYS Player1, world slot 0. Retail's campaign start (0x4d290c)
    // creates game player 0 as the local human from the .ota's first player record
    // and takes only its logo (0x4d292a); the computer players come from records
    // 2..10 (the 0x4d2957 loop starts at record 1). Player1's own keywords are never
    // read: takx22_mt and takx24_mt declare `Player1=strategic opponent ... creon`,
    // and that is the city the player defends (the briefing: hold off the Aramon
    // invaders), not a second enemy. The old "first non-AI player" guess found no
    // human there, put an AI on the player's own units, and teamed them with Aramon.
    std::vector<int> otaToWorld(17, -1);   // [.ota player 1..16] -> world slot
    std::vector<MatchSlot> slots;
    const int human = 0;
    uint32_t budgetClasses = 0, passiveSlots = 0;
    const char* kingdoms[5] = {"aramon", "taros", "veruna", "zhon", "creon"};
    for (int n = 1; n <= 16 && int(slots.size()) < kMaxPlayers; ++n) {
        const std::string* v = gh->value("player" + std::to_string(n));
        // Retail keeps record 0 for the human even when an .ota omits Player1.
        if (!v && n != 1) continue;
        std::string def = v ? *v : std::string();
        std::transform(def.begin(), def.end(), def.begin(), ::tolower);
        int slot = int(slots.size());
        otaToWorld[n] = slot;
        if (out) { out->playerDefs.resize(size_t(slot) + 1); out->playerDefs[size_t(slot)] = v ? *v : std::string(); }
        MatchSlot s;
        s.used = false;   // no monarch spawn -- units come from [Map Data][units]
        for (int f = 0; f < 5; ++f) if (def.find(kingdoms[f]) != std::string::npos) s.faction = f;
        if (slot == human) { slots.push_back(s); continue; }   // team 0, no AI, manual gates
        s.team = def.find("opponent") != std::string::npos ? 1 : 0;
        bool strategic = def.find("strategic") != std::string::npos;
        s.automaticGates=strategic;
        slots.push_back(s);
        // Only a STRATEGIC player gets a brain. A "passive neutral" is scenery --
        // villagers, wildlife, props -- and must stay inert.
        if (strategic && out) out->aiSlots.push_back(slot);
        if (strategic && def.find("passive") == std::string::npos) budgetClasses |= 1u << slot;
        else passiveSlots |= 1u << slot;   // shares no sight (Player::passiveNeutral)
    }
    // Retail's 5x path-budget class: a mission's computer players carry it only
    // when "strategic" (0x4c83fb; a later "passive" match clears it again at
    // 0x4c8419) and 0x4d2a12 stores it; the human keeps 0x4f6379's 1.
    budgetClasses |= 1u << human;
    auto mapPlayer = [&](int otaP) {
        if (otaP >= 1 && otaP <= 16 && otaToWorld[size_t(otaP)] >= 0) return otaToWorld[size_t(otaP)];
        return std::clamp(otaP - 1, 0, int(slots.size()) - 1);
    };

    MatchConfig cfg;
    cfg.vfs = &vfs;
    cfg.mapPath = base + ".tnt";
    cfg.pathfindingMode = PathfindingMode::Retail; // campaign mission movement is fixed
    cfg.loadCrt = false;              // campaign units/rules come from OTA/COB
    cfg.slots = slots;                 // no used slots -> setupMatch spawns no monarchs
    cfg.unitCap = int(gh->numberOr("maxunits", 500));
    setupMatch(world, reg, cfg);       // terrain + features + player teams
    world.setPathBudgetClasses(budgetClasses);
    for (int p = 0; p < int(slots.size()); ++p) world.player(p).passiveNeutral = (passiveSlots >> p & 1) != 0;
    if (gh->numberOr("waterdoesdamage", 0) != 0)
        world.setWaterDamage(int32_t(gh->numberOr("waterdamage", 0)));

    // Placed units from [Map Data][units].
    int spawned = 0;
    std::vector<std::pair<int, std::string>> initialOrders;   // (unit id, InitialMission)
    // Every [units] record in file order with the unit it made (0: none), for
    // the order clauses that name a placed unit (Ident=, else Unitname).
    std::vector<MissionScript::Placement> placements;
    if (const tdf::Node* md = gh->child("map data"))
        if (const tdf::Node* units = md->child("units"))
            for (const auto& key : units->childOrder) {
                const tdf::Node& u = units->children.at(key);
                std::string name = u.valueOr("unitname", "");
                std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                placements.push_back({u.valueOr("ident", ""), name, 0});
                const UnitType* t = reg.find(name);
                if (!t) continue;
                // XPos/ZPos/Angle are read with atoi (4c8ce1); see placedPoint.
                auto integer = [&](const char* key) { return std::atoi(u.valueOr(key, "0").c_str()); };
                const MissionScript::Point at = MissionScript::placedPoint(*t, integer("xpos"), integer("zpos"));
                int player = mapPlayer(int(u.numberOr("player", 1)));
                const uint16_t angle = MissionScript::placedHeading(integer("angle"));
                int id = world.spawn(t, at.x.toFloat(), at.z.toFloat(), std::nullopt, player);
                if (id >= 0) {
                    ++spawned;
                    placements.back().unitId = id;
                    if (auto* su = world.unit(id)) {
                        // 514d02 overwrites the birth heading (+0x7e) with the record's.
                        su->heading = retailHeadingToPort(angle);
                        su->tickStartHeadingBam = uint16_t(su->heading.v);
                        if (t->canFly) su->flightNavigation.heading = angle;
                        // HealthPercentage (default 100) and ManaPercentage (default 0),
                        // read with atoi into words (4c8dc5..4c8dec), set at 514cb9 and
                        // 514cd3: a caster opens a mission with the mana the designer
                        // gave it (usually 0 -- it must recharge before casting).
                        const int16_t health = int16_t(std::atoi(u.valueOr("healthpercentage", "100").c_str()));
                        su->hp = Fixed::fromInt(MissionScript::percentHp(*t, health));
                        // Kills is not read: TotalA.exe's [units] parse (0x436c30)
                        // has no such key (the "Kills" strings' xrefs are the score
                        // and stats screens, 0x41e6de / 0x42003e / 0x4661f3), though
                        // 1100-odd shipped records say 1..66. A placed unit starts
                        // with 0 kills (docs/retail-campaign.md section 2.4).
                        world.finishAsFeature(id);   // 51230c: a placed wall becomes its feature
                    }
                    // InitialMission: the per-unit order queue in the SAME mini-language
                    // the god script's SetMission uses (move/patrol/attack/stance/wait/
                    // ambush/build/board). 4416 placed units across the shipped missions
                    // carry one -- it IS the NPC choreography (patrol routes, ambush
                    // holds, factory waves, troops aboard ships, the mission-9
                    // alchemist's scripted walk). Queued here and applied by the
                    // script once every placed unit exists, as retail's 0x514b40 does.
                    if (const std::string* im = u.value("initialmission"))
                        if (!im->empty()) initialOrders.push_back({id, *im});
                }
            }

    // Where each player's forces actually are, so an AI can march on a real base
    // instead of a start position a mission never declares. The centroid of a
    // slot's placed units is the closest thing a mission has to a "start".
    if (out) {
        // Fog, as TotalA.exe reads the header (docs/retail-fog.md section 6):
        // mapping=1 is the shroud on (most missions), 0 the map revealed;
        // lineofsight=0 is LOS off.
        out->fullVision = gh->numberOr("lineofsight", 1) == 0;   // LOS off (Permanent)
        out->preMapped = gh->numberOr("mapping", 1) == 0;        // the shroud off: map revealed
        if (const tdf::Node* md = gh->child("map data"))
            {
                out->aiProfile = md->valueOr("aiprofile", "");
                std::transform(out->aiProfile.begin(), out->aiProfile.end(),
                               out->aiProfile.begin(), ::tolower);
            }
        if (out->aiProfile == "default") out->aiProfile.clear();
        out->slotPos.assign(size_t(kMaxPlayers), {0.0f, 0.0f});
        std::vector<int> n(size_t(kMaxPlayers), 0);
        for (const auto& u : world.units()) {
            if (!u.alive() || !u.type) continue;
            if (u.player < 0 || u.player >= kMaxPlayers) continue;
            out->slotPos[size_t(u.player)].first += u.x.toFloat();
            out->slotPos[size_t(u.player)].second += u.z.toFloat();
            ++n[size_t(u.player)];
        }
        for (size_t i = 0; i < out->slotPos.size(); ++i)
            if (n[i] > 0) {
                out->slotPos[i].first /= float(n[i]);
                out->slotPos[i].second /= float(n[i]);
            }
    }

    // Attach + start the in-sim "god" script (its spawns/triggers run from World::tick).
    // A data-only mission has no cob: MissionScript still runs the .ota conditions and
    // applies the InitialMission queues, it just has no bytecode VM.
    std::vector<uint8_t> cobBytes;
    if (vfs.has(base + ".cob")) cobBytes = vfs.read(base + ".cob");
    auto ms = std::make_unique<MissionScript>(std::move(cobBytes), *gh, reg, human, stem, otaToWorld);
    // Native commander conditions compare the unit name with the owning
    // player's authored sidedata commander (ZONHURT is not ZONHUNT).
    std::vector<const UnitType*> commanders(slots.size(),nullptr);
    if(vfs.has("gamedata/sidedata.tdf")) {
        const auto bytes=vfs.read("gamedata/sidedata.tdf");
        const auto sides=tdf::parseText(std::string(bytes.begin(),bytes.end()),"gamedata/sidedata.tdf");
        for(size_t i=0;i<slots.size();++i)
            for(const auto& [key,side]:sides.children) {
                auto name=side.valueOr("name","");
                std::transform(name.begin(),name.end(),name.begin(),::tolower);
                if(slots[i].faction>=0 && slots[i].faction<5 && name==kingdoms[slots[i].faction])
                    commanders[i]=reg.find(side.valueOr("commander",""));
            }
    }
    ms->setPlayerCommanders(std::move(commanders));
    ms->setInitialOrders(std::move(initialOrders));
    ms->setPlacements(std::move(placements));
    world.setMission(std::move(ms));
    prebuildReachability(world, reg);   // the placed units' classes (setupMatch saw none of them)
    humanOut = human;
    std::fprintf(stderr, "setupMission: %s -- %d placed units, human=player%d\n",
                 stem.c_str(), spawned, human);
    return true;
}

}  // namespace ta::sim
