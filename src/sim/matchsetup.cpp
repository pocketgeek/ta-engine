#include "sim/matchsetup.h"

#include "util/strcase.h"

#include "tdf/sidedata.h"

#include "sim/detmath.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

#include "hpi/hpi.h"
#include "sim/mission.h"
#include "tdf/tdf.h"

#include <cstdio>
#include <memory>
#include "tdf/tdf.h"
#include "tnt/tnt.h"
#include "tnt/mapgen.h"

namespace ta::sim {

// Cached per Vfs identity is not worth it: setupMatch reads this once per match,
// and SIDEDATA is a few KB.
std::vector<std::string> sideCommandersImpl(const hpi::Vfs& vfs) {
    std::vector<std::string> out;
    for (const auto& s : ta::tdf::SideData::load(vfs).sides) out.push_back(s.commander);
    return out;
}

void applyCommand(World& world, const TypeRegistry& reg, const ta::net::Command& c) {
    using ta::net::Cmd;
    auto owns = [&](int id) {
        const auto* u = world.unit(id);
        return u && u->player == int(c.player);
    };
    auto redirect = [&] { if (!c.queue) world.cancelBuilds(c.unitId); };
    switch (c.kind) {
        case Cmd::Move:
            if (owns(c.unitId)) { redirect(); world.order(c.unitId, c.x, c.z, c.queue); }
            break;
        case Cmd::Attack:
            if (owns(c.unitId)) { redirect(); world.attack(c.unitId, c.targetId, c.queue); }
            break;
        case Cmd::AttackMove:
            if (owns(c.unitId)) { redirect(); world.attackMove(c.unitId, c.x, c.z, c.queue); }
            break;
        case Cmd::Patrol:
            // Shift-patrol ADDS a waypoint to the route. patrol() builds a fresh
            // two-point loop and was the only thing wired up, so a queued patrol
            // silently replaced the route instead of extending it -- patrolTo has
            // existed for this the whole time, used only by mission scripts.
            if (owns(c.unitId)) {
                world.cancelBuilds(c.unitId);
                if (c.queue) world.patrolTo(c.unitId, c.x, c.z, true);
                else world.patrol(c.unitId, c.x, c.z);
            }
            break;
        case Cmd::Stop:
            if (owns(c.unitId)) { world.cancelBuilds(c.unitId); world.stop(c.unitId); }
            break;
        case Cmd::Train:
            if (owns(c.unitId)) world.train(c.unitId, reg.find(c.type), c.targetId);
            break;
        case Cmd::Unqueue:
            if (owns(c.unitId)) world.dequeue(c.unitId, reg.find(c.type), std::max(1, c.targetId));
            break;
        case Cmd::Build:
            if (owns(c.unitId)) world.queueBuild(c.unitId, reg.find(c.type), c.x, c.z, c.queue);
            break;
        case Cmd::Assist:
            // Resume/assist an existing conjure: allowed only if the builder can
            // actually build the site's type (authoritative check, same as the UI).
            if (owns(c.unitId)) {
                const Unit* b = world.unit(c.unitId);
                const Unit* site = world.unit(c.targetId);
                if (b && b->type && site && site->type) {
                    const auto& menu = reg.buildable(b->type->id);
                    if (std::find(menu.begin(), menu.end(), site->type->id) != menu.end()) {
                        redirect();
                        world.assist(c.unitId, c.targetId, c.queue);
                    }
                }
            }
            break;
        case Cmd::Guard:
            if (owns(c.unitId)) world.guard(c.unitId, c.targetId, c.queue);
            break;
        case Cmd::Load:
            if (owns(c.unitId)) world.loadInto(c.unitId, c.targetId);
            break;
        case Cmd::Unload:
            if (owns(c.unitId)) world.unloadAt(c.unitId, c.x, c.z);
            break;
        case Cmd::SetWeapon:
            if (owns(c.unitId)) world.setWeapon(c.unitId, c.targetId);
            break;
        case Cmd::RepeatTrain:
            if (owns(c.unitId)) world.setRepeat(c.unitId, reg.find(c.type));
            break;
        case Cmd::Destroy:
            if (owns(c.unitId)) world.destroy(c.unitId);
            break;
        case Cmd::Disco:
            world.startDisco(int(c.player));   // cosmetic; no ownership needed
            break;
        case Cmd::Headbang:
            world.startHeadbang(int(c.player));   // cosmetic; no ownership needed
            break;
        case Cmd::Reclaim:
            if (owns(c.unitId)) { redirect(); world.reclaim(c.unitId, c.targetId, c.queue); }
            break;
        case Cmd::Stance:
            if (owns(c.unitId)) world.setStance(c.unitId, c.targetId);
            break;
        case Cmd::Cloak:
            if (owns(c.unitId)) world.setCloak(c.unitId, c.targetId != 0);
            break;
        case Cmd::SetActive:
            if (owns(c.unitId)) world.setActive(c.unitId, c.targetId != 0);
            break;
        case Cmd::Repair:
            if (owns(c.unitId)) { redirect(); world.repair(c.unitId, c.targetId, c.queue); }
            break;
        case Cmd::SetSquad:
            if (owns(c.unitId)) world.setSquad(c.unitId, c.targetId);
            break;
    }
}

void applyEvent(World& world, const ta::net::Event& e) {
    int p = e.player;
    if (p < 0 || p >= world.numPlayers()) return;
    for (auto& u : world.units())
        if (u.alive() && u.player == p) world.stop(u.id);
}

std::vector<std::string> sideCommanders(const hpi::Vfs& vfs) {
    return sideCommandersImpl(vfs);
}

void setupRegistry(TypeRegistry& reg, const hpi::Vfs& vfs) {
    reg.loadMoveInfo(vfs, "gamedata/moveinfo.tdf");
    // The VFS already merges base + the expansions + the patch + community units
    // into one namespace, resolved by retail newest-date precedence, so a single
    // loadDir("units") covers the whole Commander Pack.
    reg.loadDir(vfs, "units");
    reg.loadBuildTree(vfs);
}

// Read the map's economy inputs out of its .ota. Wind, tide and metal richness
// are map properties in TA, so a generator's output is worthless without them.
ta::sim::MapEconomy parseMapEconomy(const hpi::Vfs& vfs, const std::string& mapPath) {
    ta::sim::MapEconomy e;
    std::filesystem::path ota = mapPath;
    ota.replace_extension(".ota");
    std::string otaPath = ota.generic_string();
    if (!vfs.has(otaPath)) return e;
    try {
        auto b = vfs.read(otaPath);
        auto root = ta::tdf::parseText(std::string(b.begin(), b.end()), otaPath);
        const auto* gh = root.child("globalheader");
        if (!gh) return e;
        e.tidalStrength = float(gh->numberOr("tidalstrength", 0));
        e.minWind = float(gh->numberOr("minwindspeed", 0));
        e.maxWind = float(gh->numberOr("maxwindspeed", 0));
        // Metal richness is per-SCHEMA. Take the first schema: the lobby has no
        // schema picker yet, and every shipped map's schemas agree on metal.
        for (const auto& name : gh->childOrder) {
            const auto* sc = gh->child(name);
            if (!sc || name.rfind("schema", 0) != 0) continue;
            e.surfaceMetal = float(sc->numberOr("surfacemetal", 0));
            e.mohoMetal = float(sc->numberOr("mohometal", 0));
            break;
        }
    } catch (const std::exception&) {}
    return e;
}

std::vector<std::pair<float, float>> parseStartPositions(const hpi::Vfs& vfs,
                                                         const std::string& mapPath) {
    std::vector<std::pair<float, float>> out;
    std::filesystem::path ota = mapPath;
    ota.replace_extension(".ota");
    std::string otaPath = ota.generic_string();
    if (!vfs.has(otaPath)) return out;
    try {
        auto b = vfs.read(otaPath);
        auto root = ta::tdf::parseText(std::string(b.begin(), b.end()), otaPath);
        const auto* gh = root.child("globalheader");
        const auto* md = gh ? gh->child("map data") : nullptr;
        const auto* sp = md ? md->child("specials") : nullptr;
        if (!sp) return out;
        std::map<int, std::pair<float, float>> byIndex;
        for (const auto& name : sp->childOrder) {
            const auto* s = sp->child(name);
            if (!s) continue;
            std::string what = s->valueOr("specialwhat", "");
            if (what.rfind("StartPos", 0) != 0 && what.rfind("startpos", 0) != 0) continue;
            int n = std::atoi(what.c_str() + 8);
            if (n <= 0) continue;
            byIndex[n] = {float(s->numberOr("xpos", 0) * 16),
                          float(s->numberOr("zpos", 0) * 16)};
        }
        for (auto& [n, pos] : byIndex) out.push_back(pos);
    } catch (const std::exception&) {}
    return out;
}

namespace {
// Feature definition fields the sim cares about: is it a mana deposit, and its
// footprint (for nav blocking). Loaded from the feature TDFs.
struct FeatDef { int blocking = 0; int fx = 1, fz = 1;
                 int reclaimable = 0; float energy = 0;
                 // Burning chain (see World::tickBurning).
                 bool flamable = false; bool hasBurnAnim = false;
                 int spreadChance = 0; int sparkTicks = 0; std::string burnt;
                 // Corpse lifecycle (features/corpses).
                 int decomposeTicks = 0;
                 bool indestructible = false;
                 float hp = 0; std::string dead;
                 std::string object;
                 // TA: metal= is the reclaim metal yield, and on a category=metal
                 // feature it is also the per-cell richness an extractor draws.
                 float metal = 0; bool isMetal = false;
};

std::unordered_map<std::string, FeatDef> loadFeatureDefs(const hpi::Vfs& vfs) {
    std::unordered_map<std::string, FeatDef> defs;
    try {
        for (const std::string& path : vfs.list("features")) {
            // Case-INSENSITIVE: TA ships both cases (features/archi/METAL.TDF next to
                // features/all worlds/DragonsTeeth.tdf), and a case-sensitive test here
                // silently skipped every uppercase file -- which is how a map with 120
                // features, 10 of them metal patches, loaded with none of them.
                if (!ta::iendsWith(path, ".tdf")) continue;
            try {
                auto fb = vfs.read(path);
                auto root = ta::tdf::parseText(std::string(fb.begin(), fb.end()), path);
                for (const auto& n : root.childOrder) {
                    std::string k = n;
                    std::transform(k.begin(), k.end(), k.begin(), ::tolower);
                    const auto& node = root.children.at(n);
                    FeatDef d;
                    std::string cat = node.valueOr("category", "");
                    std::transform(cat.begin(), cat.end(), cat.begin(), ::tolower);


                    d.blocking = int(node.numberOr("blocking", 0));
                    d.fx = int(node.numberOr("footprintx", 1));
                    d.fz = int(node.numberOr("footprintz", 1));
                    d.reclaimable = int(node.numberOr("reclaimable", 0));
                    d.energy = float(node.numberOr("energy", 0));   // reclaim energy yield
                    // TA: metal= is both the reclaim metal yield and, on a
                    // category=metal feature, the per-cell richness an extractor
                    // built over it draws from. There is no separate metal plane.
                    d.metal = float(node.numberOr("metal", 0));
                    d.isMetal = (cat == "metal");
                    d.flamable = node.numberOr("flamable", 0) != 0;
                    d.hasBurnAnim = !node.valueOr("seqnameburn", "").empty();
                    d.spreadChance = int(node.numberOr("spreadchance", 0));
                    d.sparkTicks = int(node.numberOr("sparktime", 0) * 30);
                    d.burnt = node.valueOr("featureburnt", "");
                    std::transform(d.burnt.begin(), d.burnt.end(), d.burnt.begin(), ::tolower);
                    d.decomposeTicks = int(node.numberOr("decomposetime", 0) * 30);
                    d.object = node.valueOr("object", "");
                    std::transform(d.object.begin(), d.object.end(), d.object.begin(), ::tolower);
                    d.indestructible = node.numberOr("indestructible", 0) != 0;
                    d.hp = float(node.numberOr("damage", 0));
                    d.dead = node.valueOr("featuredead", "");
                    std::transform(d.dead.begin(), d.dead.end(), d.dead.begin(), ::tolower);
                    defs[k] = d;
                }
            } catch (const std::exception&) {}
        }
    } catch (const std::exception&) {}
    return defs;
}
struct FeatTypeInterner {
    const std::unordered_map<std::string, FeatDef>& defs;
    std::vector<ta::sim::FeatType> table;
    std::unordered_map<std::string, int> byName;
    explicit FeatTypeInterner(const std::unordered_map<std::string, FeatDef>& d) : defs(d) {}
    int intern(const std::string& nm) {
        if (nm.empty()) return -1;
        if (auto it = byName.find(nm); it != byName.end()) return it->second;
        auto di = defs.find(nm);
        if (di == defs.end()) return -1;
        int idx = int(table.size());
        byName[nm] = idx;                  // reserve BEFORE recursing (cycle guard)
        ta::sim::FeatType t;
        t.name = nm;
        t.flamable = di->second.flamable;
        t.hasBurnAnim = di->second.hasBurnAnim;
        t.spreadChance = di->second.spreadChance;
        t.sparkTicks = di->second.sparkTicks;
        t.energy = di->second.energy;
        t.metal = di->second.metal;
        t.isMetal = di->second.isMetal;
        t.fx = di->second.fx; t.fz = di->second.fz;
        t.blocking = di->second.blocking != 0;
        t.decomposeTicks = di->second.decomposeTicks;
        t.reclaimable = di->second.reclaimable != 0;
        t.indestructible = di->second.indestructible;
        t.hp = di->second.hp;
        t.object = di->second.object;
        table.push_back(std::move(t));
        table[size_t(idx)].burntType = intern(di->second.burnt);
        table[size_t(idx)].deadType = intern(di->second.dead);
        return idx;
    }
};
}  // namespace


// ONE feature-plane walk, shared by setupMatch and registerMapFeatures.
//
// These were two near-identical loops, and the duplication is exactly how they
// drifted: setupMatch blocked features into the nav overlay, registerMapFeatures
// did not, and the paths that use the latter (campaign missions, CRT scenarios,
// the local harness) leaned on the VIEWER to block instead -- from
// GameView::loadFeatures, which wrote to nav_.cells_. The referee never runs the
// viewer, so its ground grid lacked those blockers and losBetween disagreed,
// which desynced auto-acquisition. Blocking belongs here, in the sim, where both
// peers run it; having one function makes it impossible for the two to diverge
// again.
static void scanFeaturePlane(World& world, const ta::tnt::Map& map,
                             const std::unordered_map<std::string, FeatDef>& defs,
                             FeatTypeInterner& types,
                             std::vector<std::pair<float, float>>& rawMana,
                             std::vector<std::pair<float, float>>& rawAll) {
    if (map.featureNames.empty()) return;
    for (int cz = 0; cz < map.height; ++cz)
        for (int cx = 0; cx < map.width; ++cx) {
            uint16_t v = map.features[size_t(cz) * map.width + cx];
            if (v >= map.featureNames.size()) continue;
            std::string key = map.featureNames[v];
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            auto di = defs.find(key);
            if (di == defs.end()) continue;
            float x = float(cx) * 16 + 8, z = float(cz) * 16 + 8;
            // TA's metal patches (category=metal: ArchMetal*, and each world's
            // equivalents). A patch is where an extractor goes, so it is recorded as
            // a build spot -- and unlike Kingdoms' Sacred Stones there is no separate
            // "glowy centre": the whole 3x3 is the spot and all of it stays walkable.
            if (di->second.isMetal) {
                rawAll.push_back({x, z});
                rawMana.push_back({x, z});
            }
            // Nav-blocking: obstacle features block; a metal patch never does --
            // units walk over metal, and an extractor is built on top of it.
            if (!di->second.isMetal) {
                int fx = di->second.fx, fz = di->second.fz;
                world.blockCells(int(x) / 16 - fx / 2, int(z) / 16 - fz / 2, fx, fz, true);
            }
            // Which features enter the sim:
            //   * reclaimable / flamable obstacles (trees, rocks, wreckage), so a
            //     builder can clear them and fire can spread through them;
            //   * METAL PATCHES, which are neither -- ArchMetal is indestructible
            //     and not reclaimable -- but which the sim must still see, because
            //     an extractor's yield is the `metal=` richness of the feature
            //     under its footprint. Registering the patch only as a build SPOT
            //     left World::extractorYield with nothing to find, so every mex
            //     silently fell back to the map's background SurfaceMetal.
            // The id is derived from the cell, so every peer records the identical
            // feature.
            if (di->second.reclaimable || di->second.flamable || di->second.isMetal) {
                float work = std::max(di->second.energy, 60.0f);   // rocks (energy 0) still take a beat
                world.addFeature(cz * map.width + cx, x, z, di->second.energy,
                                 di->second.metal, work,
                                 di->second.fx, di->second.fz,
                                 // A metal patch never blocks: units walk over it and
                                 // the extractor is built on top.
                                 !di->second.isMetal && di->second.blocking != 0,
                                 types.intern(key));
            }
        }
}

// Cluster the mana features into deposits, publish them, and carve each one clear.
// Also shared: the carve is nav state, so a path that registers features without it
// would let Standing Stones make their own deposit unbuildable.
static void installManaSpots(World& world,
                             std::vector<std::pair<float, float>> rawMana,
                             const std::vector<std::pair<float, float>>& rawAll) {
    // The buildable spot is the glowing Sacred Stone centre, not the ring of static
    // Standing Stones (both are category=mana). Fallback: a deposit with no glowy
    // centre in the data uses its category=mana features instead.
    if (rawMana.empty()) rawMana = rawAll;
    std::vector<int> par(rawMana.size());
    for (size_t i = 0; i < par.size(); ++i) par[i] = int(i);
    std::function<int(int)> find = [&](int a) {
        while (par[size_t(a)] != a) { par[size_t(a)] = par[size_t(par[size_t(a)])]; a = par[size_t(a)]; }
        return a;
    };
    const float link2 = 60.0f * 60.0f;
    for (size_t i = 0; i < rawMana.size(); ++i)
        for (size_t j = i + 1; j < rawMana.size(); ++j) {
            float dx = rawMana[i].first - rawMana[j].first, dz = rawMana[i].second - rawMana[j].second;
            if (dx * dx + dz * dz < link2) par[size_t(find(int(i)))] = find(int(j));
        }
    std::map<int, std::pair<std::pair<double, double>, int>> acc;
    for (size_t i = 0; i < rawMana.size(); ++i) {
        auto& a = acc[find(int(i))];
        a.first.first += rawMana[i].first; a.first.second += rawMana[i].second; ++a.second;
    }
    std::vector<std::pair<float, float>> manaSpots;
    for (auto& [root, a] : acc)
        manaSpots.push_back({float(a.first.first / a.second), float(a.first.second / a.second)});
    world.setManaSpots(manaSpots);
    // Standing Stones block nav, but a large one can reach the glowy centre; keep
    // every deposit buildable by carving the 2x2 lodestone footprint clear at each
    // spot (canPlace tests exactly these cells).
    for (const auto& [sx, sz] : manaSpots)
        world.blockCells(int(sx) / 16 - 1, int(sz) / 16 - 1, 2, 2, false);
}

// Register the map's obstacle features into the sim world (reclaim + burning),
// exactly as setupMatch's placement loop does -- for the client's LOCAL harness
// and mission paths, which build their worlds without setupMatch. Nav blocking
// is NOT done here (those paths already block via their own feature placement).
void registerMapFeatures(World& world, const ta::tnt::Map& map, const hpi::Vfs& vfs,
                         const TypeRegistry* reg) {
    auto defs = loadFeatureDefs(vfs);
    FeatTypeInterner types(defs);
    std::vector<std::pair<float, float>> rawMana, rawAll;
    scanFeaturePlane(world, map, defs, types, rawMana, rawAll);
    installManaSpots(world, rawMana, rawAll);
    if (reg)
        for (const auto& [tid, ut] : reg->types()) {
            if (!ut.corpse.empty()) {
                int ci = types.intern(ut.corpse);
                if (ci >= 0) world.mapCorpse(&ut, ci);
            }
        }
    world.setFeatureTypes(std::move(types.table));
}

std::vector<std::pair<float, float>> setupMatch(World& world, const TypeRegistry& reg,
                                                const MatchConfig& cfg) {
    const hpi::Vfs& vfs = *cfg.vfs;
    // A "~gen1~" mapPath is a random-map recipe: generate it in memory (identically
    // on client and referee -- the params ride the mapId, generation is integer-only).
    const bool generated = ta::mapgen::isGeneratedMapId(cfg.mapPath);
    std::vector<std::pair<float, float>> genStarts;
    ta::tnt::Map map;
    if (generated) {
        auto g = ta::mapgen::generate(ta::mapgen::decodeMapId(cfg.mapPath), vfs);
        map = std::move(g.map);
        for (auto& [scx, scz] : g.starts)   // cell -> px, matching parseStartPositions
            genStarts.push_back({float(scx * 16), float(scz * 16)});
    } else {
        map = ta::tnt::Map::load(vfs.read(cfg.mapPath), cfg.mapPath);
    }
    world.setTerrain(map.heights, map.width, map.height, map.seaLevel, &map.features);
    // One nav grid per distinct movement-limit tuple, as retail bakes one per class.
    // After setTerrain (it needs the heights) and before anything blocks a cell.
    world.buildNavClasses(reg);
    // Retail's background pathfinder. Every match gets it; see
    // docs/retail-engine.md for what it is and what it still cannot do.
    world.setPathService(true);
    // Retail's own default is 12000 work units a tick (icd 0x41617b), which it
    // exposes as a quality setting scaling that by 5%..1000%. We run at 1500 --
    // 12.5%, well inside retail's range -- because measured on the 8-AI
    // benchmark 12000 costs 42.2ms/tick against 23.5 at 1500, and routing is
    // IDENTICAL at 12000, 3000 and 1500 (same percentage of the distance closed
    // for 40-, 80- and 160-cell goals). The budget decides how fast a search
    // finishes, not whether it can: the unit walks its straight segment
    // meanwhile and the route lands a few ticks later either way.
    world.setPathBudget(1500);
    world.clearFeatures();   // authoritative rebuild (the client ctor pre-registers)

    // Features: block nav footprints, and gather mana-deposit positions. Iterate
    // in the same (row-major) order the client does so clustering is identical.
    auto defs = loadFeatureDefs(vfs);
    // Intern feature names into the world's FeatType table, resolving the
    // featureburnt chain recursively (AraTree01 -> AraTree01a -> Arasmudge01).
    // First-seen order over the row-major cell walk = identical on every peer.
    FeatTypeInterner types(defs);
    auto featTypeIdx = [&](const std::string& nm) { return types.intern(nm); };
    std::vector<std::pair<float, float>> rawMana, rawAll;
    scanFeaturePlane(world, map, defs, types, rawMana, rawAll);
    // Corpse defs: every unit type's FBI corpse= feature (and its chains) joins
    // the table so death can mint corpse records without art/def lookups later.
    for (const auto& [tid, ut] : reg.types()) {
        if (!ut.corpse.empty()) {
            int ci = featTypeIdx(ut.corpse);
            if (ci >= 0) world.mapCorpse(&ut, ci);
        }
    }
    world.setFeatureTypes(std::move(types.table));
    installManaSpots(world, rawMana, rawAll);

    // Players + teams.
    world.setPlayerCount(int(cfg.slots.size()));
    for (int i = 0; i < int(cfg.slots.size()); ++i) {
        world.setTeam(i, cfg.slots[i].team);
        world.player(i).incomeMult = cfg.slots[i].manaMult;   // Absurd AI = 2x income
    }
    world.setUnitCap(cfg.unitCap);
    world.setMonarchExpendable(cfg.monarchExpendable);

    // Assign the used slots to start positions (ring fallback if the map has too few).
    int used = 0;
    for (auto& s : cfg.slots) if (s.used) ++used;
    auto starts = generated ? genStarts : parseStartPositions(vfs, cfg.mapPath);
    // Wind/tide/metal come off the same .ota. A generated map has none, so its
    // economy stays at the defaults (no wind, no tide, no background metal).
    if (!generated) world.setMapEconomy(parseMapEconomy(vfs, cfg.mapPath));
    float cx = map.blocksX * 16.0f, cz = map.blocksY * 16.0f;
    std::vector<std::pair<float, float>> spots = starts;
    // Benchmark: ignore the map's (few) start positions and spread all factions evenly
    // on a big ring around the map centre, so their large armies don't pile onto each
    // other. Radius is kept well inside the map edge (each army block is ~2840px wide).
    if (cfg.benchmark) {
        spots.clear();
        float radius = std::min(cx, cz) * 0.62f;
        for (int k = 0; k < used; ++k) {
            float a = float(k) / float(std::max(used, 1)) * 6.2831853f;
            spots.push_back({cx + detmath::cos(a) * radius, cz + detmath::sin(a) * radius});
        }
    }
    // Random Start Locations: shuffle the spot list before slots are assigned to it.
    // A Fisher-Yates on a small seeded LCG -- integer-only and order-stable, so every
    // peer and the referee land on the same arrangement.
    if (cfg.randomStarts && spots.size() > 1) {
        uint32_t rs = cfg.startSeed ? cfg.startSeed : 0x54414B53u;
        for (size_t i = spots.size() - 1; i > 0; --i) {
            rs = rs * 1103515245u + 12345u;
            size_t j = size_t((rs >> 16) % uint32_t(i + 1));
            std::swap(spots[i], spots[j]);
        }
    }
    while (int(spots.size()) < used) {
        float a = float(spots.size()) / float(std::max(used, 1)) * 6.2831853f;
        spots.push_back({cx + detmath::cos(a) * 300, cz + detmath::sin(a) * 300});
    }
    std::vector<std::pair<float, float>> assigned;
    int spot = 0;
    // Benchmark: a gradual ramp -- each faction spawns 1 unit every 1/spawnsPerSec seconds
    // for 60s (=1800 ticks). Intensity level (cfg.benchmark 1..5) sets the count; stage k
    // fires at tick (k+1)*1800/N via integer math so every peer agrees. Units filled below.
    // Both block layouts below are BLIND grids -- a fixed lattice centred on the start
    // spot -- and World::spawn does no terrain check at all, so a unit whose lattice
    // point lands in a lake, on a cliff face or inside a blocked footprint is simply
    // placed there. Measured on Ulasem with 8 players: 4.5% of benchmark units at
    // intensity Low, 7.2% at Extra Absurd, and 20.5% of a stress-test fill -- one unit
    // in five starting somewhere it cannot stand. They cannot walk out (a blocked cell
    // is blocked in both directions), so they sit there for the whole run, which both
    // looks wrong and quietly makes the benchmark measure a different thing than it
    // claims to: a fifth of the army is inert.
    //
    // So each lattice point gets snapped to the nearest cell the unit can actually
    // occupy. `claimed` stops the snap from stacking bodies: without it, everything
    // that lands in the same lake converges on the one shoreline cell nearest it, and
    // bodies are solid with nothing to pull an overlap apart. Deterministic -- a fixed
    // ring scan over the nav grid, and a claim set filled in a fixed order -- so every
    // peer lays out the identical army and lockstep holds.
    // Cells already spoken for by a placed body. A flat bitmap over the nav grid, not
    // a std::set: the ring scan below can touch tens of thousands of cells per unit on
    // a crowded map, and a set lookup per cell made setup take minutes.
    const int claimW = std::max(world.nav().width(), 1);
    const int claimH = std::max(world.nav().height(), 1);
    std::vector<uint8_t> claimBits(size_t(claimW) * size_t(claimH), 0);
    // Floor division by 16: a body can extend left of x=0, where C's truncation would
    // round the wrong way and claim the wrong cell.
    auto floorDiv16 = [](int v) { return v >= 0 ? v / 16 : -((-v + 15) / 16); };
    auto claimedAt = [&](int x, int z) {
        if (x < 0 || z < 0 || x >= claimW || z >= claimH) return true;   // off-map = taken
        return claimBits[size_t(z) * size_t(claimW) + size_t(x)] != 0;
    };
    // Reserve a whole FOOTPRINT, never a centre cell. fits(x,z,foot) asks about the
    // square [x,x+foot) x [z,z+foot), so that is what a body occupies and that is what
    // has to be claimed. Reserving only {nx,nz} was not overlap protection at all: two
    // 2x2 units could take adjacent cells 16px apart while needing 32, which is exactly
    // where snapping packs them -- along a shoreline, where the free cells are a thin
    // line. Measured on Inner Circle with 8 players: 19059 overlapping pairs with
    // centre-cell claims.
    auto claimFoot = [&](int nx, int nz, int foot) {
        // CENTRED, like everything else that reasons about a footprint: NavGrid::fits
        // and World::cellScore both take (cx,cz) as the body's centre and work from
        // cx - foot/2. Claiming [nx, nx+foot) instead offset every reservation by half
        // a footprint, so with MIXED sizes two disjoint claims could still describe
        // overlapping bodies -- a size-2 and a size-4 two cells apart passed the check
        // while actually overlapping.
        // Claim the cells the BODY ACTUALLY COVERS, not a foot x foot block.
        //
        // A snapped unit sits at the CENTRE of cell nx (ux = nx*16 + 8) with half-width
        // foot*8, so an EVEN footprint straddles half a cell on each side and touches
        // foot+1 cells. Claiming only foot of them let two disjoint claims describe
        // bodies that overlap: a size-2 and a size-3 could end up 32px apart needing 40.
        // Every overlapping pair in the benchmark fill was mixed-size for exactly this
        // reason -- 857 of them over 3140 units, invisible until the test that was
        // supposed to catch them stopped halving its own threshold.
        const int hs = foot * 8;
        const int lox = floorDiv16(nx * 16 + 8 - hs), hix = floorDiv16(nx * 16 + 8 + hs - 1);
        const int loz = floorDiv16(nz * 16 + 8 - hs), hiz = floorDiv16(nz * 16 + 8 + hs - 1);
        for (int z = loz; z <= hiz; ++z)
            for (int x = lox; x <= hix; ++x)
                if (claimedAt(x, z)) return false;
        for (int z = loz; z <= hiz; ++z)
            for (int x = lox; x <= hix; ++x)
                if (x >= 0 && z >= 0 && x < claimW && z < claimH)
                    claimBits[size_t(z) * size_t(claimW) + size_t(x)] = 1;
        return true;
    };
    // Fallback scan position, PER (footprint, nav grid) -- not one shared cursor.
    //
    // The cursor is forward-only so the whole fill costs one pass over the grid rather
    // than a rescan per unit. Sharing it across sizes made that correctness-visible: a
    // 4x4 body advances the cursor past every gap only a 2x2 would fit in, and a later
    // 2x2 whose local search fails cannot go back for them -- so its player's fill
    // stopped early on a map that still had room. One cursor per size and domain keeps
    // the single-pass cost (there are only a handful of distinct sizes) without one
    // roster entry consuming another's space.
    //
    // Grids are identified by FIRST-SEEN ORDER, not by pointer value: snapSpawn is
    // called in a deterministic order on every peer, so the indices match, whereas
    // pointer values would not.
    std::vector<const NavGrid*> sweepGrids;
    std::map<std::pair<int, int>, int> sweepCursors;
    auto sweepGridIdx = [&](const NavGrid* g) {
        for (size_t i = 0; i < sweepGrids.size(); ++i)
            if (sweepGrids[i] == g) return int(i);
        sweepGrids.push_back(g);
        return int(sweepGrids.size() - 1);
    };
    int mapCapacity = -1;  // bodies this map can hold, computed once (see capacityFor)
    // How many bodies of `roster`'s typical size this map can hold, minus 5% slack so
    // the snap is never scraping the last few free cells. Walkable AREA divided by what
    // a roster unit actually occupies -- footprints here run 2x2 to 4x4, so counting
    // cells alone would overstate capacity several-fold.
    auto capacityFor = [&](const std::vector<const UnitType*>& roster) {
        if (mapCapacity >= 0 || roster.empty()) return mapCapacity;
        long walkable = 0;
        const NavGrid& g0 = world.navFor(roster.front());
        for (int z = 0; z < g0.height(); ++z)
            for (int x = 0; x < g0.width(); ++x)
                if (g0.fits(x, z, 1)) ++walkable;
        long footArea = 0;
        for (const UnitType* t : roster) {
            const int fo = std::clamp(std::max(t->footX, t->footZ), 1, 15);
            footArea += long(fo) * long(fo);
        }
        const long meanFoot = std::max<long>(1, footArea / long(roster.size()));
        mapCapacity = int((walkable / meanFoot) * 95 / 100);
        return mapCapacity;
    };
    // How far a snap may look, in cells. Generous on purpose: `claimed` makes units
    // compete for cells, so the radius has to hold the whole block, not just clear the
    // nearest lake. A radius-r disc holds about pi*r^2 cells before terrain takes its
    // cut, and the first attempt at 24 (~1810) quietly ran out against a 1900-unit
    // stress fill, leaving 2.4% of the army in terrain anyway -- the exhaustion looked
    // exactly like a terrain limit, and was not. 96 leaves 1 unit of 15208 misplaced.
    //
    // It costs nothing to be generous here: the ring scan returns the instant it finds
    // a cell, so a wide bound is only ever walked by the units that genuinely need it.
    // Measured over the whole 8-player setup, 24 vs 96 is 3.98s vs 4.08s -- noise. A
    // formula that sized the radius to the unit count was written first and deleted:
    // it was more code, and slower to converge on the right answer than the constant.
    constexpr int kSnapCells = 96;
    auto snapSpawn = [&](const UnitType* t, float& ux, float& uz) {
        if (!t || t->canFly) return true;         // flyers are fine over anything
        const NavGrid& g = world.navFor(t);
        if (g.empty()) return true;
        const int foot = std::clamp(std::max(t->footX, t->footZ), 1, 15);
        const int cx = int(ux) / 16, cz = int(uz) / 16;
        for (int r = 0; r <= kSnapCells; ++r)
            for (int j = -r; j <= r; ++j)
                for (int i = -r; i <= r; ++i) {
                    if (std::max(std::abs(i), std::abs(j)) != r) continue;   // ring only
                    const int nx = cx + i, nz = cz + j;
                    if (!g.fits(nx, nz, foot)) continue;
                    if (!claimFoot(nx, nz, foot)) continue;                  // taken
                    ux = float(nx) * 16 + 8;
                    uz = float(nz) * 16 + 8;
                    return true;
                }
        // Nothing free within kSnapCells of where this unit wanted to be. Sweep a
        // GLOBAL cursor for the next free spot anywhere rather than giving up or
        // dropping the body on top of someone. The cursor only moves forward, so the
        // whole fill costs one pass over the grid however many units ask.
        int& sweepCursor = sweepCursors[{foot, sweepGridIdx(&g)}];
        for (; sweepCursor < claimW * claimH; ++sweepCursor) {
            const int nx = sweepCursor % claimW, nz = sweepCursor / claimW;
            if (!g.fits(nx, nz, foot)) continue;
            if (!claimFoot(nx, nz, foot)) continue;
            ux = float(nx) * 16 + 8;
            uz = float(nz) * 16 + 8;
            return true;
        }
        return false;                             // the map really is full
    };
    // The Monarch is placed before any of this and was never reserved, so a snapped
    // unit could be sent to stand on top of it. Claim its square up front.
    auto claimMonarch = [&](const UnitType* t, float ux, float uz) {
        if (!t || t->canFly) return;
        const int foot = std::clamp(std::max(t->footX, t->footZ), 1, 15);
        claimFoot(int(ux) / 16, int(uz) / 16, foot);
    };

    const int kBenchSpawns = benchmarkSpawns(cfg.benchmark);   // 0 when off
    std::vector<BenchStage> benchPlan;
    if (kBenchSpawns > 0) {
        benchPlan.resize(size_t(kBenchSpawns));
        for (int s = 0; s < kBenchSpawns; ++s)
            benchPlan[size_t(s)].tick = uint32_t((uint64_t(s + 1) * 1800) / uint64_t(kBenchSpawns));
    }
    const std::vector<std::string> commanders =
        cfg.vfs ? sideCommandersImpl(*cfg.vfs) : std::vector<std::string>{};
    for (int i = 0; i < int(cfg.slots.size()); ++i) {
        if (!cfg.slots[i].used) continue;
        // The side's commander, straight out of SIDEDATA. A slot pointing past
        // the side list (a stale lobby, a modded install with fewer sides) falls
        // back to side 0 rather than spawning nothing.
        const UnitType* monarch = nullptr;
        if (!commanders.empty()) {
            size_t si = size_t(cfg.slots[i].faction) % commanders.size();
            monarch = reg.find(commanders[si]);
        }
        float mx = spots[size_t(spot)].first, mz = spots[size_t(spot)].second;
        assigned.push_back({mx, mz});
        ++spot;
        world.spawn(monarch, mx, mz, 0, i);
        claimMonarch(monarch, mx, mz);   // nothing else may be snapped onto it
        world.player(i).metal.cur = cfg.startMana;
        world.player(i).energy.cur = cfg.startMana;


        // Stress test: fill this player to ~95% of the unit cap with its faction's
        // combat units right now, so an all-AI game starts under a heavy sim load.
        // Deterministic: fixed roster (name-sorted), round-robin, grid placement --
        // every peer builds the identical army, so lockstep holds.
        if (cfg.stressTest && monarch) {
            // Land+air combat units only (exclude Water-domain so no boats spawn on land).
            std::vector<const UnitType*> roster;
            for (const UnitType* t : reg.combatUnits(monarch->side))
                if (t->domain != UnitType::Domain::Water) roster.push_back(t);
            int cap = cfg.unitCap > 0 ? cfg.unitCap : 1000;   // unlimited -> a sane default
            int target = (cap * 95) / 100;
            // ...but never more than the MAP can hold. The fill used to ask for 95% of
            // the cap per player regardless -- 15200 bodies at cap 2000 with 8 players,
            // on an Inner Circle that holds about 12000 -- and the excess had nowhere
            // legal to go: it ended up either in terrain (permanently stuck, because a
            // body cannot walk out of a cell it could not walk into) or inside another
            // body. There is no placement that satisfies both once it is
            // over-subscribed, so do not over-subscribe: take 95% of real capacity and
            // leave the last 5% as slack, so the snap is never scraping the last few
            // cells on the map.
            //
            // Capacity is the walkable area divided by what a roster unit actually
            // occupies -- footprints here run 2x2 to 4x4, so counting cells alone would
            // overstate it several-fold.
            mapCapacity = capacityFor(roster);
            // Divide by the players who ACTUALLY SPAWN, not the slot-vector length.
            // Both client and server size that vector to maxSlot + 1, so a lobby with
            // gaps (say players in slots 0 and 7) counts 8 and hands each of the two a
            // quarter of what the map can hold -- reserving room for six armies that
            // never arrive, and quietly shrinking every stress and benchmark run on a
            // capacity-limited map.
            const int players = std::max(1, used);
            target = std::min(target, std::max(1, mapCapacity / players));
            if (!roster.empty() && target > 0) {
                int cols = 1;                                 // integer ceil(sqrt(target))
                while (cols * cols < target) ++cols;
                // Space units out so they don't start piled inside one another. This was
                // sized for the separation pass (~1.5 per 32px nav cell left it a handful
                // of neighbours to push, against the pathological ~4/cell a tight 16px
                // grid produced -- the whole initial separation spike, a pure spawn
                // artifact). That pass is gone, and the spacing matters as much now for a
                // different reason: bodies are solid and nothing pulls an overlap apart,
                // so units spawned inside each other would simply stay that way.
                const float spacing = 24.0f;
                float x0 = mx - float(cols) * spacing * 0.5f; // centre the block on the start
                float z0 = mz + spacing;                      // just south of the Monarch
                for (int k = 0; k < target; ++k) {
                    const UnitType* t = roster[size_t(k) % roster.size()];
                    float ux = x0 + float(k % cols) * spacing;
                    float uz = z0 + float(k / cols) * spacing;
                    // The lattice ignores terrain; this does not. False means the map
                    // is genuinely full -- stop rather than stack the remainder.
                    if (!snapSpawn(t, ux, uz)) break;
                    world.spawn(t, ux, uz, 0, i);
                }
            }
        }
        // Benchmark: append this faction's staged units (land+air combat only -- Water-
        // domain excluded, so no boats on land) to the plan, on a deterministic grid
        // centred on the start. Same roster/order/grid on every peer -> lockstep.
        if (cfg.benchmark && monarch) {
            std::vector<const UnitType*> roster;
            for (const UnitType* t : reg.combatUnits(monarch->side))
                if (t->domain != UnitType::Domain::Water) roster.push_back(t);
            if (!roster.empty()) {
                // Cap the plan at what the map can hold, exactly as the stress fill
                // does. Intensity 5 asks for 960 units per player -- 7680 across 8 --
                // and Inner Circle holds about 3100 bodies of this roster's footprint,
                // so most of the top intensities were queueing units that had nowhere
                // legal to stand. The RAMP is what the benchmark measures, so this
                // shortens it rather than thinning it: the run still spawns at the
                // chosen rate, it just stops once the map is full.
                mapCapacity = capacityFor(roster);
                const int players = std::max(1, int(cfg.slots.size()));
                const int myShare = std::max(1, mapCapacity / players);
                const int plan = std::min(kBenchSpawns, myShare);
                int cols = 1; while (cols * cols < plan) ++cols;   // ceil(sqrt(plan))
                const float spacing = 40.0f;
                float x0 = mx - float(cols) * spacing * 0.5f;   // centre the block on the start
                float z0 = mz - float(cols) * spacing * 0.5f;
                // One unit per stage (each 0.25s tick), cycling the roster, on a grid cell.
                // Snapped at PLAN time, not spawn time: the plan is built identically on
                // every peer here, whereas snapping during the run would have to agree
                // about a world that has moved on.
                for (int s = 0; s < plan; ++s) {
                    const UnitType* t = roster[size_t(s) % roster.size()];
                    float ux = x0 + float(s % cols) * spacing;
                    float uz = z0 + float(s / cols) * spacing;
                    if (!snapSpawn(t, ux, uz)) break;   // map full: stop, do not stack
                    benchPlan[size_t(s)].units.push_back({t, ux, uz, i});
                }
            }
        }
    }
    if (cfg.benchmark) world.setBenchmarkPlan(std::move(benchPlan), 1800);   // end at tick 1800 (60s)
    // Block the (structure) footprints just spawned. Monarchs move, so this is a
    // no-op today, but it mirrors the client and covers any non-mover spawns.
    for (auto& u : world.units()) {
        // isStructure (maxVel <= 0), not canMove: see the note in World::startBuild --
        // two of the four walls set canmove=1 and would otherwise block nothing.
        if (!u.type || !u.type->isStructure()) continue;
        world.blockFoot(*u.type, u.x, u.z, true);
    }
    return assigned;
}

bool setupMission(World& world, const TypeRegistry& reg, const hpi::Vfs& vfs,
                  const std::string& stem, int& humanOut, MissionSetup* out) {
    const std::string base = "missions/" + stem;
    // The .cob is OPTIONAL: 11 of the 74 shipped missions (takmission05_dh --
    // chapter 5 of Book of Darien! -- 07/23/28/40_ph/dh and takx07/10/12/14/15/17)
    // are pure DATA missions with no god script at all; they run entirely on the
    // .ota win/lose conditions and the placed units' InitialMission order queues,
    // both of which we implement. Requiring a cob dead-ended the campaign at
    // chapter 5.
    if (!vfs.has(base + ".tnt") || !vfs.has(base + ".ota")) {
        std::fprintf(stderr, "setupMission: '%s' not found\n", stem.c_str());
        return false;
    }
    auto otaBytes = vfs.read(base + ".ota");
    tdf::Node root = tdf::parseText(std::string(otaBytes.begin(), otaBytes.end()), base + ".ota");
    const tdf::Node* gh = root.child("globalheader");
    if (!gh) { std::fprintf(stderr, "setupMission: %s no [GlobalHeader]\n", stem.c_str()); return false; }

    // Map the .ota's sparse, 1-based Player<N> ids to compact 0-based World slots in
    // ascending N order, so gaps (e.g. Player1,2,9,10) don't collide when clamped into
    // kMaxPlayers slots. The SAME map applies to the placed units and to the script's
    // Create player refs (passed to MissionScript). Diplomacy from each player's role:
    // opponents fight the human (team 1); the human, allies and neutrals share team 0,
    // so the human never auto-attacks them. Human = the first interactive (non-AI) slot.
    std::vector<int> otaToWorld(17, -1);   // [.ota player 1..16] -> world slot
    std::vector<MatchSlot> slots;
    int human = 0;
    bool foundHuman = false;
    const char* kingdoms[5] = {"aramon", "taros", "veruna", "zhon", "creon"};
    for (int n = 1; n <= 16 && int(slots.size()) < kMaxPlayers; ++n) {
        const std::string* v = gh->value("player" + std::to_string(n));
        if (!v) continue;
        std::string def = *v;
        std::transform(def.begin(), def.end(), def.begin(), ::tolower);
        int slot = int(slots.size());
        otaToWorld[n] = slot;
        MatchSlot s;
        s.used = false;   // no monarch spawn -- units come from [Map Data][units]
        s.team = def.find("opponent") != std::string::npos ? 1 : 0;
        for (int f = 0; f < 5; ++f) if (def.find(kingdoms[f]) != std::string::npos) s.faction = f;
        slots.push_back(s);
        bool strategic = def.find("strategic") != std::string::npos;
        bool ai = strategic || def.find("passive") != std::string::npos;
        if (!ai && !foundHuman) { human = slot; foundHuman = true; }
        // Only a STRATEGIC player gets a brain. A "passive neutral" is scenery --
        // villagers, wildlife, props -- and must stay inert.
        if (strategic && out) out->aiSlots.push_back(slot);
    }
    if (slots.empty()) { slots.push_back(MatchSlot{}); otaToWorld[1] = 0; }   // at least the human
    auto mapPlayer = [&](int otaP) {
        if (otaP >= 1 && otaP <= 16 && otaToWorld[size_t(otaP)] >= 0) return otaToWorld[size_t(otaP)];
        return std::clamp(otaP - 1, 0, int(slots.size()) - 1);
    };

    MatchConfig cfg;
    cfg.vfs = &vfs;
    cfg.mapPath = base + ".tnt";
    cfg.slots = slots;                 // no used slots -> setupMatch spawns no monarchs
    cfg.unitCap = int(gh->numberOr("maxunits", 500));
    setupMatch(world, reg, cfg);       // terrain + features + player teams
    if (gh->numberOr("waterdoesdamage", 0) != 0)
        world.setWaterDamage(float(gh->numberOr("waterdamage", 0)));

    // Placed units from [Map Data][units].
    int spawned = 0;
    std::vector<std::pair<int, std::string>> initialOrders;   // (unit id, InitialMission)
    std::unordered_map<std::string, int> idents;              // Ident= -> unit id
    if (const tdf::Node* md = gh->child("map data"))
        if (const tdf::Node* units = md->child("units"))
            for (const auto& key : units->childOrder) {
                const tdf::Node& u = units->children.at(key);
                std::string name = u.valueOr("unitname", "");
                std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                const UnitType* t = reg.find(name);
                if (!t) continue;
                float x = float(u.numberOr("xpos", 0)) * 16 + 8;
                float z = float(u.numberOr("zpos", 0)) * 16 + 8;
                int player = mapPlayer(int(u.numberOr("player", 1)));
                float ang = float(u.numberOr("angle", 0)) * (3.14159265f / 180.0f);
                int id = world.spawn(t, x, z, ang, player);
                if (id >= 0) {
                    ++spawned;
                    if (auto* su = world.unit(id))
                        su->hp = su->type->maxHp *
                                 float(u.numberOr("healthpercentage", 100)) / 100.0f;
                    // InitialMission: the per-unit order queue in the SAME mini-language
                    // the god script's SetMission uses (move/patrol/attack/stance/wait/
                    // ambush/reinforce). 4416 placed units across the shipped missions
                    // carry one -- it IS the NPC choreography (patrol routes, ambush
                    // holds, the mission-9 alchemist's scripted walk). Queued here and
                    // applied by the script once the world is built.
                    if (const std::string* im = u.value("initialmission"))
                        if (!im->empty()) initialOrders.push_back({id, *im});
                    // Ident=<name>: 275 placed units are named, and a bodyguard's
                    // `g <name>` clause resolves against these.
                    if (const std::string* idn = u.value("ident"))
                        if (!idn->empty()) {
                            std::string k = *idn;
                            std::transform(k.begin(), k.end(), k.begin(), ::tolower);
                            idents[k] = id;
                        }
                }
            }

    // Where each player's forces actually are, so an AI can march on a real base
    // instead of a start position a mission never declares. The centroid of a
    // slot's placed units is the closest thing a mission has to a "start".
    if (out) {
        // Fog: retail presents some missions fully revealed (lineofsight=0, 6 of
        // them) and others with the terrain already mapped but units still hidden
        // (mapping=1, 30). Both start blacked out for us, which changes how a
        // scripted reveal reads and hides set-pieces the designer meant you to see.
        out->fullVision = gh->numberOr("lineofsight", 1) == 0;
        out->preMapped = gh->numberOr("mapping", 0) != 0;
        if (const tdf::Node* md = gh->child("map data"))
            {
                out->aiProfile = md->valueOr("aiprofile", "");
                std::transform(out->aiProfile.begin(), out->aiProfile.end(),
                               out->aiProfile.begin(), ::tolower);
            }
        if (out->aiProfile == "default") out->aiProfile.clear();
        out->slotPos.assign(size_t(kMaxPlayers), {0.0f, 0.0f});
        std::vector<int> n(size_t(kMaxPlayers), 0);
        for (const auto& u : world.units()) {
            if (!u.alive() || !u.type) continue;
            if (u.player < 0 || u.player >= kMaxPlayers) continue;
            out->slotPos[size_t(u.player)].first += u.x;
            out->slotPos[size_t(u.player)].second += u.z;
            ++n[size_t(u.player)];
        }
        for (size_t i = 0; i < out->slotPos.size(); ++i)
            if (n[i] > 0) {
                out->slotPos[i].first /= float(n[i]);
                out->slotPos[i].second /= float(n[i]);
            }
    }

    // Attach + start the in-sim "god" script (its spawns/triggers run from World::tick).
    // A data-only mission has no cob: MissionScript still runs the .ota conditions and
    // applies the InitialMission queues, it just has no bytecode VM.
    std::vector<uint8_t> cobBytes;
    if (vfs.has(base + ".cob")) cobBytes = vfs.read(base + ".cob");
    auto ms = std::make_unique<MissionScript>(std::move(cobBytes), *gh, reg, human, stem, otaToWorld);
    ms->setInitialOrders(std::move(initialOrders));
    ms->setIdents(std::move(idents));
    world.setMission(std::move(ms));
    humanOut = human;
    std::fprintf(stderr, "setupMission: %s -- %d placed units, human=player%d\n",
                 stem.c_str(), spawned, human);
    return true;
}

}  // namespace ta::sim
