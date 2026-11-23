#include <cstdlib>
#include "sim/mission.h"

#include "sim/sim.h"
#include "sim/footprint.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace tak::sim {

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
// Deterministic per-type numeric id for GetUtype / GET(30) comparisons
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

MissionScript::MissionScript(std::vector<uint8_t> cobBytes, const tak::tdf::Node& header,
                             const TypeRegistry& reg, int humanPlayer, std::string origin,
                             std::vector<int> playerMap)
    : reg_(reg), human_(humanPlayer), origin_(std::move(origin)), playerMap_(std::move(playerMap)),
      pointScale_(header.child("map data") ? 16 : 1) {   // 4c86eb / 4c8748
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
    wireVm();
    parseConditions(header);
}

std::unique_ptr<MissionScript> MissionScript::clone() const {
    std::unique_ptr<MissionScript> copy(new MissionScript(*this));
    if (copy->vm_) copy->wireVm();
    return copy;
}

void MissionScript::wireVm() {
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
    vm_->onPlaySound = [this](int32_t nameIdx,int32_t flags) {
        if (!world_) return;
        const std::string& n = cob_.name(size_t(nameIdx));
        if (n.empty()) return;
        if (holdSounds_) heldSounds_.push_back({n, flags});
        else world_->requestSound(n,flags);
    };
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
    // Retail (0x514b40) makes every placed unit first and then parses each
    // record's InitialMission in file order, so a clause can name any of them.
    for (const auto& [id, orders] : initialOrders_) applyOrders(w, id, orders, true);
    initialOrders_.clear();
    placements_.clear();
    // 4d2dc0 starts Start with its run-now flag (56c640 -> 56c680 -> 56c870):
    // it runs at once, up to its first sleep, before the first game tick, so
    // the units it makes take their first update (and their MakeSelectable)
    // before the first outcome check.
    if (vm_ && vm_->start("Start")) {
        holdSounds_ = true;
        vm_->tick(0.0f);
        holdSounds_ = false;
    }
}

void MissionScript::step(World& w, float dt) {
    world_ = &w;
    // Retail's game tick (5262xx..5264c0, S): the units' updates (51d3e0),
    // then the outcome check (4f6c70: the condition manager), then the
    // scenario's slot 3 (5264c0: the god script's VM, 56c870). So the
    // conditions see what the units did this tick but not what the script
    // does after them: a unit the script makes with orders (unselectable
    // until its MakeSelectable runs in its next update) is never caught
    // without its selectable bit. The trigger sweep (4d33a0) runs at the
    // start of each unit's next update (51de93) and runs its handlers there;
    // sweeping after the script here runs them in that same next tick, after
    // the units rather than among them.
    for (auto& [name, flags] : heldSounds_) w.requestSound(std::move(name), flags);
    heldSounds_.clear();
    ++clock_;
    if (outcome_ == 0) evalConditions(w, dt);
    if (vm_) vm_->tick(dt);
    sweepTriggers(w);
}

int32_t MissionScript::value(const std::string& name) const {
    auto it = vars_.find(lower(name));
    return it != vars_.end() ? it->second : 0;
}

int MissionScript::retailOwner(int player) const {
    // The inverse of playerMap_: the .ota Player-1 the slot came from.
    for (size_t ota = 1; ota < playerMap_.size(); ++ota)
        if (playerMap_[ota] == player) return int(ota) - 1;
    return player;
}

int MissionScript::conditionOwner(int player) const {
    const int enemy=playerMap_.size()>2 ? playerMap_[2] : 1;
    return player==human_ ? 0 : player==enemy ? 1 : 2;
}

void MissionScript::unitCaptured(World& w,int id) {
    const Unit* u=w.unit(id);
    if (!u || conditionOwner(u->player)!=1) return;
    for (auto& c:conds_)
        if(c.kind==Cond::CaptureUnitType && u->type==c.type) {
            c.met=true;emitConditionSound(w,c);
        }
}

void MissionScript::conditionDeaths(World& w, const UnitType* type, int player, int dead) {
    const int owner=conditionOwner(player);
    for(auto& c:conds_) {
        bool survivor=false;
        if(c.kind==Cond::KillAllMobileUnits || c.kind==Cond::KillAllOfType || c.kind==Cond::AllUnitsKilledOfType) {
            for(const auto& u:w.units()) {
                if(u.id==dead || !u.alive() || u.hp<=Fixed() || !u.type)continue;
                const int side=conditionOwner(u.player);
                if(c.kind==Cond::AllUnitsKilledOfType ? side>1 : side!=1)continue;
                if(c.kind==Cond::KillAllMobileUnits ? u.type->isStructure() : u.type!=c.type)continue;
                survivor=true;break;
            }
        }
        retailCampaignDeath(c.kind,c,owner,type==c.type,
            player>=0 && size_t(player)<playerCommanders_.size() &&
                type && type==playerCommanders_[size_t(player)],
            type && !type->isStructure(),survivor);
        emitConditionSound(w,c);
    }
}

void MissionScript::unitDied(World& w, int id) {
    const Unit* victim=w.unit(id);
    if (victim) conditionDeaths(w,victim->type,victim->player,id);
    triggerBits_.erase(id);
    runUnitDestroyed(w,id,victim ? victim->player : -1);
}

// 512b4e: the scenario's +0x18 (4d2d90) starts UnitDestroyed with the run-now
// flag (56c640 -> 56c680 -> 56c870), so its first slice runs inside the
// retirement, before 512e52 takes the unit off its owner's population word:
// a GET(5) there counts the retiring unit and the ones retiring after it this
// tick (retiring_, set by World's feed).
void MissionScript::runUnitDestroyed(World& w, int id, int owner) {
    const bool listed=!retiring_.empty() && retiring_.front().first==id;
    if (!listed) retiring_.insert(retiring_.begin(),{id,owner});
    if (vm_ && cob_.scriptIndex("UnitDestroyed") >= 0) {
        world_ = &w;
        vm_->startNow("UnitDestroyed", {id});
    }
    retiring_.erase(retiring_.begin());
}

void MissionScript::objectReplaced(int id) { triggerBits_.erase(id); }

// 514e93 / 514fc2 kill the old object with 30000 of kind 6; 512610 gives it
// severity 0, so it retires at once (512ae0): 523510 hands every condition
// the death of a unit of its type and old owner, and the scenario's +0x18
// starts UnitDestroyed(old id). The new object holds the id here, so it is
// not taken for the dead one in the survivor scans, and its latch (reset at
// the change, objectReplaced) is left alone.
void MissionScript::replacedObjectDied(World& w, int id, const UnitType* type, int oldPlayer) {
    conditionDeaths(w,type,oldPlayer,0);
    runUnitDestroyed(w,id,oldPlayer);
}

// ---- MAP_COMMAND dispatch (on the resolved verb string) --------------------

int32_t MissionScript::command(World& w, const std::string& name, const std::vector<int32_t>& a) {
    world_ = &w;
    auto [verb, rest] = splitVerb(name);
    if (ieq(verb, "create"))        return doCreate(w, rest, a);
    if (ieq(verb, "setmission"))  { if (!a.empty()) applyOrders(w, a[0], rest, false); return 0; }
    if (ieq(verb, "settrigger")) {
        // 4d3a53: a slot already armed, or 16 armed, ignores the call. 4 args
        // make a circle (4d2b00), any other count a rectangle (4d2ae0); the
        // cells are kept as given.
        const int armed = int(std::count_if(regions_.begin(), regions_.end(), [](const Region& r) { return r.armed; }));
        if (a.empty() || a[0] < 0 || a[0] >= 16 || regions_[size_t(a[0])].armed || armed >= 16) return 0;
        auto arg = [&](size_t i) { return i < a.size() ? a[i] : 0; };
        Region& r = regions_[size_t(a[0])];
        r = Region{};
        r.armed = true;
        r.rect = a.size() != 4;
        r.x1 = int16_t(arg(1)); r.z1 = int16_t(arg(2));
        if (r.rect) { r.x2 = int16_t(arg(3)); r.z2 = int16_t(arg(4)); }
        else r.r = arg(3);
        return a[0];
    }
    if (ieq(verb, "removetrigger")) {
        // 4d3b22: frees an armed slot and clears its latch bit on every unit.
        if (!a.empty() && a[0] >= 0 && a[0] < 16 && regions_[size_t(a[0])].armed) {
            regions_[size_t(a[0])].armed = false;
            for (auto& [id, bits] : triggerBits_) bits &= ~(1u << a[0]);
        }
        return 0;
    }
    if (ieq(verb, "getutype"))      return typeHash(findType(rest));
    if (ieq(verb, "setattribute")) { if (a.size() >= 2) doSetAttribute(w, rest, a[0], a[1]); return 0; }
    if (ieq(verb, "writevalue"))   { if (!a.empty()) vars_[lower(rest)] = a[0]; return 0; }
    if (ieq(verb, "readvalue"))    { auto it = vars_.find(lower(rest)); return it != vars_.end() ? it->second : 0; }
    if (ieq(verb, "capture")) {
        if (!a.empty()) if (Unit* u=w.unit(a[0])) {
            const int next=a.size()>=2 ? a[1] : human_;
            // Retail's Capture (0x4d3c30) changes the owner through 514da0, the
            // CRT transfer's new object: orders, last hitter and experience go.
            if(u->player!=next) w.scenarioTransfer(u->id,next,this);
        }
        return 0;
    }
    // Retail's Kill (0x4d36eb -> 513130) ignores its arguments and kills every
    // unit with death kind 10 (512610), which credits no killer and scores
    // nothing (512ae0). No shipped mission uses it.
    if (ieq(verb, "kill")) {
        w.missionKillAll();
        return 0;
    }
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
    // Retail 4d35c0: exactly 3 args (player byte, x, z) or 2 (x, z); any
    // other count makes nothing.
    int player, cx, cy;
    if (a.size() == 3)      { player = uint8_t(a[0]); cx = int16_t(a[1]); cy = int16_t(a[2]); }   // player 0-based
    else if (a.size() == 2) { player = 0;             cx = int16_t(a[0]); cy = int16_t(a[1]); }   // default -> the human
    else return 0;
    // The script's 0-based player is a .ota id (0 == Player1); map it to the World slot
    // the placements used, so scripted units land on the right side.
    int otaN = player + 1;
    if (otaN >= 1 && otaN < int(playerMap_.size()) && playerMap_[size_t(otaN)] >= 0)
        player = playerMap_[size_t(otaN)];
    player = std::clamp(player, 0, w.numPlayers() - 1);
    // createPoint: the footprint's corner at the cell, which 5085c0 leaves as
    // it is. No heading is given, so the unit keeps its birth heading (511990).
    const Point at = createPoint(*t, cx, cy);
    int id = w.spawn(t, at.x.toFloat(), at.z.toFloat(), std::nullopt, player);
    w.creditCreationStorage(id);   // 4d36cd: Create's 512060 passes 1
    w.finishAsFeature(id);         // 51230c: created finished
    return id >= 0 ? id : 0;
}

// SetMission / InitialMission order strings (see docs/campaign-design.md), as
// retail's parser 0x513fb0 reads them. The string is cut at every comma
// (strcspn ","); a clause is its text up to the next comma after leading
// blanks, and one of 256 characters or more is skipped. Its first letter, in
// either case, picks the order, and the rest is read with sscanf: each form
// needs exactly the conversions it lists, else the clause does nothing. So a
// clause never reads into the next one, and "w 900 40 b ARAPRIES 1" is one
// wait clause whose tail is ignored, as retail ignores it. The point of m, ma,
// p, u and b is the pixel X*16,Y*16 (orderPoint): a cell's corner, not its centre.
//   m X Y / ma X Y   move / fight-move to cell X,Y
//   a X Y            fire at the ground there (SUPPRESS; see below)
//   a TYPE           AttackUType: every unit of TYPE, nearest first, until none
//   a NAME           attack the placed unit NAME
//   b TYPE [N]       a factory builds N (default 1) of TYPE, then goes on
//   b TYPE N X Y     a mobile builder builds TYPE at X,Y (MobileBuild; N unused)
//   bw...            BuildWeapon stockpiling: no shipped mission uses it
//   p X Y [T]        patrol (the time T is not modelled)
//   w F [N]          wait F s; with N, a visible enemy within N cells ends it
//   wa [NAME]        WaitForAttack: hold until NAME (else this unit) is hit
//   o A / o A B      standing orders: 2 offensive, 1 defensive, 0 passive /
//                    the move and fire fields themselves
//   v F              fixed veterancy multiplier F (rank shows 10)
//   i NAME           start aboard the transport NAME
//   g NAME           guard NAME
//   u X Y            unload there
//   s                MakeSelectable (no owner change): the unit is selectable again
//   d                self-destruct when the order comes up
//   c                cloak, if the type can
//   r ...            resurrect: no shipped mission uses it
// When any clause pushed a mission, the parse ends by clearing the unit's
// selectable bit and, unless a patrol, an `a X Y` that got an order, an `s` or
// a `d` was among them, appending MakeSelectable (514a7d..514aba; E,
// tools/re/check_mission_selectable.py): its owner cannot select it, and the
// retail AI leaves it alone, until its script is through. `i`, `v`, `o` and `c`
// push nothing. A structure's move or patrol is its rally here (retail pushes
// QMove/QPatrol); it counts as a push.
// Retail queues every order behind what the unit already has (4d78a0 with its
// queue flag set). Names (i, g, wa, a) resolve against the placed units only
// in an InitialMission: SetMission's call passes retail an empty unit table.
namespace {
// sscanf's view of a clause: every format starts with a blank, which skips any
// whitespace before the conversion.
struct ClauseScan {
    const std::string& s;
    size_t i;
    void blank() { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; }
    bool number(float& out) {             // " %f"
        blank();
        if (i >= s.size()) return false;
        const char* begin = s.c_str() + i;
        char* end = nullptr;
        const float v = std::strtof(begin, &end);
        if (end == begin) return false;
        i += size_t(end - begin);
        out = v;
        return true;
    }
    bool integer(int& out) {              // " %d"
        blank();
        if (i >= s.size()) return false;
        const char* begin = s.c_str() + i;
        char* end = nullptr;
        const long v = std::strtol(begin, &end, 10);
        if (end == begin) return false;
        i += size_t(end - begin);
        out = int(std::clamp<long>(v, INT32_MIN, INT32_MAX));
        return true;
    }
    bool name(std::string& out, bool underscore) {   // " %[a-zA-Z0-9_.]" / " %[a-zA-Z0-9.]"
        blank();
        const size_t j = i;
        while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '.' || (underscore && s[i] == '_'))) ++i;
        if (i == j) return false;
        out = s.substr(j, i - j);
        return true;
    }
};
}  // namespace

int MissionScript::resolvePlacement(const std::string& name) const {
    // 0x513ec0: in record order, the record's Ident, then its Unitname
    // (case-insensitive), the first that matches and has a unit.
    for (const Placement& p : placements_) {
        if (!p.unitId) continue;
        if ((!p.ident.empty() && ieq(p.ident, name.c_str())) ||
            (!p.unitname.empty() && ieq(p.unitname, name.c_str())))
            return p.unitId;
    }
    return 0;
}

float MissionScript::orderPoint(float cell, int32_t scale) {
    // 5d3d54 is fistp under round-toward-zero into 64 bits, its low dword kept
    // (out of range, the integer indefinite, whose low dword is 0); imul wraps.
    const double chopped = std::trunc(double(cell) * 65536.0);
    const int64_t whole = std::fabs(chopped) < 9.2e18 ? int64_t(chopped) : INT64_MIN;
    const int32_t raw = int32_t(uint32_t(uint64_t(whole)) * uint32_t(scale));
    return float(double(raw) / 65536.0);
}

MissionScript::Point MissionScript::placedPoint(const UnitType& t, int xpos, int zpos) {
    Point p{Fixed::fromInt(xpos * 16), Fixed::fromInt(zpos * 16)};
    // 4c1768 reads any bmcode other than 0 or 1 as 0, and 5085c0 snaps those.
    if (t.buildMovementCode != 1) { p.x = footprintSnap(p.x, t.footX); p.z = footprintSnap(p.z, t.footZ); }
    return p;
}

int32_t MissionScript::percentHp(const UnitType& t, int32_t pct) {
    return int32_t(uint16_t((uint32_t(t.maxHp) * uint32_t(pct)) / 100u));
}

double MissionScript::percentMana(const UnitType& t, int32_t pct) {
    const float v = float(double(t.maxMana) * double(pct) * double(0.01f));
    return double(v > t.maxMana ? t.maxMana : v);
}

MissionScript::Point MissionScript::createPoint(const UnitType& t, int x, int z) {
    return {Fixed::fromInt(x * 16 + t.footX * 8), Fixed::fromInt(z * 16 + t.footZ * 8)};
}

void MissionScript::applyOrders(World& w, int unitId, const std::string& orders, bool initial) {
    if (!w.unit(unitId)) return;
    const bool queue = true;   // retail appends; it never clears the unit's orders
    bool pushed = false;       // 514a7d's [ebp-4]: a clause pushed a mission
    bool held = false;         // [ebp-0x20]: a patrol, a given `a X Y`, `s` or `d`
    size_t at = 0;
    while (at < orders.size()) {
        while (at < orders.size() && std::isspace((unsigned char)orders[at])) ++at;
        const size_t comma = orders.find(',', at);
        const size_t length = (comma == std::string::npos ? orders.size() : comma) - at;
        const std::string clause = orders.substr(at, length);
        at += length;
        if (at < orders.size() && orders[at] == ',') ++at;
        if (clause.empty() || length >= 256) continue;
        Unit* u = w.unit(unitId);
        if (!u || !u->type) return;
        const char c = char(std::tolower((unsigned char)clause[0]));
        const char c2 = clause.size() > 1 ? char(std::tolower((unsigned char)clause[1])) : 0;
        ClauseScan scan{clause, 1};
        const size_t ordersBefore = u->orders.size(), rallyBefore = u->rally.size();
        bool holds = false;
        float fx = 0, fz = 0;
        int n = 0;
        std::string nm;
        switch (c) {
        case 'm': {                                       // 514120: m X Y / ma X Y
            // m, p, u, b and a take orderPoint's X*16, Y*16 px (scenario +0xdc5 =
            // 16), no half cell (E, check_mission_points.py,
            // check_mission_attack_point.py).
            const bool fight = c2 == 'a';
            scan.i = fight ? 2 : 1;
            if (!scan.number(fx) || !scan.number(fz)) break;
            if (fight) w.attackMove(unitId, orderPoint(fx), orderPoint(fz), queue);
            else       w.order(unitId, orderPoint(fx), orderPoint(fz), queue);
            break;
        }
        case 'a': {                                       // 5143b5
            ClauseScan point = scan;
            if (point.number(fx) && point.number(fz)) {
                // 5143b5: " %f %f" -> orderPoint's X*16, Y*16 px (scenario +0xdc5 =
                // 16, no half cell), 4de530 kind 3 -> 4d78a0 queued: the click's
                // resolver (SUPPRESS for a ranged unit or tower, a flyer's point
                // order, nothing for a toairweapon or a melee one off a blocker).
                // No order given: the clause does nothing and the next one still
                // reads. A kamikaze type's ATTACK_KAMIKAZE is not ported (no
                // shipped use).
                holds = w.suppress(unitId, orderPoint(fx), orderPoint(fz), queue);
                break;
            }
            if (!scan.name(nm, true)) break;
            if (const UnitType* t = findType(nm)) {      // 515ac0: a type -> ATTACKUTYPE
                w.orderAttackType(unitId, t, queue);
                break;
            }
            // 513ec0: a placed unit -> an attack on it (4de530 kind 3).
            // A unit with no weapon gets no attack mission; the combat drops
            // one it cannot hurt when it comes up, as 4de530 would refuse it.
            const int target = initial ? resolvePlacement(nm) : 0;
            if (target && target != unitId && !u->type->weapons.empty()) w.attack(unitId, target, queue);
            break;
        }
        case 'b': {                                       // 51451d
            if (c2 == 'w') break;                         // BuildWeapon: not used by any shipped mission
            if (u->type->isStructure()) {
                // 51461d: " %[a-zA-Z0-9_.] %d", N defaulting to 1, no build menu
                // test. BuildingBuild builds N, then the next order runs.
                n = 1;
                if (!scan.name(nm, true)) break;
                scan.integer(n);
                if (const UnitType* t = findType(nm)) w.orderTrain(unitId, t, n, queue);
                break;
            }
            // 514543: all four, the count read and unused (MobileBuild gets 1).
            // The builder must have TYPE on its menu (519850). The point is
            // orderPoint's: MobileBuild snaps it to the footprint as a player's
            // click is snapped.
            if (!scan.name(nm, true) || !scan.integer(n) || !scan.number(fx) || !scan.number(fz)) break;
            const UnitType* t = findType(nm);
            if (!t) break;
            const auto& menu = u->type->buildMenu;
            if (std::find(menu.begin(), menu.end(), t->id) == menu.end()) break;
            w.queueBuild(unitId, t, orderPoint(fx), orderPoint(fz), queue);
            break;
        }
        case 'p': {                                       // 514307: p X Y [T]
            if (!scan.number(fx) || !scan.number(fz)) break;
            w.patrolTo(unitId, orderPoint(fx), orderPoint(fz), queue);
            holds = true;   // 5143ad, when it pushed
            break;
        }
        case 'w': {                                       // 514787
            if (c2 == 'a') {
                // 514806: wa [NAME]. A NAME that resolves to nothing drops the
                // clause; no NAME waits on the unit itself.
                scan.i = 2;
                int watched = unitId;
                if (scan.name(nm, false)) {
                    watched = initial ? resolvePlacement(nm) : 0;
                    if (!watched) break;
                }
                w.orderWaitAttack(unitId, queue, watched);
                break;
            }
            // " %f %d": one number waits F s; with the second, a visible enemy
            // within N cells (x16 px) ends it early.
            if (!scan.number(fx)) break;
            int cells = 0;
            if (!scan.integer(cells)) cells = 0;
            w.orderWait(unitId, fx, queue, cells * 16);
            break;
        }
        case 'o': {                                       // 514086
            // " %d %d": both in 0..2 set the move and fire fields themselves;
            // else " %d" in 0..2 is the standing order (0x5198a0).
            ClauseScan pair = scan;
            int fire = 0;
            if (pair.integer(n) && pair.integer(fire) && n >= 0 && n <= 2 && fire >= 0 && fire <= 2) {
                w.scriptStandingFields(unitId, n, fire);
                break;
            }
            if (scan.integer(n) && n >= 0 && n <= 2) w.scriptStandingOrder(unitId, n);
            break;
        }
        case 'v':                                         // 5148e1: v F -> unit +0xf4
            if (scan.number(fx)) w.setVeterancyOverride(unitId, fx);
            break;
        case 'i': {                                       // 514893: i NAME -> 51b4f0
            const int carrier = initial && scan.name(nm, true) ? resolvePlacement(nm) : 0;
            if (carrier) w.embarkInto(unitId, carrier);
            break;
        }
        case 'g': {                                       // 514265: g NAME (4de530 kind 7)
            const int ward = initial && scan.name(nm, true) ? resolvePlacement(nm) : 0;
            if (ward && ward != unitId) w.guard(unitId, ward, queue);
            break;
        }
        case 'u':                                         // 5141d3: u X Y
            if (scan.number(fx) && scan.number(fz)) w.unloadAt(unitId, orderPoint(fx), orderPoint(fz), {}, queue);
            break;
        case 's':                                         // 514876: MakeSelectable
            w.orderMakeSelectable(unitId);
            holds = true;
            break;
        case 'd':                                         // 514858: SelfDestructFG
            w.orderSelfDestruct(unitId, queue);
            holds = true;
            break;
        case 'c':                                         // 514a48: cloak if cloakcost > 0
            w.setCloak(unitId, true);
            break;
        default:                                          // 'r' and any other letter
            break;
        }
        if (c == 'i') continue;   // 51b4f0 is no push (and clears the queue)
        if (const Unit* now = w.unit(unitId);
            now && (now->orders.size() > ordersBefore || now->rally.size() > rallyBefore)) {
            pushed = true;
            held = held || holds;
        }
    }
    if (pushed) {
        if (Unit* u = w.unit(unitId)) u->selectable = false;
        if (!held) w.orderMakeSelectable(unitId);
    }
    // Orders queued aboard a transport wait there (retail's BeCarried heads the
    // queue): no search for a route from inside the hold.
    if (const Unit* u = w.unit(unitId); u && u->embarked()) w.cancelRoute(unitId);
}

void MissionScript::doSetAttribute(World& w, const std::string& sub, int unitId, int pct) {
    Unit* u = w.unit(unitId);
    // 4d3e27: only a unit whose object lives, a dying one too; a retired one
    // is left as it is (E, check_new_unit_mana.py setattribute).
    if (!u || !World::retailObjectLives(*u)) return;
    float f = float(pct) / 100.0f;
    if (ieq(sub, "healthpercentage"))     u->hp = Fixed::fromInt(percentHp(*u->type, pct));
    else if (ieq(sub, "manapercentage"))  u->mana = percentMana(*u->type, pct);
    else if (ieq(sub, "armorpercentage")) u->armBuff = f;      // live armour multiplier
    else if (ieq(sub, "attackpercentage")) u->atkBuff = f;
}

// ---- GET / SET -------------------------------------------------------------

int MissionScript::worldPlayer(int record) const {
    // A retail player record index is the .ota Player number less one, the
    // index Create's player argument is (4d36cc); map it as placements do.
    const int ota = record + 1;
    if (ota >= 1 && ota < int(playerMap_.size()) && playerMap_[size_t(ota)] >= 0) return playerMap_[size_t(ota)];
    return record;
}

int32_t MissionScript::getValue(World& w, int32_t valId, const std::vector<int32_t>& a) {
    // The mission GET table, 4d4060 (E: tools/re/check_mission_get.py). GET and
    // GET_UNIT_VALUE both reach it (the host's slot +0x54, 56d0cd: the latter
    // with four zero arguments). The shipped scripts use ids 5, 7, 30, 31, 35,
    // 36 and 40; the others read display and settings fields and are 0 here.
    int32_t a1 = a.empty() ? 0 : a[0];
    switch (valId) {
        case 5: {
            // Player record a1's population word (+0x24ec): every object of the
            // player from its creation (512334, 512574) to its retirement
            // (512e52), a unit being built or one whose Dying runs too. a1 is
            // the 0-based record index (the scripts test GET(5, 1) before
            // creating units for player 1).
            const int player = worldPlayer(a1);
            int n = 0;
            for (const auto& u : w.units())
                if (World::retailObjectLives(u) && u.player == player) ++n;
            for (const auto& [id, owner] : retiring_) n += owner == player;
            return n;
        }
        case 7: {
            // The unit cap word (+0x14e7c), whatever the arguments: every shipped
            // GET_UNIT_VALUE(7) is `GET(5, p) < GET_UNIT_VALUE(7) - n` ahead of
            // creating n units for p. An uncapped World reads as the word's top.
            const int cap = w.unitCap();
            return cap > 0 ? std::min(cap, 0xffff) : 0xffff;
        }
        case 30:   // a named unit's TYPE -- compared against the getUtype command,
                   // so it must hash identically
            if (Unit* u = w.unit(a1)) return typeHash(u->type);
            return 0;
        case 31:   // the unit's owner byte (+0xfd), 10 for no unit
            if (Unit* u = a1 ? w.unit(a1) : nullptr; u && u->type) return retailOwner(u->player);
            return 10;
        case 35:   // the unit's footprint-origin cell (+0x74 / +0x76, 51b3b0), as words
            if (Unit* u = w.unit(a1); u && u->type) return int16_t(footprintOrigin(u->x, u->type->footX));
            return 0;
        case 36:
            if (Unit* u = w.unit(a1); u && u->type) return int16_t(footprintOrigin(u->z, u->type->footZ));
            return 0;
        case 40: {   // native 4d42f3: player record a1's score
            const int player = worldPlayer(a1);
            return player>=0 && player<w.numPlayers() ? w.player(player).score : 0;
        }
        default:
            return 0;
    }
}

void MissionScript::setUnitValue(int32_t valId, int32_t value) {
    if (valId == 2) outcome_ = value ? 1 : -1;   // scripted force victory / defeat
    if (valId == 40 && world_) world_->setScriptScore(value); // native 4d3ffe/4d4011
}

// ---- triggers --------------------------------------------------------------

void MissionScript::sweepTriggers(World& w) {
    // 4d33a0, unit by unit: a live, finished unit that is not dying first
    // fires UnitCreated(unit, owner) once, then TriggerHit(region, unit, owner)
    // once for each armed region 0..15 that holds its footprint-origin cell.
    // Each fires once per unit (latch +0xd4); leaving and coming back does
    // not fire again. A unit aboard a transport counts too, at its carrier's
    // attach point with its own footprint (4dad30 -> 51b3b0 keeps its +0x68 and
    // +0x74 there, E; World's cargo step does the same). 51d3e0 walks
    // the players' pools in owner-byte order, so the handlers start (and run)
    // owner by owner; within a pool, in spawn order.
    if (!vm_) return;
    const bool created = cob_.scriptIndex("UnitCreated") >= 0, hit = cob_.scriptIndex("TriggerHit") >= 0;
    std::vector<std::pair<int, const Unit*>> order;
    order.reserve(w.units().size());
    for (const auto& u : w.units())
        if (u.alive() && u.hp > Fixed() && u.type && !u.underConstruction) order.push_back({retailOwner(u.player), &u});
    std::stable_sort(order.begin(), order.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
    for (const auto& [owner, unit] : order) {
        const Unit& u = *unit;
        uint32_t& bits = triggerBits_[u.id];
        if (!(bits & 0x80000000u)) {
            bits |= 0x80000000u;
            if (created) vm_->start("UnitCreated", {u.id, owner});
        }
        const int ox = footprintOrigin(u.x, u.type->footX), oz = footprintOrigin(u.z, u.type->footZ);
        for (size_t rid = 0; rid < regions_.size(); ++rid) {
            const Region& r = regions_[rid];
            if (!r.armed || (bits >> rid & 1)) continue;
            if (!(r.rect ? retailTriggerRect(ox, oz, r.x1, r.z1, r.x2, r.z2)
                         : retailTriggerCircle(ox, oz, r.x1, r.z1, r.r))) continue;
            bits |= 1u << rid;
            if (hit) vm_->start("TriggerHit", {int32_t(rid), u.id, owner});
        }
    }
}

// ---- data-driven win/lose conditions --------------------------------------

void MissionScript::parseConditions(const tak::tdf::Node& h) {
    auto text=[&](const char* key){return h.valueOr(key,"");};
    auto integer=[](const std::string& value) {
        const long long n=std::strtoll(value.c_str(),nullptr,0);
        return int32_t(std::clamp(n,-2147483648LL,2147483647LL));
    };
    auto add=[&](Cond::Kind kind,bool victory,const char* key,bool typed,int numbers,bool positive=false) {
        const std::string value=text(key);
        if(value.empty())return;
        Cond c;c.kind=kind;c.victory=victory;
        size_t begin=0;
        if(typed) {
            const auto comma=value.find(',');
            std::string name=value.substr(0,comma);
            const auto first=name.find_first_not_of(" \t"),last=name.find_last_not_of(" \t");
            name=first==std::string::npos?"":name.substr(first,last-first+1);
            c.type=findType(name);
            c.wildcard=ieq(name,"ANYTYPE") && (kind==Cond::MoveUnitToRadius || kind==Cond::UnitTypePassesX || kind==Cond::UnitTypePassesZ);
            begin=comma==std::string::npos?value.size():comma+1;
        }
        int32_t* fields[]={&c.a,&c.b,&c.c};
        for(int i=0;i<numbers && i<3;++i) {
            const auto comma=value.find(',',begin);
            *fields[i]=integer(value.substr(begin,comma==std::string::npos?comma:comma-begin));
            begin=comma==std::string::npos?value.size():comma+1;
        }
        if(positive && ((kind==Cond::AnyUnitPassesX || kind==Cond::AnyUnitPassesZ) ? c.a<0 : c.a<=0))return;
        if(!typed && !numbers && integer(value)==0)return;
        c.remaining=c.a;
        conds_.push_back(c);
    };
    // Match native parser order. Victory is the conjunction of every rule;
    // defeat is the disjunction. Boolean zero and nonpositive timers disable.
    add(Cond::KillEnemyCommander,true,"KillEnemyCommander",false,0);
    add(Cond::DestroyAllUnits,true,"DestroyAllUnits",false,0);
    add(Cond::KillAllMobileUnits,true,"KillAllMobileUnits",false,0);
    add(Cond::BuildUnitType,true,"BuildUnitType",true,0);
    add(Cond::CaptureUnitType,true,"CaptureUnitType",true,0);
    add(Cond::KillAllOfType,true,"KillAllOfType",true,0);
    add(Cond::KillUnitType,true,"KillUnitType",true,1);
    add(Cond::MoveUnitToRadius,true,"MoveUnitToRadius",true,3);
    add(Cond::UnitTypePassesX,true,"UnitTypePassesX",true,1);
    add(Cond::UnitTypePassesZ,true,"UnitTypePassesZ",true,1);
    add(Cond::VictoryTimerRunsOut,true,"VictoryTimerRunsOut",false,1,true);
    add(Cond::CommanderKilled,false,"CommanderKilled",false,0);
    add(Cond::AllUnitsKilled,false,"AllUnitsKilled",false,0);
    add(Cond::AllUnitsKilledOfType,false,"AllUnitsKilledOfType",true,0);
    add(Cond::UnitTypeKilled,false,"UnitTypeKilled",true,1);
    add(Cond::DeathTimerRunsOut,false,"DeathTimerRunsOut",false,1,true);
    add(Cond::AnyUnitPassesX,false,"AnyUnitPassesX",false,1,true);
    add(Cond::AnyUnitPassesZ,false,"AnyUnitPassesZ",false,1,true);
    // 522f48 skips the default DestroyAllUnits in campaign mode (1).
    // Empty victory lists stay pending for a scripted win, e.g. takx13_mt.
    // The default defeat AllUnitsKilled at522f78 is unconditional.
    if(std::none_of(conds_.begin(),conds_.end(),[](const Cond& c){return !c.victory;})) {
        Cond c;c.kind=Cond::AllUnitsKilled;conds_.push_back(c);
    }
}

void MissionScript::emitConditionSound(World& w, Cond& condition) {
    // Native non-timer victory classes retain an independent soundplayed +8
    // flag. DestroyAllUnits can become false again, so completion alone cannot
    // replace this once-only cosmetic latch. Defeat/timer rules have no cue.
    if(!condition.victory || condition.kind==Cond::VictoryTimerRunsOut ||
       !condition.met || condition.soundPlayed)return;
    condition.soundPlayed=true;
    w.requestSound("Victory Condition",7);
}

void MissionScript::evalConditions(World& w, float) {
    // 523eb0 / 5243f0: a unit whose mission orders still run (not selectable)
    // neither reaches a radius nor keeps its owner from AllUnitsKilled.
    auto ready=[](const Unit& u){return u.type && retailCampaignReady(u.alive(),u.hp<=Fixed(),u.selectable,
        !u.underConstruction,false,u.embarked(),false);};
    bool victory=std::any_of(conds_.begin(),conds_.end(),[](const Cond& c){return c.victory;});
    bool defeat=false;
    for(Cond& c:conds_) {
        switch(c.kind) {
        case Cond::DestroyAllUnits:
            c.met=std::none_of(w.units().begin(),w.units().end(),[&](const Unit& u){return u.alive() && u.hp>Fixed() && conditionOwner(u.player)==1;});
            break;
        case Cond::AllUnitsKilled:
            c.met=std::none_of(w.units().begin(),w.units().end(),[&](const Unit& u){return conditionOwner(u.player)==0 && ready(u);});
            break;
        case Cond::VictoryTimerRunsOut:case Cond::DeathTimerRunsOut:
            c.met=uint32_t(clock_)>=uint32_t(c.a)*30u;break;
        case Cond::BuildUnitType:
            for(const auto& u:w.units())if(u.alive() && u.hp>Fixed() && conditionOwner(u.player)==0 && u.type==c.type && !u.underConstruction)c.met=true;
            break;
        case Cond::MoveUnitToRadius:
            for(const auto& u:w.units()) {
                if(conditionOwner(u.player)!=0 || !ready(u) || (!c.wildcard && u.type!=c.type))continue;
                if(retailCampaignRadius(u.x.v,u.z.v,int32_t(uint32_t(c.a)<<20),int32_t(uint32_t(c.b)<<20),int32_t(uint32_t(c.c)<<20)))c.met=true;
            }
            break;
        case Cond::UnitTypePassesX:case Cond::UnitTypePassesZ:
        case Cond::AnyUnitPassesX:case Cond::AnyUnitPassesZ: {
            const bool enemy=c.kind==Cond::AnyUnitPassesX || c.kind==Cond::AnyUnitPassesZ;
            const bool x=c.kind==Cond::UnitTypePassesX || c.kind==Cond::AnyUnitPassesX;
            for(const auto& u:w.units()) {
                if(!u.alive() || u.hp<=Fixed() || !u.type || conditionOwner(u.player)!=(enemy?1:0))continue;
                if(!enemy && !c.wildcard && u.type!=c.type)continue;
                const int coordinate=x?footprintOrigin(u.x,u.type->footX):footprintOrigin(u.z,u.type->footZ);
                if(retailCampaignAxis(coordinate,c.a))c.met=true;
            }
            break;
        }
        default:break; // Event conditions retain their native completion latch.
        }
        emitConditionSound(w,c);
        if(c.victory)victory=bool(victory && c.met);
        else defeat=bool(defeat || c.met);
    }
    if(victory)outcome_=1;
    else if(defeat)outcome_=-1;
}

// ---- lockstep hash ---------------------------------------------------------

void MissionScript::foldHash(uint64_t& h) const {
    auto mix = [&](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
    mix(uint64_t(int64_t(outcome_)));
    for (const Region& r : regions_) {
        mix(r.armed ? 1u : 0u); mix(r.rect ? 1u : 0u);
        mix(uint16_t(r.x1)); mix(uint16_t(r.z1)); mix(uint16_t(r.x2)); mix(uint16_t(r.z2)); mix(uint32_t(r.r));
    }
    std::vector<std::pair<int, uint32_t>> latches(triggerBits_.begin(), triggerBits_.end());
    std::sort(latches.begin(), latches.end());
    for (const auto& [id, bits] : latches) { mix(uint32_t(id)); mix(bits); }
    mix(uint32_t(clock_));
    for(const UnitType* type:playerCommanders_) {
        if(type)for(unsigned char ch:type->id)mix(ch);
        mix(0);
    }
    // Event counters and completion latches must survive lockstep checks.
    for (const Cond& c : conds_) {
        mix(c.met ? 1u : 0u);
        mix(uint32_t(c.remaining));
    }
    if (vm_) for (size_t i = 0; i < cob_.numStatics; ++i) mix(uint64_t(uint32_t(vm_->getStatic(i))));
    std::vector<std::pair<std::string,int32_t>> variables(vars_.begin(),vars_.end());
    std::sort(variables.begin(),variables.end());
    for (const auto& [k, v] : variables) { for (char ch : k) mix(uint64_t((unsigned char)ch)); mix(uint64_t(uint32_t(v))); }
}

}  // namespace tak::sim
