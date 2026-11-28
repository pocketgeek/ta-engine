#include "client/gameview.h"
#include "client/clickrules.h"
#include "client/cursorrange.h"
#include "client/friendlyorder.h"
#include "sim/groupspots.h"
#include "sim/retailheight.h"

#include <unordered_map>

// What a click orders (client/clickrules.h, PLAN 2.1-2.11): the per-unit facts the
// pure rules read, the commands they become, the boxes, and the minimap's orders.
// Every decision here is the one the cursor shows (desiredCursor reads planClick
// and planBox), so the cursor describes the click.

namespace {
    using ta::client::ClickOrder;

    // A frame position as the sim's raw 16.16 (the frame holds Fixed::toFloat).
    int32_t flyerRaw(float v) { return ta::sim::Fixed::fromFloat(v).v; }

    int32_t unloadDestinationHeight(const ta::tnt::Map& map,float x,float z) {
        const int height=map.heights.empty() ? 0 : ta::sim::retailTerrainHeight(
            ta::sim::Fixed::fromFloat(x).v,ta::sim::Fixed::fromFloat(z).v,
            map.width,map.height,[&](int cx,int cz) {
                return map.heights[size_t(cz)*size_t(map.width)+size_t(cx)];
            });
        return std::max(height,int(uint8_t(map.seaLevel)))*65536;
    }

    // World::reclaim's caster test, for one of the local player's units.
    bool canReclaimAsLocal(const UnitR& u, int localPlayer) {
        return u.alive() && u.type && u.type->isBuilder && u.type->canMove &&
               u.type->canReclaim && u.player == localPlayer;
    }
}

    uint8_t GameView::keyQueue() {
        const SDL_Keymod m = SDL_GetModState();
        return ta::client::clickQueue((m & KMOD_SHIFT) != 0, (m & KMOD_CTRL) != 0);
    }

    bool GameView::shiftHeld() { return (SDL_GetModState() & KMOD_SHIFT) != 0; }

    std::vector<const UnitR*> GameView::ownClickUnits() const {
        std::vector<const UnitR*> out;
        out.reserve(selection_.size());
        for (int id : selection_) {
            const UnitR* u = frameUnitP(id);
            if (u && u->alive() && u->type && u->player == localPlayer_ && !u->embarked() && !u->underConstruction)
                out.push_back(u);
        }
        return out;
    }

    int GameView::ackUnit(const std::vector<int>& ids) {
        if (ids.empty()) return 0;
        ackRng_ = ackRng_ * 1664525u + 1013904223u;
        return ids[size_t(ackRng_ >> 8) % ids.size()];
    }

    GameView::ClickPlan GameView::planClick(char mode, float wx, float wz, const UnitR* hovered, uint8_t queue,
                                            bool alt, bool pointTargets) {
        using namespace ta::client;
        ClickPlan plan;
        plan.wx = wx; plan.wz = wz;
        const auto units = ownClickUnits();
        if (units.empty()) return plan;
        ClickTarget target;
        target.kind = cursorHoverKind(hovered);
        target.flies = hovered && hovered->type && hovered->type->canFly;
        plan.target = hovered ? hovered->id : 0;
        const bool friendlyTarget = target.kind == HoverKind::OwnSelectable || target.kind == HoverKind::Friendly;
        // Bodies and features at the point, in the main view: armed CLEAR acts on the
        // point whatever stands there; a right-click only with no unit hovered.
        if (pointTargets && (mode == 'c' || (mode == 0 && !hovered))) {
            plan.body = raiseTargetAt(wx, wz);
            if (haveReclaimer()) plan.reclaim = reclaimTargetAt(wx, wz);
            // No body: a wreck or heap FEATURE a selected caster can raise
            // (Cmd::Resurrect's target is then -featureId, World::reviveKind).
            if (!plan.body && plan.reclaim && *plan.reclaim > 0)
                for (int id : selection_)
                    if (const auto* caster = frameUnitP(id); caster && reviveKindOnFeature(*caster, *plan.reclaim)) {
                        plan.body = -*plan.reclaim;
                        break;
                    }
        }
        const UnitR* body = plan.body > 0 ? frameUnitP(plan.body) : nullptr;
        // The selection's transports, the hovered unit left out (0x520b60).
        int transports = 0;
        for (const UnitR* u : units)
            if (u->type->canTransport && u != hovered) ++transports;
        const bool damaged = hovered && hovered->type && !hovered->underConstruction &&
                             hovered->hp < hovered->type->maxHp;

        ClickUnit f;
        ArmedSelection sel;
        sel.singleTransport = transports == 1;
        sel.queue = ta::client::queuesBehind(queue);
        const auto facts = [&](const UnitR& u) {
            f = ClickUnit{};
            f.self = hovered && u.id == hovered->id;
            if (f.self) return;
            const auto& t = *u.type;
            f.mobile = !t.isStructure();
            f.rallyHolder = u.holdsRally();
            f.attacker = isCursorAttacker(u);
            f.canPatrol = t.canPatrol;
            f.canGuard = t.canGuard;
            f.flies = t.canFly;
            f.transport = t.canTransport;
            f.loaded = t.canTransport && !u.cargo.empty();
            if (f.attacker) f.airstrike = unitHasAirstrikeCursor(t, 0);
            // Armed ATTACK at a point (ground or a friendly unit): the sim's own
            // resolver rule (World::suppressRule).
            if (mode == 'a' && f.attacker && target.kind != HoverKind::Enemy) {
                using Rule = ta::sim::World::SuppressRule;
                const Rule r = ta::sim::World::suppressRule(t, 0);
                f.pointFire = r == Rule::Fire;
            }
            if (hovered && hovered->type) {
                if (f.attacker && target.kind == HoverKind::Enemy && (mode == 0 || mode == 'a'))
                    f.canHit = cursorCanAttackEnemy(t, 0, u.worldPosition, *hovered->type,
                                                    hovered->worldPosition, hovered->flightGroundMode);
                if (friendlyTarget && (mode == 0 || mode == 'm' || mode == 'f' || mode == 'r')) {
                    f.assist = canAssistSite(u, *hovered);
                    f.pickup = f.transport && !f.attacker && transports == 1 && canLoadPassenger(*hovered, u);
                    f.friendly = friendlyUnitOrder(t, true, u.repeatType != nullptr, false, damaged);
                    f.land = mode == 0 && t.canFly && hovered->type->isAirBase;
                }
                if (mode == 'l' && f.transport) f.loadTarget = canLoadPassenger(*hovered, u);
            }
            if (body) f.raise = reviveKindOn(u, *body) != 0;
            else if (plan.body < 0) f.raise = reviveKindOnFeature(u, -plan.body) != 0;
            if (plan.reclaim) f.reclaim = canReclaimAsLocal(u, localPlayer_);
            // RECLAIMUNIT: the armed reclaim on any unit, a right-click on an
            // enemy (clickrules.h; World::reclaimUnit's caster test).
            if (hovered && hovered->type && hovered->alive() && (mode == 'c' || mode == 0))
                f.reclaimUnit = canReclaimAsLocal(u, localPlayer_);
        };
        if (mode == 'g' && friendlyTarget)
            for (const UnitR* u : units)
                if (u != hovered && u->type->canGuard && (u->type->canFly || !target.flies)) { sel.guardOk = true; break; }
        for (const UnitR* u : units) {
            facts(*u);
            ClickOrder o;
            if (mode == 0) o = rightClick(target, f, alt);
            else {
                const UnitClick c = armedClick(mode, target, f, sel);
                plan.cursor = lowerCursorSlot(plan.cursor, c.cursor);
                o = c.order;
            }
            if (o != ClickOrder::None) plan.orders.push_back({u->id, o});
        }
        return plan;
    }

    void GameView::issueClickPlan(const ClickPlan& plan, char mode, uint8_t queue) {
        ClickScope click(*this);   // one click: a Legion net game sends it as explicit orders
        if (plan.orders.empty()) return;
        // One run per order kind, in selection order within it, so a Legion net game
        // packs each kind as one explicit order (net/orderpack.h).
        static constexpr ClickOrder kRuns[] = {
            ClickOrder::Attack, ClickOrder::AttackGround, ClickOrder::Assist, ClickOrder::Repair, ClickOrder::Land,
            ClickOrder::Pickup, ClickOrder::Guard, ClickOrder::Resurrect, ClickOrder::Reclaim, ClickOrder::ReclaimUnit,
            ClickOrder::Unload,
            ClickOrder::AttackMove, ClickOrder::Patrol, ClickOrder::Move,
        };
        std::vector<int> all, ids;
        bool attacked = false;
        for (const ClickOrder kind : kRuns) {
            ids.clear();
            for (const auto& [id, o] : plan.orders) if (o == kind) ids.push_back(id);
            if (ids.empty()) continue;
            all.insert(all.end(), ids.begin(), ids.end());
            // A fight-move's AttackMove is a resolved Move_Ground / VTOL_Move /
            // QMove, so it voices that mission's sound, as armed MOVE does.
            attacked |= kind == ClickOrder::Attack || kind == ClickOrder::AttackGround ||
                        (kind == ClickOrder::AttackMove && mode != 'f');
            const auto send = [&](ta::net::Cmd k, int unit, int targetId) {
                ta::net::Command c;
                c.kind = k; c.unitId = unit; c.targetId = targetId; c.queue = queue;
                issue(c);
            };
            switch (kind) {
                case ClickOrder::Attack: for (int id : ids) send(ta::net::Cmd::Attack, id, plan.target); break;
                // At the click point, over ground or a friendly unit alike (4de530
                // kind 3 takes the pointer's point, not the unit's).
                case ClickOrder::AttackGround:
                    for (int id : ids) {
                        ta::net::Command c;
                        c.kind = ta::net::Cmd::AttackGround;
                        c.unitId = id; c.x = plan.wx; c.z = plan.wz; c.queue = queue;
                        issue(c);
                    }
                    break;
                case ClickOrder::Assist: for (int id : ids) send(ta::net::Cmd::Assist, id, plan.target); break;
                case ClickOrder::Repair: for (int id : ids) send(ta::net::Cmd::Repair, id, plan.target); break;
                case ClickOrder::Guard:  for (int id : ids) send(ta::net::Cmd::Guard, id, plan.target); break;
                case ClickOrder::Land:   for (int id : ids) send(ta::net::Cmd::Land, id, plan.target); break;
                // The transport picks the hovered unit up (its LOAD order).
                case ClickOrder::Pickup: for (int id : ids) send(ta::net::Cmd::Load, plan.target, id); break;
                case ClickOrder::Resurrect: for (int id : ids) send(ta::net::Cmd::Resurrect, id, plan.body); break;
                case ClickOrder::Reclaim:
                    if (plan.reclaim) for (int id : ids) send(ta::net::Cmd::Reclaim, id, *plan.reclaim);
                    break;
                case ClickOrder::ReclaimUnit:
                    for (int id : ids) send(ta::net::Cmd::ReclaimUnit, id, plan.target);
                    break;
                case ClickOrder::Unload:
                    for (int id : ids) {
                        ta::net::Command c;
                        c.kind = ta::net::Cmd::Unload;
                        c.targetId = unloadDestinationHeight(mapView_.map(), plan.wx, plan.wz);
                        c.unitId = id; c.x = plan.wx; c.z = plan.wz; c.queue = queue;
                        issue(c);
                    }
                    break;
                case ClickOrder::AttackMove: case ClickOrder::Patrol: case ClickOrder::Move: {
                    const auto cmd = kind == ClickOrder::AttackMove ? ta::net::Cmd::AttackMove
                                   : kind == ClickOrder::Patrol ? ta::net::Cmd::Patrol : ta::net::Cmd::Move;
                    // In a Legion net game the run goes as ONE explicit order with the
                    // click in its template (OrderShape::Offsets for the right-click
                    // move, FlyerOffsets for an armed move, patrol or fight-move); the
                    // sim gives the members the same goals as below, from their
                    // positions at activation (sim/matchsetup.cpp).
                    if (mode == 0 && kind == ClickOrder::Move) setClickMove(plan.wx, plan.wz);
                    else setFlyerSpread(plan.wx, plan.wz);
                    // Every unit gets its OWN spot (sim/groupspots.h; an engine feature
                    // in both modes, docs/ta-port-plan.md "Known deviations"): within
                    // TotalA.exe's reach (0x48cf30, sqrt(3000 n) px of the centre) the
                    // click plus its offset, else the nearest free footprint spot to
                    // the click -- where retail sends the stragglers to the click
                    // itself. Legion places its own surface movers on the shared
                    // point (legionSharedClick), so they keep the click. Members in
                    // increasing id order, as the sim applies an explicit order's.
                    const bool legion = ta::sim::isLegionPathfinding(worldTables().pathfindingMode());
                    std::vector<int> members;
                    for (int id : ids) if (frameUnitP(id)) members.push_back(id);
                    std::sort(members.begin(), members.end());
                    std::vector<ta::sim::GroupSpotMember> spotMembers;
                    for (int id : members) {
                        const auto* u = frameUnitP(id);
                        const bool legionPlans = legion && u->type &&
                            ta::sim::legionSharedClick(u->type->canFly, u->type->footX, u->type->footZ);
                        spotMembers.push_back({flyerRaw(u->x), flyerRaw(u->z), u->type ? u->type->footX : 1,
                                               u->type ? u->type->footZ : 1,
                                               u->type && !legionPlans && !u->type->isStructure(), legionPlans});
                    }
                    const auto& map = mapView_.map();
                    const auto spots = ta::sim::groupSpots(spotMembers, flyerRaw(plan.wx), flyerRaw(plan.wz),
                                                           map.width * 16, map.height * 16);
                    std::unordered_map<int, size_t> slot;
                    for (size_t i = 0; i < members.size(); ++i) slot[members[i]] = i;
                    for (int id : ids) {
                        const auto it = slot.find(id);
                        if (it == slot.end()) continue;
                        ta::net::Command c;
                        c.kind = cmd;
                        c.unitId = id;
                        c.x = plan.wx; c.z = plan.wz;
                        if (spotMembers[it->second].place) {
                            c.x = ta::sim::Fixed::raw(spots[it->second][0]).toFloat();
                            c.z = ta::sim::Fixed::raw(spots[it->second][1]).toFloat();
                        }
                        c.queue = queue;
                        issue(c);
                    }
                    break;
                }
                default: break;
            }
        }
        // Retail voices the mission a unit resolved to (0x5217a5 S: 0x50aa30 with
        // the mission's +0x10 byte); the 0x400 bit changes only the response mode,
        // so a fight-move voices exactly as armed MOVE does, never an attack.
        const char* event = attacked || mode == 'a' ? "attack"
                          : mode == 'p' ? "patrol" : mode == 'g' ? "guard" : "move";
        voice(ackUnit(all), event);
    }

    void GameView::rightClickOrder(float wx, float wz, uint8_t queue) {
        ClickScope click(*this);   // one click: a Legion net game sends it as explicit orders
        if (selection_.empty()) return;
        const UnitR* hovered = hoveredUnitAt(ta::client::PointerArea::World, mouseX_, mouseY_);
        const bool alt = (SDL_GetModState() & KMOD_ALT) != 0;
        issueClickPlan(planClick(0, wx, wz, hovered, queue, alt, true), 0, queue);
    }

    bool GameView::armedClickAt(char cmd, float wx, float wz, const UnitR* hovered, uint8_t queue, bool pointTargets) {
        ClickScope click(*this);
        if (cmd == 'd') {
            // TA's D-gun (the Commander's BLAST button; AttackSpecial 0x403190):
            // a unit target only, one commandfire shot per order.
            if (!hovered || !hovered->alive()) return false;
            for (int id : selection_) {
                const UnitR* u = frameUnitP(id);
                if (!u || !u->type || !u->type->canDgun || id == hovered->id) continue;
                ta::net::Command c;
                c.kind = ta::net::Cmd::DGun;
                c.unitId = id;
                c.targetId = hovered->id;
                c.queue = queue & 3;
                issue(c);
            }
            pendingCmd_ = 0;
            return true;
        }
        const ClickPlan plan = planClick(cmd, wx, wz, hovered, queue, false, pointTargets);
        // P4 (0x527dd0 S): only an order cursor issues; Normal leaves it armed.
        if (!ta::client::isOrderCursor(plan.cursor)) return false;
        issueClickPlan(plan, cmd, queue);
        return true;
    }

    void GameView::issueArmedOrder(char cmd, float wx, float wz, uint8_t queue, bool precise) {
        ClickScope click(*this);
        if (selection_.empty()) return;
        const UnitR* hovered = precise ? hoveredUnitAt(ta::client::PointerArea::World, mouseX_, mouseY_)
                             : pointerArea() == ta::client::PointerArea::Minimap
                                 ? hoveredUnit(ta::client::PointerArea::Minimap) : nullptr;
        issueClickPlan(planClick(cmd, wx, wz, hovered, queue, false, true), cmd, queue);
    }

    bool GameView::minimapOrder(float mx, float my, int winW, int winH, uint8_t queue) {
        float wx, wz;
        if (watchOnly() || selection_.empty() || !minimapToWorld(mx, my, winW, winH, wx, wz)) return false;
        ClickScope click(*this);
        const UnitR* dot = hoveredUnitAt(ta::client::PointerArea::Minimap, mx, my);
        const bool alt = (SDL_GetModState() & KMOD_ALT) != 0;
        issueClickPlan(planClick(0, wx, wz, dot, queue, alt, false), 0, queue);
        return true;
    }

    bool GameView::minimapArmedOrder(float mx, float my, int winW, int winH) {
        if (watchOnly() || !pendingCmd_) return false;
        float wx, wz;
        if (!minimapToWorld(mx, my, winW, winH, wx, wz)) return false;
        // The armed order with the dot as its target, gated by the cursor (P4); a
        // Normal cursor leaves the order armed, and the press is still taken.
        if (armedClickAt(pendingCmd_, wx, wz, hoveredUnitAt(ta::client::PointerArea::Minimap, mx, my), keyQueue(), false))
            armedOrderIssued(shiftHeld());
        return true;
    }

    bool GameView::minimapPlaceOk(float wx, float wz) {
        if (!placing_ || selection_.empty()) return false;
        return canPlacePreview(placing_, wx, wz) || placePreviewClearable(placing_, wx, wz);
    }

    void GameView::playGuiSound(const std::string& entry, const char* where, const std::string& gadget) {
        if (entry.empty()) return;
        if (!guiSounds_.loaded()) guiSounds_.load(vfs_);
        const std::string wav = guiSounds_.wav(entry);   // 0x47f1a0: an unknown entry plays nothing
        lastGuiSound_ = entry;
        lastGuiWav_ = wav;
        // The SoundBank indexes sounds/ by stem; played non-positional, at UI priority.
        const std::string stem = wav.size() > 4 ? wav.substr(0, wav.size() - 4) : std::string();
        const bool known = !stem.empty() && sounds_.has(stem);
        ta::guisound::log(where, gadget, entry, wav, known, known);
        if (!known) return;
        sounds_.play(stem, 1.f, 1.f, 7, false);
    }

    // The map's per-cell metal (World::cellMetal, fixed once the match is set up),
    // re-read when the map changes; the spots are found again from it.
    void GameView::loadMetalCells() {
        const auto& w = worldTables();
        const int cw = mapView_.map().blocksX * 2, ch = mapView_.map().blocksY * 2;
        if (cw == metalCellsW_ && ch == metalCellsH_ && w.surfaceMetal() == metalCellsSurface_) return;
        metalCellsW_ = cw; metalCellsH_ = ch; metalCellsSurface_ = w.surfaceMetal();
        metalCellBuf_.assign(size_t(std::max(0, cw)) * size_t(std::max(0, ch)), 0);
        for (int z = 0; z < ch; ++z)
            for (int x = 0; x < cw; ++x) metalCellBuf_[size_t(z) * size_t(cw) + size_t(x)] = uint8_t(w.cellMetal(x, z));
        metalSpots_.clear();
        metalPlanet_ = ta::client::metalspots::metalPlanet({metalCellBuf_.data(), cw, ch, metalCellsSurface_});
    }

    bool GameView::snapsToMetal(const ta::sim::UnitType* t) {
        if (!t || t->extractsMetal <= 0) return false;
        loadMetalCells();
        return !metalPlanet_;
    }

    const std::vector<ta::client::metalspots::Spot>& GameView::metalSpots(const ta::sim::UnitType* t) {
        static const std::vector<ta::client::metalspots::Spot> kNone;
        if (!snapsToMetal(t)) return kNone;
        const int key = (std::max(1, t->footX) << 8) | std::max(1, t->footZ);
        auto it = metalSpots_.find(key);
        if (it == metalSpots_.end())
            it = metalSpots_.emplace(key, ta::client::metalspots::find(
                     {metalCellBuf_.data(), metalCellsW_, metalCellsH_, metalCellsSurface_},
                     std::max(1, t->footX), std::max(1, t->footZ))).first;
        return it->second;
    }

    bool GameView::snapToMetal(const ta::sim::UnitType* t, float& wx, float& wz) {
        if (!snapsToMetal(t)) return true;
        const auto* s = ta::client::metalspots::nearest(metalSpots(t), wx, wz);
        if (!s) return false;
        wx = s->x; wz = s->z;
        return true;
    }

    int GameView::upgradableExtractorAt(const ta::sim::UnitType* type, int builderId, float x, float z) const {
        if (!type || type->extractsMetal <= 0) return 0;
        const UnitR* b = frameUnitP(builderId);
        if (!b || !b->type || !b->type->canReclaim || !b->type->canMove || b->type->isStructure()) return 0;
        // Only where the ground takes the better one (its slope and water limits):
        // a deposit some extractors fit and it does not keeps its old extractor.
        if (!liveWorld()->yardTerrainFits(type, x, z)) return 0;
        const float hx = type->footX * 8.0f, hz = type->footZ * 8.0f;
        for (const UnitR* u : front().live) {
            if (!u->alive() || !u->type || u->player != b->player || u->underConstruction) continue;
            if (u->type->extractsMetal <= 0 || u->type->extractsMetal >= type->extractsMetal) continue;
            const float ux = u->type->footX * 8.0f, uz = u->type->footZ * 8.0f;
            if (std::abs(u->x - x) < hx + ux && std::abs(u->z - z) < hz + uz) return u->id;
        }
        return 0;
    }

    // Reclaim the weaker extractor, then build the better one in its place, as one
    // step of the queue: `queue` places the pair (front: the build goes in first, so
    // the reclaim ends up ahead of it).
    bool GameView::issueUpgradeBuild(int builderId, const ta::sim::UnitType* type, float x, float z, uint8_t queue) {
        const int old = upgradableExtractorAt(type, builderId, x, z);
        if (!old) return false;
        ta::net::Command r;
        r.kind = ta::net::Cmd::ReclaimUnit;
        r.unitId = builderId;
        r.targetId = old;
        ta::net::Command c;
        c.kind = ta::net::Cmd::Build;
        c.unitId = builderId;
        c.x = x; c.z = z;
        std::snprintf(c.type, sizeof c.type, "%s", type->id.c_str());
        if ((queue & 3) == ta::net::kQueueFront) {
            c.queue = ta::net::kQueueFront; issue(c);
            r.queue = ta::net::kQueueFront; issue(r);
        } else {
            r.queue = queue; issue(r);
            c.queue = ta::net::kQueueAppend; issue(c);
        }
        notice_ = "UPGRADING EXTRACTOR";
        noticeTimer_ = 2;
        return true;
    }

    bool GameView::placeMetalAt(float wx, float wz, uint8_t queue) {
        if (!placing_ || selection_.empty() || !snapToMetal(placing_, wx, wz)) return false;
        const int builder = selectedBuilder() ? selectedBuilder()->id : selection_.front();
        if (canPlaceLocked(placing_, wx, wz)) {
            ta::net::Command c;
            c.kind = ta::net::Cmd::Build;
            c.unitId = builder;
            c.x = wx; c.z = wz;
            c.queue = queue;
            std::snprintf(c.type, sizeof c.type, "%s", placing_->id.c_str());
            issue(c);
            return true;
        }
        // A weaker extractor of ours first: the deposit (a feature) under it would
        // otherwise read as clearable, and the build behind the clearing would find
        // the extractor still there.
        if (issueUpgradeBuild(builder, placing_, wx, wz, queue)) return true;
        std::vector<int> feats;
        if (clearableAt(placing_, wx, wz, feats) && !feats.empty()) {
            issueClearThenBuild(builder, placing_, wx, wz, feats, queue);
            return true;
        }
        return false;
    }

    // The deposits a box catches, in the order the builder will walk them (nearest
    // first from where it stands), less those already taken (an extractor, a site).
    std::vector<ta::client::metalspots::Spot> GameView::metalBoxSpots(float x0, float z0, float x1, float z1) {
        std::vector<ta::client::metalspots::Spot> out;
        if (!placing_ || selection_.empty()) return out;
        const int builder = selectedBuilder() ? selectedBuilder()->id : selection_.front();
        const UnitR* b = frameUnitP(builder);
        const float fx = b ? b->x : (x0 + x1) / 2, fz = b ? b->z : (z0 + z1) / 2;
        for (const auto& s : ta::client::metalspots::inBox(metalSpots(placing_), x0, z0, x1, z1, fx, fz))
            if (canPlacePreview(placing_, s.x, s.z) || placePreviewClearable(placing_, s.x, s.z) ||
                upgradableExtractorAt(placing_, builder, s.x, s.z)) out.push_back(s);
        return out;
    }

    // An extractor on every free deposit in the box, as one queue: a plain drag
    // replaces the builder's work with them, Shift adds them after it, Ctrl puts them
    // ahead of it (in walking order).
    bool GameView::placeMetalBox(float x0, float z0, float x1, float z1, uint8_t queue) {
        const auto spots = metalBoxSpots(x0, z0, x1, z1);
        if (spots.empty()) return false;
        const int builder = selectedBuilder() ? selectedBuilder()->id : selection_.front();
        const uint8_t place = queue & 3;
        auto issueAt = [&](const ta::client::metalspots::Spot& s, uint8_t q) {
            std::vector<int> feats;
            if (canPlaceLocked(placing_, s.x, s.z)) {
                ta::net::Command c;
                c.kind = ta::net::Cmd::Build;
                c.unitId = builder;
                c.x = s.x; c.z = s.z;
                c.queue = q;
                std::snprintf(c.type, sizeof c.type, "%s", placing_->id.c_str());
                issue(c);
                return true;
            }
            if (issueUpgradeBuild(builder, placing_, s.x, s.z, q)) return true;   // before the clearing (placeMetalAt)
            if (clearableAt(placing_, s.x, s.z, feats) && !feats.empty()) {
                issueClearThenBuild(builder, placing_, s.x, s.z, feats, q);
                return true;
            }
            return false;
        };
        bool any = false;
        if (place == ta::net::kQueueFront) {   // each to the head, the last first: they run in walking order
            for (auto it = spots.rbegin(); it != spots.rend(); ++it) any |= issueAt(*it, ta::net::kQueueFront);
        } else {
            for (const auto& s : spots) {
                // The first replaces (plain) or appends (Shift); the rest follow it. No
                // toggle bit: a box never takes back a site it re-catches.
                const uint8_t q = (!any && place == ta::net::kQueueReplace) ? ta::net::kQueueReplace : ta::net::kQueueAppend;
                any |= issueAt(s, q);
            }
        }
        return any;
    }

    // Retail mode 14 on the minimap (0x527eb0 S): MOBILEBUILD at the minimap point
    // with the "oktobuild" sound when the placement flag passes, else
    // "notoktobuild". Its flag is the main view's (stale, I); ours is checked at
    // the minimap point, with our clear-then-build where only clearable features
    // block (owner C2b). Shift queues and keeps the item armed, as in the view.
    bool GameView::minimapPlace(float mx, float my, int winW, int winH) {
        float wx, wz;
        if (!placing_ || watchOnly() || !minimapToWorld(mx, my, winW, winH, wx, wz)) return false;
        const uint8_t queue = keyQueue();   // Ctrl puts the build at the head of the queue
        if (selection_.empty()) { playGuiSound(ta::guisound::kNotOkToBuild, "minimap"); return true; }
        const int builder = selectedBuilder() ? selectedBuilder()->id : selection_.front();
        std::vector<int> feats;
        bool placed = false;
        if (!snapToMetal(placing_, wx, wz)) {
            // An extractor with no deposit near the point: refused, as in the view.
        } else if (canPlaceLocked(placing_, wx, wz)) {
            ta::net::Command c;
            c.kind = ta::net::Cmd::Build;
            c.unitId = builder;
            c.x = wx; c.z = wz;
            c.queue = queue;
            std::snprintf(c.type, sizeof c.type, "%s", placing_->id.c_str());
            issue(c);
            placed = true;
        } else if (clearableAt(placing_, wx, wz, feats) && !feats.empty()) {
            issueClearThenBuild(builder, placing_, wx, wz, feats, queue);
            placed = true;
        }
        playGuiSound(placed ? ta::guisound::kOkToBuild : ta::guisound::kNotOkToBuild, "minimap");
        if (placed) {
            if (shiftHeld()) shiftBuildPlaced_ = true;
            else { placing_ = nullptr; shiftBuildPlaced_ = false; }
        }
        return true;
    }

    GameView::BoxPlan GameView::planBox(ta::client::BoxMode mode, float sx0, float sy0, float sx1, float sy1) {
        using namespace ta::client;
        BoxPlan plan;
        const auto units = ownClickUnits();
        if (units.empty()) return plan;
        // What the box holds, by where each unit is DRAWN (the left select box's
        // test): visible enemies, and own finished units a transport could carry.
        std::vector<const UnitR*> enemies, own;
        for (const UnitR* up : front().live) {
            const UnitR& u = *up;
            if (!u.alive() || !u.type || u.embarked()) continue;
            const bool mine = u.player == localPlayer_;
            const bool enemy = !worldTables().allied(u.player, localPlayer_);
            if (!mine && !enemy) continue;
            if (enemy && !canPickUnit(u)) continue;
            if (mine && u.underConstruction) continue;
            const UnitR& fu = frameUnit(u.id);
            const SDL_FPoint p = dotsFrame_ ? dotCentre(fu) : unitScreen(fu);
            if (p.x < sx0 || p.x > sx1 || p.y < sy0 || p.y > sy1) continue;
            (enemy ? enemies : own).push_back(&u);
        }
        int transports = 0;
        for (const UnitR* u : units) transports += u->type->canTransport;
        for (const UnitR* u : units) {
            BoxUnit f;
            const auto& t = *u->type;
            f.attacker = isCursorAttacker(*u);
            f.airstrike = f.attacker && unitHasAirstrikeCursor(t, 0);
            f.reclaimer = canReclaimAsLocal(*u, localPlayer_);
            f.raiser = t.canResurrect;
            f.transport = t.canTransport;
            if (f.attacker && mode != BoxMode::ArmedLoad && mode != BoxMode::ArmedClear)
                for (const UnitR* e : enemies)
                    f.hits += cursorCanAttackEnemy(t, 0, u->worldPosition, *e->type,
                                                   e->worldPosition, e->flightGroundMode);
            if (f.transport && transports == 1)
                for (const UnitR* p : own) f.loadable += p != u && canLoadPassenger(*p, *u);
            plan.units.push_back(u->id);
            plan.facts.push_back(f);
        }
        plan.orders = boxOrders(mode, plan.facts);
        plan.cursor = boxCursor(plan.facts, plan.orders);
        // Nearest first (owner C10b, where retail draws 3n random picks): enemies by
        // distance from the attackers' centre, passengers from their transport.
        float cx = 0, cz = 0;
        int n = 0;
        const UnitR* carrier = nullptr;
        for (size_t i = 0; i < plan.units.size(); ++i) {
            const UnitR* u = frameUnitP(plan.units[i]);
            if (plan.orders[i] == ClickOrder::Attack) { cx += u->x; cz += u->z; ++n; }
            if (plan.orders[i] == ClickOrder::Pickup) carrier = u;
        }
        const auto byDistance = [](std::vector<const UnitR*>& list, float x, float z, std::vector<int>& out) {
            std::stable_sort(list.begin(), list.end(), [&](const UnitR* a, const UnitR* b) {
                const float da = (a->x - x) * (a->x - x) + (a->z - z) * (a->z - z);
                const float db = (b->x - x) * (b->x - x) + (b->z - z) * (b->z - z);
                return da < db;
            });
            for (const UnitR* u : list) out.push_back(u->id);
        };
        if (n) byDistance(enemies, cx / float(n), cz / float(n), plan.enemies);
        if (carrier) {
            std::erase_if(own, [&](const UnitR* p) { return p == carrier || !canLoadPassenger(*p, *carrier); });
            byDistance(own, carrier->x, carrier->z, plan.passengers);
        }
        return plan;
    }

    bool GameView::issueBoxPlan(const BoxPlan& plan, float x0, float z0, float x1, float z1, uint8_t queue) {
        ClickScope click(*this);
        std::vector<int> ordered, clearers;
        // Step L: the transport fetches each passenger in turn. A Load is the
        // carrier's order (World::loadInto: its GROUND_PICKUP / VTOL_PICKUP), so
        // the first goes with the click's own queue flag and the rest queue
        // behind it. Ctrl puts each pickup at the head of the carrier's list, so
        // they go in reverse, the first passenger ending up in front.
        const bool frontBox = (queue & ta::net::kQueuePlace) == ta::net::kQueueFront;
        for (size_t i = 0; i < plan.units.size(); ++i) {
            if (plan.orders[i] != ClickOrder::Pickup) continue;
            for (size_t k = 0; k < plan.passengers.size(); ++k) {
                ta::net::Command c;
                c.kind = ta::net::Cmd::Load;
                c.unitId = plan.passengers[frontBox ? plan.passengers.size() - 1 - k : k];
                c.targetId = plan.units[i];
                c.queue = k && !frontBox ? uint8_t(ta::net::kQueueAppend) : queue;
                issue(c);
            }
            if (!plan.passengers.empty()) ordered.push_back(plan.units[i]);
        }
        // Step A: one Attack per box enemy each attacker can hurt, nearest first; the
        // first replaces (Shift: queues), the rest queue behind it. Ctrl puts each at
        // the head (clickrules.h), so they go farthest first, the nearest ending up
        // in front.
        std::vector<char> started(plan.units.size(), 0);
        for (size_t k = 0; k < plan.enemies.size(); ++k) {
            const int enemyId = plan.enemies[frontBox ? plan.enemies.size() - 1 - k : k];
            const UnitR* e = frameUnitP(enemyId);
            if (!e || !e->type) continue;
            for (size_t i = 0; i < plan.units.size(); ++i) {
                if (plan.orders[i] != ClickOrder::Attack) continue;
                const UnitR* u = frameUnitP(plan.units[i]);
                if (!u || !ta::client::cursorCanAttackEnemy(*u->type, 0, u->worldPosition, *e->type,
                                                e->worldPosition, e->flightGroundMode)) continue;
                ta::net::Command c;
                c.kind = ta::net::Cmd::Attack; c.unitId = u->id; c.targetId = enemyId;
                c.queue = started[i] ? ta::client::followQueue(queue) : queue;
                issue(c);
                if (!started[i]) ordered.push_back(u->id);
                started[i] = 1;
            }
        }
        // Step C: the units with no attack order that can reclaim clear the box.
        for (size_t i = 0; i < plan.units.size(); ++i)
            if (plan.orders[i] == ClickOrder::ReclaimArea) clearers.push_back(plan.units[i]);
        if (!clearers.empty()) issueReclaimBox(x0, z0, x1, z1, queue, clearers);
        if (!ordered.empty() && clearers.empty()) {   // issueReclaimBox acknowledges a clear
            const bool attacked = std::any_of(started.begin(), started.end(), [](char s) { return s != 0; });
            voice(ackUnit(ordered), attacked ? "attack" : "move");
        }
        return !ordered.empty() || !clearers.empty();
    }

    bool GameView::armedRelease(float ex, float ey) {
        armedPress_ = false;
        if (!pendingCmd_ || watchOnly()) return false;
        const uint8_t queue = keyQueue();
        const auto a = dragStartOnScreen(dragAnchor_);
        if (areaBoxAt(dragAnchor_, ex, ey)) {
            // 0x527d00 S: ATTACK attacks the box's enemies, LOAD loads its units,
            // CLEAR clears it; any other command box-selects and stays armed.
            const auto mode = armedBoxMode(pendingCmd_);
            if (!mode) return false;
            dragging_ = false;
            float wx, wz;
            pickWorld(ex, ey, wx, wz);
            const BoxPlan plan = planBox(*mode, std::min(a.x, ex), std::min(a.y, ey),
                                         std::max(a.x, ex), std::max(a.y, ey));
            // The cursor said Normal: nothing happens and the command stays armed.
            if (plan.cursor != ta::CursorId::Normal && issueBoxPlan(plan, apX0_, apZ0_, wx, wz, queue))
                armedOrderIssued(shiftHeld());
            return true;
        }
        dragging_ = false;
        // A click at the press, over the unit drawn there now.
        if (armedClickAt(pendingCmd_, apX0_, apZ0_,
                         hoveredUnitAt(ta::client::PointerArea::World, a.x, a.y), queue, true))
            armedOrderIssued(shiftHeld());
        return true;
    }
