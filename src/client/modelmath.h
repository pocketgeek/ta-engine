#pragma once

// Model-space math shared by the standalone model viewer (ModelView) and
// GameView's in-world unit renderer: a projected textured triangle, the piece-tree
// transform composed down the model hierarchy, and the COB piece rotation with the
// mirrored-basis negation. Extracted from client/main.cpp; kept at global scope so
// its unqualified use sites there are unchanged.

#include <SDL.h>

#include "cob/vm.h"   // ta::cob::PieceState (scriptRot)

#include <cmath>

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

// COB piece rotations for Xform::then(), with the piece YAW negated to match the
// retail engine. Retail composes every piece as Ry(-y)*Rx(+x)*Rz(+z): the y
// negation is a literal `fchs` at 0x4eea98 in KINGDOMS.icd's piece-matrix call
// site -- the SAME negation it applies to the root heading (0x4ee6a9), which we
// already reproduce as facing = -u.heading and which is verified correct. Our
// piece Ry and the root yaw reduce to the identical matrix, so pieces must carry
// the same negation. Without it, every scripted y-axis TURN/SPIN played mirrored
// (janky walks, garbled wing strokes). The negation lives HERE, at the
// script->composer boundary, so then() stays a generic rotation utility and the
// COB VM's script-space state (WAIT_TURN, shortest-path) is untouched.
// See docs/model-rendering-plan.md.
inline const float* scriptRot(const ta::cob::PieceState* ps, float (&tmp)[3]) {
    static const float kZero[3] = {0, 0, 0};
    if (!ps) return kZero;
    // COB piece angles compose with BOTH pitch (X) and yaw (Y) negated: the models
    // are authored front=-z/right=-x (a mirrored basis), so scripted X- and Y-turns
    // play mirrored unless negated here. Verified in-game: negating X fixes walker
    // leg-swing direction ("feet backwards") and flyer body-roll (was flying supine,
    // "back to the ground"); Z passes through. Retail applies the same via `fchs`
    // at the piece-matrix call site (0x4eea98).
    tmp[0] = -ps->rot[0];
    tmp[1] = -ps->rot[1];
    tmp[2] = ps->rot[2];
    return tmp;
};
