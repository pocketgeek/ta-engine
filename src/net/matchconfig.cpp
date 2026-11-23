#include "net/matchconfig.h"

#include "ai/ai.h"   // incomeMultFor / difficultyFromLevel (inline)
#include "version.h"

namespace tak::net {

sim::MatchConfig matchConfigFor(const GameOptions& o, const SlotInfo* slots, uint32_t seed, bool mission) {
    sim::MatchConfig cfg;
    cfg.unitCap = o.unitCap;
    cfg.monarchExpendable = o.monarchExpendable != 0;
    cfg.doubleSight = !mission && o.doubleSight != 0;
    cfg.pathfindingMode = mission ? sim::PathfindingMode::Retail : o.pathfindingMode;
    cfg.stressTest = o.stressTest != 0;
    cfg.stressImmediate = o.stressTest == kStressImmediate;
    cfg.benchmark = o.benchmark;
    cfg.startMode = sim::StartMode(normalizeStartMode(o.randomStarts));
    cfg.mapScenario = !mission && o.mapScenario != 0;   // the map's own scenario
    cfg.powerCodes = !mission && o.powerCodes != 0;     // never in a campaign (retail 0x499610)
    cfg.powerMapRevealed = cfg.powerCodes && o.fogExplored >= 1;   // +Mapping's start (Map Revealed)
    cfg.roomMapRevealed = !mission && o.fogExplored >= 1;   // a mission's own .ota decides (setupMission)
    cfg.roomLosOff = !mission && o.fogExplored == 2;
    cfg.startSeed = seed;   // the seed sent in GameStarting
    int maxSlot = 0;
    for (int i = 0; i < kMaxSlots; ++i)
        if (slots[i].type == 1 || slots[i].type == 2) maxSlot = i;
    cfg.slots.resize(size_t(maxSlot + 1));
    for (int i = 0; i <= maxSlot; ++i) {
        const SlotInfo& s = slots[i];
        const auto level = ai::difficultyFromLevel(s.aiLevel);
        const float mm = s.type == 2 ? ai::incomeMultFor(level) : 1.0f;
        cfg.slots[size_t(i)] = {s.type == 1 || s.type == 2, s.faction % 5, s.team, mm, s.type == 2,
                                s.type == 2 && ai::defensiveLevel(level)};
    }
    return cfg;
}

ReplayHeader replayHeaderFor(const GameOptions& o, const std::string& mapId, const std::string& mission,
                             const SlotInfo* startSlots, uint32_t seed) {
    ReplayHeader h;
    h.mapId = mapId;
    h.mission = mission;
    h.engineVersion = tak::kVersion;
    h.crusades = o.crusades;
    h.forfeitSelfDestruct = o.forfeitSelfDestruct;
    h.gameMods = o.gameMods;
    h.unitCap = o.unitCap;
    h.monarchExpendable = o.monarchExpendable;
    h.doubleSight = o.doubleSight;
    h.pathfindingMode = o.pathfindingMode;
    h.stressTest = o.stressTest;
    h.randomStarts = o.randomStarts;
    h.benchmark = o.benchmark;
    h.mapScenario = o.mapScenario;
    // Bit 0: POWER CODES on. Bit 1: the room started with the map revealed, the one
    // part of the display-only FOG OF WAR option the sim reads (powerMapRevealed).
    h.powerCodes = o.powerCodes ? uint8_t(1 | (o.fogExplored >= 1 ? 2 : 0)) : uint8_t(0);
    h.fogExplored = o.fogExplored;   // format 15: area clear's fog test reads it
    h.seed = seed;
    for (int i = 0; i < kMaxSlots; ++i) {
        const SlotInfo& s = startSlots[i];   // start config, not the forfeited end state
        h.slotType[i] = s.type;
        h.slotFaction[i] = s.faction;
        h.slotColor[i] = s.color;
        h.slotTeam[i] = s.team;
        h.slotAiLevel[i] = s.aiLevel;
    }
    return h;
}

void matchInputsFrom(const ReplayHeader& h, GameOptions& o, SlotInfo* slots) {
    o = GameOptions{};
    o.crusades = h.crusades;
    o.forfeitSelfDestruct = h.forfeitSelfDestruct;
    o.gameMods = h.gameMods;
    o.unitCap = uint16_t(h.unitCap);
    o.monarchExpendable = h.monarchExpendable;
    o.doubleSight = h.doubleSight;
    o.pathfindingMode = h.pathfindingMode;
    o.stressTest = h.stressTest;
    o.randomStarts = h.randomStarts;
    o.benchmark = h.benchmark;
    o.mapScenario = h.mapScenario;
    o.powerCodes = h.powerCodes & 1;
    o.fogExplored = h.fogExplored;
    for (int i = 0; i < kMaxSlots; ++i) {
        // Every field explicitly from the header, never SlotInfo's defaults
        // (aiLevel 2).
        slots[i] = SlotInfo{};
        slots[i].type = h.slotType[i];
        slots[i].faction = h.slotFaction[i];
        slots[i].color = h.slotColor[i];
        slots[i].team = h.slotTeam[i];
        slots[i].aiLevel = h.slotAiLevel[i];
    }
}

}  // namespace tak::net
