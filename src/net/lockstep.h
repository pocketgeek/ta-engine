#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace tak::net {

// Player command wire format, shared by the client and server and reused by the
// sim's applyCommand(). This was originally the payload of a 2-peer TCP lockstep
// session -- hence the file name -- but that Session is gone; the transport is
// now the server-sequenced client-server protocol in net/protocol.h.

enum class Cmd : uint8_t {
    Move, Attack, AttackMove, Patrol, Stop, Train, Build, Load, Unload, Guard,
    SetWeapon,     // targetId = weapon slot (0=primary, 1, 2)
    RepeatTrain,   // ctrl+click a conjure icon: toggle infinite production of `type`
    Destroy,       // self-destruct unitId (Ctrl+D) -- via the command path so
                   // networked sims stay in lockstep
    Unqueue,       // remove `targetId` copies of `type` from unitId's build queue
                   // (Train also carries a count in targetId; 0 => 1)
    Assist,        // unitId (a mobile builder) resumes/assists conjuring the
                   // existing construction site targetId (revives a decaying one)
    Disco,         // cosmetic emote: unitId dances for 10s (Shift+D); 0 = living monarch.
                   // queue=0 starts a real-time emote; queue=1 appends selection targets.
    Reclaim,       // mobile builder unitId reclaims feature targetId for mana
    Stance,        // set unitId's combat stance to targetId (0=offensive/1=defensive/
                   // 2=passive) -- gates auto-acquire and chase leash
    Cloak,         // canCloak unitId: targetId != 0 enables cloaking, 0 disables
    SetActive,     // onOffable unitId: targetId != 0 powers on, 0 powers down
    Repair,        // mobile builder unitId repairs damaged friendly targetId (restores
                   // HP at its build rate, draining mana)
    Headbang,      // cosmetic emote: unitId headbangs 10s (Shift+H); 0 = living monarch.
    SetSquad,      // assign unitId to a control squad: targetId = 0 none, +N group N,
                   // -N formation N (N=1..10). A unit is in exactly one squad; formations
                   // move at their slowest member's speed and their stragglers rejoin.
    ReclaimArea,   // persistent area clear: x/z through x2/z2, chooses each next target in sim
    GiveUnit,      // unitId = owned eligible unit, targetId = allied recipient player
    ShareMana,     // targetId = allied recipient, queue = enabled; no unit required
    BuildManaArea, // mobile builder: lodestones in x/z through x2/z2, exploring as needed
    // An explicit selection order (Legion 10,000 stage 3): one
    // player intent over a whole selection, `order` holds it. Never sent in a
    // PlayerCommands message: the uplink carries it as an OrderBegin /
    // OrderMembers / OrderCommit transaction (net/protocol.h) and the server
    // puts the committed order into ONE tick bundle, its activation tick.
    Order,
    // A power code (sim/powercodes.h), honoured only in a match created with
    // POWER CODES on: targetId = the sim::PowerCode (Atm..NowISee), queue = the
    // sim::PowerArg (LOS On/Off), unitId = the unit for ManaMe (one command per
    // selected unit). The server drops it when the room's option is off, and the
    // sim ignores it there as well. An ordinary PlayerCommands item (base layout).
    PowerCode,
    // Protocol 305: the caster unitId raises the body of dead unit targetId
    // (retail RESURRECT: a resurrect, else an animate; World::resurrect). The
    // client sends it where retail's right-click resolver (0x4de530 mode 1)
    // would pick it over a reclaim. An ordinary PlayerCommands item (base layout).
    Resurrect,
    // Protocol 312: armed ATTACK on the ground: unitId fires at (x, z)
    // (retail 4de530 kind 3 with a point: SUPPRESS, World::suppress). An
    // ordinary PlayerCommands item (base layout); an order verb (Shared).
    AttackGround,
    // Protocol 312: the server's classic (Retail) AI tells the sims its squads
    // (World::setAiGroup): unitId != 0 puts that unit in squad targetId (0
    // none); unitId == 0 sets squad targetId's formation flag to queue != 0.
    // Applied only for a computer-owned player; never accepted from a client.
    AiGroup,
    // Protocol 315: the server's wall clock for a Benchmark run (World::benchmarkClock):
    // targetId = how many of the spawn plan's stages are due by now, queue != 0 = the run's
    // 60 real seconds are over. Sent only by the server, in a benchmark room; never
    // accepted from a client.
    BenchClock,
};

struct SelectionOrder;

// Command::queue on an order verb (Move, Attack, AttackMove, Patrol, Build,
// BuildManaArea, Assist, Guard, Load, Unload, Reclaim, ReclaimArea,
// Resurrect, Repair): the low two bits place the order -- kQueueReplace
// clears the queue, kQueueAppend adds it at the end (Shift), kQueueFront puts
// it at the head without clearing anything (Ctrl; retail 4d7a09 -> 4d7750) --
// and kQueueToggle marks a player's own click, whose Shift or Ctrl order
// instead removes a queued one of the same kind on its target or within 16 px
// of its point (retail 4d7a30). A producer's (or a repeating mobile builder's)
// Move, AttackMove, Patrol and Attack act on its rally list. The AI and
// scripts send only kQueueReplace and kQueueAppend. Other verbs keep their
// own uses of the byte (ShareMana, Disco, PowerCode).
inline constexpr uint8_t kQueueReplace = 0, kQueueAppend = 1, kQueueFront = 2, kQueuePlace = 3,
                         kQueueToggle = 4;

struct Command {
    Cmd kind = Cmd::Move;
    uint8_t player = 0;
    int32_t unitId = 0;
    int32_t targetId = 0; // target unit; for Unload, destination Y in signed 16.16
    float x = 0, z = 0;
    uint8_t queue = 0;
    char type[16] = {};   // unit type id for Train/Build
    float x2 = 0, z2 = 0; // ReclaimArea upper rectangle endpoint
    // Cmd::Order only: the order (immutable once built; shared by the copies a
    // command makes between the wire, the server's queue and the sim).
    std::shared_ptr<const SelectionOrder> order;
};

// How an order's members get their goal (only Move uses Offsets; only Move,
// Patrol and AttackMove use FlyerOffsets):
//   Shared        every member is given the template's command as it stands;
//   Offsets       the right-click move rule (client/gameview_hud.cpp): a flyer
//                 takes retail's flyer spread (sim/retailflyerspread.h: the
//                 click plus its unclamped offset from the members' centre
//                 when within sqrt(3000 n) px, else the click); a member Legion
//                 plans on a shared point (legionSharedClick, Legion mode only)
//                 takes the click itself; every other member the click plus its
//                 offset from the centroid of the order's live owned members,
//                 clamped to 60 px per axis;
//   FlyerOffsets  the armed MOVE / PATROL / fight-move rule: a flyer takes
//                 retail's flyer spread as above, every other member the click
//                 itself.
// The sim computes the offsets at activation from hashed positions (integer
// Fixed arithmetic), so no per-member coordinates travel.
enum class OrderShape : uint8_t { Shared = 0, Offsets = 1, FlyerOffsets = 2 };

// One explicit selection order: the player's intent over a selection, applied
// to every member in ONE tick (the activation tick the server gives it). The
// identity is (the issuing player, serial): serials are client-assigned,
// strictly increasing per player per game, 1..kOrderMaxSerial.
struct SelectionOrder {
    uint32_t serial = 0;
    Command tmpl;                    // the verb and its arguments; unitId and player unused
    OrderShape shape = OrderShape::Shared;
    std::vector<int32_t> members;    // unit ids, strictly increasing (no duplicates by construction)
    // The bundle record's size (orderRecordBytes), kept by whoever builds the
    // order (packClick, the decoder, the server's commit) so the send window and
    // the tick budget do not walk the members again; 0 = not known, computed.
    uint32_t bytes = 0;
};

} // namespace tak::net
