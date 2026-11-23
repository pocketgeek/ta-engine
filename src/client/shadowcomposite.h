#pragma once
#include <SDL.h>
#include <vector>
namespace tak {
// SDL geometry ignores texture color/alpha modulation, so encode the silhouette
// tint explicitly. Keep each quad in painter order: overlapping units still
// darken independently rather than being merged into one shadow.
inline void appendShadowComposite(std::vector<SDL_Vertex>& vertices,const SDL_Rect& src,
                                  const SDL_FRect& dst,int width,int height,Uint8 alpha) {
    const float u=float(src.x)/width,v=float(src.y)/height;
    const float du=float(src.w)/width,dv=float(src.h)/height;
    const SDL_Color color{0,0,0,alpha};
    const SDL_Vertex q[4]={{{dst.x,dst.y},color,{u,v}},{{dst.x+dst.w,dst.y},color,{u+du,v}},
        {{dst.x+dst.w,dst.y+dst.h},color,{u+du,v+dv}},{{dst.x,dst.y+dst.h},color,{u,v+dv}}};
    for(int i:{0,1,2,0,2,3})vertices.push_back(q[i]);
}
}
