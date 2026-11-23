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
using namespace tak;
int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: footprobe <install> [map...]\n"); return 2; }
    auto vfs = hpi::mountRetailRoot(argv[1]);
    sim::TypeRegistry reg;
    reg.loadMoveInfo(vfs, "gamedata/moveinfo.tdf");
    sim::setupRegistry(reg, vfs, false);
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
        const int W = w.mapW(), H = w.mapH();
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
                            if (seen[id]) continue;
                            seen[id] = 1; q.push_back(int(id));
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
