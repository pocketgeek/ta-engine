#pragma once

// Total Annihilation's metal/energy economy, as TotalA.exe runs it
// (docs/retail-economy.md). Retail settles each player once every 30 ticks
// (0x401360), not per tick: consumers file requests into per-unit accumulators all
// second, and the settle pays them at one shared fraction, leaving unpaid work as
// debt that stalls the unit until it is cleared. Float32 throughout, in the
// spec's operation order, so every peer computes the same bits.

#include <algorithm>
#include <cstdint>

namespace ta::sim::econ {

inline constexpr uint32_t kSettlePeriod = 30;
inline constexpr uint32_t kSharePeriod = 60;
inline constexpr float kBaseStorageMin = 200.0f;
inline constexpr float kWindDiv = 5000.0f;
inline constexpr float kTidalDefault = 0.5f;
enum Res : int { E = 0, M = 1 };

// One resource's books for one unit (or the player's own accumulator).
struct Acc {
    float prod = 0, req = 0, newReq = 0, debt = 0, lastProd = 0, lastReq = 0;
};

struct UnitBooks {
    Acc r[2];
    float extractRate = 0;   // metal per second (ExtractsMetal * sum(cell metal + 1)), set at creation
    uint32_t noCloakUntil = 0;
    bool indebted() const { return r[E].debt > 0 || r[M].debt > 0; }
};

struct PlayerBooks {
    float stored[2] = {0, 0};
    float cap[2] = {0, 0};
    float base[2] = {0, 0};        // base storage max(start, 200); 0 = none
    bool hasBase = false;
    Acc own[2];                    // the player accumulator (gifts, reclaim credits)
    float income[2] = {0, 0};      // last second: sum of prod
    float expense[2] = {0, 0};     // last second: sum of req
    // Lifetime totals (the result plate's columns), retail's qwords: produced
    // (player +0xac E / +0xb4 M) gains the settle's sum of production
    // (0x4019ce / 0x401a0d), wasted (+0xcc / +0xd4) the overflow above the cap
    // (0x401ae4 / 0x401b21). Double, as retail keeps them; hashed.
    double produced[2] = {0, 0};
    double wasted[2] = {0, 0};
    float mult = 1.0f;             // AI difficulty production multiplier (0.5/0.7/1.0)
    uint32_t nextSettle = 0;
    float shareThreshold[2] = {0, 0};
};

// request(r, x): always counts as requested; pooled (and the caller acts) only
// when the unit carries no debt in that resource.
inline bool request(Acc& a, float x) {
    a.req += x;
    if (a.debt <= 0) { a.newReq += x; return true; }
    return false;
}
// requestBoth(e, m): both count; pooled together only with no debt in either.
inline bool requestBoth(UnitBooks& b, float e, float m) {
    b.r[E].req += e;
    b.r[M].req += m;
    if (b.r[E].debt <= 0 && b.r[M].debt <= 0) {
        b.r[E].newReq += e;
        b.r[M].newReq += m;
        return true;
    }
    return false;
}
// payNow(r, x): from storage at once (cloak, shots, gifts); counts as requested.
inline bool payNow(PlayerBooks& p, Acc& a, int r, float x) {
    if (p.stored[r] >= x) { p.stored[r] -= x; a.req += x; return true; }
    return false;
}

// Sums gathered over a player's units for one settle.
struct Sums {
    float prod[2] = {0, 0}, req[2] = {0, 0}, newReq[2] = {0, 0}, debt[2] = {0, 0};
    void add(const Acc* a) {
        for (int r = 0; r < 2; ++r) {
            prod[r] += a[r].prod; req[r] += a[r].req; newReq[r] += a[r].newReq; debt[r] += a[r].debt;
        }
    }
};

// Step 5 of the settle: pay old debt first, then new requests, each at one shared
// fraction; overflow above the cap is wasted. Returns the fractions (fD, fN) per resource.
struct Fractions { float fD[2] = {1, 1}, fN[2] = {1, 1}; };
inline Fractions pay(PlayerBooks& p, const Sums& s) {
    Fractions f;
    for (int r = 0; r < 2; ++r) p.produced[r] += double(s.prod[r]);
    for (int r = 0; r < 2; ++r) {
        float avail = p.stored[r] + s.prod[r];
        f.fD[r] = s.debt[r] <= avail ? 1.0f : avail / s.debt[r];
        avail -= std::min(s.debt[r], avail);
        f.fN[r] = s.newReq[r] <= avail ? 1.0f : avail / s.newReq[r];
        avail -= std::min(s.newReq[r], avail);
        if (avail > p.cap[r]) { p.wasted[r] += double(avail) - double(p.cap[r]); avail = p.cap[r]; }
        p.stored[r] = avail;
        p.income[r] = s.prod[r];
        p.expense[r] = s.req[r];
    }
    return f;
}
// Step 6 for one accumulator pair.
inline void roll(Acc* a, const Fractions& f) {
    for (int r = 0; r < 2; ++r) {
        a[r].lastProd = a[r].prod;
        a[r].lastReq = a[r].req;
        a[r].debt = a[r].newReq * (1.0f - f.fN[r]) + a[r].debt * (1.0f - f.fD[r]);
        a[r].prod = a[r].req = a[r].newReq = 0;
    }
}

// The result plate's Score (TotalA.exe 0x41ddbe..0x41de10, filled at the game's
// end by 0x41dc20): trunc((tick / 60) * timemul) + trunc(kills * killmul), never
// below 0. killmul / timemul are the map's [GlobalHeader] floats (read at
// 0x4365e6 / 0x4365fe, default 0); the tick count divides as an unsigned integer
// first (0x41ddc4: * 0x88888889 >> 37).
inline int32_t plateScore(uint32_t tick, int32_t kills, float killMul, float timeMul) {
    const double t = double(tick / 60u) * double(timeMul);
    const double k = double(kills) * double(killMul);
    const int32_t score = int32_t(uint32_t(int64_t(t)) + uint32_t(int64_t(k)));   // _ftol's low dwords
    return std::max(score, 0);
}

// The MSVC rand LCG retail uses (unsynced in retail) for the wind-change interval;
// the engine seeds its own instance from the game seed so lockstep holds.
struct MsvcRand {
    uint32_t s = 1;
    int next() { s = s * 214013u + 2531011u; return int((s >> 16) & 0x7fff); }
};

struct Wind {
    int32_t minSpeed = 100, maxSpeed = 2000;
    int32_t speed = 0;
    uint16_t dir = 0;
    float factor = 0;          // min(speed / 5000, 1)
    uint32_t nextChange = 0;
    float tidal = kTidalDefault;
    MsvcRand lcg;
};

}  // namespace ta::sim::econ
