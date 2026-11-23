#pragma once

// Model-space math shared by the standalone model viewer (ModelView) and
// GameView's in-world unit renderer: a projected textured triangle, the piece-tree
// transform composed down the model hierarchy, and the COB piece rotation with the
// mirrored-basis negation. Extracted from client/main.cpp; kept at global scope so
// its unqualified use sites there are unchanged.

#include <SDL.h>

#include "cob/emissionpose.h"
#include "cob/vm.h"   // tak::cob::PieceState (scriptRot)

#include <cmath>
#include <bit>
#include <cstdint>
#include <array>
#include <span>

struct Tri {
    SDL_Vertex v[3];
    SDL_Texture* tex;
    float depth;
};

// Column-major-ish 3x3 rotation + translation, composed down the piece tree.
struct Xform {
    float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float t[3] = {0, 0, 0};

    Xform then(float ox, float oy, float oz, const float rot[3]) const {
        // Local = T(offset) * Ry * Rx * Rz
        float cx = std::cos(rot[0]), sx = std::sin(rot[0]);
        float cy = std::cos(rot[1]), sy = std::sin(rot[1]);
        float cz = std::cos(rot[2]), sz = std::sin(rot[2]);
        float r[9] = {
            cy * cz + sy * sx * sz, -cy * sz + sy * sx * cz, sy * cx,
            cx * sz, cx * cz, -sx,
            -sy * cz + cy * sx * sz, sy * sz + cy * sx * cz, cy * cx,
        };
        Xform out;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                out.m[i * 3 + j] = 0;
                for (int k = 0; k < 3; ++k)
                    out.m[i * 3 + j] += m[i * 3 + k] * r[k * 3 + j];
            }
        out.t[0] = t[0] + m[0] * ox + m[1] * oy + m[2] * oz;
        out.t[1] = t[1] + m[3] * ox + m[4] * oy + m[5] * oz;
        out.t[2] = t[2] + m[6] * ox + m[7] * oy + m[8] * oz;
        return out;
    }

    void apply(float x, float y, float z, float out[3]) const {
        out[0] = t[0] + m[0] * x + m[1] * y + m[2] * z;
        out[1] = t[1] + m[3] * x + m[4] * y + m[5] * z;
        out[2] = t[2] + m[6] * x + m[7] * y + m[8] * z;
    }
};

// The retail model loader mirrors authored X/Z. Expressing its renderer
// (4ee620/4eea20) in our authored coordinates conjugates each piece transform:
// all three script angles negate, and script MOVE X/Z negate while Y stays.
inline const float* scriptRot(const tak::cob::PieceState* ps, float (&tmp)[3]) {
    static const float kZero[3] = {0, 0, 0};
    if (!ps) return kZero;
    for(int axis=0;axis<3;++axis)tmp[axis]=-ps->rot[axis];
    return tmp;
}

inline Xform scriptTransform(const Xform& parent,float x,float y,float z,
                             const tak::cob::PieceState* pose) {
    float rotation[3];
    return parent.then(x-(pose ? pose->move[0]:0),y+(pose ? pose->move[1]:0),
                       z-(pose ? pose->move[2]:0),scriptRot(pose,rotation));
}

inline Xform modelBodyTransform(uint16_t pitch,uint16_t roll) {
    constexpr float radians=6.28318530717959f/65536.f;
    const float angles[3]={-float(std::bit_cast<int16_t>(pitch))*radians,0,
                           -float(std::bit_cast<int16_t>(roll))*radians};
    return Xform{}.then(0,0,0,angles);
}

// A compact instruction-time pose, ordered from root to emitting piece.
// Keep this display transform separate from integer gameplay point queries.
using ModelEmissionPose=tak::cob::EmissionPose;

// Rendered vertices use x87's 64-bit rounded conversion, retaining the low
// word. Invalid conversion has a zero low word, rather than a saturated i32.
inline int32_t modelEmissionFixed(float coordinate) {
    const double rounded=std::nearbyint(double(coordinate)*65536.0);
    constexpr double limit=9223372036854775808.0;
    if(!std::isfinite(rounded) || rounded>=limit || rounded< -limit)return 0;
    return std::bit_cast<int32_t>(uint32_t(int64_t(rounded)));
}

inline std::array<float,3> modelEmissionPoint(std::span<const ModelEmissionPose> chain,
        const std::array<int32_t,3>& vertex,uint16_t heading,uint16_t pitch,uint16_t roll) {
    constexpr float radians=6.28318530717959f/65536.f;
    auto transform=modelBodyTransform(pitch,roll);
    for(const auto& piece:chain) {
        tak::cob::PieceState pose;
        for(size_t axis=0;axis<3;++axis) {
            pose.move[axis]=float(piece.move[axis])/65536.f;
            pose.rot[axis]=float(piece.turn[axis])*radians;
        }
        transform=scriptTransform(transform,float(piece.offset[0])/65536.f,
            float(piece.offset[1])/65536.f,float(piece.offset[2])/65536.f,&pose);
    }
    float result[3];
    transform.apply(float(vertex[0])/65536.f,float(vertex[1])/65536.f,
                    float(vertex[2])/65536.f,result);
    const float angle=-float(uint16_t(heading-32768))*radians;
    const float c=std::cos(angle),s=std::sin(angle);
    return {result[0]*c+result[2]*s,result[1],-result[0]*s+result[2]*c};
}
