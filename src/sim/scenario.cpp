#include "sim/scenario.h"

#include "sim/sim.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace ta::sim {

namespace {
std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
int toInt(const std::string& s) { return std::atoi(s.c_str()); }
// cell coord -> world pixel centre (matches the placed-unit spawn convention).
float cellToWorld(float cell) { return cell * 16.0f + 8.0f; }
}  // namespace

ScenarioScript::ScenarioScript(const ta::crt::Scenario& scen, const TypeRegistry& reg,
                               int viewPlayer, int maxPlayer, int mapWCells, int mapHCells)
    : reg_(reg), view_(viewPlayer), maxPlayer_(std::max(1, maxPlayer)),
      mapW_(mapWCells), mapH_(mapHCells), regions_(scen.regions) {
    // Fold each .crt player's groups into the clamped world-slot range so a
    // scenario authored for 8 players still runs on a 4-slot scenario world.
    players_.assign(size_t(maxPlayer_), {});
    for (size_t p = 0; p < scen.players.size(); ++p) {
        int slot = std::min(int(p), maxPlayer_ - 1);
        for (const auto& g : scen.players[p])
            players_[size_t(slot)].push_back(g);
    }
    disabled_.resize(players_.size());
    fired_.resize(players_.size());
    state_.resize(players_.size());
    for (size_t p = 0; p < players_.size(); ++p) {
        disabled_[p].assign(players_[p].size(), 0);
        fired_[p].assign(players_[p].size(), 0);
    }
}

const UnitType* ScenarioScript::findType(const std::string& name) const {
    return reg_.find(lower(name));
}

const ta::crt::Region* ScenarioScript::region(const std::string& name) const {
    std::string want = lower(name);
    if (want.empty() || want == "anywhere") return nullptr;   // whole map
    for (const auto& r : regions_)
        if (lower(r.name) == want) return &r;
    return nullptr;
}

bool ScenarioScript::inRegion(World&, float x, float z, const std::string& loc) const {
    const ta::crt::Region* r = region(loc);
    int cx = int(x / 16.0f), cz = int(z / 16.0f);
    if (!r) return cx >= 0 && cz >= 0 && cx < mapW_ && cz < mapH_;   // whole map
    int lox = std::min(r->x1, r->x2), hix = std::max(r->x1, r->x2);
    int loz = std::min(r->z1, r->z2), hiz = std::max(r->z1, r->z2);
    return cx >= lox && cx <= hix && cz >= loz && cz <= hiz;
}

void ScenarioScript::regionCenter(const std::string& loc, float& x, float& z) const {
    const ta::crt::Region* r = region(loc);
    if (!r) { x = cellToWorld(float(mapW_) * 0.5f); z = cellToWorld(float(mapH_) * 0.5f); return; }
    x = cellToWorld(float(r->x1 + r->x2) * 0.5f);
    z = cellToWorld(float(r->z1 + r->z2) * 0.5f);
}

int ScenarioScript::parsePlayer(const std::string& s) const {
    std::string lo = lower(s);
    if (lo.rfind("all", 0) == 0) return -1;             // All Players
    // Trailing number: "Player 3" -> slot 2 (clamped).
    int i = int(s.size()) - 1, val = 0, mul = 1;
    bool any = false;
    while (i >= 0 && std::isdigit(static_cast<unsigned char>(s[size_t(i)]))) {
        val += (s[size_t(i)] - '0') * mul; mul *= 10; --i; any = true;
    }
    if (!any) return -1;
    return std::clamp(val - 1, 0, maxPlayer_ - 1);
}

int ScenarioScript::countControl(World& w, int player, const UnitType* t,
                                 const std::string& loc) const {
    if (!t) return 0;
    int n = 0;
    for (const auto& u : w.units())
        if (u.alive() && u.player == player && u.type == t &&
            inRegion(w, u.x, u.z, loc)) ++n;
    return n;
}

void ScenarioScript::forceDefeatOthers(World& w, int player) {
    for (int q = 0; q < w.numPlayers(); ++q)
        if (q != player && !w.allied(player, q)) w.forceDefeat(q);
}
void ScenarioScript::forceDefeatTeam(World& w, int player, bool allies) {
    for (int q = 0; q < w.numPlayers(); ++q)
        if (q == player || (allies && w.allied(player, q))) w.forceDefeat(q);
}

// -------- conditions (opcode space per the RE table) --------
bool ScenarioScript::evalCond(World& w, int player, const ta::crt::Rule& c) {
    const auto& s = c.slot;
    PState& ps = state_[size_t(player)];
    switch (c.opcode) {
        case 0:  return clock_ == 0.0f;                         // Start of game
        case 1:  return clock_ > float(toInt(s[0]));            // Gametime > v
        case 2:  return clock_ < float(toInt(s[0]));            // Gametime < v
        case 3: { auto it = ps.timers.find(toInt(s[0]));   // Timer > v (unset => false)
                  return it != ps.timers.end() && it->second.value > float(toInt(s[1])); }
        case 4: { auto it = ps.timers.find(toInt(s[0]));   // Timer < v (unset => false)
                  return it != ps.timers.end() && it->second.value < float(toInt(s[1])); }
        case 5:  return ps.killed[lower(s[1])] > toInt(s[0]);   // killed more than N X
        case 6:  return ps.killed[lower(s[1])] < toInt(s[0]);   // killed less than N X
        case 9:  return ps.lost[lower(s[1])] > toInt(s[0]);     // lost more than N X
        case 10: return ps.lost[lower(s[1])] < toInt(s[0]);     // lost less than N X
        case 7: case 8: case 11: case 12: {                    // killed/lost most/least X
            const UnitType* t = findType(s[0]);
            std::map<std::string, int32_t>& mine = (c.opcode <= 8) ? ps.killed : ps.lost;
            bool most = (c.opcode == 7 || c.opcode == 11);
            int32_t v = mine[lower(s[0])];
            (void)t;
            bool win = true;
            for (size_t q = 0; q < state_.size(); ++q) {
                if (int(q) == player) continue;
                int32_t o = ((c.opcode <= 8) ? state_[q].killed : state_[q].lost)[lower(s[0])];
                if (most ? (o >= v) : (o <= v)) win = false;
            }
            return win && v > 0;
        }
        case 13: case 14: {                                    // control the most/least X at L
            const UnitType* t = findType(s[0]);
            int mine = countControl(w, player, t, s[1]);
            bool most = (c.opcode == 13);
            bool win = mine > 0 || !most;
            for (int q = 0; q < maxPlayer_; ++q) {
                if (q == player) continue;
                int o = countControl(w, q, t, s[1]);
                if (most ? (o >= mine) : (o < mine)) win = false;
            }
            return win;
        }
        case 15: return countControl(w, player, findType(s[0]), s[2]) > toInt(s[1]);   // control > N X at L
        case 16: return countControl(w, player, findType(s[0]), s[2]) < toInt(s[1]);   // control < N X at L
        case 17: return ps.flags[s[0]] > toInt(s[1]);          // Flag f > v
        case 18: return ps.flags[s[0]] < toInt(s[1]);          // Flag f < v
        case 19: return true;                                  // Always
        case 20: return false;                                 // Never
        case 21: case 22: {                                    // opponents left </> N
            int opp = 0;
            for (int q = 0; q < w.numPlayers(); ++q)
                if (q != player && !w.allied(player, q) && !w.player(q).defeated) ++opp;
            return c.opcode == 21 ? opp < toInt(s[0]) : opp > toInt(s[0]);
        }
        case 23: {                                             // Random: N percent true
            rng_ = rng_ * 6364136223846793005ULL + 1442695040888963407ULL;
            uint32_t roll = uint32_t((rng_ >> 33) % 100);
            return int(roll) < toInt(s[0]);
        }
        case 24: return w.player(player).mana < float(toInt(s[0]));   // Resources < v
        case 25: return w.player(player).mana > float(toInt(s[0]));   // Resources > v
        default: return false;
    }
}

// -------- actions (independent opcode space) --------
void ScenarioScript::runAction(World& w, int player, int group, const ta::crt::Rule& a) {
    const auto& s = a.slot;
    PState& ps = state_[size_t(player)];
    switch (a.opcode) {
        case 0: ps.timers[toInt(s[0])] = {float(toInt(s[1])), false}; break;   // countdown t=v
        case 1: ps.timers[toInt(s[0])] = {float(toInt(s[1])), true};  break;   // countup t=v
        case 2: ps.flags[s[0]] = toInt(s[1]); break;                           // set flag f=v
        case 3: ps.flags[s[1]] += toInt(s[0]); break;                          // add v to flag f
        case 4: ps.flags[s[1]] -= toInt(s[0]); break;                          // sub v from flag f
        case 5: forceDefeatOthers(w, player); break;                           // Victory for me
        case 6: w.forceDefeat(player); break;                                  // Defeat for me
        case 7: {                                                              // Create X at L
            if (const UnitType* t = findType(s[0])) {
                float x, z; regionCenter(s[1], x, z);
                w.spawn(t, x, z, 0.0f, player);
            }
            break;
        }
        case 8:                                                                // Destroy X at L
            if (const UnitType* t = findType(s[0]))
                for (auto& u : w.units())
                    if (u.alive() && u.player == player && u.type == t && inRegion(w, u.x, u.z, s[1]))
                        u.hp = 0;
            break;
        case 9:                                                                // I own all X at L
            if (const UnitType* t = findType(s[0]))
                for (auto& u : w.units())
                    if (u.alive() && u.type == t && inRegion(w, u.x, u.z, s[1])) u.player = player;
            break;
        case 10:                                                               // Heal X by v at L
            if (const UnitType* t = findType(s[0]))
                for (auto& u : w.units())
                    if (u.alive() && u.player == player && u.type == t && inRegion(w, u.x, u.z, s[2]))
                        u.hp = std::min(u.type->maxHp, u.hp + u.type->maxHp * float(toInt(s[1])) / 100.0f);
            break;
        case 11:                                                               // Damage X by v at L
            if (const UnitType* t = findType(s[0]))
                for (auto& u : w.units())
                    if (u.alive() && u.player == player && u.type == t && inRegion(w, u.x, u.z, s[2]))
                        u.hp = std::max(0.0f, u.hp - u.type->maxHp * float(toInt(s[1])) / 100.0f);
            break;
        case 12: showClock_ = true; break;                                     // Display gameclock
        case 13: pending_.push_back({clock_, parsePlayer(s[0]), s[1]}); break;  // Display P text
        case 14: disabled_[size_t(player)][size_t(group)] = 1; break;          // Disable rule
        case 15:                                                               // Move all X at L1 to L2
            if (const UnitType* t = findType(s[0])) {
                float dx, dz; regionCenter(s[2], dx, dz);
                for (auto& u : w.units())
                    if (u.alive() && u.player == player && u.type == t && inRegion(w, u.x, u.z, s[1]))
                        w.order(u.id, dx, dz, false);
            }
            break;
        case 16: w.player(player).storage = float(toInt(s[0])); break;         // resource limit
        case 17: w.player(player).mana = float(toInt(s[0])); break;            // resources = v
        case 18: w.player(player).mana += float(toInt(s[0])); break;           // add v
        case 19: w.player(player).mana = std::max(0.0f, w.player(player).mana - float(toInt(s[0]))); break;
        case 20: break;                                                        // resources normal (no-op)
        case 21: forceDefeatOthers(w, player); break;                          // Victory me + teammates
        case 22: forceDefeatTeam(w, player, true); break;                      // Defeat me + teammates
        case 23: forceDefeatTeam(w, player, true); break;                      // Victory for opponents
        case 24: forceDefeatOthers(w, player); break;                          // Defeat for opponents
        case 25: pending_.push_back({clock_, parsePlayer(s[0]), s[1]}); break;  // Display P text flag text
        default: break;
    }
}

void ScenarioScript::start(World&) { started_ = true; }

void ScenarioScript::step(World& w, float dt) {
    // Advance per-player timers first (so a "Timer < 1" fires after the tick the
    // countdown was set, not the same tick).
    for (auto& ps : state_)
        for (auto& [id, t] : ps.timers) {
            (void)id;
            t.value += t.countUp ? dt : -dt;
            if (!t.countUp && t.value < 0) t.value = 0;
        }

    for (int p = 0; p < int(players_.size()); ++p) {
        if (p < w.numPlayers() && w.player(p).defeated) continue;   // skip dead players
        for (int g = 0; g < int(players_[size_t(p)].size()); ++g) {
            if (disabled_[size_t(p)][size_t(g)]) continue;
            const auto& grp = players_[size_t(p)][size_t(g)];
            // A rule fires its actions once on the rising edge of "all conditions
            // true" (the retail model: e.g. "set countdown timer 0 to 180" runs
            // once, then the timer counts down; authors gate repeats with flags).
            bool all = !grp.conditions.empty();
            for (const auto& c : grp.conditions)
                if (!evalCond(w, p, c)) { all = false; break; }
            uint8_t& fired = fired_[size_t(p)][size_t(g)];
            if (all && !fired) {
                for (const auto& a : grp.actions) runAction(w, p, g, a);
                fired = 1;
            } else if (!all) {
                fired = 0;
            }
        }
    }
    clock_ += dt;
    ++ticks_;
}

void ScenarioScript::unitDied(World& w, int id) {
    const Unit* u = w.unit(id);
    if (!u || !u->type) return;
    std::string ty = lower(u->type->name);
    if (u->player >= 0 && u->player < int(state_.size())) state_[size_t(u->player)].lost[ty]++;
    // Kill credit to the last attacker's owner.
    if (const Unit* k = w.unit(u->lastHitBy))
        if (k->player >= 0 && k->player < int(state_.size())) state_[size_t(k->player)].killed[ty]++;
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
    mix(rng_);
    for (size_t p = 0; p < state_.size(); ++p) {
        const PState& ps = state_[p];
        for (const auto& [k, v] : ps.flags) {
            for (char c : k) mix(uint64_t(uint8_t(c)));
            mix(uint64_t(int64_t(v)));
        }
        for (const auto& [id, t] : ps.timers) {
            uint32_t bits; std::memcpy(&bits, &t.value, 4);
            mix((uint64_t(uint32_t(id)) << 1) ^ (uint64_t(bits) << 8) ^ uint64_t(t.countUp));
        }
        for (const auto& [k, v] : ps.killed) { mix(0x11); for (char c : k) mix(uint8_t(c)); mix(uint64_t(int64_t(v))); }
        for (const auto& [k, v] : ps.lost)   { mix(0x22); for (char c : k) mix(uint8_t(c)); mix(uint64_t(int64_t(v))); }
        for (uint8_t d : disabled_[p]) mix(d);
        for (uint8_t f : fired_[p]) mix(uint64_t(f) << 1);
    }
}

}  // namespace ta::sim
