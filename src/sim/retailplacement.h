#pragma once

#include <cstdint>

namespace ta::sim {

struct RetailPlacementCell {
    // `entity` is the occupant's unit id (0: none). Retail keeps a 16-bit slot
    // index here; World's ids are never reused and pass 65,535 in a long game,
    // so the port carries them whole (ours, not retail).
    uint32_t entity=0;
    uint16_t feature=0xffff;
    uint8_t high=0,low=0,backX=0,backZ=0;
};

struct RetailPlacementEntity {
    bool valid=false,live=false,mover=false;
    uint32_t identity=0;
};

// 507d10, mobile placement mode with the footprint check enabled. Other
// placement modes delegate to the building host and are not handled here.
// CellAt reads an absolute map cell, including referenced feature anchors.
// EntityAt validates the retail slot range before exposing its fields.
template<class CellAt,class FeatureFlags,class EntityAt>
bool retailMobilePlacement(int x,int z,int footX,int footZ,int width,int height,
        int sea,int maxDepth,int minDepth,int maxSlope,int maxWaterSlope,
        uint32_t self,bool allowMoving,uint32_t featureCount,
        CellAt cellAt,FeatureFlags featureFlags,EntityAt entityAt) {
    if (x<0 || z<0 || x+footX>=width || z+footZ>=height) return false;
    for (int j=0;j<footZ;++j) for (int i=0;i<footX;++i) {
        const auto cell=cellAt(x+i,z+j);
        uint16_t feature=cell.feature;
        if (feature!=0xffff) {
            if (feature<0xfffa) {
                if (feature>=featureCount) return false;
            } else {
                if (feature!=0xfffe) return false;
                feature=cellAt(x+i-cell.backX,z+j-cell.backZ).feature;
            }
            if (feature<0xfffa && (featureFlags(feature)&0x20u)) return false;
        }
        if (cell.entity) {
            const auto entity=entityAt(cell.entity);
            if (entity.valid && entity.live && entity.identity!=self &&
                (!allowMoving || !entity.mover)) return false;
        }
        if (int(cell.low)<sea-maxDepth || int(cell.high)>sea-minDepth) return false;
        if (int(cell.high)-int(cell.low)>(cell.low<sea?maxWaterSlope:maxSlope)) return false;
    }
    return true;
}

} // namespace ta::sim
