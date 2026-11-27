#include "ai/taretail.h"

#include "net/lockstep.h"
#include "sim/sim.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace ta::ai {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool armed(const sim::UnitType& t) { return !t.weapons.empty(); }
bool builds(const sim::TypeRegistry& reg, const sim::UnitType& t) { return t.isBuilder && !reg.buildable(t.id).empty(); }
// Retail 0x488f30: a unit's net energy (negative = it makes energy).
float energyNet(const sim::UnitType& t, float windSpeed, float tidal) {
    if (t.energyUse != 0) return t.energyUse;
    if (t.windGenerator > 0) return -t.windGenerator * windSpeed;
    if (t.tidalGenerator > 0) return -t.tidalGenerator * tidal;
    return 0;
}
constexpr const char* kDifficultyWord[3] = {"easy", "medium", "hard"};

}  // namespace

TaRetailBrain::TaRetailBrain(int player, uint32_t gameSeed, const sim::TypeRegistry& registry, int difficulty)
    : player_(player), difficulty_(std::clamp(difficulty, 0, 2)), reg_(registry) {
    // One Park-Miller stream per AI player (docs/retail-ai.md "RNG in lockstep").
    seed_ = (gameSeed ^ (uint32_t(player + 1) * 0x9E3779B9u)) % 2147483647u;
    if (!seed_) seed_ = 1;
    for (const auto& [id, t] : reg_.types()) types_.push_back(&t);   // name-sorted, as retail sorts
    const size_t n = types_.size();
    weight_.assign(n, 100); limit_.assign(n, -1); wlock_.assign(n, 0); llock_.assign(n, 0);
    count_.assign(n, 0); value_.assign(n, 0); base_.assign(n, 0); baseML_.assign(n, 0); baseEL_.assign(n, 0);
    squadOf_.clear();
    if (std::getenv("TA_AILOG")) std::fprintf(stderr, "ai p%d: TA retail brain, difficulty %d\n", player, difficulty_);
}

uint32_t TaRetailBrain::random(uint32_t n) {
    if (n < 2) return 0;   // 0x4b6c30: no draw
    seed_ = uint32_t((uint64_t(seed_) * 16807u) % 2147483647u);
    return seed_ % n;
}

size_t TaRetailBrain::typeIndex(const sim::UnitType* t) const {
    auto it = std::lower_bound(types_.begin(), types_.end(), t,
                               [](const sim::UnitType* a, const sim::UnitType* b) { return a->id < b->id; });
    return it != types_.end() && *it == t ? size_t(it - types_.begin()) : types_.size();
}

int TaRetailBrain::weightOf(const std::string& u) const {
    for (size_t i = 0; i < types_.size(); ++i) if (types_[i]->id == lower(u)) return weight_[i];
    return -1;
}
int TaRetailBrain::limitOf(const std::string& u) const {
    for (size_t i = 0; i < types_.size(); ++i) if (types_[i]->id == lower(u)) return limit_[i];
    return -2;
}

// One profile line (docs/retail-ai.md section 1 grammar).
void TaRetailBrain::applyProfileLine(const std::vector<std::string>& tok, bool& planOn, bool fbiLine) {
    if (tok.empty()) return;
    const std::string cmd = lower(tok[0]);
    if (cmd == "plan") {
        planOn = false;
        for (size_t i = 1; i < tok.size(); ++i) {
            const std::string w = lower(tok[i]);
            if (w == "any" || w == kDifficultyWord[difficulty_]) planOn = true;
        }
        return;
    }
    if (!planOn && !fbiLine) return;
    if ((cmd != "weight" && cmd != "limit") || tok.size() < 2) return;
    const std::string name = lower(tok[1]);
    std::vector<size_t> set;
    bool unit = false;
    for (size_t i = 0; i < types_.size(); ++i)
        if (types_[i]->id == name) { set.push_back(i); unit = true; }
    if (!unit)
        for (size_t i = 0; i < types_.size(); ++i)
            for (const auto& c : types_[i]->categories)
                if (c == name || name == "all") { set.push_back(i); break; }
    if (cmd == "weight") {
        const double f = tok.size() > 2 ? std::atof(tok[2].c_str()) : 0.0;
        for (size_t i : set) {
            if (wlock_[i]) continue;
            weight_[i] = uint8_t(std::clamp(int(std::trunc(double(weight_[i]) * f)), 0, 100));
            if (unit) wlock_[i] = 1;
        }
    } else {
        const int v = tok.size() > 2 ? std::atoi(tok[2].c_str()) : 0;
        for (size_t i : set) {
            if (llock_[i]) continue;
            limit_[i] = v;
            if (unit) llock_[i] = 1;
        }
    }
}

void TaRetailBrain::useProfile(const std::string& text) {
    bool planOn = true;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
        std::istringstream ls(line);
        std::vector<std::string> tok;
        for (std::string t; ls >> t && tok.size() < 20;) tok.push_back(t);
        applyProfileLine(tok, planOn, false);
    }
    // Then each unit's FBI ai_weight line (0x409f80). FBI ai_limit is never applied
    // in retail (0x40a040 re-reads ai_weight) -- kept as retail.
    for (size_t i = 0; i < types_.size(); ++i) {
        const std::string& w = types_[i]->classicFbi.aiWeight;
        if (w.empty() || wlock_[i]) continue;
        std::istringstream ls(w);
        std::vector<std::string> tok;
        for (std::string t; ls >> t;) tok.push_back(t);
        bool on = true;
        applyProfileLine(tok, on, true);
    }
#ifndef NDEBUG
    // Debug-only harness hook (tools/cartographer_ai_profile_e2e_test.py): TA_RETAIL_WEIGHTS
    // names an output path; each brain writes <path>.p<player> once its profile is applied,
    // one row per type in retail's PrintWeights column layout,
    //     <limit, or n/a> - <weight> = <weight> - '<unit>'
    // (retail's tables are static: the profile's weight is the priority). It only writes a
    // file, so --mpai hashes are the same with it on; a release build reads no variable.
    if (const char* path = std::getenv("TA_RETAIL_WEIGHTS"); path && *path) {
        const std::string out = std::string(path) + ".p" + std::to_string(player_);
        if (std::FILE* f = std::fopen(out.c_str(), "w")) {
            std::fprintf(f, "player %d: TA retail brain, difficulty %d\n<limit> - <weight> = <weight> - <unit>\n", player_, difficulty_);
            for (size_t i = 0; i < types_.size(); ++i) {
                char limit[16];
                if (limit_[i] < 0) std::snprintf(limit, sizeof limit, "n/a ");
                else std::snprintf(limit, sizeof limit, "%4d", limit_[i]);
                std::fprintf(f, "%s - %3d = %3d - '%s'\n", limit, weight_[i], weight_[i], types_[i]->id.c_str());
            }
            std::fclose(f);
        } else std::fprintf(stderr, "TA_RETAIL_WEIGHTS: cannot write %s\n", out.c_str());
    }
#endif
}

void TaRetailBrain::buildLattice(const sim::World&) {
    // 0x40a150, RNG order: Random(10), Random(3), Random(sx), Random(sz), then water.
    // E (+0xf9 / +0x105) is uninitialised in retail; treated as 0 (docs [U]).
    land_.sx = int(random(10)) + 8;
    land_.sz = int(random(3)) + 8;
    land_.ox = int(random(uint32_t(land_.sx))) - land_.sx / 2;
    land_.oz = int(random(uint32_t(land_.sz))) - land_.sz / 2;
    water_.sx = int(random(20)) + 8;
    water_.sz = int(random(3)) + 8;
    water_.ox = int(random(uint32_t(water_.sx))) - water_.sx / 2;
    water_.oz = int(random(uint32_t(water_.sz))) - water_.sz / 2;
    latticeBuilt_ = true;
}

void TaRetailBrain::cache(const sim::World& world) {
    std::fill(count_.begin(), count_.end(), 0);
    buildCapable_ = 0;
    double sw = 0, sx = 0, sz = 0;
    for (const auto& u : world.units()) {
        if (!u.alive() || !u.type || u.player != player_ || u.underConstruction) continue;
        const size_t i = typeIndex(u.type);
        if (i < count_.size()) ++count_[i];
        const bool b = builds(reg_, *u.type);
        if (b) ++buildCapable_;
        const int w = (u.type->isStructure() ? 40 : 0) + (b ? 20 : 0);
        sw += w; sx += w * double(u.x.toFloat()); sz += w * double(u.z.toFloat());
    }
    haveBase_ = sw > 0;
    if (haveBase_) { baseX_ = float(sx / sw); baseZ_ = float(sz / sw); }
    else { baseX_ = baseZ_ = 0; }
}

void TaRetailBrain::tables(const sim::World& world) {
    const auto& P = world.player(player_);
    const float windSpeed = float(world.wind().speed);
    const float tidal = 0.5f;
    const int maxUnits = 250;
    for (size_t i = 0; i < types_.size(); ++i) {
        const sim::UnitType& t = *types_[i];
        const int c = count_[i];
        const float eNet = energyNet(t, windSpeed, tidal);
        int s = (t.extractsMetal != 0 ? 11 : 1) + (t.makesMetal ? 10 : 0) + (eNet < 0 ? 10 : 0);
        s = int(std::trunc(double(s) + 0.01 * t.buildCostMetal));
        s = int(std::trunc(double(s) + 0.002 * t.buildCostEnergy));
        int w = armed(t) ? 11 : 1;
        for (const auto& wp : t.weapons) if (wp.taId > 0) w += wp.range / 100 + wp.damage / 40 + 5;
        value_[i] = int8_t(std::clamp(s + std::clamp(w, -100, 100), -100, 100));
        int g = (armed(t) ? 21 : 1) + (t.isBuilder && c < 3 ? 30 : 0) + (eNet < 0 ? 50 : 0) +
                (t.extractsMetal != 0 ? 50 : 0) + (t.makesMetal ? 25 : 0) + (t.canFly ? 40 : 0) +
                (t.sonar != 0 ? 15 : 0) + (t.radar != 0 ? 5 : 0);
        int v = int(std::trunc(double(g) + std::clamp(double(t.energyMake), 0.0, 30.0)));
        if (c == 0) v *= 4;
        if (c == 1) v *= 2;
        if (t.landMinWaterDepth >= 0) v *= 3;
        if (P.unitCount > maxUnits / 2) v += value_[i] / 2;
        if (t.canTransport || t.classicFbi.isFeature) v = 0;
        if (t.windGenerator != 0 && world.wind().maximum < 2500) v = 0;
        base_[i] = int8_t(std::min(v, 100));
        baseML_[i] = int8_t(std::trunc(std::clamp((t.extractsMetal != 0 ? 100.0 : 0.0) + (t.makesMetal ? 25.0 : 0.0) -
                                                  0.02 * t.buildCostMetal, 0.0, 100.0)));
        baseEL_[i] = int8_t(std::trunc(std::clamp(-0.0025 * t.buildCostEnergy - 5.0 * eNet, 0.0, 100.0)));
    }
}

int TaRetailBrain::score(const sim::World& world, size_t i) const {
    const auto& b = world.player(player_).books;
    const float eStored = b.stored[0], mStored = b.stored[1];
    if (eStored < 50 || mStored < 25) return 0;
    // TotalA.exe 0x40bb70..0x40bbaa: in scenario mode 1 (0x435100 == 1, a campaign
    // mission) a type with downloadable=1 (type+0x241 bit 0x20) scores 0, so the AI
    // never builds the add-on units (docs/retail-ai.md section 2).
    if (campaign_ && i < types_.size() && types_[i]->downloadable) return 0;
    int E = int(std::trunc(std::max(0.0f, (std::min(b.cap[0], 1000.0f) - eStored) * 0.125f)));
    int M = int(std::trunc(std::max(0.0f, (std::min(b.cap[1], 500.0f) - mStored) * 0.25f)));
    if (b.income[0] - b.expense[0] < 1) E += 20;
    if (b.income[1] - b.expense[1] < 1) M += 20;
    if (b.income[0] < 50) E += 100; else if (b.income[0] < 200) E += 10;
    if (b.income[1] < 3) M += 100; else if (b.income[1] < 5) M += 20;
    M = std::clamp(M, 0, 100);
    E = std::clamp(E - M, 0, 100);
    const int G = std::max(0, 100 - M - E);
    if (!(limit_[i] == -1 || count_[i] < limit_[i])) return 0;
    return int(std::trunc(double(G * base_[i] + M * baseML_[i] + E * baseEL_[i]) * weight_[i] / 10000.0));
}

const sim::UnitType* TaRetailBrain::choose(const sim::World& world, const sim::UnitType* builder) {
    const sim::UnitType* choice = nullptr;
    int total = 0;
    for (const auto& name : reg_.buildable(builder->id)) {
        const sim::UnitType* t = reg_.find(name);
        if (!t) continue;
        const size_t i = typeIndex(t);
        if (i >= types_.size()) continue;
        const int s = score(world, i);
        if (s <= 0) continue;
        total += s;
        if (int(random(uint32_t(total))) < s) choice = t;
    }
    if (choice && lower(choice->side) != lower(builder->side)) choice = nullptr;   // no cross-side builds
    return choice;
}

bool TaRetailBrain::findSite(const sim::World& world, const sim::UnitType*, float px, float pz,
                             const sim::UnitType* t, float& outX, float& outZ) {
    const float W = float(world.mapW()) * 16.0f, H = float(world.mapH()) * 16.0f;
    if (R_ < std::max(W, H)) R_ += 160;
    float cx = baseX_, cz = baseZ_;
    const float dx = baseX_ - px, dz = baseZ_ - pz, d = std::sqrt(dx * dx + dz * dz);
    if (!haveBase_ || d > R_) {
        if (!haveBase_ || d <= 0) { cx = px; cz = pz; }
        else { cx = px + dx / d * R_; cz = pz + dz / d * R_; }
    }
    // Extractors go to the ore features when Random(255) > SurfaceMetal (0x40bfe0):
    // the most metal under the footprint (0x40a260), no fallback when none fits.
    if (t->extractsMetal != 0 && int(random(255)) > world.surfaceMetal()) {
        if (!oreBuilt_) {
            for (int z = 0; z < world.mapH(); ++z)
                for (int x = 0; x < world.mapW(); ++x) {
                    // Ore = cells under an indestructible metal feature (0x40a7b0):
                    // the stamped cells, whose metal differs from SurfaceMetal.
                    const int m = world.cellMetal(x, z);
                    if (m > 0 && m != world.surfaceMetal()) oreSpots_.push_back({x, z, m});
                }
            oreBuilt_ = true;
            if (std::getenv("TA_AILOG")) std::fprintf(stderr, "ai p%d: %zu ore cells, surface metal %d\n", player_, oreSpots_.size(), world.surfaceMetal());
        }
        const int pcx = int(px) / 16, pcz = int(pz) / 16;
        const int64_t r4 = int64_t(4 * R_ / 16) * int64_t(4 * R_ / 16);
        std::vector<std::pair<int64_t, size_t>> cand;
        for (size_t i = 0; i < oreSpots_.size(); ++i) {
            const int64_t ddx = oreSpots_[i][0] - pcx, ddz = oreSpots_[i][1] - pcz;
            const int64_t d2 = ddx * ddx + ddz * ddz;
            if (d2 <= r4) cand.push_back({d2, i});
        }
        std::sort(cand.begin(), cand.end());
        int64_t firstOk = -1;
        int best = -1;
        for (const auto& [d2, i] : cand) {
            if (firstOk >= 0 && d2 > firstOk + 160) break;
            const int gx = oreSpots_[i][0] - (t->footX - 3) / 2, gz = oreSpots_[i][1] - (t->footZ - 3) / 2;
            const float x = float((2 * gx + t->footX) * 8), z = float((2 * gz + t->footZ) * 8);
            if (!world.canPlace(t, x, z, player_)) continue;
            if (firstOk < 0) firstOk = d2;
            int sum = 0;
            for (int zz = 0; zz < t->footZ; ++zz) for (int xx = 0; xx < t->footX; ++xx) sum += world.cellMetal(gx + xx, gz + zz);
            if (sum > best) { best = sum; outX = x; outZ = z; }
        }
        if (std::getenv("TA_AILOG")) std::fprintf(stderr, "ai p%d: ore search %zu candidates, best metal %d\n", player_, cand.size(), best);
        return best >= 0;   // no fallback to generic placement, as retail
    }
    // Generic placement on the lattice (0x40a5d0), 30 tries.
    const Lattice& L = t->landMinWaterDepth >= 0 ? water_ : land_;
    for (int tries = 0; tries < 30; ++tries) {
        const float r = float(random(uint32_t(std::max(1.0f, R_))));
        const double a = double(random(65536)) * (2.0 * 3.14159265358979323846 / 65536.0);
        const float ptx = cx - r * float(std::sin(a)), ptz = cz - r * float(std::cos(a));
        const int ccx = (int(ptx) - t->footX * 8 + 8) >> 4, ccz = (int(ptz) - t->footZ * 8 + 8) >> 4;
        const int gx = (L.sx > 0 ? (ccx / L.sx) * L.sx : ccx) + L.ox + int(random(uint32_t(std::max(1, L.sx - t->footX))));
        const int gz = (L.sz > 0 ? (ccz / L.sz) * L.sz : ccz) + L.oz + int(random(uint32_t(std::max(1, L.sz - t->footZ))));
        const float x = float((2 * gx + t->footX) * 8), z = float((2 * gz + t->footZ) * 8);
        if (!world.canPlace(t, x, z, player_)) continue;
        int metal = 0;
        for (int zz = 0; zz < t->footZ; ++zz) for (int xx = 0; xx < t->footX; ++xx) metal += world.cellMetal(gx + xx, gz + zz);
        // Keep the ore spots free: at most 2 x SurfaceMetal per footprint cell.
        if (metal > 2 * world.surfaceMetal() * t->footX * t->footZ) continue;
        outX = x; outZ = z;
        return true;
    }
    return false;
}

void TaRetailBrain::assignSquads(const sim::World& world) {
    for (const auto& u : world.units()) {
        if (!u.alive() || !u.type || u.player != player_ || u.underConstruction) continue;
        // TotalA.exe 0x408846..0x40884e: only a unit with flag 0x20 (selectable)
        // joins a squad. A campaign unit under its InitialMission script has the
        // bit cleared (docs/retail-campaign.md section 2.6), so the AI leaves its
        // patrols, waits and ambushes alone until the script hands it back.
        if (!u.selectable) continue;
        if (size_t(u.id) >= squadOf_.size()) squadOf_.resize(size_t(u.id) + 1, 0);
        if (squadOf_[size_t(u.id)] != 0) continue;
        const sim::UnitType& t = *u.type;
        int sq = 0;
        if (t.isStructure()) sq = armed(t) ? 5 : 1;
        else if (builds(reg_, t)) sq = 4;
        else if (t.canFly) sq = 8;
        else if (t.landMinWaterDepth > 0) sq = 7;
        else if (armed(t)) sq = 3;
        if (!sq) continue;
        squadOf_[size_t(u.id)] = sq;
        squads_[size_t(sq)].units.push_back(u.id);
    }
    // Drop the dead.
    for (auto& s : squads_)
        std::erase_if(s.units, [&](int id) {
            const sim::Unit* u = world.unit(id);
            return !u || !u->alive() || u->player != player_;
        });
}

bool TaRetailBrain::centroid(const sim::World& world, int squad, float& x, float& z) const {
    double sx = 0, sz = 0;
    int n = 0;
    for (int id : squads_[size_t(squad)].units)
        if (const sim::Unit* u = world.unit(id)) { sx += u->x.toFloat(); sz += u->z.toFloat(); ++n; }
    if (!n) return false;
    x = float(sx / n); z = float(sz / n);
    return true;
}

int TaRetailBrain::nearestEnemy(const sim::World& world, float x, float z) const {
    int best = 0;
    double bd = 1e30;
    for (const auto& u : world.units()) {   // no visibility test, as retail (0x4071f0)
        if (!u.alive() || !u.type || u.player < 0 || u.player >= world.numPlayers()) continue;
        if (u.player == player_ || world.allied(u.player, player_)) continue;
        const double dx = u.x.toFloat() - x, dz = u.z.toFloat() - z, d = dx * dx + dz * dz;
        if (d < bd) { bd = d; best = u.id; }
    }
    return best;
}

void TaRetailBrain::taskEconomy(const sim::World& world, uint32_t, const CommandSink& sink) {
    const auto& b = world.player(player_).books;
    for (int id : squads_[1].units) {
        const sim::Unit* u = world.unit(id);
        if (!u || !u->type) continue;
        if (u->type->makesMetal) {   // metal makers on when energy is plentiful
            int want = -1;
            if (b.stored[1] * 2 < b.stored[0] && b.income[0] - b.expense[0] > 0 && random(5) != 0) want = 1;
            else if (b.stored[1] * 2 >= b.stored[0]) want = 0;
            if (want >= 0 && int(u->active) != want) {
                net::Command c; c.kind = net::Cmd::SetActive; c.player = uint8_t(player_);
                c.unitId = id; c.targetId = want; sink(c);
            }
        }
        if (builds(reg_, *u->type) && u->buildQueue.empty()) {
            if (const sim::UnitType* t = choose(world, u->type)) {
                net::Command c; c.kind = net::Cmd::Train; c.player = uint8_t(player_); c.unitId = id;
                std::strncpy(c.type, t->id.c_str(), sizeof c.type - 1);
                c.targetId = 1;
                sink(c);
            }
        }
    }
}

void TaRetailBrain::taskBuilders(const sim::World& world, uint32_t now, const CommandSink& sink) {
    const float W = float(world.mapW()) * 16.0f, H = float(world.mapH()) * 16.0f;
    static const bool kLogB = std::getenv("TA_AILOG") != nullptr;
    if (kLogB) std::fprintf(stderr, "ai p%d t%u: builders %zu\n", player_, now, squads_[4].units.size());
    for (int id : squads_[4].units) {   // pass 1: build
        const sim::Unit* u = world.unit(id);
        if (!u || !u->type || !builds(reg_, *u->type)) continue;
        if (kLogB) std::fprintf(stderr, "ai p%d   %s orders %zu hold %u\n", player_, u->type->id.c_str(), u->orders.size(), cmdHoldUntil_);
        if (u->type->commander && (buildCapable_ >= 5 || now < cmdHoldUntil_)) continue;
        if (!u->orders.empty()) continue;
        const sim::UnitType* t = choose(world, u->type);
        static const bool kLog = std::getenv("TA_AILOG") != nullptr;
        if (!t) { if (kLog) std::fprintf(stderr, "ai p%d t%u: %s chose nothing\n", player_, now, u->type->id.c_str()); continue; }
        float sx = 0, sz = 0;
        if (!findSite(world, u->type, u->x.toFloat(), u->z.toFloat(), t, sx, sz)) {
            if (kLog) std::fprintf(stderr, "ai p%d t%u: no site for %s (R %.0f)\n", player_, now, t->id.c_str(), R_);
            continue;
        }
        if (kLog) std::fprintf(stderr, "ai p%d t%u: %s builds %s at %.0f,%.0f\n", player_, now, u->type->id.c_str(), t->id.c_str(), sx, sz);
        if (u->type->commander && haveBase_ &&
            std::hypot(baseX_ - sx, baseZ_ - sz) > ((W - 32) + (H - 128)) / 3) continue;
        net::Command c; c.kind = net::Cmd::Build; c.player = uint8_t(player_); c.unitId = id;
        std::strncpy(c.type, t->id.c_str(), sizeof c.type - 1);
        c.x = sx; c.z = sz; c.queue = net::kQueueReplace;
        sink(c);
    }
    for (int id : squads_[4].units) {   // pass 2: idle builders patrol the base
        const sim::Unit* u = world.unit(id);
        if (!u || !u->type || !u->orders.empty() || !haveBase_) continue;
        const float ux = u->x.toFloat(), uz = u->z.toFloat();
        const float d = std::hypot(baseX_ - ux, baseZ_ - uz);
        float px, pz;
        if (u->type->commander) {
            if (buildCapable_ < 5) continue;
            if (d > 640) {
                const double a = double(random(65536)) * (2.0 * 3.14159265358979323846 / 65536.0);
                px = baseX_ - 640 * float(std::sin(a)); pz = baseZ_ - 640 * float(std::cos(a));
            } else { px = 2 * baseX_ - ux; pz = 2 * baseZ_ - uz; }
            net::Command m; m.kind = net::Cmd::Move; m.player = uint8_t(player_); m.unitId = id; m.x = px; m.z = pz;
            sink(m);
            net::Command p; p.kind = net::Cmd::Patrol; p.player = uint8_t(player_); p.unitId = id;
            p.x = baseX_; p.z = baseZ_; p.queue = net::kQueueAppend;
            sink(p);
            continue;
        }
        if (d >= 320) { px = baseX_; pz = baseZ_; }
        else if (d < 16) {
            const double a = double(random(65536)) * (2.0 * 3.14159265358979323846 / 65536.0);
            px = ux - 320 * float(std::sin(a)); pz = uz - 320 * float(std::cos(a));
        } else { px = ux + (baseX_ - ux) / d * 320; pz = uz + (baseZ_ - uz) / d * 320; }
        net::Command p; p.kind = net::Cmd::Patrol; p.player = uint8_t(player_); p.unitId = id; p.x = px; p.z = pz;
        sink(p);
    }
}

void TaRetailBrain::taskStrike(const sim::World& world, int squad, int src, int64_t R, const CommandSink& sink) {
    auto& S = squads_[size_t(squad)];
    auto& Src = squads_[size_t(src)];
    auto pos = [&](int id, int64_t& x, int64_t& z) {
        const sim::Unit* u = world.unit(id);
        if (!u) return false;
        x = int64_t(u->x.toFloat()); z = int64_t(u->z.toFloat());
        return true;
    };
    if (S.units.empty() && !Src.units.empty()) { S.units.push_back(Src.units.front()); Src.units.erase(Src.units.begin()); }
    auto centre = [&](int64_t& cx, int64_t& cz) {
        int64_t sx = 0, sz = 0, n = 0, x, z;
        for (int id : S.units) if (pos(id, x, z)) { sx += x; sz += z; ++n; }
        if (n) { cx = sx / n; cz = sz / n; }
        return n;
    };
    for (;;) {   // send the farthest straggler back while it is out of range
        if (S.units.size() <= 1) break;
        int64_t cx = 0, cz = 0;
        centre(cx, cz);
        int far = -1;
        int64_t fd = -1, x, z;
        for (size_t i = 0; i < S.units.size(); ++i)
            if (pos(S.units[i], x, z)) {
                const int64_t d = (x - cx) * (x - cx) + (z - cz) * (z - cz);
                if (d > fd) { fd = d; far = int(i); }
            }
        if (far < 0 || fd < R * int64_t(S.units.size())) break;
        Src.units.push_back(S.units[size_t(far)]);
        S.units.erase(S.units.begin() + far);
    }
    {
        int64_t cx = 0, cz = 0;
        if (centre(cx, cz)) {
            const int64_t size = int64_t(S.units.size());
            for (size_t i = 0; i < Src.units.size();) {
                int64_t x, z;
                if (pos(Src.units[i], x, z) && (x - cx) * (x - cx) + (z - cz) * (z - cz) < R * size) {
                    S.units.push_back(Src.units[i]);
                    Src.units.erase(Src.units.begin() + long(i));
                } else ++i;
            }
        }
    }
    if (S.units.empty()) return;
    const size_t size = S.units.size();
    if (size <= 3 || (S.state == 0 && size < 6)) {   // rally at the defences
        float rx, rz;
        if (centroid(world, 5, rx, rz) || centroid(world, 1, rx, rz) || centroid(world, 4, rx, rz)) {
            S.state = 0;
            for (int id : S.units) {
                net::Command c; c.kind = net::Cmd::Move; c.player = uint8_t(player_); c.unitId = id; c.x = rx; c.z = rz;
                sink(c);
            }
            return;
        }
    }
    if (!aggressive_) return;   // defensive levels hold at the rally point
    S.state = 1;   // attack the nearest enemy, no visibility check
    float cx, cz;
    if (!centroid(world, squad, cx, cz)) return;
    const int target = nearestEnemy(world, cx, cz);
    if (!target) return;
    for (int id : S.units) {
        net::Command c; c.kind = net::Cmd::Attack; c.player = uint8_t(player_); c.unitId = id; c.targetId = target;
        sink(c);
    }
}

void TaRetailBrain::taskFollow(const sim::World& world, int squad, int lead, const CommandSink& sink) {
    if (squads_[size_t(squad)].units.empty() || squads_[size_t(lead)].units.empty()) return;
    float x, z;
    if (!centroid(world, lead, x, z)) return;
    for (int id : squads_[size_t(squad)].units) {
        net::Command c; c.kind = net::Cmd::Move; c.player = uint8_t(player_); c.unitId = id; c.x = x; c.z = z;
        sink(c);
    }
}

void TaRetailBrain::taskAir(const sim::World& world, const CommandSink& sink) {
    auto& S = squads_[8];
    if (S.units.empty()) return;
    const int W = world.mapW() * 16, H = world.mapH() * 16;
    if (S.units.size() < 5) {
        if (!haveBase_) {
            float cx, cz;
            if (!centroid(world, 8, cx, cz)) return;
            const int t = nearestEnemy(world, cx, cz);
            const sim::Unit* e = t ? world.unit(t) : nullptr;
            if (!e) return;
            for (int id : S.units) {
                net::Command c; c.kind = net::Cmd::Patrol; c.player = uint8_t(player_); c.unitId = id;
                c.x = e->x.toFloat(); c.z = e->z.toFloat(); c.queue = net::kQueueAppend;
                sink(c);
            }
            return;
        }
        const int n = 2 + int(random(2));
        for (int k = 0; k < n; ++k) {
            const float x = baseX_ + float(int(random(uint32_t(std::max(1, W / 8)))) - W / 16);
            const float z = baseZ_ + float(int(random(uint32_t(std::max(1, H / 8)))) - H / 16);
            for (int id : S.units) {
                net::Command c; c.kind = k == 0 ? net::Cmd::Move : net::Cmd::Patrol; c.player = uint8_t(player_);
                c.unitId = id; c.x = x; c.z = z; c.queue = k == 0 ? net::kQueueReplace : net::kQueueAppend;
                sink(c);
            }
        }
        return;
    }
    float x, z;
    if (random(2)) { x = float(random(uint32_t(W))); z = random(2) ? 0.0f : float(H - 1); }
    else { x = random(2) ? 0.0f : float(W - 1); z = float(random(uint32_t(H))); }
    for (int id : S.units) {
        net::Command c; c.kind = net::Cmd::Patrol; c.player = uint8_t(player_); c.unitId = id; c.x = x; c.z = z;
        sink(c);
    }
}

void TaRetailBrain::tick(const sim::World& world, uint32_t now, const CommandSink& sink) {
    if (!latticeBuilt_) { buildLattice(world); cache(world); tables(world); lastCache_ = now; cacheRan_ = true; }
    // Commander hit: hold new builds for 30 + Random(300) ticks.
    for (const auto& u : world.units())
        if (u.alive() && u.type && u.player == player_ && u.type->commander) {
            const int hp = u.hp.floorInt();
            if (lastCommanderHp_ >= 0 && hp < lastCommanderHp_) cmdHoldUntil_ = now + 30 + random(300);
            lastCommanderHp_ = hp;
            break;
        }
    if (--squadCountdown_ <= 0) { squadCountdown_ = 30; assignSquads(world); }
    for (int k = 1; k <= 9; ++k) {
        Task& T = tasks_[size_t(k)];
        if (now < T.next) continue;
        switch (k) {
        case 1: T.next = now + 30; taskEconomy(world, now, sink); break;
        case 2: T.next = now + 300; taskStrike(world, 2, 3, 20000, sink); break;
        case 3: T.next = now + 150; taskFollow(world, 3, 2, sink); break;
        case 4: T.next = now + 90; taskBuilders(world, now, sink); break;
        case 5: T.next = now + 30; break;   // E: no-op
        case 6: T.next = now + 300; taskStrike(world, 6, 7, 50000, sink); break;
        case 7: T.next = now + 150; taskFollow(world, 7, 6, sink); break;
        case 8: T.next = now + 30 + random(900); taskAir(world, sink); break;
        case 9: T.next = now + 30 + random(150); break;   // I: never fed (squad 9 unused)
        }
    }
    if (now >= lastCache_ + 30) {
        cache(world);
        lastCache_ = now;
        if (random(30) == 0) tables(world);
    }
}

}  // namespace ta::ai
