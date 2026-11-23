#pragma once
#include <algorithm>
#include <cmath>

namespace tak {
// The body transform already converts native heading zero to the authored
// model's half-turn. Final model-space vertices share the native rasterizer's
// axes; flipping light X/Z again lights the opposite faces (4ed7ba).
inline int retailModelShadeLevel(float nx,float ny,float nz) {
    const float length=std::sqrt(nx*nx+ny*ny+nz*nz);
    const float dot=length>0 ? (-0.464991f*nx+0.813733f*ny-0.348743f*nz)/length : 0;
    return std::clamp(5+int(19*std::max(0.0f,dot)),5,24);
}
}
