#include "sim/scenario.h"

#include "sim/sim.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <charconv>
#include <bit>

namespace tak::sim {

namespace {
std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
int toInt(const std::string& s) { return std::atoi(s.c_str()); }
std::string flagKey(const std::string& name) { return name.empty() ? std::string(1,'\0') : name.substr(0,1); }
}  // namespace

ScenarioScript::ScenarioScript(const tak::crt::Scenario& scen, const TypeRegistry& reg,
                               int viewPlayer, int maxPlayer, int mapWCells, int mapHCells,
                               uint32_t participants)
    : reg_(reg), view_(viewPlayer), maxPlayer_(std::max(1, maxPlayer)),
      participants_(participants & ((1u << std::min(8, maxPlayer_)) - 1)),
      outcomes_(size_t(maxPlayer_), 0),
      mapW_(mapWCells), mapH_(mapHCells), regions_(scen.regions) {
    // Never transfer an absent player's rules to another owner. Production match
    // setup rejects unseated authored players before constructing the runner.
    players_.assign(size_t(maxPlayer_), {});
    // CRT bank zero is All Players; banks 1..8 belong to owners 0..7.
    // Native loader 4ccb20 copies global rules first into each seated player's
    // own rule list, so disabling a global rule affects only that player.
    for (size_t p = 0; p < players_.size(); ++p) if (participates(int(p))) {
        if (!scen.players.empty()) players_[p] = scen.players[0];
        if (p + 1 < scen.players.size())
            players_[p].insert(players_[p].end(), scen.players[p+1].begin(), scen.players[p+1].end());
    }
    disabled_.resize(players_.size());
    state_.resize(players_.size());
    for (size_t p = 0; p < players_.size(); ++p) {
        disabled_[p].assign(players_[p].size(), 0);
    }
}

const UnitType* ScenarioScript::findType(const std::string& name) const {
    return reg_.find(lower(name));
}

bool ScenarioScript::typeMatches(const Unit& unit, const std::string& name) const {
    // Native 515ac0 resolves unknown names to type zero, the wildcard ID.
    const auto* type=findType(name);
    return unit.type && (!type || unit.type==type);
}

const tak::crt::Region* ScenarioScript::region(const std::string& name) const {
    const auto want=lower(name);
    for (const auto& r:regions_) if (lower(r.name)==want) return &r;
    return nullptr;
}

bool ScenarioScript::regionBounds(const std::string& name,int& x1,int& z1,int& x2,int& z2) const {
    // Native loader inserts Anywhere before authored regions. It includes an
    // off-map margin, so temporarily off-map units still count.
    if (lower(name)=="anywhere") {
        x1=z1=-10000;x2=mapW_+10000;z2=mapH_+10000;return true;
    }
    const auto* r=region(name);
    if (!r) return false;
    x1=r->x1;z1=r->z1;x2=r->x2;z2=r->z2;return true;
}

bool ScenarioScript::inRegion(World&, const Unit& unit, const std::string& loc) const {
    int x1,z1,x2,z2;
    if (!regionBounds(loc,x1,z1,x2,z2)) return false;
    if (!unit.type) return false;
    const int cx=footprintOrigin(unit.x,unit.type->footX),cz=footprintOrigin(unit.z,unit.type->footZ);
    return cx>=x1 && cx<=x2 && cz>=z1 && cz<=z2;
}

bool ScenarioScript::regionCenter(const std::string& loc,float& x,float& z) const {
    int x1,z1,x2,z2;
    if (!regionBounds(loc,x1,z1,x2,z2)) return false;
    // Signed half-distance truncates toward zero; no half-cell offset.
    auto coordinate=[](int low,int high) {
        const int32_t difference=std::bit_cast<int32_t>(uint32_t(high)-uint32_t(low));
        const uint32_t cell=uint32_t(low)+uint32_t(difference/2);
        return Fixed::raw(std::bit_cast<int32_t>(cell << 20)).toFloat();
    };
    x=coordinate(x1,x2);z=coordinate(z1,z2);
    return true;
}

int32_t ScenarioScript::deathCount(const PState& state,bool killed,const std::string& name) const {
    const auto* type=findType(name);
    const auto& values=killed ? state.killed : state.lost;
    const auto& key=type ? type->id : (killed ? state.firstKilled : state.firstLost);
    const auto it=values.find(key);
    return it==values.end() ? 0 : it->second;
}

int ScenarioScript::parsePlayer(const std::string& s) const {
    std::string lo = lower(s);
    if (lo.rfind("all", 0) == 0) return -1;             // All Players
    // Trailing number: "Player 3" -> slot 2. Invalid recipients never alias
    // the final real player (nor broadcast to everybody).
    const auto begin=s.find_last_not_of("0123456789");
    const size_t start=begin==std::string::npos?0:begin+1;
    int value=0;
    const auto result=std::from_chars(s.data()+start,s.data()+s.size(),value);
    return result.ec==std::errc() && value>0 && value<=maxPlayer_ ? value-1 : maxPlayer_;

}

int ScenarioScript::countControl(World& w, int player, const std::string& typeName,
                                 const std::string& loc) const {
    int n=0;
    for (const auto& u:w.units())
        if (u.alive() && u.hp>Fixed() && !u.underConstruction && u.player==player && typeMatches(u,typeName) &&
            inRegion(w,u,loc)) ++n;
    return n;
}

void ScenarioScript::applyOutcome(World& w, int player, int result, int recipients) {
    // Retail 4cad50 routes self/allies/opponents separately; 4f6b90/4f6be0
    // latch the first result. A victory never fabricates another player's defeat.
    for (int q = 0; q < maxPlayer_; ++q) {
        const bool selected = recipients == 0 ? q == player :
            recipients == 1 ? w.allied(player, q) : !w.allied(player, q);
        if (participates(q) && selected) {
            if (!outcomes_[size_t(q)]) outcomes_[size_t(q)] = int8_t(result);
            // Native defeat marks gameplay state even after a displayed win.
            if (result < 0) w.forceDefeat(q);
        }
    }
}

// -------- conditions (opcode space per the RE table) --------
bool ScenarioScript::evalCond(World& w, int player, const tak::crt::Rule& c, bool initial) {
    const auto& s = c.slot;
    PState& ps = state_[size_t(player)];
    switch (c.opcode) {
        case 0:  return initial;                         // Start of game
        case 1:  return int64_t(clock_) > int64_t(toInt(s[0])) * 30;            // Gametime > v
        case 2:  return int64_t(clock_) < int64_t(toInt(s[0])) * 30;            // Gametime < v
        case 3: { auto it = ps.timers.find(toInt(s[0]));   // Timer > v (unset => false)
                  return it != ps.timers.end() && it->second.value > toInt(s[1]); }
        case 4: { auto it = ps.timers.find(toInt(s[0]));   // Timer < v (unset => false)
                  return it != ps.timers.end() && it->second.value < toInt(s[1]); }
        case 5: return deathCount(ps,true,s[1])>toInt(s[0]);
        case 6: return deathCount(ps,true,s[1])<toInt(s[0]);
        case 9: return deathCount(ps,false,s[1])>toInt(s[0]);
        case 10:return deathCount(ps,false,s[1])<toInt(s[0]);
        case 7: case 8: case 11: case 12: {
            const bool killed=c.opcode<=8,most=c.opcode==7 || c.opcode==11;
            const auto mine=deathCount(ps,killed,s[0]);
            for (int q=0;q<maxPlayer_;++q) {
                if (q==player || !participates(q) || w.player(q).defeated) continue;
                const auto other=deathCount(state_[size_t(q)],killed,s[0]);
                if (most ? other>=mine : other<=mine) return false;
            }
            return true;
        }
        case 13: case 14: {
            const int mine=countControl(w,player,s[0],s[1]);
            for (int q=0;q<maxPlayer_;++q) {
                if (q==player || !participates(q) || w.player(q).defeated) continue;
                const int other=countControl(w,q,s[0],s[1]);
                if (c.opcode==13 ? other>=mine : other<=mine) return false;
            }
            return true;
        }
        case 15: return countControl(w, player, s[1], s[2]) > toInt(s[0]);   // control > N X at L
        case 16: return countControl(w, player, s[1], s[2]) < toInt(s[0]);   // control < N X at L
        case 17: case 18: {
            const auto it=ps.flags.find(flagKey(s[0]));
            return it!=ps.flags.end() && (c.opcode==17 ? it->second>toInt(s[1]) : it->second<toInt(s[1]));
        }
        case 19: return true;                                  // Always
        case 20: return false;                                 // Never
        case 21: case 22: {                                    // opponents left </> N
            int opp = 0;
            for (int q = 0; q < w.numPlayers(); ++q)
                if (participates(q) && q != player && !w.allied(player, q) && !w.player(q).defeated) ++opp;
            return c.opcode == 21 ? opp < toInt(s[0]) : opp > toInt(s[0]);
        }
        case 23: return w.scenarioRandomPercent(toInt(s[0]));
        case 24: return w.player(player).mana < float(toInt(s[0]));   // Resources < v
        case 25: return w.player(player).mana > float(toInt(s[0]));   // Resources > v
        default: return false;
    }
}

// -------- actions (independent opcode space) --------
void ScenarioScript::runAction(World& w, int player, int group, const tak::crt::Rule& a) {
    const auto& s = a.slot;
    PState& ps = state_[size_t(player)];
    switch (a.opcode) {
        case 0: ps.timers[toInt(s[0])] = {toInt(s[1]), false}; break;   // countdown t=v
        case 1: ps.timers[toInt(s[0])] = {toInt(s[1]), true};  break;   // countup t=v
        case 2: ps.flags[flagKey(s[0])] = toInt(s[1]); break;                           // set flag f=v
        case 3: case 4: {
            const auto it=ps.flags.find(flagKey(s[1]));
            if(it!=ps.flags.end()) {
                const uint32_t amount=uint32_t(toInt(s[0]));
                it->second=std::bit_cast<int32_t>(a.opcode==3 ? uint32_t(it->second)+amount : uint32_t(it->second)-amount);
            }
            break;
        }
        case 5: applyOutcome(w, player, 1, 0); break;                         // Victory for me
        case 6: applyOutcome(w, player, -1, 0); break;                        // Defeat for me
        case 7: {                                                              // Create X at L
            const UnitType* t = findType(s[0]);
            int lx, lz, hx, hz;
            if (!t || !regionBounds(s[1], lx, lz, hx, hz)) break;
            w.scenarioCreate(t,player,lx,lz,hx,hz);
            break;
        }
        case 8: {                                                              // Destroy X at L
            std::vector<int> targets;
            for (const auto& u:w.units())
                if (u.alive() && u.player==player && typeMatches(u,s[0]) && inRegion(w,u,s[1])) targets.push_back(u.id);
            for (int id:targets) w.scenarioDestroy(id);
            break;
        }
        case 9: {                                                              // I own all X at L
            std::vector<int> targets;
            for (const auto& u : w.units())
                if (u.alive() && u.player != player && typeMatches(u,s[0]) && inRegion(w,u,s[1]))
                    targets.push_back(u.id);
            for (int id : targets) w.scenarioTransfer(id,player);
            break;
        }
        case 10:                                                               // Heal X by HP at L
        case 11: {                                                             // Damage X by HP at L
            // Native 51a140 sends an unsigned 16-bit HP amount, not a percentage.
            std::vector<int> targets;
            for (const auto& u:w.units())
                if (u.alive() && u.player==player && typeMatches(u,s[0]) && inRegion(w,u,s[2])) targets.push_back(u.id);
            for (int id:targets) w.scenarioAdjustHealth(id,toInt(s[1]),a.opcode==10);
            break;
        }
        case 12: showClock_ = true; break;                                     // Display gameclock
        case 13: pending_.push_back({clock_, parsePlayer(s[0]), s[1]}); break;  // Display P text
        case 14: disabled_[size_t(player)][size_t(group)] = 1; break;          // Disable rule
        case 15: {                                                             // Move all X at L1 to L2
            float dx, dz;
            if (!regionCenter(s[2],dx,dz)) break;
            for (auto& u : w.units())
                if (u.alive() && u.hp > Fixed() && !u.underConstruction && u.player == player &&
                    typeMatches(u,s[0]) && inRegion(w,u,s[1]))
                    w.order(u.id,dx,dz,false);
            break;
        }
        case 16: case 17: case 18: case 19: case 20:
            w.applyScenarioResourceAction(player,a.opcode,toInt(s[0])); break;
        case 21: applyOutcome(w, player, 1, 1); break;                        // Victory me + teammates
        case 22: applyOutcome(w, player, -1, 1); break;                       // Defeat me + teammates
        case 23: applyOutcome(w, player, 1, 2); break;                        // Victory for opponents
        case 24: applyOutcome(w, player, -1, 2); break;                       // Defeat for opponents
        case 25: {
            const auto it=ps.flags.find(flagKey(s[2]));
            if(it!=ps.flags.end())pending_.push_back({clock_,parsePlayer(s[0]),s[1]+std::to_string(it->second)+s[3]});
            break;
        }
        default: break;
    }
}

void ScenarioScript::start(World& w) {
    if (started_) return;
    started_ = true;
    evaluate(w, true);
}

void ScenarioScript::trace(int player,int group,int action,const crt::Rule* rule) noexcept {
    if(!trace_)return;
    try {trace_(clock_,player,group,action,rule);}
    catch(...) {trace_={};} // A failed diagnostic sink cannot interrupt lockstep.
}

void ScenarioScript::step(World& w, float) {
    if (!started_) start(w);
    ++clock_;
    ++ticks_;
    if (clock_ % 30 == 0) evaluate(w, false);
}

void ScenarioScript::evaluate(World& w, bool initial) {
    // Native 4ccf90: initial evaluation, then integer game-second boundaries.
    // Timers advance by one before rules; countdowns may become negative.
    for (auto& ps : state_)
        for (auto& [id, t] : ps.timers) {
            (void)id;
            t.value=std::bit_cast<int32_t>(uint32_t(t.value)+(t.countUp ? 1u : uint32_t(-1)));
        }
    for (int p = 0; p < int(players_.size()); ++p) {
        if (!participates(p) || w.player(p).defeated) continue;
        for (int g = 0; g < int(players_[size_t(p)].size()); ++g) {
            if (disabled_[size_t(p)][size_t(g)]) continue;
            const auto& grp = players_[size_t(p)][size_t(g)];
            bool all = true;
            for (const auto& c : grp.conditions)
                if (!evalCond(w, p, c, initial)) { all = false; break; }
            if (all) {
                trace(p,g,-1,nullptr);
                for(size_t i=0;i<grp.actions.size();++i) {
                    const auto& a=grp.actions[i];trace(p,g,int(i),&a);
                    runAction(w,p,g,a);
                }
            }
        }
    }
}

void ScenarioScript::unitDied(World& w, int id) {
    const Unit* u = w.unit(id);
    if (!u || !u->type) return;
    objectDied(u->type, u->player, u->lastHitPlayer);
}

void ScenarioScript::objectDied(const UnitType* type, int owner, int lastHitPlayer) {
    if (!type) return;
    const std::string& ty = type->id; // CRT operands identify FBI types, not localized display names
    if (owner >= 0 && owner < int(state_.size())) {
        auto& state=state_[size_t(owner)];
        if(state.firstLost.empty())state.firstLost=ty;
        state.lost[ty]++;
    }
    // Native 4cd626 uses the owner recorded by the hit, not the attacker's
    // current owner (the attacker may since have been captured or retired).
    const int killer=lastHitPlayer;
    if (killer>=0 && killer<int(state_.size()) && killer!=owner) {
        auto& state=state_[size_t(killer)];
        if(state.firstKilled.empty())state.firstKilled=ty;
        state.killed[ty]++;
    }
}

std::vector<ScenarioScript::Msg> ScenarioScript::drainMessages() {
    std::vector<Msg> out;
    out.swap(pending_);
    // Filter to the viewing player (or All Players); -1 view sees everything.
    if (view_ >= 0)
        out.erase(std::remove_if(out.begin(), out.end(),
                   [&](const Msg& m) { return m.player >= 0 && m.player != view_; }), out.end());
    return out;
}

void ScenarioScript::foldHash(uint64_t& h) const {
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ULL; };
    mix(uint64_t(ticks_));
    mix(participants_);
    for (auto outcome : outcomes_) mix(uint8_t(outcome));
    for (size_t p = 0; p < state_.size(); ++p) {
        const PState& ps = state_[p];
        for (const auto& [k, v] : ps.flags) {
            for (char c : k) mix(uint64_t(uint8_t(c)));
            mix(uint64_t(int64_t(v)));
        }
        for (const auto& [id, t] : ps.timers) {
            const uint32_t bits=uint32_t(t.value);
            mix((uint64_t(uint32_t(id)) << 1) ^ (uint64_t(bits) << 8) ^ uint64_t(t.countUp));
        }
        for (const auto& [k, v] : ps.killed) { mix(0x11); for (char c : k) mix(uint8_t(c)); mix(uint64_t(int64_t(v))); }
        for (const auto& [k, v] : ps.lost)   { mix(0x22); for (char c : k) mix(uint8_t(c)); mix(uint64_t(int64_t(v))); }
        for (const auto* key:{&ps.firstKilled,&ps.firstLost}) {
            mix(key->size());for(unsigned char c:*key)mix(c);
        }
        for (uint8_t d : disabled_[p]) mix(d);
    }
}

}  // namespace tak::sim
