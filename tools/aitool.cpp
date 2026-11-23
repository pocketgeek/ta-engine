// aitool: run the skirmish AI headless (one AI player vs an idle opponent) and report
// how its economy + army develop and whether it marches on the enemy. A dev harness
// for tuning the AI -- not shipped in a game.
//
//   aitool <retail-install-dir> [map] [defensive|easy|normal|hard|absurd|absurdly-defensive|retail] [seconds]
//          [standard|crusades]
//
// `retail` is the lobby's RETAIL level (internally Classic, the retail AI); the
// older spellings `default` and `classic` are still accepted.

#include <cstdlib>
#include "ai/ai.h"
#include "ai/builtinprofile.h"
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>

using namespace tak;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: aitool <install> [map] [defensive|easy|normal|hard|absurd|absurdly-defensive|retail] [seconds] [standard|crusades]\n"); return 2; }
    std::string dataRoot = argv[1];
    std::string map = argc >= 3 ? argv[2] : "Inner Circle";
    std::string dstr = argc >= 4 ? argv[3] : "normal";
    int seconds = argc >= 5 ? std::atoi(argv[4]) : 180;
    ai::Difficulty diff = dstr == "defensive" ? ai::Difficulty::Defensive
                        : dstr == "passive" ? ai::Difficulty::Passive
                        : dstr == "easy"    ? ai::Difficulty::Easy
                        : dstr == "hard"    ? ai::Difficulty::Hard
                        : dstr == "absurd"  ? ai::Difficulty::Absurd
                        : dstr == "absurdly-defensive" ? ai::Difficulty::AbsurdlyDefensive
                        : (dstr == "retail" || dstr == "default" || dstr == "classic") ? ai::Difficulty::Classic
                                            : ai::Difficulty::Normal;

    hpi::Vfs vfs = hpi::mountRetailRoot(dataRoot);
    sim::TypeRegistry reg;
    sim::setupRegistry(reg, vfs, argc>5 && std::string(argv[5])=="crusades");
    const ai::Profile& profile = ai::builtinProfile(diff);

    // 2-player 1v1: slot 0 = idle human (Aramon), slot 1 = the AI under test (Taros).
    sim::World w;
    sim::MatchConfig cfg;
    cfg.vfs = &vfs;
    cfg.mapPath = hpi::findMap(vfs, map);
    // The lobby's default per-player unit cap; TAK_UNIT_CAP overrides (0 = none).
    cfg.unitCap = std::getenv("TAK_UNIT_CAP") ? std::atoi(std::getenv("TAK_UNIT_CAP")) : 2000;
    if (cfg.mapPath.empty()) { std::fprintf(stderr, "aitool: map '%s' not found\n", map.c_str()); return 1; }
    // Slot 0 = opponent (idle, or a 2nd AI under TAK_2AI), slot 1 = the AI under test.
    // Only AI slots receive the difficulty income multiplier.
    // TAK_FACTION=0..4 (ara/tar/ver/zon/cre) sets the AI-under-test's faction (default
    // 1 = Taros) so each faction's AI can be exercised.
    float mm = ai::incomeMultFor(diff);
    int fac = 1;
    if (const char* fe = std::getenv("TAK_FACTION")) fac = std::clamp(std::atoi(fe), 0, 4);
    cfg.slots = {{true, 0, 0, 1.0f}, {true, fac, 1, mm, true, ai::defensiveLevel(diff)}};
    if (std::getenv("TAK_2AI")) { cfg.slots[0].manaMult=mm;cfg.slots[0].automaticGates=true;
        cfg.slots[0].defensiveAi=ai::defensiveLevel(diff); }
    auto spots = sim::setupMatch(w, reg, cfg);
    std::vector<std::pair<float, float>> enemyStarts;
    if (!spots.empty()) enemyStarts.push_back(spots[0]);   // the human's start
    float ex = enemyStarts.empty() ? 0 : enemyStarts[0].first;
    float ez = enemyStarts.empty() ? 0 : enemyStarts[0].second;

    // How many mana deposits exist, and how many are near the AI's start (a rough
    // proxy for how much economy it can build).
    int nearAi = 0;
    for (const auto& [sx, sz] : w.manaSpots()) {
        float dx = sx - spots[1].first, dz = sz - spots[1].second;
        if (dx * dx + dz * dz < 900.f * 900.f) ++nearAi;
    }
    std::printf("map mana spots: %zu total, %d within 900px of the AI start\n",
                w.manaSpots().size(), nearAi);
    // Is the enemy base reachable by ground from the AI base? (a common attack blocker)
    const sim::UnitType* ground = reg.find("tarknigh");
    if (ground)
        std::printf("enemy base reachable by ground (tarknigh): %s  (AI start %.0f,%.0f -> enemy %.0f,%.0f)\n",
                    w.pathExists(ground, ex, ez, spots[1].first, spots[1].second) ? "YES" : "NO",
                    spots[1].first, spots[1].second, ex, ez);

    // Controlled path test: spawn a knight at the AI base, order it straight at the
    // enemy base, and watch it travel -- isolates long-range nav from the AI logic.
    if (std::getenv("TAK_PATHTEST")) {
        const sim::UnitType* kt = reg.find("tarknigh");
        int id = w.spawn(kt, spots[1].first, spots[1].second, 0, 1);
        bool am = std::getenv("TAK_PATHTEST_MOVE") == nullptr;   // default: attackMove (what the AI uses)
        if (am) w.attackMove(id, ex, ez, false); else w.order(id, ex, ez, false);
        std::printf("PATHTEST (%s): knight %d from (%.0f,%.0f) -> (%.0f,%.0f)\n",
                    am ? "attackMove" : "move", id, spots[1].first, spots[1].second, ex, ez);
        for (int t = 0; t < 90 * 30; ++t) {
            w.tick(1.0f / 30.0f);
            if (t % (10 * 30) == 0) {
                const sim::Unit* u = w.unit(id);
                if (u) std::printf("  t=%2ds pos=(%.0f,%.0f) dist=%.0f orders=%zu\n",
                                   t / 30, u->x.toFloat(), u->z.toFloat(),
                                   std::sqrt((u->x.toFloat() - ex) * (u->x.toFloat() - ex) + (u->z.toFloat() - ez) * (u->z.toFloat() - ez)),
                                   u->orders.size());
            }
        }
        return 0;
    }

    ai::Controller ctl(1, reg, profile, 0x1234, diff, enemyStarts);
    // TAK_2AI: put a second AI on player 0 (instead of an idle opponent) so we can see
    // whether two active AIs actually fight -- the realistic case.
    std::vector<std::pair<float, float>> starts0;
    if (!spots.empty()) starts0.push_back(spots[1]);   // AI-1's base is AI-0's enemy
    bool twoAi = std::getenv("TAK_2AI") != nullptr;
    ai::Controller ctl0(0, reg, profile, 0x5678, diff, starts0);
    std::map<int, int> cmdCount;   // Cmd kind -> count
    int fullSeconds = 0;           // seconds the AI's store sat at 90% or more
    std::map<std::string,std::set<int>> completedWater,completedAmphibious;
    int navalAttacks=0,amphibiousAttacks=0;
    auto sink = [&](const net::Command& c) {
        cmdCount[int(c.kind)]++;
        if (c.player==1 && (c.kind==net::Cmd::Attack || c.kind==net::Cmd::AttackMove))
            if (const auto* u=w.unit(c.unitId);u && u->type && !u->type->canFly) {
                if (u->type->domain==sim::UnitType::Domain::Water) ++navalAttacks;
                if (u->type->domain==sim::UnitType::Domain::Hover) ++amphibiousAttacks;
            }
        if (std::getenv("TAK_AI_PICK") && c.kind==net::Cmd::Build)
            std::fprintf(stderr,"    BUILD %s by #%d at (%.2f,%.2f)\n",c.type,c.unitId,c.x,c.z);
        sim::applyCommand(w, reg, c);
        // Did the sim take the order? A Build it refuses leaves the builder idle,
        // and the AI re-issues the same pick next think.
        if (std::getenv("TAK_AI_PICK") && c.kind==net::Cmd::Build)
            if (const auto* u=w.unit(c.unitId))
                std::fprintf(stderr,"      -> orders=%zu site=%d\n",u->orders.size(),u->buildSiteId);
    };

    auto report = [&](int t) {
        std::map<std::string, int> comp;
        int army = 0; float mana = 0, income = 0;
        float minDist = 1e9f;   // closest army unit to the enemy base (shows advances)
        for (const auto& u : w.units())
            if (u.alive() && u.player == 1 && u.type) {
                comp[u.type->id]++;
                if (u.type->canMove && !u.type->isBuilder) {
                    ++army;
                    float d = std::sqrt((u.x.toFloat() - ex) * (u.x.toFloat() - ex) + (u.z.toFloat() - ez) * (u.z.toFloat() - ez));
                    minDist = std::min(minDist, d);
                }
            }
        mana = w.player(1).mana; income = w.player(1).income;
        std::string s;
        for (const auto& [id, n] : comp) { s += id + ":" + std::to_string(n) + " "; }
        // storage too: a treasury pinned near zero can be a spending problem OR a
        // CAP problem, and the two want opposite fixes.
        std::printf("t=%3ds  income=%.0f mana=%.0f/%.0f  army=%d closest-to-enemy=%.0f  | %s\n",
                    t, income, mana, w.player(1).storage, army, army ? minDist : -1, s.c_str());
        // One machine-readable line per report: the empire's size against its income.
        int units = 0, structs = 0, builders = 0, unfinished = 0;
        for (const auto& u : w.units())
            if (u.alive() && u.player == 1 && u.type) {
                ++units;
                if (u.type->isStructure()) ++structs;
                else if (u.type->isBuilder) ++builders;
                if (u.underConstruction) ++unfinished;
            }
        std::printf("SUM t=%d income=%.1f mana=%.0f capacity=%.0f units=%d structs=%d mobile=%d builders=%d "
                    "unfinished=%d fullsecs=%d cap=%d\n",
                    t, income, mana, w.player(1).manaCapacity(), units, structs, units - structs, builders,
                    unfinished, fullSeconds, w.unitCap());
        if (std::getenv("TAK_AI_ECON")) {
            // Where is the income going? Dump every under-construction site and every
            // builder's job so we can see a stalled/over-expensive build freezing the economy.
            for (const auto& u : w.units()) {
                if (!u.alive() || u.player != 1 || !u.type) continue;
                if (u.underConstruction)
                    std::printf("      UC %s#%d hp=%.0f%% cost=%.0f btime=%.0f\n",
                                u.type->id.c_str(), u.id, 100.f * u.hp.toFloat() / std::max(float(u.type->maxHp), 1.f),
                                u.type->buildCost, u.type->buildTime);
                // Producer eligibility, mirroring Controller::tick's own test. A
                // builder is only offered work when it is idle AND siteless, so a
                // faction with few builders that are always on a site can never
                // train anything -- which is what a "stalled" economy looks like
                // from outside.
                if (u.type->isBuilder && !u.type->isStructure())
                    std::printf("      cand %s#%d uc=%d orders=%zu site=%d -> %s\n",
                                u.type->id.c_str(), u.id, int(u.underConstruction),
                                u.orders.size(), u.buildSiteId,
                                (!u.underConstruction && u.orders.empty() && u.buildSiteId == 0)
                                    ? "PRODUCER" : "busy"),
                    std::printf("           menu(%s) = %zu entries\n", u.type->id.c_str(),
                                reg.buildable(u.type->id).size());
                if (u.type->isBuilder && (u.buildSiteId || !u.buildQueue.empty() || u.repeatType)) {
                    std::string q;
                    for (const auto* qt : u.buildQueue) if (qt) q += qt->id + " ";
                    std::printf("      builder %s#%d site=%d prog=%d queue=[%s] repeat=%s\n",
                                u.type->id.c_str(), u.id, u.buildSiteId, u.buildProgress,
                                q.c_str(), u.repeatType ? u.repeatType->id.c_str() : "-");
                }
            }
        }
        if (std::getenv("TAK_AI_UNITS"))
            for (const auto& u : w.units())
                if (u.alive() && u.player == 1 && u.type && u.type->canMove && !u.type->isBuilder) {
                    float d = std::sqrt((u.x.toFloat() - ex) * (u.x.toFloat() - ex) + (u.z.toFloat() - ez) * (u.z.toFloat() - ez));
                    bool reach = w.pathExists(u.type, ex, ez, u.x.toFloat(), u.z.toFloat());
                    std::printf("      %s#%d pos=(%.0f,%.0f) dist=%.0f spd=%.1f reach-enemy=%s orders=%zu%s\n",
                                u.type->id.c_str(), u.id, u.x.toFloat(), u.z.toFloat(), d, u.speed.toFloat(), reach ? "Y" : "N", u.orders.size(),
                                u.orders.empty() ? "" : (u.orders.front().attackMove ? " [attackMove]" :
                                                         u.orders.front().targetId ? " [attack]" : " [move]"));
                }
    };

    const float dt = 1.0f / 30.0f;
    const int reportEvery = std::getenv("TAK_AI_EVERY") ? std::max(1, std::atoi(std::getenv("TAK_AI_EVERY"))) : 30;
    int totalTicks = seconds * 30;
    std::printf("=== AI %s on '%s' (enemy start %.0f,%.0f) ===\n", dstr.c_str(), map.c_str(), ex, ez);
    for (int t = 0; t < totalTicks; ++t) {
        ctl.tick(w, uint32_t(t), sink);
        if (twoAi) ctl0.tick(w, uint32_t(t), sink);
        w.tick(dt);
        if (t%30==0 && w.player(1).manaCapacity() > 0.01f &&
            w.player(1).mana >= w.player(1).manaCapacity() * 0.9f) ++fullSeconds;
        if (t%30==0) for (const auto& u:w.units()) {
            if (!u.alive() || u.player!=1 || !u.type || u.underConstruction ||
                u.type->isStructure() || u.type->canFly || u.type->isBuilder) continue;
            if (u.type->domain==sim::UnitType::Domain::Water) completedWater[u.type->id].insert(u.id);
            if (u.type->domain==sim::UnitType::Domain::Hover) completedAmphibious[u.type->id].insert(u.id);
        }
        if (t % (30 * reportEvery) == 0) {
            report(t / 30);
            if (twoAi)
                std::printf("      [2AI] player0 kills=%d units=%d | player1 kills=%d\n",
                            w.player(0).kills,
                            [&]{ int n=0; for (const auto& u : w.units()) if (u.alive() && u.player==0) ++n; return n; }(),
                            w.player(1).kills);
        }
    }
    report(seconds);
    for (const auto& [label,units]:{std::pair{"naval",&completedWater},std::pair{"amphibious",&completedAmphibious}}) {
        int total=0;for (const auto& [id,seen]:*units) total+=int(seen.size());
        std::printf("%s: completed=%d attack_orders=%d |",label,total,
            units==&completedWater?navalAttacks:amphibiousAttacks);
        for (const auto& [id,seen]:*units) std::printf(" %s:%zu",id.c_str(),seen.size());
        std::puts("");
    }
    int deposits=0,remoteDeposits=0,remoteDefenses=0;float furthest=0;
    for(const auto& u:w.units()) {
        if(!u.alive() || u.player!=1 || !u.type || u.underConstruction)continue;
        const float dx=u.x.toFloat()-spots[1].first,dz=u.z.toFloat()-spots[1].second;
        const float distance=std::sqrt(dx*dx+dz*dz);
        if(u.type->onMana) {++deposits;if(distance>900)++remoteDeposits;furthest=std::max(furthest,distance);}
        if(distance>900 && u.type->isStructure() && u.type->weapon.damage>0)++remoteDefenses;
    }
    std::printf("expansion: completed_deposits=%d remote_deposits=%d furthest=%.0f remote_defenses=%d\n",
        deposits,remoteDeposits,furthest,remoteDefenses);
    std::printf("commands issued:");
    for (const auto& [k, n] : cmdCount)
        std::printf(" kind%d=%d", k, n);
    std::printf("  (Build=%d Train=%d Move=%d AttackMove=%d Attack=%d Stop=%d)\n",
                int(net::Cmd::Build), int(net::Cmd::Train), int(net::Cmd::Move),
                int(net::Cmd::AttackMove), int(net::Cmd::Attack), int(net::Cmd::Stop));
    return 0;
}
