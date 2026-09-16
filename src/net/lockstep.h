#pragma once

#include <cstdint>

namespace ta::net {

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
    Disco,         // cosmetic emote: player's monarchs dance for 10s (Shift+D).
                   // Synced via lockstep so all peers see it; affects no sim state.
    Reclaim,       // mobile builder unitId reclaims feature targetId for mana
                   // (right-click-drag issues one per feature in the box, queue=1)
    Stance,        // set unitId's combat stance to targetId (0=offensive/1=defensive/
                   // 2=passive) -- gates auto-acquire and chase leash
    Cloak,         // canCloak unitId: targetId != 0 enables cloaking, 0 disables
    SetActive,     // onOffable unitId: targetId != 0 powers on, 0 powers down
    Repair,        // mobile builder unitId repairs damaged friendly targetId (restores
                   // HP at its build rate, draining mana)
    Headbang,      // cosmetic emote: player's monarchs headbang to metal 10s (Shift+H)
    SetSquad,      // assign unitId to a control squad: targetId = 0 none, +N group N,
                   // -N formation N (N=1..10). A unit is in exactly one squad; formations
                   // move at their slowest member's speed and their stragglers rejoin.
};

struct Command {
    Cmd kind = Cmd::Move;
    uint8_t player = 0;
    int32_t unitId = 0;
    int32_t targetId = 0;
    float x = 0, z = 0;
    uint8_t queue = 0;
    char type[16] = {};   // unit type id for Train/Build
};

} // namespace ta::net
