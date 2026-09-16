#include "crt/crt.h"

#include <cmath>
#include <cstring>

namespace ta::crt {

namespace {

// ---- little-endian readers ----
uint32_t u32(const std::vector<uint8_t>& d, size_t off) {
    return d[off] | (d[off + 1] << 8) | (d[off + 2] << 16) | (uint32_t(d[off + 3]) << 24);
}
int32_t i32(const std::vector<uint8_t>& d, size_t off) { return int32_t(u32(d, off)); }

// A NUL-terminated string in a fixed-size field starting at off (cap = field
// size); trailing bytes after the NUL are residue and ignored.
std::string str(const std::vector<uint8_t>& d, size_t off, size_t cap) {
    size_t n = 0;
    while (n < cap && off + n < d.size() && d[off + n]) ++n;
    return std::string(reinterpret_cast<const char*>(&d[off]), n);
}

// ---- little-endian writers ----
struct Writer {
    std::vector<uint8_t> b;
    void u32v(uint32_t v) {
        b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8));
        b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 24));
    }
    void i32v(int32_t v) { u32v(uint32_t(v)); }
    void f32v(float v) { uint32_t u; std::memcpy(&u, &v, 4); u32v(u); }
    // Fixed-size field: the string (truncated to cap-1) then zero fill to cap.
    void field(const std::string& s, size_t cap) {
        size_t n = std::min(s.size(), cap ? cap - 1 : 0);
        b.insert(b.end(), s.begin(), s.begin() + n);
        b.insert(b.end(), cap - n, 0);
    }
    void zero(size_t n) { b.insert(b.end(), n, 0); }
};

constexpr size_t kUnitRec = 568;
constexpr size_t kRuleRec = 324;
constexpr size_t kTypeRec = 272;
constexpr size_t kRegionRec = 272;

// Parse a 324-byte rule at off. Assumes off + 324 <= d.size().
Rule parseRule(const std::vector<uint8_t>& d, size_t off) {
    Rule r;
    r.opcode = i32(d, off);
    for (int i = 0; i < 5; ++i) r.slot[i] = str(d, off + 4 + size_t(i) * 64, 64);
    return r;
}

void writeRule(Writer& w, const Rule& r) {
    w.i32v(r.opcode);
    for (int i = 0; i < 5; ++i) w.field(r.slot[i], 64);
}

} // namespace

Scenario parse(const std::vector<uint8_t>& d) {
    Scenario s;
    if (d.size() < 12) return s;
    float version;
    std::memcpy(&version, d.data(), 4);
    if (std::abs(version - 1.0f) > 0.01f) return s;

    size_t p = 4;
    auto need = [&](size_t n) { return p + n <= d.size(); };

    // Custom types.
    if (!need(4)) return {};
    int32_t nCustom = i32(d, p); p += 4;
    if (nCustom < 0 || nCustom > 100000 || !need(size_t(nCustom) * kTypeRec)) return {};
    for (int i = 0; i < nCustom; ++i) {
        CustomType c;
        c.name = str(d, p, 256);
        for (int k = 0; k < 4; ++k) c.stat[k] = i32(d, p + 0x100 + size_t(k) * 4);
        s.customTypes.push_back(std::move(c));
        p += kTypeRec;
    }

    // Placed units.
    if (!need(4)) return {};
    int32_t nUnits = i32(d, p); p += 4;
    if (nUnits < 0 || nUnits > 100000 || !need(size_t(nUnits) * kUnitRec)) return {};
    for (int i = 0; i < nUnits; ++i) {
        size_t o = p;
        Unit u;
        u.objectName = str(d, o, 256);
        u.uniqueName = str(d, o + 0x100, 256);
        u.x = i32(d, o + 0x200);
        u.y = i32(d, o + 0x204);
        u.z = i32(d, o + 0x208);
        u.player = i32(d, o + 0x20c);
        u.health = i32(d, o + 0x210);
        u.armor = i32(d, o + 0x214);
        u.weapon = i32(d, o + 0x218);
        u.angle = i32(d, o + 0x21c);
        u.veteran = i32(d, o + 0x220);
        s.units.push_back(std::move(u));
        p += kUnitRec;
    }

    // Per-player rule groups.
    if (!need(4)) return {};
    int32_t nPlayers = i32(d, p); p += 4;
    if (nPlayers < 0 || nPlayers > 64) return {};
    s.players.resize(nPlayers);
    for (int pl = 0; pl < nPlayers; ++pl) {
        if (!need(4)) return {};
        int32_t nGroups = i32(d, p); p += 4;
        if (nGroups < 0 || nGroups > 100000) return {};
        for (int g = 0; g < nGroups; ++g) {
            RuleGroup grp;
            if (!need(4)) return {};
            int32_t nC = i32(d, p); p += 4;
            if (nC < 0 || nC > 100000 || !need(size_t(nC) * kRuleRec)) return {};
            for (int c = 0; c < nC; ++c) { grp.conditions.push_back(parseRule(d, p)); p += kRuleRec; }
            if (!need(4)) return {};
            int32_t nA = i32(d, p); p += 4;
            if (nA < 0 || nA > 100000 || !need(size_t(nA) * kRuleRec)) return {};
            for (int a = 0; a < nA; ++a) { grp.actions.push_back(parseRule(d, p)); p += kRuleRec; }
            s.players[pl].push_back(std::move(grp));
        }
    }

    // Regions.
    if (!need(4)) return {};
    int32_t nReg = i32(d, p); p += 4;
    if (nReg < 0 || nReg > 100000 || !need(size_t(nReg) * kRegionRec)) return {};
    for (int i = 0; i < nReg; ++i) {
        Region r;
        r.name = str(d, p, 64);
        while (!r.name.empty() && r.name.back() == ' ') r.name.pop_back();
        r.x1 = i32(d, p + 0x100);
        r.z1 = i32(d, p + 0x104);
        r.x2 = i32(d, p + 0x108);
        r.z2 = i32(d, p + 0x10c);
        s.regions.push_back(std::move(r));
        p += kRegionRec;
    }

    s.version = version;
    return s;
}

std::vector<uint8_t> write(const Scenario& s) {
    Writer w;
    w.f32v(s.version == 0.0f ? 1.0f : s.version);

    w.i32v(int32_t(s.customTypes.size()));
    for (const auto& c : s.customTypes) {
        w.field(c.name, 256);                    // 0x000
        for (int k = 0; k < 4; ++k) w.i32v(c.stat[k]);   // 0x100
    }

    w.i32v(int32_t(s.units.size()));
    for (const auto& u : s.units) {
        size_t start = w.b.size();
        w.field(u.objectName, 256);              // 0x000
        w.field(u.uniqueName, 256);              // 0x100
        w.i32v(u.x);                             // 0x200
        w.i32v(u.y);                             // 0x204
        w.i32v(u.z);                             // 0x208
        w.i32v(u.player);                        // 0x20c
        w.i32v(u.health);                        // 0x210
        w.i32v(u.armor);                         // 0x214
        w.i32v(u.weapon);                        // 0x218
        w.i32v(u.angle);                         // 0x21c
        w.i32v(u.veteran);                       // 0x220
        w.zero(kUnitRec - (w.b.size() - start)); // 0x224..0x238 residue -> zero
    }

    w.i32v(int32_t(s.players.size()));
    for (const auto& groups : s.players) {
        w.i32v(int32_t(groups.size()));
        for (const auto& g : groups) {
            w.i32v(int32_t(g.conditions.size()));
            for (const auto& r : g.conditions) writeRule(w, r);
            w.i32v(int32_t(g.actions.size()));
            for (const auto& r : g.actions) writeRule(w, r);
        }
    }

    w.i32v(int32_t(s.regions.size()));
    for (const auto& r : s.regions) {
        size_t start = w.b.size();
        w.field(r.name, 64);                     // 0x000
        w.zero(0x100 - (w.b.size() - start));    // 0x040..0x0ff residue -> zero
        w.i32v(r.x1); w.i32v(r.z1); w.i32v(r.x2); w.i32v(r.z2);   // 0x100
    }
    return std::move(w.b);
}

} // namespace ta::crt
