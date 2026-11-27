#pragma once

// Total Annihilation's own computer player, ported from TotalA.exe
// (docs/retail-ai.md): the AI profile (plan / weight / limit), the per-type value
// tables, the build score and weighted pick, the base centre and lattice site
// finder, extractors sent to ore features, and nine squads each run by a timed task
// (economy/factories, land and naval strike groups and their reserves, builders,
// air patrols). Deterministic: one Park-Miller stream per AI player (retail draws
// from the shared stream on one host; lockstep needs its own), and every order goes
// out as an ordinary sim command.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ai/ai.h"

namespace ta::sim { class World; class TypeRegistry; struct UnitType; struct Unit; }

namespace ta::ai {

class TaRetailBrain {
public:
    // difficulty: 0 Easy, 1 Medium, 2 Hard (the profile's plan words and the income
    // multiplier, applied by the sim through Player books).
    TaRetailBrain(int player, uint32_t gameSeed, const ta::sim::TypeRegistry& registry, int difficulty);
    // The map's AI profile text (ai/<aiprofile>.txt, chosen by the server).
    void useProfile(const std::string& text);
    // The engine's defensive levels: build and defend, never launch strike groups.
    void setAggressive(bool on) { aggressive_ = on; }
    // Retail scenario mode 1 (a campaign mission): never build downloadable units.
    void setCampaign(bool on) { campaign_ = on; }
    int difficulty() const { return difficulty_; }
    bool aggressive() const { return aggressive_; }
    void tick(const ta::sim::World& world, uint32_t now, const CommandSink& sink);

    // Read-only, for tests and the roster dump.
    int weightOf(const std::string& unit) const;
    int limitOf(const std::string& unit) const;

private:
    struct Squad { std::vector<int> units; int state = 0; };
    struct Task { uint32_t next = 0; };
    struct Lattice { int sx = 8, sz = 8, ox = 0, oz = 0; };

    uint32_t random(uint32_t n);   // retail Random(n): Park-Miller 16807, seed % n, no draw when n < 2
    void applyProfileLine(const std::vector<std::string>& tok, bool& planOn, bool fbiLine);
    void buildLattice(const ta::sim::World& world);
    void cache(const ta::sim::World& world);
    void tables(const ta::sim::World& world);
    int score(const ta::sim::World& world, size_t t) const;
    const ta::sim::UnitType* choose(const ta::sim::World& world, const ta::sim::UnitType* builder);
    bool findSite(const ta::sim::World& world, const ta::sim::UnitType* builder, float px, float pz,
                  const ta::sim::UnitType* t, float& sx, float& sz);
    void assignSquads(const ta::sim::World& world);
    void taskEconomy(const ta::sim::World& world, uint32_t now, const CommandSink& sink);
    void taskStrike(const ta::sim::World& world, int squad, int src, int64_t radius, const CommandSink& sink);
    void taskFollow(const ta::sim::World& world, int squad, int lead, const CommandSink& sink);
    void taskBuilders(const ta::sim::World& world, uint32_t now, const CommandSink& sink);
    void taskAir(const ta::sim::World& world, const CommandSink& sink);
    size_t typeIndex(const ta::sim::UnitType* t) const;
    bool centroid(const ta::sim::World& world, int squad, float& x, float& z) const;
    int nearestEnemy(const ta::sim::World& world, float x, float z) const;

    int player_;
    int difficulty_;
    uint32_t seed_;
    const ta::sim::TypeRegistry& reg_;
    std::vector<const ta::sim::UnitType*> types_;   // sorted by unitname (retail type order)
    std::vector<uint8_t> weight_;
    std::vector<int32_t> limit_;
    std::vector<uint8_t> wlock_, llock_;
    std::vector<int> count_;
    std::vector<int8_t> value_, base_, baseML_, baseEL_;
    float baseX_ = 0, baseZ_ = 0;
    bool haveBase_ = false;
    float R_ = 0;
    int buildCapable_ = 0;
    std::vector<std::array<int, 3>> oreSpots_;   // cell x, cell z, metal
    bool oreBuilt_ = false;
    Lattice land_, water_;
    bool latticeBuilt_ = false;
    uint32_t cmdHoldUntil_ = 0;
    std::array<Squad, 10> squads_{};
    std::array<Task, 10> tasks_{};
    int squadCountdown_ = 30;
    uint32_t lastCache_ = 0;
    bool cacheRan_ = false;
    std::vector<int> squadOf_;   // unit id -> squad (0 unassigned)
    int lastCommanderHp_ = -1;
    bool aggressive_ = true;
    bool campaign_ = false;
};

}  // namespace ta::ai
