#pragma once

#include <cstdint>

namespace tak {

// Retail's common mover tail dispatches TurnDirection from inside the mover,
// then reports MoveRate and surface/flight occupancy. These callbacks can
// signal one another's script threads, so preserve that order when several
// values change during the same simulation tick.
template<class Turn, class MoveRate, class Occupancy>
void updateRetailMovementAnimationCallbacks(bool hasTurnDirection,
        int turnDegrees, int& lastTurnSign,
        uint32_t moveRate, uint32_t& lastMoveRate,
        uint32_t occupancy, uint32_t& lastOccupancy,
        Turn&& turn, MoveRate&& move, Occupancy&& occupy) {
    const int sign=(turnDegrees>0)-(turnDegrees<0);
    if (hasTurnDirection && sign!=lastTurnSign) {
        lastTurnSign=sign;
        turn(turnDegrees);
    }
    if (moveRate!=lastMoveRate) {
        lastMoveRate=moveRate;
        move(moveRate);
    }
    if (occupancy!=lastOccupancy) {
        lastOccupancy=occupancy;
        occupy(occupancy);
    }
}

} // namespace tak
