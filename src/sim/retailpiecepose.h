#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <vector>
#include <span>
#include "cob/retailpieces.h"

namespace tak::sim {

// 536143 rotates an integer pair and rounds each result to nearest/even.
// Q60 Taylor evaluation preserves that precision without host x87/libm state.
// The angle conversion is the observed decimal BAM-to-radians coefficient.
inline const std::array<int64_t,2>& retailRotationCoefficients(uint16_t angle) {
    // The coefficients depend only on the 16-bit heading. Construct once using
    // the same integer evaluation, then share immutable values across worlds.
    static const auto table=[] {
        std::vector<std::array<int64_t,2>> values(65536);
        for (uint32_t heading=0;heading<values.size();++heading) {
            using Wide=__int128_t;
            constexpr int64_t one=int64_t(1)<<60;
            const int64_t x=int64_t(Wide(std::bit_cast<int16_t>(uint16_t(heading)))*9587379924285LL*one/
                                    100000000000000000LL);
            const Wide square=Wide(x)*x/one;
            int64_t sine=x,cosine=one,st=x,ct=one;
            for (int n=1;n<19;++n) {
                st=int64_t(-Wide(st)*square/one/((2*n)*(2*n+1)));
                ct=int64_t(-Wide(ct)*square/one/((2*n-1)*(2*n)));
                sine+=st; cosine+=ct;
            }
            values[heading]={sine,cosine};
        }
        return values;
    }();
    return table[angle];
}

inline void retailRotatePair(int32_t& a,int32_t& b,uint16_t angle) {
    if (!angle) return;
    using Wide=__int128_t;
    const auto& coefficients=retailRotationCoefficients(angle);
    const int64_t sine=coefficients[0],cosine=coefficients[1];
    // Round to nearest/even at 2^60. Nearest/even is symmetric under negation,
    // so rounding the signed value from its floor equals 536143's rounding of
    // the magnitude with the sign put back; no data-dependent branches.
    auto rounded=[](Wide value) {
        constexpr uint64_t half=uint64_t(1)<<59,mask=(uint64_t(1)<<60)-1;
        int64_t whole=int64_t(value>>60);
        const uint64_t rest=uint64_t(value)&mask;
        whole+=int64_t((rest>half) | ((rest==half) & uint64_t(whole&1)));
        return whole<INT32_MIN || whole>INT32_MAX ? int32_t(INT32_MIN) : int32_t(whole);
    };
    const int32_t nextA=rounded(Wide(a)*cosine-Wide(b)*sine);
    b=rounded(Wide(a)*sine+Wide(b)*cosine); a=nextA;
}

// 535d50 applies roll, pitch, then yaw; rounding occurs after each plane.
inline void retailRotatePiece(std::array<int32_t,3>& point,
                              const std::array<uint16_t,3>& xyz) {
    retailRotatePair(point[0],point[1],xyz[2]);
    retailRotatePair(point[1],point[2],xyz[0]);
    retailRotatePair(point[0],point[2],xyz[1]);
}

struct RetailModelPiece {
    std::array<int32_t,3> offset{};
    int parent=-1,scriptPiece=-1;
    std::array<std::array<int32_t,3>,2> emissionVertices{};
    uint8_t emissionVertexCount=0;
};

inline std::array<int32_t,3> retailPieceOrigin(std::span<const RetailModelPiece> model,
        std::span<const cob::RetailPiece> poses,int scriptPiece,uint16_t heading,uint16_t pitch=0,uint16_t roll=0) {
    int index=-1;
    for (size_t i=0;i<model.size();++i)
        if (model[i].scriptPiece==scriptPiece) { index=int(i); break; }
    if (index<0) return {};
    auto translated=[&](const RetailModelPiece& piece) {
        auto result=piece.offset;
        // The model loader mirrors authored X/Z before installing the tree.
        result[0]=std::bit_cast<int32_t>(0u-uint32_t(result[0]));
        result[2]=std::bit_cast<int32_t>(0u-uint32_t(result[2]));
        if (piece.scriptPiece>=0)
            for (size_t i=0;i<3;++i)
                result[i]=std::bit_cast<int32_t>(uint32_t(result[i])+uint32_t(poses[size_t(piece.scriptPiece)].move[i]));
        return result;
    };
    auto result=translated(model[size_t(index)]);
    for (index=model[size_t(index)].parent;index>=0;index=model[size_t(index)].parent) {
        const auto& parent=model[size_t(index)];
        std::array<uint16_t,3> angles{};
        if (parent.scriptPiece>=0)
            for (size_t i=0;i<3;++i) angles[i]=uint16_t(poses[size_t(parent.scriptPiece)].turn[i]);
        if (parent.parent<0) {
            angles[0]=uint16_t(angles[0]+pitch);
            angles[1]=uint16_t(angles[1]+heading);
            angles[2]=uint16_t(angles[2]+roll);
        }
        retailRotatePiece(result,angles);
        auto offset=translated(parent);
        for (size_t i=0;i<3;++i)
            result[i]=std::bit_cast<int32_t>(uint32_t(result[i])+uint32_t(offset[i]));
    }
    result[2]=std::bit_cast<int32_t>(0u-uint32_t(result[2]));
    return result;
}

// Add an instruction-time script-piece offset to the unit's fixed-point world
// origin. EMIT_SFX effects use this same wrapped XYZ placement in both the
// simulation host and the display-side death-script bridge.
inline std::array<int32_t,3> retailScriptEffectPosition(
        const std::array<int32_t,3>& worldPosition,
        std::span<const RetailModelPiece> model,
        std::span<const cob::RetailPiece> poses,int scriptPiece,
        uint16_t heading,uint16_t pitch=0,uint16_t roll=0) {
    const auto offset=retailPieceOrigin(model,poses,scriptPiece,heading,pitch,roll);
    std::array<int32_t,3> result{};
    for(size_t axis=0;axis<3;++axis)
        result[axis]=std::bit_cast<int32_t>(uint32_t(worldPosition[axis])+uint32_t(offset[axis]));
    return result;
}

// 4dd2a0 (SweetSpot): the bounds of one piece's vertex buffer, including zero.
// The native extrema start at zero, and signed center division truncates to zero.
struct RetailPieceBounds {
    std::array<int32_t,3> minimum{},maximum{};
    void add(const std::array<int32_t,3>& point) {
        for(size_t axis=0;axis<3;++axis) {
            minimum[axis]=std::min(minimum[axis],point[axis]);
            maximum[axis]=std::max(maximum[axis],point[axis]);
        }
    }
    std::array<int32_t,3> center() const {
        std::array<int32_t,3> result{};
        for(size_t axis=0;axis<3;++axis)
            result[axis]=std::bit_cast<int32_t>(uint32_t(minimum[axis])+uint32_t(maximum[axis]))/2;
        return result;
    }
};

// 4dd2a0 reads the per-unit vertex buffer (model+0x1cc+piece*0x38+0x24) that
// the draw path rewrites in place: 4ee620 -> 4ee920 copies the definition's
// vertices back in and 4eea20 transforms them by the piece chain plus the body
// pitch/heading/roll, unit-relative (position zero, no final Z flip). A drawn
// retail unit's SweetSpot is therefore the centre of its drawn piece bounds.
// Lockstep cannot depend on drawing, so the simulation always uses the drawn
// geometry from its own pose (docs/retail-engine.md, "SweetSpot").
// `vertices` are the piece's authored vertices with X/Z mirrored as the model
// loader installs them. Each vertex takes the piece's own turn (plus the body
// attitude on the root), then its translated offset, then every parent's.
// The posed chain of one script piece's model object, nearest first: each
// step's turn (plus the body attitude on the root) and translated offset.
struct RetailDrawnChain {
    struct Step {
        std::array<uint16_t,3> angles{}; std::array<int32_t,3> offset{};
        bool operator==(const Step&) const=default;
    };
    std::array<Step,64> steps;
    size_t depth=0;
    bool found=false;
};

inline void retailPieceDrawnChain(RetailDrawnChain& chain,std::span<const RetailModelPiece> model,
        std::span<const cob::RetailPiece> poses,int scriptPiece,
        uint16_t heading,uint16_t pitch=0,uint16_t roll=0) {
    chain.depth=0; chain.found=false;
    int index=-1;
    for (size_t i=0;i<model.size();++i)
        if (model[i].scriptPiece==scriptPiece) { index=int(i); break; }
    if (index<0) return;
    chain.found=true;
    for (int at=index;at>=0 && chain.depth<chain.steps.size();at=model[size_t(at)].parent) {
        const auto& piece=model[size_t(at)];
        auto& step=chain.steps[chain.depth++];
        step.angles={};
        step.offset=piece.offset;
        step.offset[0]=std::bit_cast<int32_t>(0u-uint32_t(step.offset[0]));
        step.offset[2]=std::bit_cast<int32_t>(0u-uint32_t(step.offset[2]));
        if (piece.scriptPiece>=0 && size_t(piece.scriptPiece)<poses.size())
            for (size_t i=0;i<3;++i) {
                step.angles[i]=uint16_t(poses[size_t(piece.scriptPiece)].turn[i]);
                step.offset[i]=std::bit_cast<int32_t>(uint32_t(step.offset[i])+
                    uint32_t(poses[size_t(piece.scriptPiece)].move[i]));
            }
        if (piece.parent<0) {
            step.angles[0]=uint16_t(step.angles[0]+pitch);
            step.angles[1]=uint16_t(step.angles[1]+heading);
            step.angles[2]=uint16_t(step.angles[2]+roll);
        }
    }
}

inline std::array<int32_t,3> retailDrawnChainCenter(const RetailDrawnChain& chain,
        std::span<const std::array<int32_t,3>> vertices) {
    if (!chain.found) return {};
    RetailPieceBounds bounds;
    for (auto vertex:vertices) {
        for (size_t s=0;s<chain.depth;++s) {
            retailRotatePiece(vertex,chain.steps[s].angles);
            for (size_t i=0;i<3;++i)
                vertex[i]=std::bit_cast<int32_t>(uint32_t(vertex[i])+uint32_t(chain.steps[s].offset[i]));
        }
        bounds.add(vertex);
    }
    return bounds.center();
}

inline std::array<int32_t,3> retailPieceDrawnCenter(std::span<const RetailModelPiece> model,
        std::span<const cob::RetailPiece> poses,int scriptPiece,
        std::span<const std::array<int32_t,3>> vertices,
        uint16_t heading,uint16_t pitch=0,uint16_t roll=0) {
    RetailDrawnChain chain;
    retailPieceDrawnChain(chain,model,poses,scriptPiece,heading,pitch,roll);
    return retailDrawnChainCenter(chain,vertices);
}

// The drawn centre is a pure function of the piece's vertices (fixed per type
// and script piece) and its posed chain, so a unit keeps the last answer and
// reuses it while the chain is unchanged: bit-identical by construction.
struct RetailDrawnCenterCache {
    static constexpr size_t kDepth=8;   // deeper chains are not cached
    const void* vertices=nullptr;       // the type's vertex list for the piece
    size_t depth=0;
    std::array<RetailDrawnChain::Step,kDepth> steps{};
    std::array<int32_t,3> center{};

    std::array<int32_t,3> get(const RetailDrawnChain& chain,std::span<const std::array<int32_t,3>> list) {
        if (!chain.found) return {};
        if (vertices==list.data() && depth==chain.depth && depth<=kDepth &&
            std::equal(steps.begin(),steps.begin()+std::ptrdiff_t(depth),chain.steps.begin()))
            return center;
        center=retailDrawnChainCenter(chain,list);
        if (chain.depth<=kDepth) {
            vertices=list.data(); depth=chain.depth;
            std::copy(chain.steps.begin(),chain.steps.begin()+std::ptrdiff_t(depth),steps.begin());
        } else vertices=nullptr;
        return center;
    }
};

} // namespace tak::sim
