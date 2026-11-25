#pragma once
// A flyer parked on a repair pad (or carrier) takes its orientation from the pad
// piece it sits on, every mover tick (0x43d6d0's attached branch, 0x43d78e ->
// 0x43e180): unit +0x64/+0x66/+0x68 = the carrier's + the piece's own words.
//
// Which word is which axis: 0x43def0 composes each piece's rotation as
// (piece +0x14 + unit +0x64, piece +0x12 + unit +0x66, piece +0x10 + unit +0x68)
// (0x43df67..0x43df9c; the piece record is 0x36 bytes from the list's +0x22),
// and 0x4b6cc0 turns a point by the first word in the x/y pair (about z), then
// the third in the y/z pair (about x), then the second in the x/z pair (about
// y). So piece +0x10 is the x turn and goes to +0x68, +0x12 the y turn to the
// heading +0x66, +0x14 the z turn to +0x64; 0x43e180 reads the same three
// (+0x32 / +0x34 / +0x36 of the 0x36-byte stride from the list base).
//
// The client's body attitude is modelBodyTransform(pitch, roll): pitch the x
// turn, roll the z turn, each entering as -angle like a script turn
// (scriptRot), so the rider's words are the pad's plus the piece's x / z turns.
#include <cmath>
#include <cstdint>
#include <utility>

namespace ta::padrider {

// A script piece turn (radians, cob::PieceState::rot) as a 16-bit angle word.
inline uint16_t angleWord(float radians) {
    constexpr double kWordsPerRadian = 65536.0 / 6.283185307179586;
    return uint16_t(int32_t(std::lround(double(radians) * kWordsPerRadian)));
}

// The rider's (pitch, roll) words: the pad's plus its pad piece's x and z turns.
inline std::pair<uint16_t, uint16_t> attitude(uint16_t padPitch, uint16_t padRoll,
                                              const float* pieceRot /* x, y, z; null: none */) {
    if (!pieceRot) return {padPitch, padRoll};
    return {uint16_t(padPitch + angleWord(pieceRot[0])), uint16_t(padRoll + angleWord(pieceRot[2]))};
}

} // namespace ta::padrider
