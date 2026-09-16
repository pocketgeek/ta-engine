// aitool: run the skirmish AI headless (one AI player vs an idle opponent) and report
// how its economy + army develop and whether it marches on the enemy. A dev harness
// for tuning the AI -- not shipped in a game.
//
//   aitool <retail-install-dir> [map] [passive|easy|normal|hard|absurd] [seconds]

#include <cstdlib>
#include "ai/ai.h"
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

using namespace ta;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: aitool <install> [map] [passive|easy|normal|hard|absurd] [seconds]\n"); return 2; }
    std::string dataRoot = argv[1];
    std::string map = argc >= 3 ? argv[2] : "Inner Circle";
    std::string dstr = argc >= 4 ? argv[3] : "normal";
    int seconds = argc >= 5 ? std::atoi(argv[4]) : 180;
    ai::Difficulty diff = dstr == "passive" ? ai::Difficulty::Passive
                        : dstr == "easy"    ? ai::Difficulty::Easy
                        : dstr == "hard"    ? ai::Difficulty::Hard
                        : dstr == "absurd"  ? ai::Difficulty::Absurd
                                            : ai::Difficulty::Normal;

    hpi::Vfs vfs = hpi::mountRetailRoot(dataRoot);
    sim::TypeRegistry reg;
    sim::setupRegistry(reg, vfs);
    ai::Profile profile = ai::loadProfile(vfs);

    // 2-player 1v1: slot 0 = idle human (Aramon), slot 1 = the AI under test (Taros).
    sim::World w;
    w.setVisPlayer(-1);
    sim::MatchConfig cfg;
    cfg.vfs = &vfs;
    cfg.mapPath = hpi::findMap(vfs, map);
    if (cfg.mapPath.empty()) { std::fprintf(stderr, "aitool: map '%s' not found\n", map.c_str()); return 1; }
    // Slot 0 = opponent (idle, or a 2nd AI under TA_2AI), slot 1 = the AI under test.
    // Both carry the difficulty's income multiplier so an Absurd test is fair either way.
    // TA_FACTION=0..4 (ara/tar/ver/zon/cre) sets the AI-under-test's faction (default
    // 1 = Taros) so each faction's AI can be exercised.
    float mm = ai::incomeMultFor(diff);
    int fac = 1;
    if (const char* fe = std::getenv("TA_FACTION")) fac = std::clamp(std::atoi(fe), 0, 4);
    cfg.slots = {{true, 0, 0, mm}, {true, fac, 1, mm}};
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
    if (std::getenv("TA_PATHTEST")) {
        const sim::UnitType* kt = reg.find("tarknigh");
        int id = w.spawn(kt, spots[1].first, spots[1].second, 0, 1);
        bool am = std::getenv("TA_PATHTEST_MOVE") == nullptr;   // default: attackMove (what the AI uses)
        if (am) w.attackMove(id, ex, ez, false); else w.order(id, ex, ez, false);
        std::printf("PATHTEST (%s): knight %d from (%.0f,%.0f) -> (%.0f,%.0f)\n",
                    am ? "attackMove" : "move", id, spots[1].first, spots[1].second, ex, ez);
        for (int t = 0; t < 90 * 30; ++t) {
            w.tick(1.0f / 30.0f);
            if (t % (10 * 30) == 0) {
                const sim::Unit* u = w.unit(id);
                if (u) std::printf("  t=%2ds pos=(%.0f,%.0f) dist=%.0f orders=%zu\n",
                                   t / 30, u->x, u->z,
                                   std::sqrt((u->x - ex) * (u->x - ex) + (u->z - ez) * (u->z - ez)),
                                   u->orders.size());
            }
        }
        return 0;
    }

    ai::Controller ctl(1, reg, profile, 0x1234, diff, enemyStarts);
    // TA_2AI: put a second AI on player 0 (instead of an idle opponent) so we can see
    // whether two active AIs actually fight -- the realistic case.
    std::vector<std::pair<float, float>> starts0;
    if (!spots.empty()) starts0.push_back(spots[1]);   // AI-1's base is AI-0's enemy
    bool twoAi = std::getenv("TA_2AI") != nullptr;
    ai::Controller ctl0(0, reg, profile, 0x5678, diff, starts0);
    std::map<int, int> cmdCount;   // Cmd kind -> count
    auto sink = [&](const net::Command& c) {
        cmdCount[int(c.kind)]++;
        sim::applyCommand(w, reg, c);
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
                    float d = std::sqrt((u.x - ex) * (u.x - ex) + (u.z - ez) * (u.z - ez));
                    minDist = std::min(minDist, d);
                }
            }
        mana = w.player(1).metal.cur; income = w.player(1).metal.income;
        std::string s;
        for (const auto& [id, n] : comp) { s += id + ":" + std::to_string(n) + " "; }
        // storage too: a treasury pinned near zero can be a spending problem OR a
        // CAP problem, and the two want opposite fixes.
        std::printf("t=%3ds  income=%.0f mana=%.0f/%.0f  army=%d closest-to-enemy=%.0f  | %s\n",
                    t, income, mana, w.player(1).metal.storage, army, army ? minDist : -1, s.c_str());
        if (std::getenv("TA_AI_ECON")) {
            // Where is the income going? Dump every under-construction site and every
            // builder's job so we can see a stalled/over-expensive build freezing the economy.
            for (const auto& u : w.units()) {
                if (!u.alive() || u.player != 1 || !u.type) continue;
                if (u.underConstruction)
                    std::printf("      UC %s#%d hp=%.0f%% cost=%.0f btime=%.0f\n",
                                u.type->id.c_str(), u.id, 100.f * u.hp / std::max(u.type->maxHp, 1.f),
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
                    std::printf("      builder %s#%d site=%d prog=%.0f queue=[%s] repeat=%s\n",
                                u.type->id.c_str(), u.id, u.buildSiteId, u.buildProgress,
                                q.c_str(), u.repeatType ? u.repeatType->id.c_str() : "-");
                }
            }
        }
        if (std::getenv("TA_AI_UNITS"))
            for (const auto& u : w.units())
                if (u.alive() && u.player == 1 && u.type && u.type->canMove && !u.type->isBuilder) {
                    float d = std::sqrt((u.x - ex) * (u.x - ex) + (u.z - ez) * (u.z - ez));
                    bool reach = w.pathExists(u.type, ex, ez, u.x, u.z);
                    std::printf("      %s#%d pos=(%.0f,%.0f) dist=%.0f spd=%.1f reach-enemy=%s orders=%zu%s\n",
                                u.type->id.c_str(), u.id, u.x, u.z, d, u.speed, reach ? "Y" : "N", u.orders.size(),
                                u.orders.empty() ? "" : (u.orders.front().attackMove ? " [attackMove]" :
                                                         u.orders.front().targetId ? " [attack]" : " [move]"));
                }
    };

    const float dt = 1.0f / 30.0f;
    int totalTicks = seconds * 30;
    std::printf("=== AI %s on '%s' (enemy start %.0f,%.0f) ===\n", dstr.c_str(), map.c_str(), ex, ez);
    for (int t = 0; t < totalTicks; ++t) {
        ctl.tick(w, uint32_t(t), sink);
        if (twoAi) ctl0.tick(w, uint32_t(t), sink);
        w.tick(dt);
        if (t % (30 * 30) == 0) {
            report(t / 30);
            if (twoAi)
                std::printf("      [2AI] player0 kills=%d units=%d | player1 kills=%d\n",
                            w.player(0).kills,
                            [&]{ int n=0; for (const auto& u : w.units()) if (u.alive() && u.player==0) ++n; return n; }(),
                            w.player(1).kills);
        }
    }
    report(seconds);
    std::printf("commands issued:");
    for (const auto& [k, n] : cmdCount)
        std::printf(" kind%d=%d", k, n);
    std::printf("  (Build=%d Train=%d Move=%d AttackMove=%d Attack=%d Stop=%d)\n",
                int(net::Cmd::Build), int(net::Cmd::Train), int(net::Cmd::Move),
                int(net::Cmd::AttackMove), int(net::Cmd::Attack), int(net::Cmd::Stop));
    return 0;
}
