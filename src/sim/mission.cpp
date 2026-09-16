#include <cstdlib>
#include "sim/mission.h"

#include "sim/sim.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ta::sim {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}
bool ieq(const std::string& a, const char* b) {
    size_t n = 0;
    for (; a.size() > n && b[n]; ++n)
        if (std::tolower((unsigned char)a[n]) != std::tolower((unsigned char)b[n])) return false;
    return n == a.size() && b[n] == '\0';
}
// Deterministic per-type numeric id for GetUtype / GET_UNIT_VALUE(7) comparisons
// (FNV-1a of the lowercased type id, so both peers agree without an assignment order).
int32_t typeHash(const UnitType* t) {
    if (!t) return 0;
    uint32_t h = 2166136261u;
    for (unsigned char c : t->id) { h ^= std::tolower(c); h *= 16777619u; }
    return int32_t(h & 0x7fffffff) | 1;   // never 0
}
// Split "Verb rest of the string" into the leading verb and the remainder.
std::pair<std::string, std::string> splitVerb(const std::string& cmd) {
    size_t sp = cmd.find(' ');
    if (sp == std::string::npos) return {cmd, {}};
    return {cmd.substr(0, sp), cmd.substr(sp + 1)};
}

}  // namespace

MissionScript::MissionScript(std::vector<uint8_t> cobBytes, const ta::tdf::Node& header,
                             const TypeRegistry& reg, int humanPlayer, std::string origin,
                             std::vector<int> playerMap)
    : reg_(reg), human_(humanPlayer), origin_(std::move(origin)), playerMap_(std::move(playerMap)) {
    if (cobBytes.empty()) {
        // Data-only mission (11 of the 74 shipped ones ship no .cob): no god script.
        // The .ota conditions + the placed units' InitialMission queues drive it.
        parseConditions(header);
        return;
    }
    try {
        cob_ = cob::load(cobBytes, origin_);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "mission: cob load failed (%s): %s\n", origin_.c_str(), e.what());
        parseConditions(header);   // still honour the .ota win/lose rules
        return;
    }
    vm_ = std::make_unique<cob::Vm>(cob_, /*deterministicRand=*/true);   // hashed: mission RAND must be lockstep-identical
    vm_->onMapCommand = [this](int nameIdx, const std::vector<int32_t>& a) {
        return mapCommand(*world_, nameIdx, a);
    };
    vm_->onGet = [this](int32_t valId, const std::vector<int32_t>& a) {
        return getValue(*world_, valId, a);
    };
    vm_->onSetUnitValue = [this](int32_t valId, int32_t v) { setUnitValue(valId, v); };
    // Scripted story VO. Eight of the shipped mission cobs play a line at a beat
    // (an alchemist's death, a dragon's arrival, Monsara's cry) and we dropped all
    // of them on the floor -- the mission VM had no sound hook at all. The sim
    // cannot play audio, so record the request and let the viewer act on it.
    vm_->onPlaySound = [this](int32_t nameIdx) {
        if (!world_) return;
        const std::string& n = cob_.name(size_t(nameIdx < 0 ? 0 : nameIdx));
        if (!n.empty()) world_->requestSound(n);
    };
    parseConditions(header);
}

const UnitType* MissionScript::findType(const std::string& name) const {
    return reg_.find(lower(name));
}

// ---- lifecycle -------------------------------------------------------------

void MissionScript::start(World& w) {
    if (started_) return;
    started_ = true;
    world_ = &w;
    // The placed units' own order queues run first, so the NPC choreography is in
    // place before the god script's Start fires. A data-only mission (no cob) has
    // no VM -- these queues and the .ota conditions ARE the whole mission.
    // Two passes: register every name first, because a bodyguard's `g NAME` clause
    // routinely refers to a unit whose own `i NAME` clause comes later in the file.
    for (const auto& [id, orders] : initialOrders_) {
        size_t p = 0;
        while ((p = orders.find('i', p)) != std::string::npos) {
            bool start = p == 0 || orders[p - 1] == ',' || orders[p - 1] == ' ';
            if (start && p + 1 < orders.size() && orders[p + 1] == ' ') {
                size_t a = orders.find_first_not_of(' ', p + 1);
                size_t b = orders.find_first_of(", ", a);
                if (a != std::string::npos) {
                    std::string nm = orders.substr(a, b == std::string::npos ? b : b - a);
                    if (!nm.empty()) idents_.emplace(lower(nm), id);
                }
            }
            ++p;
        }
    }
    for (const auto& [id, orders] : initialOrders_) applyOrders(w, id, orders);
    initialOrders_.clear();
    if (vm_) vm_->start("Start");
}

void MissionScript::step(World& w, float dt) {
    world_ = &w;
    if (vm_) vm_->tick(dt);
    clock_ += dt;
    // Timed reinforcements (SetMission "b TYPE H X Y") that have come due.
    for (size_t k = 0; k < pendingSpawns_.size();) {
        if (clock_ >= pendingSpawns_[k].at) {
            const PendingSpawn& s = pendingSpawns_[k];
            if (s.type) w.spawn(s.type, s.x, s.z, 0.0f, s.player);
            pendingSpawns_.erase(pendingSpawns_.begin() + std::ptrdiff_t(k));
        } else {
            ++k;
        }
    }
    sweepTriggers(w);
    if (outcome_ == 0) evalConditions(w, dt);
}

void MissionScript::unitBuilt(World& w, int id) {
    if (!vm_ || cob_.scriptIndex("UnitCreated") < 0) return;
    world_ = &w; getUnitContext_ = id;
    vm_->start("UnitCreated", {id, 0});
}

void MissionScript::unitDied(World& w, int id) {
    if (!vm_ || cob_.scriptIndex("UnitDestroyed") < 0) return;
    world_ = &w; getUnitContext_ = id;
    vm_->start("UnitDestroyed", {id});
}

// ---- MAP_COMMAND dispatch (on the resolved verb string) --------------------

int32_t MissionScript::mapCommand(World& w, int nameIdx, const std::vector<int32_t>& a) {
    auto [verb, rest] = splitVerb(cob_.name(uint32_t(nameIdx)));
    if (ieq(verb, "create"))        return doCreate(w, rest, a);
    if (ieq(verb, "setmission"))  { if (!a.empty()) applyOrders(w, a[0], rest); return 0; }
    if (ieq(verb, "settrigger")) {
        if (!a.empty() && a[0] >= 0 && a[0] < 16) {
            Region& r = regions_[size_t(a[0])];
            r.armed = true;
            if (a.size() >= 5) { r.rect = true;  r.x = cellToWorld(float(a[1])); r.z = cellToWorld(float(a[2]));
                                                 r.x2 = cellToWorld(float(a[3])); r.z2 = cellToWorld(float(a[4])); }
            else if (a.size() >= 4) { r.rect = false; r.x = cellToWorld(float(a[1])); r.z = cellToWorld(float(a[2]));
                                                       r.r = float(a[3]) * 16.0f; }
            inside_[size_t(a[0])].clear();
        }
        return 0;
    }
    if (ieq(verb, "removetrigger")) { if (!a.empty() && a[0] >= 0 && a[0] < 16) regions_[size_t(a[0])].armed = false; return 0; }
    if (ieq(verb, "getutype"))      return typeHash(findType(rest));
    if (ieq(verb, "setattribute")) { if (a.size() >= 2) doSetAttribute(w, rest, a[0], a[1]); return 0; }
    if (ieq(verb, "writevalue"))   { if (!a.empty()) vars_[lower(rest)] = a[0]; return 0; }
    if (ieq(verb, "readvalue"))    { auto it = vars_.find(lower(rest)); return it != vars_.end() ? it->second : 0; }
    if (ieq(verb, "capture"))      { if (!a.empty()) if (Unit* u = w.unit(a[0])) u->player = a.size() >= 2 ? a[1] : human_; return 0; }
    if (ieq(verb, "kill"))         { if (!a.empty()) if (Unit* u = w.unit(a[0])) u->hp = 0; return 0; }
    if (ieq(verb, "screenshake")) {
        // Dramatic beats -- quakes, collapses -- asked for a shake and got nothing.
        // Purely cosmetic, but it is the script's only way to punctuate a scripted
        // moment. Args (when given) are magnitude and duration.
        float mag = !a.empty() ? float(a[0]) : 3.0f;
        float dur = a.size() > 1 ? float(a[1]) / 30.0f : 1.5f;
        w.requestShake(mag > 0 ? mag : 3.0f, dur > 0 ? dur : 1.5f);
        return 0;
    }
    return 0;
}

int32_t MissionScript::doCreate(World& w, const std::string& type, const std::vector<int32_t>& a) {
    const UnitType* t = findType(type);
    if (!t) { std::fprintf(stderr, "mission %s: create unknown type '%s'\n", origin_.c_str(), type.c_str()); return 0; }
    int player, cx, cy;
    if (a.size() >= 3)      { player = a[0]; cx = a[1]; cy = a[2]; }   // (player, x, y), player 0-based
    else if (a.size() >= 2) { player = 0;    cx = a[0]; cy = a[1]; }   // default -> the human
    else return 0;
    // The script's 0-based player is a .ota id (0 == Player1); map it to the World slot
    // the placements used, so scripted units land on the right side.
    int otaN = player + 1;
    if (otaN >= 1 && otaN < int(playerMap_.size()) && playerMap_[size_t(otaN)] >= 0)
        player = playerMap_[size_t(otaN)];
    player = std::clamp(player, 0, w.numPlayers() - 1);
    int id = w.spawn(t, cellToWorld(float(cx)), cellToWorld(float(cy)), 0.0f, player);
    if (id >= 0) getUnitContext_ = id;
    return id >= 0 ? id : 0;
}

// SetMission order-queue mini-language (see docs/campaign-design.md). Implements
// m/ma/a/p/s/d/w/o/v (move, move-attack, attack, patrol-loop, give, destroy, timed
// wait, stance, scalar); the timed reinforcement drop (b) and wait-for-attack (wa)
// event hold are still TODO.
void MissionScript::applyOrders(World& w, int unitId, const std::string& orders) {
    Unit* u = w.unit(unitId);
    if (!u) return;
    bool queue = false;
    size_t i = 0;
    auto skipsp = [&] { while (i < orders.size() && (orders[i] == ' ' || orders[i] == ',')) ++i; };
    auto num = [&]() -> float {
        skipsp(); size_t j = i;
        while (i < orders.size() && orders[i] != ' ' && orders[i] != ',') ++i;
        return orders.size() > j ? std::strtof(orders.substr(j, i - j).c_str(), nullptr) : 0.0f;
    };
    auto word = [&]() -> std::string {
        skipsp(); size_t j = i;
        while (i < orders.size() && orders[i] != ' ' && orders[i] != ',') ++i;
        return orders.substr(j, i - j);
    };
    while (i < orders.size()) {
        skipsp();
        if (i >= orders.size()) break;
        char c = char(std::tolower((unsigned char)orders[i]));
        char c2 = i + 1 < orders.size() ? char(std::tolower((unsigned char)orders[i + 1])) : 0;
        ++i;
        if (c == 'm') {                                   // m X Y  (move) / ma X Y (move-attack)
            bool atk = (c2 == 'a'); if (atk) ++i;
            float x = num(), y = num();
            if (atk) w.attackMove(unitId, cellToWorld(x), cellToWorld(y), queue);
            else     w.order(unitId, cellToWorld(x), cellToWorld(y), queue);
            queue = true;
        } else if (c == 'a') {                            // a TYPE (attack type) / a X Y (attack ground)
            skipsp();
            if (i < orders.size() && (std::isalpha((unsigned char)orders[i]) || orders[i] == '_')) {
                std::string ty = word();
                if (const UnitType* t = findType(ty)) {   // nearest enemy of that type
                    int best = -1; float bd = 1e18f;
                    for (const auto& e : w.units())
                        if (e.alive() && e.type == t && !w.allied(e.player, u->player)) {
                            float dx = e.x - u->x, dz = e.z - u->z, d = dx * dx + dz * dz;
                            if (d < bd) { bd = d; best = e.id; }
                        }
                    if (best >= 0) { w.attack(unitId, best, queue); queue = true; }
                }
            } else {
                float x = num(), y = num();
                w.attackMove(unitId, cellToWorld(x), cellToWorld(y), queue);
                queue = true;
            }
        } else if (c == 'p') {                            // p X Y (patrol waypoint: loops)
            float x = num(), y = num();
            w.patrolTo(unitId, cellToWorld(x), cellToWorld(y), queue); queue = true;
        } else if (c == 's') {                            // hand the unit to the human player
            u->player = human_;
        } else if (c == 'd') {                            // self-destruct / remove
            u->hp = 0;
        } else if (c == 'w') {                            // w N (wait N s) / wa (wait-for-attack)
            if (c2 == 'a') {
                ++i;
                w.orderWaitAttack(unitId, queue); queue = true;   // ambush: hold until threatened
            } else {
                float n = num();
                skipsp();
                if (i < orders.size() && std::isdigit((unsigned char)orders[i])) num();  // optional 2nd arg
                w.orderWait(unitId, n, queue); queue = true;
            }
        } else if (c == 'b') {                            // b TYPE H X Y: timed reinforcement drop
            std::string ty = word();
            float hh = num(), bx = num(), by = num();
            if (const UnitType* t = findType(ty))
                pendingSpawns_.push_back({t, u->player, cellToWorld(bx), cellToWorld(by), clock_ + hh});
        } else if (c == 'o') {                            // o A [B]: combat stance
            float a = num();
            skipsp();
            if (i < orders.size() && std::isdigit((unsigned char)orders[i])) num();
            w.setStance(unitId, int(a));
        } else if (c == 'v') {                            // v F: movement scalar (not modelled)
            num();
        } else if (c == 'i') {                            // i NAME: name this unit
            // The third most common verb (679 uses). It registers the unit under a
            // name that other units' `g` clauses -- and the .ota's own Ident= field
            // -- refer to, which is how a mission wires up bodyguards.
            std::string nm = word();
            if (!nm.empty()) idents_[lower(nm)] = unitId;
        } else if (c == 'g') {                            // g NAME: guard that unit
            std::string nm = word();
            auto it = idents_.find(lower(nm));
            if (it != idents_.end() && it->second != unitId) {
                w.guard(unitId, it->second, queue);
                queue = true;
            }
        } else if (c == 'c') {                            // c: cloak (no operand)
            w.setCloak(unitId, true);
        } else if (c == 'u') {                            // u X Y: unload cargo there
            float x = num(), y = num();
            w.unloadAt(unitId, cellToWorld(x), cellToWorld(y));
        } else {
            word();                                       // skip unrecognised token's operands
        }
    }
}

void MissionScript::doSetAttribute(World& w, const std::string& sub, int unitId, int pct) {
    Unit* u = w.unit(unitId);
    if (!u || !u->type) return;
    float f = float(pct) / 100.0f;
    if (ieq(sub, "healthpercentage"))     u->hp = u->type->maxHp * f;
    else if (ieq(sub, "armorpercentage")) u->armBuff = f;      // live armour multiplier
    else if (ieq(sub, "attackpercentage")) u->atkBuff = f;
}

// ---- GET / SET -------------------------------------------------------------

int32_t MissionScript::getValue(World& w, int32_t valId, const std::vector<int32_t>& a) {
    // The mission GET table. Scripts call GET(valId, a1..a4) 135 times across the
    // shipped missions and we answered only id 7, returning 0 for everything else --
    // which silently mis-resolved real missions: takmission04 read a player's unit
    // count as 0 and declared victory the instant anything died, takmission09 never
    // recognised Heket's death, and takmission22's POW counter never advanced.
    int32_t a1 = a.empty() ? 0 : a[0];
    switch (valId) {
        case 5: {
            // Living units belonging to an .ota player (the script uses .ota player
            // numbers, so map them into our compacted slots the way placements do).
            int player = a1;
            if (a1 >= 1 && a1 < int(playerMap_.size()) && playerMap_[size_t(a1)] >= 0)
                player = playerMap_[size_t(a1)];
            int n = 0;
            for (const auto& u : w.units())
                if (u.alive() && u.type && u.player == player) ++n;
            return n;
        }
        case 7:    // the context unit's type (GET_UNIT_VALUE form)
            if (Unit* u = w.unit(getUnitContext_)) return typeHash(u->type);
            return 0;
        case 30:   // a named unit's TYPE -- compared against the getUtype command,
                   // so it must hash identically
            if (Unit* u = w.unit(a1)) return typeHash(u->type);
            return 0;
        case 35:   // a unit's X cell (paired with 36 and compared to cell constants)
            if (Unit* u = w.unit(a1)) return int32_t(u->x) / 16;
            return 0;
        case 36:   // a unit's Z cell
            if (Unit* u = w.unit(a1)) return int32_t(u->z) / 16;
            return 0;
        case 40:   // a global counter, tested against 2500 to gate a late VO line;
                   // elapsed mission TICKS is the only reading that fits (~83s).
            return int32_t(clock_ * 30.0f);
        default:
            // id 31 (one use, semantics unclear) and anything else: 0.
            return 0;
    }
}

void MissionScript::setUnitValue(int32_t valId, int32_t value) {
    if (valId == 2) outcome_ = value ? 1 : -1;   // scripted force victory / defeat
}

// ---- triggers --------------------------------------------------------------

void MissionScript::sweepTriggers(World& w) {
    if (cob_.scriptIndex("TriggerHit") < 0) return;
    for (size_t rid = 0; rid < regions_.size(); ++rid) {
        const Region& r = regions_[rid];
        if (!r.armed) continue;
        auto& occ = inside_[rid];
        std::unordered_set<int> now;
        for (const auto& u : w.units()) {
            if (!u.alive() || u.embarked()) continue;
            bool in = r.rect ? (u.x >= std::min(r.x, r.x2) && u.x <= std::max(r.x, r.x2) &&
                                u.z >= std::min(r.z, r.z2) && u.z <= std::max(r.z, r.z2))
                             : ((u.x - r.x) * (u.x - r.x) + (u.z - r.z) * (u.z - r.z) <= r.r * r.r);
            if (!in) continue;
            now.insert(u.id);
            if (!occ.count(u.id)) {   // edge: newly entered -> fire the script once
                getUnitContext_ = u.id;
                vm_->start("TriggerHit", {int32_t(rid), u.id, u.player});
                if (outcome_ != 0) return;
            }
        }
        occ = std::move(now);
    }
}

// ---- data-driven win/lose conditions --------------------------------------

void MissionScript::parseConditions(const ta::tdf::Node& h) {
    auto add = [&](Cond::Kind k, bool vic, const UnitType* t, float a, float b, float c, float d) {
        conds_.push_back({k, t, a, b, c, d, vic});
    };
    // Each key appears at most once per header; parse the ones we model.
    auto has = [&](const char* key) { return h.value(key) != nullptr; };
    auto str = [&](const char* key) { return h.valueOr(key, ""); };
    // Comma-separated arg list "TYPE, x, z, r": the (optional) name token -> `type`,
    // the numbers packed into out[0..] in order (independent of the type's position).
    auto args = [&](const std::string& v, std::string& type, float out[4]) {
        std::string cur; int ni = 0; type.clear();
        auto flush = [&] {
            size_t a = cur.find_first_not_of(" \t"), b = cur.find_last_not_of(" \t");
            std::string t = a == std::string::npos ? "" : cur.substr(a, b - a + 1);
            cur.clear();
            if (t.empty()) return;
            if (!std::isdigit((unsigned char)t[0]) && t[0] != '-') type = t;
            else if (ni < 4) out[ni++] = std::strtof(t.c_str(), nullptr);
        };
        for (char ch : v) { if (ch == ',') flush(); else cur += ch; }
        flush();
    };
    float a[4] = {0, 0, 0, 0};
    std::string ty;
    if (has("MoveUnitToRadius")) { args(str("MoveUnitToRadius"), ty, a); add(Cond::MoveUnitToRadius, true, findType(ty), a[0], a[1], a[2], 0); }
    if (has("KillEnemyCommander")) add(Cond::KillEnemyCommander, true, nullptr, 0, 0, 0, 0);
    if (has("DestroyAllUnits"))    add(Cond::DestroyAllUnits, true, nullptr, 0, 0, 0, 0);
    if (has("KillAllMobileUnits")) add(Cond::KillAllMobileUnits, true, nullptr, 0, 0, 0, 0);
    if (has("KillAllOfType"))      add(Cond::KillAllOfType, true, findType(str("KillAllOfType")), 0, 0, 0, 0);
    if (has("KillUnitType"))       { args(str("KillUnitType"), ty, a); add(Cond::KillUnitType, true, findType(ty), a[0], 0, 0, 0); }
    if (has("VictoryTimerRunsOut")) add(Cond::VictoryTimerRunsOut, true, nullptr, std::strtof(str("VictoryTimerRunsOut").c_str(), nullptr), 0, 0, 0);
    if (has("CommanderKilled"))    add(Cond::CommanderKilled, false, nullptr, 0, 0, 0, 0);
    if (has("AllUnitsKilled"))     add(Cond::AllUnitsKilled, false, nullptr, 0, 0, 0, 0);
    if (has("DeathTimerRunsOut"))  add(Cond::DeathTimerRunsOut, false, nullptr, std::strtof(str("DeathTimerRunsOut").c_str(), nullptr), 0, 0, 0);
    // The escort/protect family. These are the most common conditions we did not
    // parse at all: AllUnitsKilledOfType alone appears in 21 of the 74 missions, so
    // the wagon-escort and protect-the-NPC missions could neither be won nor lost.
    //   AllUnitsKilledOfType=<T>      -- DEFEAT once every unit of T is dead. The
    //     type is always something on YOUR side (the escorted NPC, or your own
    //     troops in a mission where you play that kingdom).
    //   UnitTypePassesX/Z=<T>,<v>     -- VICTORY when a unit of T crosses the cell
    //     line v. Which SIDE counts as "across" depends on where it started, so the
    //     starting side is captured when the condition arms.
    //   UnitTypeKilled=<T>,<n>        -- DEFEAT once n units of T have died.
    if (has("AllUnitsKilledOfType"))
        add(Cond::AllUnitsKilledOfType, false, findType(str("AllUnitsKilledOfType")), 0, 0, 0, 0);
    if (has("UnitTypePassesX")) { args(str("UnitTypePassesX"), ty, a); add(Cond::UnitTypePassesX, true, findType(ty), a[0], 0, 0, 0); }
    if (has("UnitTypePassesZ")) { args(str("UnitTypePassesZ"), ty, a); add(Cond::UnitTypePassesZ, true, findType(ty), a[0], 0, 0, 0); }
    if (has("UnitTypeKilled"))  { args(str("UnitTypeKilled"), ty, a); add(Cond::UnitTypeKilled, false, findType(ty), a[0], 0, 0, 0); }
}

void MissionScript::evalConditions(World& w, float) {
    auto enemyAliveMobile = [&](bool mobileOnly) {
        for (const auto& u : w.units())
            if (u.alive() && !w.allied(u.player, human_) && (!mobileOnly || (u.type && u.type->canMove)))
                return true;
        return false;
    };
    auto humanHasAnyMobile = [&] {
        for (const auto& u : w.units())
            if (u.alive() && u.player == human_ && u.type && u.type->canMove) return true;
        return false;
    };
    auto enemyOfTypeAlive = [&](const UnitType* t) {
        for (const auto& u : w.units())
            if (u.alive() && u.type == t && !w.allied(u.player, human_)) return true;
        return false;
    };
    for (Cond& c : conds_) {
        bool met = false;
        switch (c.kind) {
            case Cond::MoveUnitToRadius: {   // a human-owned unit of c.type within c.c cells of (c.a,c.b)
                float cx = cellToWorld(c.a), cz = cellToWorld(c.b), rr = c.c * 16.0f;
                for (const auto& u : w.units())
                    if (u.alive() && u.type == c.type &&
                        (u.x - cx) * (u.x - cx) + (u.z - cz) * (u.z - cz) <= rr * rr) { met = true; break; }
                break;
            }
            // "Destroy all" rules arm once the target exists, then fire when it's gone --
            // never at t=0 before the enemy/human force has been placed or spawned.
            case Cond::DestroyAllUnits:
                if (enemyAliveMobile(false)) c.armed = true;
                met = c.armed && !enemyAliveMobile(false);
                break;
            case Cond::KillAllMobileUnits:
                if (enemyAliveMobile(true)) c.armed = true;
                met = c.armed && !enemyAliveMobile(true);
                break;
            case Cond::KillAllOfType:
                if (enemyOfTypeAlive(c.type)) c.armed = true;
                met = c.armed && !enemyOfTypeAlive(c.type);
                break;
            case Cond::VictoryTimerRunsOut: met = clock_ >= c.a; break;
            case Cond::CommanderKilled: {
                // Losing your MONARCH, not your last soldier. 15 missions use this,
                // and treating it as "all units dead" meant a mission whose whole
                // premise is protecting its hero only ended when the last straggler
                // fell. Arms once the commander has actually been placed.
                bool alive = false;
                for (const auto& u : w.units())
                    if (u.alive() && u.type && u.type->commander && u.player == human_) {
                        alive = true;
                        break;
                    }
                if (alive) c.armed = true;
                met = c.armed && !alive;
                break;
            }
            case Cond::AllUnitsKilled:
                if (humanHasAnyMobile()) c.armed = true;
                met = c.armed && !humanHasAnyMobile();
                break;
            case Cond::DeathTimerRunsOut:   met = clock_ >= c.a; break;
            case Cond::AllUnitsKilledOfType: {
                bool any = false;
                for (const auto& u : w.units())
                    if (u.alive() && u.type == c.type) { any = true; break; }
                if (any) c.armed = true;          // don't lose before it has spawned
                met = c.armed && !any;
                break;
            }
            case Cond::UnitTypeKilled: {
                // Count the dead of this type. Dead units linger as corpse records,
                // so counting them directly is both simple and replay-stable.
                int dead = 0;
                for (const auto& u : w.units())
                    if (!u.alive() && u.type == c.type) ++dead;
                met = dead >= int(c.a);
                break;
            }
            case Cond::UnitTypePassesX:
            case Cond::UnitTypePassesZ: {
                bool isX = c.kind == Cond::UnitTypePassesX;
                float line = cellToWorld(c.a);
                // Arm on the first sighting and remember which side of the line the
                // escort started on; the objective is to reach the OTHER side.
                if (!c.armed) {
                    for (const auto& u : w.units())
                        if (u.alive() && u.type == c.type) {
                            c.armed = true;
                            c.b = ((isX ? u.x : u.z) < line) ? -1.0f : 1.0f;
                            break;
                        }
                    break;   // never satisfied on the tick it arms
                }
                for (const auto& u : w.units()) {
                    if (!u.alive() || u.type != c.type) continue;
                    float p = isX ? u.x : u.z;
                    if (c.b < 0 ? p >= line : p <= line) { met = true; break; }
                }
                break;
            }
            default: break;   // KillEnemyCommander -> TODO
        }
        if (met) { outcome_ = c.victory ? 1 : -1; return; }
    }
}

// ---- lockstep hash ---------------------------------------------------------

void MissionScript::foldHash(uint64_t& h) const {
    auto mix = [&](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
    mix(uint64_t(int64_t(outcome_)));
    for (const Region& r : regions_) mix(r.armed ? 1u : 0u);
    // Both the armed flag and the captured crossing side are live condition state.
    for (const Cond& c : conds_) {
        mix(c.armed ? 1u : 0u);
        mix(uint64_t(int64_t(c.b * 4.0f)));
    }
    mix(uint64_t(pendingSpawns_.size()));   // timed reinforcements still pending
    for (const auto& s : pendingSpawns_) {
        uint32_t at; std::memcpy(&at, &s.at, 4); mix(at);
        mix(uint64_t(uint32_t(s.player)));
    }
    if (vm_) for (size_t i = 0; i < cob_.numStatics; ++i) mix(uint64_t(uint32_t(vm_->getStatic(i))));
    for (const auto& [k, v] : vars_) { for (char ch : k) mix(uint64_t((unsigned char)ch)); mix(uint64_t(uint32_t(v))); }
}

}  // namespace ta::sim
