// World side of the Total Annihilation economy (sim/taeconomy.h,
// docs/retail-economy.md). Every peer settles every player, in player order, once
// every 30 ticks; all arithmetic is float32 in the spec's order.
#include "sim/sim.h"
#include "sim/taeconomy.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ta::sim {

using namespace econ;

void World::stampFeatureMetal(int cx, int cz, int fx, int fz, int metal, int mapW, int mapH) {
    if (metal == 0 || mapW <= 0 || mapH <= 0) return;
    if (cellMetal_.empty() || cellMetalW_ != mapW || cellMetalH_ != mapH) {
        cellMetalW_ = mapW; cellMetalH_ = mapH;
        cellMetal_.assign(size_t(mapW) * size_t(mapH), surfaceMetal_);
    }
    const uint8_t v = uint8_t(int32_t(metal));   // (u8)(int)metal
    for (int z = cz; z < cz + std::max(1, fz); ++z)
        for (int x = cx; x < cx + std::max(1, fx); ++x)
            if (x >= 0 && z >= 0 && x < mapW && z < mapH) cellMetal_[size_t(z) * size_t(mapW) + size_t(x)] = v;
}

int World::cellMetal(int cx, int cz) const {
    if (cellMetal_.empty()) return (cx < 0 || cz < 0 || (terW_ > 0 && (cx >= terW_ || cz >= terH_))) ? 0 : surfaceMetal_;
    if (cx < 0 || cz < 0 || cx >= cellMetalW_ || cz >= cellMetalH_) return 0;
    return cellMetal_[size_t(cz) * size_t(cellMetalW_) + size_t(cx)];
}

void World::setStartResources(int player, float energy, float metal) {
    if (player < 0 || player >= numPlayers()) return;
    auto& b = players_[size_t(player)].books;
    b.stored[E] = energy;
    b.stored[M] = metal;
    b.base[E] = std::max(energy, kBaseStorageMin);
    b.base[M] = std::max(metal, kBaseStorageMin);
    b.hasBase = true;
    b.nextSettle = tickCounter_;
}

// One tick of a builder's work on a site (spec "Build step"): amount =
// floor(WorkerTime / 30) of BuildTime; the costs of that fraction are requested
// together; a builder in debt does nothing. Returns the fraction done this tick,
// or a negative value when stalled. Completion is site.buildRemaining == 0.
float World::econBuildFraction(Unit& builder, Unit& site) {
    const UnitType& t = *site.type;
    if (!site.buildStarted) { site.buildStarted = true; site.buildRemaining = 1.0f; }
    const float rem = site.buildRemaining;
    if (rem <= 0) return 0;
    const float amt = std::floor(float(int32_t(builder.type->workerTime)) / 30.0f);
    const float bt = std::max(t.buildTime, 1.0f);
    const float c = std::clamp(rem - amt / bt, 0.0f, 1.0f);
    const float d = rem - c;
    builder.books.noCloakUntil = tickCounter_ + 150;
    if (!requestBoth(builder.books, t.buildCostEnergy * d, t.buildCostMetal * d)) return -1.0f;
    const int hpMax = t.maxHp;
    const int gain = int32_t(rem * float(hpMax)) - int32_t(c * float(hpMax));
    site.hp = Fixed::fromInt(std::min(site.hp.floorInt() + gain, hpMax));
    site.buildRemaining = c;
    return d;
}

bool World::econBuildStep(Unit& builder, Unit& site) {
    const float d = econBuildFraction(builder, site);
    return d >= 0 && site.buildRemaining <= 0;
}

bool World::econRepairStep(Unit& builder, Unit& target) {
    builder.books.noCloakUntil = tickCounter_ + 150;
    if (!request(builder.books.r[E], 1.0f)) return false;
    target.hp = Fixed::fromInt(std::min(target.hp.floorInt() + 1, target.type->maxHp));
    return true;
}

bool World::econPayShot(Unit& u, const Weapon& w) {
    if (w.energyPerShot <= 0 && w.metalPerShot <= 0) return true;
    if (u.player < 0 || u.player >= numPlayers()) return false;
    auto& p = players_[size_t(u.player)].books;
    if (p.stored[E] < w.energyPerShot || p.stored[M] < w.metalPerShot) return false;
    payNow(p, u.books.r[E], E, w.energyPerShot);
    payNow(p, u.books.r[M], M, w.metalPerShot);
    return true;
}

void World::econCredit(int player, float energy, float metal) {
    if (player < 0 || player >= numPlayers()) return;
    auto& p = players_[size_t(player)].books;
    p.own[E].prod += p.mult * energy;
    p.own[M].prod += p.mult * metal;
}

void World::taSettle(int player) {
    auto& P = players_[size_t(player)].books;
    P.cap[E] = P.cap[M] = 0;
    Sums sums;
    const float mult = P.mult;
    const float windF = taWindFactor(wind_);
    for (auto& u : units_) {
        if (u.player != player || !u.alive() || !u.type) continue;
        const UnitType& t = *u.type;
        auto& b = u.books;
        const bool built = !u.underConstruction;
        if (built) {
            const float eu = t.energyUse;
            auto consume = [&]() {
                if (eu >= 0) {
                    b.r[E].req += eu;
                    const bool ok = b.r[E].debt <= 0;
                    if (ok) b.r[E].newReq += eu;
                    return ok;
                }
                b.r[E].prod += mult * (-eu);
                return false;
            };
            if (t.isStructure()) {
                if (u.active) {
                    const bool ok = consume();
                    if (t.extractsMetal > 0) {
                        if (b.extractRate == 0) {   // once, at the first settle after creation
                            const int cx = int(u.x.toFloat()) / 16 - t.footX / 2;
                            const int cz = int(u.z.toFloat()) / 16 - t.footZ / 2;
                            float sum = 0;
                            for (int z = 0; z < t.footZ; ++z)
                                for (int x = 0; x < t.footX; ++x) sum += float(cellMetal(cx + x, cz + z) + 1);
                            b.extractRate = t.extractsMetal * sum;
                        }
                        if (ok) b.r[M].prod += mult * b.extractRate;
                    } else if (t.makesMetal) {
                        if (ok) b.r[M].prod += mult * float(t.makesMetal);
                    } else if (t.windGenerator > 0) {
                        b.r[E].prod += mult * (windF * t.windGenerator);
                    } else if (t.tidalGenerator > 0) {
                        b.r[E].prod += mult * (wind_tidal_ * t.tidalGenerator);
                    }
                }
            } else if (u.active || u.speed != Fixed()) {
                consume();
            }
            b.r[E].prod += mult * t.energyMake;
            b.r[M].prod += mult * t.metalMake;
            P.cap[E] += t.energyStorage;
            P.cap[M] += t.metalStorage;
            // Cloak (0x4017d9..0x401872): wanted and not blocked by work (unit+0xb0),
            // cost = trunc(moving ? cloakcostmoving : cloakcost) taken from stored
            // energy at once and counted as requested; unaffordable -> cloak off.
            if (t.canCloak && u.cloakOn && u.cloakBlockedUntil <= tickCounter_) {
                const bool moving = u.speed != Fixed();
                const float cost = float(int32_t(moving ? t.cloakCostMove : t.cloakCost));
                if (cost <= P.stored[E]) {
                    P.stored[E] -= cost;
                    b.r[E].req += cost;
                    u.cloaked = true;
                } else {
                    u.cloaked = false;
                }
            }
        }
        sums.add(b.r);
    }
    sums.add(P.own);
    if (P.hasBase) { P.cap[E] += P.base[E]; P.cap[M] += P.base[M]; }
    // A map scenario's resource limit (action 16, applyScenarioResourceAction)
    // replaces both stores' capacity until action 20 lifts it.
    if (const float limit = players_[size_t(player)].scenarioResourceLimit; limit > 0) P.cap[E] = P.cap[M] = limit;
    const Fractions f = pay(P, sums);
    for (auto& u : units_)
        if (u.player == player && u.alive() && u.type) roll(u.books.r, f);
    roll(P.own, f);
}

void World::taEconomyTick() {
    for (int p = 0; p < numPlayers(); ++p) {
        auto& b = players_[size_t(p)].books;
        if (!b.hasBase) continue;
        if (b.nextSettle <= tickCounter_) {
            taSettle(p);
            static const bool kLog = std::getenv("TA_ECONLOG") != nullptr;
            if (kLog)
                std::fprintf(stderr, "econ t=%u p=%d E %.1f/%.0f +%.2f -%.2f  M %.1f/%.0f +%.2f -%.2f wind %d\n",
                             tickCounter_, p, b.stored[E], b.cap[E], b.income[E], b.expense[E],
                             b.stored[M], b.cap[M], b.income[M], b.expense[M], int(wind_.speed));
            b.nextSettle += kSettlePeriod;
            if (b.nextSettle <= tickCounter_) b.nextSettle = tickCounter_ + kSettlePeriod;
        }
        // Mirror into the legacy single pool the HUD and AI still read: metal.
        players_[size_t(p)].mana = double(b.stored[M]);
        players_[size_t(p)].storage = b.cap[M];
        players_[size_t(p)].income = b.income[M];
    }
}

}  // namespace ta::sim
