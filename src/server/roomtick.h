#pragma once
#include "net/protocol.h"
#include <array>
#include <optional>
#include <vector>
namespace ta::sim {class World;class TypeRegistry;}
namespace ta::ai {class Controller;}
namespace ta::srv {
struct TickInput {
    uint32_t tick=0,roomId=0;
    std::vector<net::Command> commands,scheduled;
    std::vector<net::Event> events;
    uint64_t replayBudget=0;
    // The threads this tick's simulation may use, its own included (the game's
    // share of taserver --sim-threads at submission); 0 leaves the World's width.
    unsigned simThreads=0;
    // taserver --sim-cpus: this game's CPUs at submission, by slot (its worker
    // thread, then its pool's workers; a slot past the end, or one holding -1, is
    // not pinned). simPin false: the server pins nothing, and no thread's CPUs are
    // touched.
    bool simPin=false;
    std::vector<int> simCpus;
    // Who reads the referee's hash: seated humans' desync reports (every
    // kHashPeriod ticks) and the recorded replay's checkpoint trail (every 10th
    // of those, with posDigest). Neither: the hash and posDigest are skipped.
    bool clientHashes=true,replayTrail=true;
    // The players that have had living units in this game up to the last finished
    // tick, one bit each (TickResult::fielded of that tick; 0 before the first).
    uint8_t fielded=0;
};
struct TickResult {
    std::vector<uint8_t> bundle;
    std::optional<uint64_t> hash;
    uint64_t posDigest=0;                // World::posDigest on the ticks the replay trail records (every 10th hash tick)
    std::array<bool,net::kMaxSlots> defeated{};
    int missionOutcome=0;
    bool resourceLimited=false;
    // The match's standing after this tick, as a watcher's client reads it from
    // its own sim (a skirmish or a map scenario; the server ignores it for a
    // campaign mission, which reports missionOutcome, and for a Benchmark).
    // fielded: the input's players plus those with living units now. decided: the
    // match has its result -- one team is left, or nobody is, once two teams have
    // fielded units (a game of one team never has one); in a map scenario, every
    // participant has its outcome. Recomputed each tick, as World::winningTeam()
    // is. winningTeam: World::winningTeam() when decided (-1: nobody won).
    uint8_t fielded=0;
    bool decided=false;
    int8_t winningTeam=-1;
};
// Mutates only this game's simulation. No sockets, Room, account store or logs.
TickResult simulateRoomTick(sim::World& world,const sim::TypeRegistry& registry,
                            std::vector<ai::Controller>& controllers,TickInput input);
}
