// Throwaway: how much of each shipped map admits a unit of footprint N?
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <set>
#include <cstdlib>
#include "hpi/hpi.h"
#include "sim/sim.h"
#include "sim/matchsetup.h"
using namespace ta;
int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: footprobe <install> [map...]\n"); return 2; }
    auto vfs = hpi::mountRetailRoot(argv[1], hpi::OverridePolicy::None);
    sim::TypeRegistry reg;
    reg.loadMoveInfo(vfs, "gamedata/moveinfo.tdf");
    sim::setupRegistry(reg, vfs);
    std::vector<std::string> maps;
    for (int i = 2; i < argc; ++i) maps.push_back(argv[i]);
    if (maps.empty()) maps = {"Inner Circle", "Athri Cay", "Ulasem Arena"};
    for (const auto& m : maps) {
        std::string mp = hpi::findMap(vfs, m);
        if (mp.empty()) { std::printf("%-22s (not found)\n", m.c_str()); continue; }
        sim::World w;
        sim::MatchConfig cfg;
        cfg.vfs = &vfs; cfg.mapPath = mp;
        cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
        sim::setupMatch(w, reg, cfg);
        const auto& mm = w.mapHeights();
        int W = w.mapW(), H = w.mapH(), sea = w.mapSea();
        auto at = [&](int x, int z) {
            x = x < 0 ? 0 : (x >= W ? W - 1 : x); z = z < 0 ? 0 : (z >= H ? H - 1 : z);
            return int(mm[size_t(z) * size_t(W) + size_t(x)]);
        };
        // RULE A (ours): blocked when any of the 8 NEIGHBOURS rises > cliff above us.
        // RULE B (retail-shaped): blocked when the spread across the cell's own 2x2
        // corner samples exceeds MaxSlope. Retail reads a per-cell min/max (cell+5,
        // cell+6) and compares max-min against the movement class's MaxSlope.
        auto ruleA = [&](int x, int z, int thr) {
            int self = at(x, z), hi = self;
            for (int dz = -1; dz <= 1; ++dz) for (int dx = -1; dx <= 1; ++dx)
                hi = std::max(hi, at(x + dx, z + dz));
            return hi - self <= thr;
        };
        auto ruleB = [&](int x, int z, int thr) {
            int lo = 255, hi = 0;
            for (int dz = 0; dz <= 1; ++dz) for (int dx = 0; dx <= 1; ++dx) {
                int v = at(x + dx, z + dz); lo = std::min(lo, v); hi = std::max(hi, v);
            }
            return hi - lo <= thr;
        };
        auto wet = [&](int x, int z) { return sea - at(x, z) > 20; };
        auto measure = [&](const char* label, auto rule, int thr) {
            std::vector<uint8_t> ok(size_t(W) * size_t(H), 0);
            for (int z = 0; z < H; ++z) for (int x = 0; x < W; ++x)
                ok[size_t(z) * size_t(W) + size_t(x)] = (rule(x, z, thr) && !wet(x, z)) ? 1 : 0;
            auto fits = [&](int x, int z, int n) {
                if (x < 0 || z < 0 || x + n > W || z + n > H) return false;
                for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i)
                    if (!ok[size_t(z + j) * size_t(W) + size_t(x + i)]) return false;
                return true;
            };
            auto reach = [&](int n) {
                int sx = int(w.unit(1) ? w.unit(1)->x : 0) / 16;
                int sz = int(w.unit(1) ? w.unit(1)->z : 0) / 16;
                int bx = -1, bz = -1;
                for (int r = 0; r < 60 && bx < 0; ++r)
                    for (int j = -r; j <= r && bx < 0; ++j)
                        for (int i = -r; i <= r && bx < 0; ++i)
                            if (std::max(std::abs(i), std::abs(j)) == r && fits(sx + i, sz + j, n))
                                { bx = sx + i; bz = sz + j; }
                if (bx < 0) return 0L;
                std::vector<uint8_t> seen(size_t(W) * size_t(H), 0);
                std::vector<int> q{bz * W + bx}; seen[size_t(q[0])] = 1; long cnt = 0;
                for (size_t hh = 0; hh < q.size(); ++hh) {
                    int c0 = q[hh]; ++cnt; int x = c0 % W, z = c0 / W;
                    const int dx[4]{1,-1,0,0}, dz[4]{0,0,1,-1};
                    for (int k = 0; k < 4; ++k) {
                        int nx = x + dx[k], nz = z + dz[k];
                        if (!fits(nx, nz, n)) continue;
                        size_t id = size_t(nz) * size_t(W) + size_t(nx);
                        if (seen[id]) continue; seen[id] = 1; q.push_back(int(id));
                    }
                }
                return cnt;
            };
            long r1 = reach(1);
            std::printf("  %-22s ", label);
            for (int n = 1; n <= 5; ++n)
                std::printf("f%d=%5.1f%% ", n, r1 ? 100.0 * double(reach(n)) / double(r1) : 0.0);
            std::printf("(f1 cells=%ld)\n", r1);
        };
        // Largest connected component of legal footprint placements, as a % of the
        // map, measured on the REAL nav grid -- the number that decides whether a
        // unit of that size can actually get anywhere.
        // How many distinct movement-class grids did this registry reduce to?
        {
            std::set<const void*> grids;
            for (const auto& [id, t] : reg.types())
                if (!t.canFly) grids.insert((const void*)&w.navFor(&t));
            std::printf("  %zu distinct class grids\n", grids.size());
        }
        const sim::NavGrid& g = w.nav();
        std::printf("%-24s ", m.c_str());
        for (int n = 1; n <= 5; ++n) {
            std::vector<uint8_t> seen(size_t(W) * size_t(H), 0);
            long best = 0;
            for (int z0 = 0; z0 < H; ++z0)
                for (int x0 = 0; x0 < W; ++x0) {
                    size_t s0 = size_t(z0) * size_t(W) + size_t(x0);
                    if (seen[s0] || !g.fits(x0, z0, n)) continue;
                    std::vector<int> q{int(s0)}; seen[s0] = 1; long cnt = 0;
                    for (size_t hh = 0; hh < q.size(); ++hh) {
                        int c0 = q[hh]; ++cnt;
                        int x = c0 % W, z = c0 / W;
                        const int dx[4]{1,-1,0,0}, dz[4]{0,0,1,-1};
                        for (int k = 0; k < 4; ++k) {
                            int nx = x + dx[k], nz = z + dz[k];
                            if (nx < 0 || nz < 0 || nx >= W || nz >= H) continue;
                            if (!g.fits(nx, nz, n)) continue;
                            size_t id = size_t(nz) * size_t(W) + size_t(nx);
                            if (seen[id]) continue; seen[id] = 1; q.push_back(int(id));
                        }
                    }
                    best = std::max(best, cnt);
                }
            std::printf("f%d=%5.1f%% ", n, 100.0 * double(best) / double(size_t(W) * size_t(H)));
        }
        std::printf("\n");
    }
    return 0;
}
