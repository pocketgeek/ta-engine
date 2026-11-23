#include "client/gameview.h"
#include "client/clickrules.h"
#include "client/cursorrange.h"
#include "client/friendlyorder.h"
#include "sim/retailflyerspread.h"
#include "sim/retailheight.h"

// What a click orders (client/clickrules.h, PLAN 2.1-2.11): the per-unit facts the
// pure rules read, the commands they become, the boxes, and the minimap's orders.
// Every decision here is the one the cursor shows (desiredCursor reads planClick
// and planBox), so the cursor describes the click.

namespace {
    using tak::client::ClickOrder;

    // A frame position as the sim's raw 16.16 (the frame holds Fixed::toFloat).
    int32_t flyerRaw(float v) { return tak::sim::Fixed::fromFloat(v).v; }

    // Retail's flyer spread (sim/retailflyerspread.h): the goal of a flyer at its
    // frame position for a click at (wx, wz), the same integer 16.16 arithmetic the
    // sim gives an explicit order's flyers.
    template<class Unit>
    void flyerSpreadGoal(const tak::sim::FlyerGroupCentre& air, const Unit& u, float wx, float wz,
                         float& gx, float& gz) {
        int32_t x = 0, z = 0;
        air.goal(flyerRaw(u.x), flyerRaw(u.z), flyerRaw(wx), flyerRaw(wz), x, z);
        gx = tak::sim::Fixed::raw(x).toFloat();
        gz = tak::sim::Fixed::raw(z).toFloat();
    }

    int32_t unloadDestinationHeight(const tak::tnt::Map& map,float x,float z) {
        const int height=map.heights.empty() ? 0 : tak::sim::retailTerrainHeight(
            tak::sim::Fixed::fromFloat(x).v,tak::sim::Fixed::fromFloat(z).v,
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
        return tak::client::clickQueue((m & KMOD_SHIFT) != 0, (m & KMOD_CTRL) != 0);
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

    bool GameView::pointBlocker(float wx, float wz) {
        PointBlocker& b = pointBlocker_;
        const uint32_t tick = front().gameTick;
        const bool same = b.tick != ~0u && b.x == wx && b.z == wz;
        if (same && b.tick == tick) return b.v;
        if (const auto live = tryLiveWorld()) {
            b = {wx, wz, tick, (*live)->clearableBlockerAtPoint(wx, wz)};
            return b.v;
        }
        return same && b.v;
    }

    GameView::ClickPlan GameView::planClick(char mode, float wx, float wz, const UnitR* hovered, uint8_t queue,
                                            bool alt, bool pointTargets) {
        using namespace tak::client;
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
        }
        const UnitR* body = plan.body ? frameUnitP(plan.body) : nullptr;
        // The selection's transports, the hovered unit left out (0x520b60).
        int transports = 0;
        for (const UnitR* u : units)
            if (u->type->canTransport && u != hovered) ++transports;
        const bool damaged = hovered && hovered->type && !hovered->underConstruction &&
                             hovered->hp < hovered->type->maxHp;

        ClickUnit f;
        ArmedSelection sel;
        sel.singleTransport = transports == 1;
        sel.queue = tak::client::queuesBehind(queue);
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
            if (f.attacker) f.airstrike = unitHasAirstrikeCursor(t, u.weaponSlot);
            // Armed ATTACK at a point (ground or a friendly unit): the sim's own
            // resolver rule (World::suppressRule).
            if (mode == 'a' && f.attacker && target.kind != HoverKind::Enemy) {
                using Rule = tak::sim::World::SuppressRule;
                const Rule r = tak::sim::World::suppressRule(t, u.weaponSlot);
                f.pointFire = r == Rule::Fire || (r == Rule::OverBlocker && pointBlocker(wx, wz));
            }
            if (hovered && hovered->type) {
                if (f.attacker && target.kind == HoverKind::Enemy && (mode == 0 || mode == 'a'))
                    f.canHit = cursorCanAttackEnemy(t, u.weaponSlot, u.worldPosition, *hovered->type,
                                                    hovered->worldPosition, hovered->flightGroundMode);
                if (friendlyTarget && (mode == 0 || mode == 'm' || mode == 'f' || mode == 'r')) {
                    f.assist = canAssistSite(u, *hovered);
                    f.board = canLoadPassenger(u, *hovered);
                    f.pickup = f.transport && !f.attacker && transports == 1 && canLoadPassenger(*hovered, u);
                    f.friendly = friendlyUnitOrder(t, true, u.repeatType != nullptr, false, damaged);
                }
                if (mode == 'l' && f.transport) f.loadTarget = canLoadPassenger(*hovered, u);
            }
            if (body) f.raise = reviveKindOn(u, *body) != 0;
            if (plan.reclaim) f.reclaim = canReclaimAsLocal(u, localPlayer_);
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
            ClickOrder::Attack, ClickOrder::AttackGround, ClickOrder::Assist, ClickOrder::Repair, ClickOrder::Board, ClickOrder::Pickup,
            ClickOrder::Guard, ClickOrder::Resurrect, ClickOrder::Reclaim, ClickOrder::Unload,
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
            const auto send = [&](tak::net::Cmd k, int unit, int targetId) {
                tak::net::Command c;
                c.kind = k; c.unitId = unit; c.targetId = targetId; c.queue = queue;
                issue(c);
            };
            switch (kind) {
                case ClickOrder::Attack: for (int id : ids) send(tak::net::Cmd::Attack, id, plan.target); break;
                // At the click point, over ground or a friendly unit alike (4de530
                // kind 3 takes the pointer's point, not the unit's).
                case ClickOrder::AttackGround:
                    for (int id : ids) {
                        tak::net::Command c;
                        c.kind = tak::net::Cmd::AttackGround;
                        c.unitId = id; c.x = plan.wx; c.z = plan.wz; c.queue = queue;
                        issue(c);
                    }
                    break;
                case ClickOrder::Assist: for (int id : ids) send(tak::net::Cmd::Assist, id, plan.target); break;
                case ClickOrder::Repair: for (int id : ids) send(tak::net::Cmd::Repair, id, plan.target); break;
                case ClickOrder::Guard:  for (int id : ids) send(tak::net::Cmd::Guard, id, plan.target); break;
                case ClickOrder::Board:  for (int id : ids) send(tak::net::Cmd::Load, id, plan.target); break;
                // The hovered passenger boards the transport (the carrier fetches it).
                case ClickOrder::Pickup: for (int id : ids) send(tak::net::Cmd::Load, plan.target, id); break;
                case ClickOrder::Resurrect: for (int id : ids) send(tak::net::Cmd::Resurrect, id, plan.body); break;
                case ClickOrder::Reclaim:
                    if (plan.reclaim) for (int id : ids) send(tak::net::Cmd::Reclaim, id, *plan.reclaim);
                    break;
                case ClickOrder::Unload:
                    for (int id : ids) {
                        tak::net::Command c;
                        c.kind = tak::net::Cmd::Unload;
                        c.targetId = unloadDestinationHeight(mapView_.map(), plan.wx, plan.wz);
                        c.unitId = id; c.x = plan.wx; c.z = plan.wz; c.queue = queue;
                        issue(c);
                    }
                    break;
                case ClickOrder::AttackMove: case ClickOrder::Patrol: case ClickOrder::Move: {
                    const auto cmd = kind == ClickOrder::AttackMove ? tak::net::Cmd::AttackMove
                                   : kind == ClickOrder::Patrol ? tak::net::Cmd::Patrol : tak::net::Cmd::Move;
                    tak::sim::FlyerGroupCentre air;
                    float cx = 0, cz = 0;
                    int n = 0;
                    for (int id : ids)
                        if (const auto* u = frameUnitP(id)) { cx += u->x; cz += u->z; ++n; air.add(flyerRaw(u->x), flyerRaw(u->z)); }
                    if (n) { cx /= float(n); cz /= float(n); }
                    if (mode == 0 && kind == ClickOrder::Move) {
                        // The right-click move. In a Legion net game the run goes as ONE
                        // explicit order, OrderShape::Offsets at the click: the sim gives
                        // the members the same goals as below, from their positions at
                        // activation.
                        setClickMove(plan.wx, plan.wz);
                    } else {
                        // An armed MOVE, PATROL or fight-move gives flyers retail's spread
                        // (their legs are VTOL_Move and VTOL_Patrol, which carry the offset
                        // flag; sim/retailflyerspread.h) and every other unit the point
                        // itself, as retail's Move_Ground / Patrol do.
                        setFlyerSpread(plan.wx, plan.wz);
                    }
                    const bool legion = tak::sim::isLegionPathfinding(worldTables().pathfindingMode());
                    for (int id : ids) {
                        const auto* u = frameUnitP(id);
                        if (!u) continue;
                        tak::net::Command c;
                        c.kind = cmd;
                        c.unitId = id;
                        c.x = plan.wx; c.z = plan.wz;
                        // Flyers take retail's spread (0x5214b0): the click plus their
                        // unclamped offset from the group's centre within sqrt(3000 n) px,
                        // else the click. On the right-click move ground units keep OUR
                        // +-60 px per-axis spread (retail sends them all to the click; a
                        // deliberate difference), except that Legion owns footprint-aware
                        // group spreading on the shared point for the surface movers it
                        // plans; a Shift-queued leg shares the point too.
                        if (u->type && u->type->canFly) flyerSpreadGoal(air, *u, plan.wx, plan.wz, c.x, c.z);
                        else if (mode == 0 && kind == ClickOrder::Move) {
                            const bool shared = u->type && legion &&
                                tak::sim::legionSharedClick(u->type->canFly, u->type->footX, u->type->footZ);
                            if (!shared) {
                                c.x = plan.wx + std::clamp(u->x - cx, -60.0f, 60.0f);
                                c.z = plan.wz + std::clamp(u->z - cz, -60.0f, 60.0f);
                            }
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
        const UnitR* hovered = hoveredUnitAt(tak::client::PointerArea::World, mouseX_, mouseY_);
        const bool alt = (SDL_GetModState() & KMOD_ALT) != 0;
        issueClickPlan(planClick(0, wx, wz, hovered, queue, alt, true), 0, queue);
    }

    bool GameView::armedClickAt(char cmd, float wx, float wz, const UnitR* hovered, uint8_t queue, bool pointTargets) {
        ClickScope click(*this);
        const ClickPlan plan = planClick(cmd, wx, wz, hovered, queue, false, pointTargets);
        // P4 (0x527dd0 S): only an order cursor issues; Normal leaves it armed.
        if (!tak::client::isOrderCursor(plan.cursor)) return false;
        issueClickPlan(plan, cmd, queue);
        return true;
    }

    void GameView::issueArmedOrder(char cmd, float wx, float wz, uint8_t queue, bool precise) {
        ClickScope click(*this);
        if (selection_.empty()) return;
        const UnitR* hovered = precise ? hoveredUnitAt(tak::client::PointerArea::World, mouseX_, mouseY_)
                             : pointerArea() == tak::client::PointerArea::Minimap
                                 ? hoveredUnit(tak::client::PointerArea::Minimap) : nullptr;
        issueClickPlan(planClick(cmd, wx, wz, hovered, queue, false, true), cmd, queue);
    }

    bool GameView::minimapOrder(float mx, float my, int winW, int winH, uint8_t queue) {
        float wx, wz;
        if (watchOnly() || selection_.empty() || !minimapToWorld(mx, my, winW, winH, wx, wz)) return false;
        ClickScope click(*this);
        const UnitR* dot = hoveredUnitAt(tak::client::PointerArea::Minimap, mx, my);
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
        if (armedClickAt(pendingCmd_, wx, wz, hoveredUnitAt(tak::client::PointerArea::Minimap, mx, my), keyQueue(), false))
            armedOrderIssued(shiftHeld());
        return true;
    }

    bool GameView::minimapPlaceOk(float wx, float wz) {
        if (!placing_ || selection_.empty()) return false;
        return canPlacePreview(placing_, wx, wz) || placePreviewClearable(placing_, wx, wz);
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
        if (selection_.empty()) { sounds_.play("notoktobuild", 1.f, 1.f, 7, false); return true; }
        const int builder = selectedBuilder() ? selectedBuilder()->id : selection_.front();
        std::vector<int> feats;
        bool placed = false;
        if (canPlaceLocked(placing_, wx, wz)) {
            tak::net::Command c;
            c.kind = tak::net::Cmd::Build;
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
        sounds_.play(placed ? "oktobuild" : "notoktobuild", 1.f, 1.f, 7, false);
        if (placed) {
            if (shiftHeld()) shiftBuildPlaced_ = true;
            else { placing_ = nullptr; shiftBuildPlaced_ = false; }
        }
        return true;
    }

    GameView::BoxPlan GameView::planBox(tak::client::BoxMode mode, float sx0, float sy0, float sx1, float sy1) {
        using namespace tak::client;
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
            f.airstrike = f.attacker && unitHasAirstrikeCursor(t, u->weaponSlot);
            f.reclaimer = canReclaimAsLocal(*u, localPlayer_);
            f.raiser = t.canResurrect;
            f.transport = t.canTransport;
            if (f.attacker && mode != BoxMode::ArmedLoad && mode != BoxMode::ArmedClear)
                for (const UnitR* e : enemies)
                    f.hits += cursorCanAttackEnemy(t, u->weaponSlot, u->worldPosition, *e->type,
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
        // Step L: the transport fetches each passenger in turn. Every Load but the
        // first keeps the carrier's pickup list (World::loadInto appends while a
        // pickup leads it), so all go with the click's own queue flag. Ctrl puts
        // each pickup at the head of the carrier's list (and the passenger's), so
        // they go in reverse, the first passenger ending up in front.
        const bool frontBox = (queue & tak::net::kQueuePlace) == tak::net::kQueueFront;
        for (size_t i = 0; i < plan.units.size(); ++i) {
            if (plan.orders[i] != ClickOrder::Pickup) continue;
            for (size_t k = 0; k < plan.passengers.size(); ++k) {
                tak::net::Command c;
                c.kind = tak::net::Cmd::Load;
                c.unitId = plan.passengers[frontBox ? plan.passengers.size() - 1 - k : k];
                c.targetId = plan.units[i]; c.queue = queue;
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
                if (!u || !tak::client::cursorCanAttackEnemy(*u->type, u->weaponSlot, u->worldPosition, *e->type,
                                                e->worldPosition, e->flightGroundMode)) continue;
                tak::net::Command c;
                c.kind = tak::net::Cmd::Attack; c.unitId = u->id; c.targetId = enemyId;
                c.queue = started[i] ? tak::client::followQueue(queue) : queue;
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
            if (plan.cursor != tak::CursorId::Normal && issueBoxPlan(plan, apX0_, apZ0_, wx, wz, queue))
                armedOrderIssued(shiftHeld());
            return true;
        }
        dragging_ = false;
        // A click at the press, over the unit drawn there now.
        if (armedClickAt(pendingCmd_, apX0_, apZ0_,
                         hoveredUnitAt(tak::client::PointerArea::World, a.x, a.y), queue, true))
            armedOrderIssued(shiftHeld());
        return true;
    }
