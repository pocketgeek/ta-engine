#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace tak::cob {

// Piece animation state and integer updates observed at 56c9ef..56cd48 and
// 56d850. Positions remain 16.16; rotations are unsigned 16-bit turns.
struct RetailPiece {
    std::array<int32_t,3> moveTarget{},moveSpeed{},turnTarget{},turnSpeed{},spinTarget{},spinAcceleration{};
    std::array<int32_t,3> move{},turn{};
    bool visible=true,cached=true,shaded=true,rendered=true,active=false;
    bool operator==(const RetailPiece&) const = default;
    static int32_t wrap(uint32_t value) { return std::bit_cast<int32_t>(value); }
    static int32_t add(int32_t a,int32_t b) { return wrap(uint32_t(a)+uint32_t(b)); }
    static int32_t negate(int32_t a) { return wrap(0u-uint32_t(a)); }

    void command(uint32_t op,int axis,int32_t target,int32_t speed,int32_t rate=30) {
        if (axis<0 || axis>2 || rate<=0) throw std::runtime_error("invalid COB piece command");
        const size_t a=size_t(axis);
        switch (op) {
        case 0x10001000:
            moveTarget[a]=target; moveSpeed[a]=speed/rate;
            if (target<move[a]) moveSpeed[a]=negate(moveSpeed[a]);
            active=true; break;
        case 0x10002000: {
            turnTarget[a]=int32_t(uint32_t(target)&65535); spinAcceleration[a]=0;
            turnSpeed[a]=speed/rate;
            const int32_t difference=turnTarget[a]-turn[a];
            if (!difference) turnSpeed[a]=0;
            else if ((int64_t(difference<0 ? -int64_t(difference) : difference)>32768) != (difference<0))
                turnSpeed[a]=negate(turnSpeed[a]);
            active=true; break;
        }
        case 0x10003000:
            turnTarget[a]=-1; spinTarget[a]=target/rate; spinAcceleration[a]=speed/rate;
            if (!spinAcceleration[a]) turnSpeed[a]=spinTarget[a];
            active=true; break;
        case 0x10004000:
            spinTarget[a]=0; spinAcceleration[a]=negate(target/rate);
            if (!spinAcceleration[a]) turnSpeed[a]=0;
            break;
        case 0x1000b000: moveTarget[a]=move[a]=target; moveSpeed[a]=0; break;
        case 0x1000c000:
            turnTarget[a]=turn[a]=int32_t(uint32_t(target)&65535); turnSpeed[a]=spinAcceleration[a]=0; break;
        default: throw std::runtime_error("unsupported COB piece command");
        }
    }

    void tick(int32_t elapsed) {
        if (!elapsed || !active) return;
        active=false;
        for (size_t a=0;a<3;++a) {
            if (moveSpeed[a]) {
                int32_t value=add(move[a],wrap(uint32_t(moveSpeed[a])*uint32_t(elapsed)));
                if (moveSpeed[a]>0 ? value>=moveTarget[a] : value<=moveTarget[a]) {
                    value=moveTarget[a]; moveSpeed[a]=0;
                } else active=true;
                move[a]=value;
            }
            if (spinAcceleration[a]) {
                turnSpeed[a]=add(turnSpeed[a],spinAcceleration[a]);
                if (spinAcceleration[a]>0 ? turnSpeed[a]>=spinTarget[a] : turnSpeed[a]<=spinTarget[a]) {
                    turnSpeed[a]=spinTarget[a]; spinAcceleration[a]=0;
                }
            }
            if (turnSpeed[a]) {
                const int32_t delta=wrap(uint32_t(turnSpeed[a])*uint32_t(elapsed));
                int32_t value=add(turn[a],delta);
                if (turnTarget[a]!=-1) {
                    const int32_t distance=turnSpeed[a]>0 ?
                        add(add(turnTarget[a],negate(turn[a])),65536)%65536 :
                        add(add(turn[a],negate(turnTarget[a])),65536)%65536;
                    if (distance<=(turnSpeed[a]>0 ? delta : negate(delta))) {
                        value=turnTarget[a]; turnSpeed[a]=0;
                    } else active=true;
                } else active=true;
                turn[a]=int32_t(uint32_t(value)&65535);
            }
        }
    }
};

} // namespace tak::cob
