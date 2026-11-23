// setup_reachability_test: no whole-map reachability labelling inside a tick.
//
//   setup_reachability_test <retail-install-dir>
//
// World::pathExists and a factory's output parking (parkProductionOutput) answer
// "can this body get there?" from a connected-component labelling per movement
// grid and footprint (World::components). It was built the first time each was
// asked for -- inside the tick a factory released its first unit of that class --
// as one flood fill over the whole map: 82-110 ms on a 2048-cell map, four times
// in the first three minutes of every game, on every peer. Later walkability
// edits are carried across incrementally, so only that first build is a whole-map
// one. setupMatch now builds the labelling for every class the seated sides can
// bring into play (World::prebuildComponents), before tick 0.
//
// Two sides on a shipped map. Each side's first factory makes its first ground
// unit, and every ground or sea type each Monarch's build tree reaches answers a
// pathExists query, without World::compStats().relabels moving after setup.
//
// A dev harness; not shipped in a game.

#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace ta;

namespace {
int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

bool mobileOnGround(const sim::UnitType* t) { return t && !t->canFly && !t->isStructure(); }

// The types `root` brings into play: itself, what it builds, and so on.
std::vector<const sim::UnitType*> buildTree(const sim::TypeRegistry& reg, const sim::UnitType* root) {
    std::set<std::string> seen;
    std::vector<const sim::UnitType*> open, out;
    auto add = [&](const sim::UnitType* t) { if (t && seen.insert(t->id).second) { open.push_back(t); out.push_back(t); } };
    add(root);
    while (!open.empty()) {
        const sim::UnitType* t = open.back(); open.pop_back();
        for (const auto& name : reg.buildable(t->id)) add(reg.find(name));
    }
    return out;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: setup_reachability_test <retail-install-dir>\n");
        return 2;
    }
    hpi::Vfs vfs = hpi::mountRetailRoot(argv[1]);
    sim::TypeRegistry reg;
    sim::setupRegistry(reg, vfs, false);

    sim::World w;
    w.setSerialThreads(true);
    sim::MatchConfig cfg;
    cfg.vfs = &vfs;
    cfg.mapPath = "maps/Two Castles.tnt";
    cfg.slots.resize(2);                 // the map's two starts
    const int factions[2] = {0, 4};      // Aramon and Creon: each has a ground factory
    for (int i = 0; i < 2; ++i) { cfg.slots[size_t(i)].used = true; cfg.slots[size_t(i)].faction = factions[i]; cfg.slots[size_t(i)].team = i; }
    const auto spots = sim::setupMatch(w, reg, cfg);
    const uint64_t built = w.compStats().relabels;
    std::printf("setup built %llu labellings\n", (unsigned long long)built);
    check(built > 0, "setupMatch builds the reachability labellings");

    std::vector<int> monarchs;
    for (const auto& u : w.units()) if (u.alive() && u.type && u.type->commander) monarchs.push_back(u.id);
    check(monarchs.size() == 2, "two Monarchs");

    // Each side's first factory releases its first ground unit: the parking ring
    // asks for its class's labelling (the audited game's four freezes).
    std::vector<int> factories;
    std::vector<const sim::UnitType*> outputs;
    for (const int m : monarchs) {
        const sim::Unit* king = w.unit(m);
        const sim::UnitType* factory = nullptr; const sim::UnitType* output = nullptr;
        for (const auto& name : reg.buildable(king->type->id)) {
            const sim::UnitType* f = reg.find(name);
            if (!f || !f->isStructure()) continue;
            for (const auto& made : reg.buildable(f->id)) {
                const sim::UnitType* o = reg.find(made);
                if (o && mobileOnGround(o) && o->domain == sim::UnitType::Domain::Ground) { factory = f; output = o; break; }
            }
            if (factory) break;
        }
        if (!factory) { check(false, king->type->id + " has a ground factory"); continue; }
        // On the side of the Monarch facing the middle of the map, clear of the edge.
        const float kx = king->x.toFloat(), kz = king->z.toFloat();
        const int player = king->player;   // `king` does not outlive the spawn
        const float mx = float(w.nav().width()) * 8, mz = float(w.nav().height()) * 8;
        const float len = std::max(1.f, std::hypot(mx - kx, mz - kz));
        const int id = w.spawn(factory, kx + (mx - kx) / len * 320, kz + (mz - kz) / len * 320, 0, player);
        w.player(player).mana = 1000000;
        w.train(id, output, 1);
        factories.push_back(id);
        outputs.push_back(output);
        std::printf("  player %d: %s at (%.0f,%.0f) makes a %s\n", player, factory->id.c_str(),
                    w.unit(id)->x.toFloat(), w.unit(id)->z.toFloat(), output->id.c_str());
    }
    const uint64_t beforeTicks = w.compStats().relabels;
    std::vector<bool> out(factories.size(), false);
    int ticks = 0;
    for (; ticks < 6000; ++ticks) {
        w.tick(1.f / 30);
        bool all = true;
        for (size_t i = 0; i < factories.size(); ++i) {
            if (!out[i])
                for (const auto& u : w.units())
                    if (u.alive() && u.type == outputs[i] && !u.underConstruction && u.player == w.unit(factories[i])->player) out[i] = true;
            all = all && out[i];
        }
        if (all) break;
    }
    int made = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        made += out[i];
        if (!out[i]) {
            const sim::Unit* f = w.unit(factories[i]);
            std::printf("  no %s from %s: queue %zu, site %d\n", outputs[i]->id.c_str(), f->type->id.c_str(),
                        f->buildQueue.size(), f->productionSiteId);
        }
    }
    std::printf("  %d of %zu outputs out after %d ticks; labellings built in those ticks: %llu\n", made, out.size(), ticks,
                (unsigned long long)(w.compStats().relabels - beforeTicks));
    check(made == int(factories.size()) && made == 2, "every factory releases its first ground unit");
    check(w.compStats().relabels == beforeTicks, "no whole-map labelling inside a tick");

    // Every class a side can field answers from a labelling built at setup.
    const uint64_t beforeQueries = w.compStats().relabels;
    int asked = 0;
    for (const int m : monarchs) {
        const sim::Unit* king = w.unit(m);
        for (const sim::UnitType* t : buildTree(reg, king->type)) {
            if (!mobileOnGround(t)) continue;
            (void)w.pathExists(t, king->x.toFloat() + 200, king->z.toFloat(), king->x.toFloat(), king->z.toFloat());
            ++asked;
        }
    }
    std::printf("asked %d ground and sea types; labellings built for them: %llu\n", asked,
                (unsigned long long)(w.compStats().relabels - beforeQueries));
    check(asked > 10 && w.compStats().relabels == beforeQueries, "no labelling is built for a reachability query after setup");
    (void)spots;
    if (failures) {
        std::printf("setup_reachability: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("setup_reachability: all passed\n");
    return 0;
}
