#pragma once
#include <cstdint>

namespace tak {
// KINGDOMS.icd 50abf0: global AlarmMon, priority 7, 15000 ms cooldown.
struct MonarchAlert {
    uint64_t nextMs = 0;
    bool admit(uint8_t attackedPlayers, int localPlayer, bool spectator, uint64_t nowMs) {
        if (spectator || localPlayer < 0 || localPlayer >= 8 ||
            !(attackedPlayers & (1u << localPlayer)) || nowMs < nextMs) return false;
        nextMs = nowMs + 15000;
        return true;
    }
};
}
