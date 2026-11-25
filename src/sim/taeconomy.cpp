// World side of the Total Annihilation economy (sim/taeconomy.h,
// docs/retail-economy.md). Every peer settles every player, in player order, once
// every 30 ticks; all arithmetic is float32 in the spec's order.
#include "sim/sim.h"
#include "sim/taeconomy.h"
#include "sim/taweapons.h"

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

bool World::payShot(Unit& u, const Weapon& w, int slot) {
    // 0x49e3d5..0x49e41e: a stockpile weapon fires only with stock in its
    // slot (S+0x1a); what it spends is that stock (0x49e455), never storage.
    if (w.hasTaFlag(kWStockpile)) return slot >= 0 && slot < 3 && u.stock[size_t(slot)] > 0;
    return econPayShot(u, w);
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
                    if (!u.cloaked) noteCloaked(u);   // 0x48b090(1, 4): status bit 2 newly set
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
    }
}

// TotalA.exe's auto-share (0x457d30, run from the tick at 0x49563a after the
// wind; docs/retail-economy.md section 4), every tick where tick % 60 == 0.
// Retail runs it for the local player only, in a multiplayer game
// (game +0x2a44 & 1), and gives only to remote humans (owner kind 3); in
// lockstep every peer runs it for every human player, in player order, so
// the gifts land identically everywhere. Metal first (switch info +0x97 bit 2,
// threshold +0xe4), then energy (bit 4, +0xe8): when stored is above the
// threshold, the LAST allied human by index with less stored gets
// min((stored - threshold) * k, their cap - their stored), k = 1/3 for metal
// (0x4fd4b8) and 1/2 for energy (0x4fd4bc), through the manual give
// (0x464c60 metal, 0x464b30 energy): the amount, capped at the giver's
// stored, leaves at once (counted as requested) and joins the receiver's own
// accumulator as production, so it lands at their next settle (capped there).
void World::taShare() {
    if (tickCounter_ % kSharePeriod != 0) return;
    const int n = numPlayers();
    auto human = [&](int i) {
        const Player& pl = players_[size_t(i)];
        return pl.books.hasBase && !pl.defeated && !pl.passiveNeutral && !computerOwned(i);
    };
    for (int giver = 0; giver < n; ++giver) {
        if (!human(giver)) continue;
        Player& from = players_[size_t(giver)];
        for (const int r : {int(M), int(E)}) {
            const uint8_t bit = r == M ? 1u : 2u;
            auto& fb = from.books;
            if (!(from.shareResources & bit) || !(fb.stored[r] > fb.shareThreshold[r])) continue;
            int to = -1;
            for (int i = 0; i < n; ++i)
                if (i != giver && human(i) && allied(giver, i) &&
                    players_[size_t(i)].books.stored[r] < fb.stored[r]) to = i;
            if (to < 0) continue;
            auto& tb = players_[size_t(to)].books;
            const float k = r == M ? 1.0f / 3.0f : 0.5f;
            float amount = float(double(fb.stored[r] - fb.shareThreshold[r]) * double(k));
            const float room = tb.cap[r] - tb.stored[r];
            if (!(amount < room)) amount = room;
            // The manual give: capped at what the giver stores, paid at once.
            amount = std::min(amount, fb.stored[r]);
            if (!(amount > 0)) continue;
            payNow(fb, fb.own[r], r, amount);
            tb.own[r].prod += tb.mult * amount;
        }
    }
}

void World::setResourceSharing(int player, int resource, bool enabled) {
    if (player < 0 || player >= numPlayers() || (resource != M && resource != E)) return;
    const uint8_t bit = resource == M ? 1u : 2u;
    auto& mask = players_[size_t(player)].shareResources;
    mask = enabled ? uint8_t(mask | bit) : uint8_t(mask & ~bit);
}

void World::setShareThreshold(int player, int resource, float amount) {
    if (player < 0 || player >= numPlayers() || (resource != M && resource != E)) return;
    auto& b = players_[size_t(player)].books;
    // 0x419340 / 0x419400: "OK.  Will share metal if above %d" stores min(N, cap).
    b.shareThreshold[resource] = std::max(0.0f, std::min(amount, b.cap[resource]));
}

}  // namespace ta::sim
