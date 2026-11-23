// v10 retail terrain kits: section discovery and marching-squares classification.
#include "tnt/mapgen_internal.h"
#include "hpi/hpi.h"
#include "sim/sim.h"

#include <cctype>
#include <cstdlib>

namespace tak::mapgen {
namespace detail {

std::vector<std::string> kitDirectories() {
    std::vector<std::string> out;
    for (const auto& k : kKitSpec)
        for (const char* path : {k.edges[0], k.edges[1], k.top, k.mountains, k.mountainFill, k.snowLine, k.snowFill,
                                 k.peaks, k.lowTowns, k.lowTownFill, k.highTowns, k.highTownFill})
            if (path) out.emplace_back(path);
    return out;
}

const tnt::Map* SectionCache::load(const std::string& path) {
    auto it = cache_.find(path);
    if (it != cache_.end()) return it->second.get();
    std::unique_ptr<tnt::Map> map;
    if (vfs_.has(path)) map = std::make_unique<tnt::Map>(tnt::Map::load(vfs_.read(path), path));
    return cache_.emplace(path, std::move(map)).first->second.get();
}

std::vector<std::string> SectionCache::list(const std::string& prefix) const {
    std::vector<std::string> out;
    for (const auto& path : vfs_.list(prefix))
        if (hpi::MountSet::key(path).ends_with(".tnt")) out.push_back(path);
    std::sort(out.begin(), out.end());
    return out;
}

namespace {
// Retail role names -> marching case (bit set = corner inside the shape). A piece
// named for a side sits on that side of the shape: "n" has the shape to its south.
// Inner corners name the one corner left outside ("n_w"/"w_n": only NW is out).
int roleCase(std::string role) {
    static const std::pair<const char*, int> roles[] = {
        {"n", 12}, {"s", 3}, {"e", 5}, {"w", 10}, {"ne", 4}, {"nw", 8}, {"se", 1}, {"sw", 2},
        {"n_w", 14}, {"w_n", 14}, {"n_e", 13}, {"e_n", 13}, {"s_e", 7}, {"e_s", 7}, {"s_w", 11}, {"w_s", 11},
        {"center", 15}, {"int", 15},
        // Zhon's jungle cliffs: 1 = outer corner, 2 = inner corner, and ramps.
        {"1ne", 4}, {"1nw", 8}, {"1se", 1}, {"1sw", 2}, {"2ne", 13}, {"2nw", 14}, {"2se", 7}, {"2sw", 11},
        {"rampn", 12}, {"ramps", 3}, {"rampe", 5}, {"rampw", 10}};
    for (const auto& [name, value] : roles) if (role == name) return value;
    return -1;
}
// "sett_n_e01_80" -> n_e, "E_N01" -> e_n, "hwy_trans_e" -> e (gate), "n01agate" -> n (gate).
int nameCase(const std::string& path, bool& gate) {
    std::string n = hpi::MountSet::key(path);
    n = n.substr(n.rfind('/') + 1);
    n = n.substr(0, n.size() - 4);
    gate = false;
    for (const char* tag : {"hwy_trans_", "hwy_", "gate_", "_gate", "gate"}) {
        const auto at = n.find(tag);
        if (at != std::string::npos) { n.erase(at, std::char_traits<char>::length(tag)); gate = true; }
    }
    for (const char* prefix : {"sett_", "mttp_"}) if (n.starts_with(prefix)) n.erase(0, std::char_traits<char>::length(prefix));
    for (const char* suffix : {"_80", "_200", "_v2"}) if (n.ends_with(suffix)) n.erase(n.size() - std::char_traits<char>::length(suffix));
    while (!n.empty() && std::isdigit(static_cast<unsigned char>(n.back()))) n.pop_back();
    int c = roleCase(n);
    if (c < 0 && n.size() > 1 && n.back() == 'a') {   // alternates: Taros "na", Creon "n01a"
        n.pop_back();
        while (!n.empty() && std::isdigit(static_cast<unsigned char>(n.back()))) n.pop_back();
        c = roleCase(n);
    }
    return c;
}

// A movement class (the generator's 6-cell, slope-16 army, or GROUND3) can climb a
// straight piece from its low side to the high plateau, with the piece's own
// blockers in place. The piece is tested between flat low and high ground, with
// its sides blocked: in a rim its neighbours are cliffs.
bool climbable(const tnt::Map& piece, int c, int low, int high, int slope, int foot) {
    constexpr int pad = 16, size = 32 + 2 * pad;
    std::vector<uint8_t> heights(size * size), obstacles(size * size);
    for (int z = 0; z < size; ++z) for (int x = 0; x < size; ++x) {
        const int px = std::clamp(x - pad, 0, 31), pz = std::clamp(z - pad, 0, 31);
        int h = piece.heights[size_t(pz) * 32 + px];
        const bool north = z < pad, south = z >= pad + 32, west = x < pad, east = x >= pad + 32;
        if ((c == 12 && north) || (c == 3 && south) || (c == 5 && east) || (c == 10 && west)) h = low;
        if ((c == 12 && south) || (c == 3 && north) || (c == 5 && west) || (c == 10 && east)) h = high;
        heights[size_t(z) * size + x] = uint8_t(h);
        // Beside the piece (along its rim) stand cliffs: the climb must stay on it.
        const bool beside = (c == 12 || c == 3) ? (west || east) : (north || south);
        obstacles[size_t(z) * size + x] = beside || piece.features[size_t(pz) * 32 + px] == 0xfffc;
    }
    sim::NavGrid::Limits limits; limits.maxSlope = slope; limits.maxWaterDepth = 0;
    sim::NavGrid nav(heights, size, size, 0, limits);
    nav.setObstacles(&obstacles);
    // Flood from the low strip; succeed on reaching the high strip.
    const bool lowNorth = c == 12, lowSouth = c == 3, lowEast = c == 5;
    auto onLow = [&](int x, int z) { return lowNorth ? z < 4 : lowSouth ? z >= size - 4 : lowEast ? x >= size - 4 : x < 4; };
    auto onHigh = [&](int x, int z) { return lowNorth ? z >= size - 4 : lowSouth ? z < 4 : lowEast ? x < 4 : x >= size - 4; };
    std::vector<uint8_t> seen(size * size);
    std::vector<int> queue;
    for (int z = 0; z < size; ++z) for (int x = 0; x < size; ++x)
        if (onLow(x, z) && nav.fits(x, z, foot)) { seen[size_t(z) * size + x] = 1; queue.push_back(z * size + x); }
    for (size_t head = 0; head < queue.size(); ++head) {
        const int x = queue[head] % size, z = queue[head] / size;
        if (onHigh(x, z)) return true;
        for (auto [dx, dz] : std::array<std::pair<int, int>, 4>{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}}) {
            const int nx = x + dx, nz = z + dz;
            if (nx < 0 || nz < 0 || nx >= size || nz >= size || seen[size_t(nz) * size + nx] || !nav.fits(nx, nz, foot)) continue;
            seen[size_t(nz) * size + nx] = 1; queue.push_back(nz * size + nx);
        }
    }
    return false;
}
}  // namespace

CaseKit edgeKit(SectionCache& cache, const KitSpec& spec) {
    CaseKit kit;
    const int low = spec.low, high = spec.high;
    for (const char* dir : spec.edges) {
        if (!dir) continue;
        for (const auto& path : cache.list(dir)) {
            const std::string key = hpi::MountSet::key(path);
            // Dead ends and voided fills are special shapes, and the Veruna quarry
            // is a pit; none of them interlock as plain marching pieces.
            if (key.find("deadend") != std::string::npos || key.find("voided") != std::string::npos ||
                key.find("quarry") != std::string::npos) continue;
            const tnt::Map* piece = cache.load(path);
            if (!piece || piece->width != 32 || piece->height != 32) continue;
            auto h = [&](int x, int z) { return int(piece->heights[size_t(z) * 32 + x]); };
            // Each corner takes the nearer authored level of its 2x2 cells. Gentle
            // hillsides slope right into their corners, so the match is loose.
            const int cx[4] = {0, 30, 0, 30}, cz[4] = {0, 0, 30, 30};
            int byHeight = 0;
            bool settled = true;
            for (int k = 0; k < 4; ++k) {
                const int mean = (h(cx[k], cz[k]) + h(cx[k] + 1, cz[k]) + h(cx[k], cz[k] + 1) + h(cx[k] + 1, cz[k] + 1)) / 4;
                const int state = std::abs(mean - low) <= std::abs(mean - high) ? 0 : 1;
                if (std::abs(mean - (state ? high : low)) > 48) settled = false;
                byHeight |= state << k;
            }
            // Zhon's south-facing cliff pieces reach the plateau level only beyond
            // their rim (retail lays them against the plateau fill all the same), so
            // their role comes from the name; only their walkable low side is checked.
            bool gate = false;
            const int byName = nameCase(path, gate);
            const bool named = !settled || byHeight == 0 || byHeight == 15;
            const int c = named ? byName : byHeight;
            if (c <= 0 || c == 15 || c == 6 || c == 9) continue;
            // An edge between two low corners meets the walkable low ground: it must
            // sit at that level along its outer two rows. A mixed edge always meets
            // another edge piece, which retail authored to interlock; a high edge
            // meets the plateau at the cliff lip, where several retail pieces stop a
            // little short of the plateau level (a ramp's top is proven by the climb).
            static constexpr int edgeCorners[4][2] = {{0, 1}, {2, 3}, {0, 2}, {1, 3}};   // N S W E
            bool good = true;
            for (int e = 0; e < 4 && good; ++e) {
                const int a = (c >> edgeCorners[e][0]) & 1, b = (c >> edgeCorners[e][1]) & 1;
                if (a != b || a) continue;
                const int want = low;
                for (int i = 0; i < 32 && good; ++i) for (int r = 0; r < 2; ++r) {
                    const int x = e < 2 ? i : (e == 2 ? r : 31 - r), z = e < 2 ? (e == 0 ? r : 31 - r) : i;
                    if (std::abs(h(x, z) - want) > 32) { good = false; break; }
                }
            }
            if (!good) continue;
            const bool straight = c == 3 || c == 5 || c == 10 || c == 12;
            std::string stem = key.substr(key.rfind('/') + 1);
            stem = stem.substr(0, stem.size() - 4);
            kit[size_t(c)].push_back({piece, straight && climbable(*piece, c, low, high, 16, 6), false, dir == spec.edges[1],
                                      straight && climbable(*piece, c, low, high, 30, 3), stem});
        }
    }
    return kit;
}

CaseKit namedKit(SectionCache& cache, const char* dir, int size, bool towns) {
    CaseKit kit;
    if (!dir) return kit;
    for (const auto& path : cache.list(dir)) {
        bool gate = false;
        const int c = nameCase(path, gate);
        if (c < 0) continue;
        const tnt::Map* piece = cache.load(path);
        if (!piece || piece->width != size || piece->height != size) continue;
        if (towns) {
            // The paved inside of a retail settlement is road-flagged: its corners
            // must agree with the role name, or the piece is mislabelled.
            int roads = 0;
            const int last = size - 2, cx[4] = {0, last, 0, last}, cz[4] = {0, 0, last, last};
            for (int k = 0; k < 4; ++k) {
                int count = 0;
                for (int dz = 0; dz < 2; ++dz) for (int dx = 0; dx < 2; ++dx)
                    count += piece->features[size_t(cz[k] + dz) * size + cx[k] + dx] == 0xfffb;
                roads |= (count >= 3) << k;
            }
            if (roads != c || c == 15) continue;
            // Corner gates (Creon "ne_gate_e") are not used; straight gates are.
            if (gate && c != 3 && c != 5 && c != 10 && c != 12) continue;
        }
        kit[size_t(c)].push_back({piece, false, gate});
    }
    return kit;
}

std::vector<const tnt::Map*> standaloneKit(SectionCache& cache, const char* prefix) {
    std::vector<const tnt::Map*> out;
    if (!prefix) return out;
    std::string dir(prefix);
    dir = dir.substr(0, dir.rfind('/') + 1);
    const std::string stem = hpi::MountSet::key(prefix);
    for (const auto& path : cache.list(dir)) {
        if (!hpi::MountSet::key(path).starts_with(stem)) continue;
        const tnt::Map* piece = cache.load(path);
        if (!piece || piece->width != 32 || piece->height != 32) continue;
        // A mountain is impassable art with a level rim matching the plateau.
        int blockers = 0, lo = 255, hi = 0;
        for (auto f : piece->features) blockers += f == 0xfffc;
        for (int i = 0; i < 32; ++i)
            for (int v : {int(piece->heights[size_t(i)]), int(piece->heights[size_t(31) * 32 + i]),
                          int(piece->heights[size_t(i) * 32]), int(piece->heights[size_t(i) * 32 + 31])}) {
                lo = std::min(lo, v); hi = std::max(hi, v);
            }
        if (blockers >= 100 && hi - lo <= 16) out.push_back(piece);
    }
    return out;
}

}  // namespace detail

bool kitAvailable(uint8_t world, uint8_t kit) {
    if (world >= kMapTypes) return false;
    const auto& spec = detail::kKitSpec[world];
    switch (kit) {
        case KitPlateaus: return spec.edges[0] && spec.top;
        case KitMountains: return spec.mountains || spec.peaks;
        case KitTowns: return spec.lowTowns || spec.buildings.n > 0;
        default: return false;
    }
}

bool automaticPlateaus(uint8_t layout) { return layout == Maze || layout == Highlands; }

bool layoutAllowed(uint8_t world, uint8_t layout) {
    if (layout == IndustrialZone) return lavaWorld(world);
    return !(lavaWorld(world) && layout == Islands);
}

uint8_t layoutForSlot(uint8_t world, int slot) {
    if (slot == 3) return themedLayout(world);
    if (slot == 2 && lavaWorld(world)) return IndustrialZone;
    return uint8_t(std::clamp(slot, 0, 2));
}

int slotOfLayout(uint8_t layout) {
    return layout == IndustrialZone ? 2 : layout >= Maze ? 3 : layout;
}

}  // namespace tak::mapgen
