#pragma once

#include <SDL.h>
#include <SDL_opengl.h>
#include "client/rendertelemetry.h"
#include <cstdlib>
#include <cstring>
#include <span>

namespace tak {
// Opaque atlas coverage needs positions and one constant black colour. The
// compatibility OpenGL renderer otherwise expands that colour for every vertex.
// Prime/flush SDL's solid draw state, then submit the same ordered triangles with
// a constant colour. Restore all changed GL state before SDL resumes.
// Other backends use the ordinary SDL path. This owns no context resources.
class OpaqueShadowSubmit {
public:
    int draw(SDL_Renderer* renderer,std::span<const SDL_FPoint> points,bool direct=true) {
        if (points.empty()) return 0;
        const SDL_Color black{0,0,0,255};
        static constexpr float uv[2]={0,0};
        const auto normal=[&] {
            render::countSdlGeometry(points.size(),points.size_bytes());
            return SDL_RenderGeometryRaw(renderer,nullptr,&points[0].x,sizeof(SDL_FPoint),
                &black,0,uv,0,int(points.size()),nullptr,0,0);
        };
        // Atlas vertices already include AA scale. SDL applies its render scale
        // while copying vertices into its queue; direct arrays bypass that copy.
        float sx=1,sy=1;
        SDL_RenderGetScale(renderer,&sx,&sy);
        if (sx!=1.f || sy!=1.f) return normal();
        SDL_RendererInfo info{};
        if (!direct || SDL_GetRendererInfo(renderer,&info)!=0 ||
            !info.name || std::strcmp(info.name,"opengl")!=0) return normal();
        // A degenerate triangle changes no pixels but establishes SDL's target,
        // viewport, projection, clipping, blend and solid shader state. Flush is
        // required before mixing SDL rendering with native API calls.
        static constexpr SDL_FPoint prime[3]={{0,0},{0,0},{0,0}};
        render::countPrime();
        if (SDL_RenderGeometryRaw(renderer,nullptr,&prime[0].x,sizeof(SDL_FPoint),
                &black,0,uv,0,3,nullptr,0,0)!=0 || SDL_RenderFlush(renderer)!=0) return -1;
        const auto context=SDL_GL_GetCurrentContext();
        if (!checked_ || context!=context_) {
            checked_=true;context_=context;available_=load();
        }
        if (!available_) return normal();
        GLint buffer=0;
        getInteger_(GL_ARRAY_BUFFER_BINDING,&buffer);
        pushAttrib_(GL_CURRENT_BIT);
        pushClientAttrib_(GL_CLIENT_VERTEX_ARRAY_BIT);
        bindBuffer_(GL_ARRAY_BUFFER,0);
        enableClient_(GL_VERTEX_ARRAY);
        disableClient_(GL_COLOR_ARRAY);
        disableClient_(GL_TEXTURE_COORD_ARRAY);
        color_(0,0,0,255);
        vertex_(2,GL_FLOAT,sizeof(SDL_FPoint),points.data());
        draw_(GL_TRIANGLES,0,GLsizei(points.size()));
        render::countNativeDraw(points.size(),points.size_bytes(),false);
        popClientAttrib_();
        bindBuffer_(GL_ARRAY_BUFFER,GLuint(buffer));
        popAttrib_();
        return 0;
    }
    bool active() const { return available_; }
private:
    template<class T> static bool proc(T& out,const char* name) {
        out=reinterpret_cast<T>(SDL_GL_GetProcAddress(name));return out!=nullptr;
    }
    bool load() {
        if (!proc(getString_,"glGetString") || !proc(getInteger_,"glGetIntegerv") ||
            !proc(pushAttrib_,"glPushAttrib") || !proc(popAttrib_,"glPopAttrib") ||
            !proc(pushClientAttrib_,"glPushClientAttrib") || !proc(popClientAttrib_,"glPopClientAttrib") ||
            !proc(bindBuffer_,"glBindBuffer") || !proc(enableClient_,"glEnableClientState") ||
            !proc(disableClient_,"glDisableClientState") || !proc(color_,"glColor4ub") ||
            !proc(vertex_,"glVertexPointer") || !proc(draw_,"glDrawArrays")) return false;
        const auto* version=reinterpret_cast<const char*>(getString_(GL_VERSION));
        if (!version) return false;
        // GL 3+ core contexts don't have fixed-function client arrays. SDL's
        // "opengl" backend is compatible, but explicitly reject a future core
        // implementation instead of issuing invalid compatibility operations.
        return std::atoi(version)<3 || SDL_GL_ExtensionSupported("GL_ARB_compatibility");
    }
    bool checked_=false,available_=false;
    SDL_GLContext context_=nullptr;
    const GLubyte* (APIENTRY *getString_)(GLenum)=nullptr;
    void (APIENTRY *getInteger_)(GLenum,GLint*)=nullptr;
    void (APIENTRY *pushAttrib_)(GLbitfield)=nullptr;
    void (APIENTRY *popAttrib_)()=nullptr;
    void (APIENTRY *pushClientAttrib_)(GLbitfield)=nullptr;
    void (APIENTRY *popClientAttrib_)()=nullptr;
    void (APIENTRY *bindBuffer_)(GLenum,GLuint)=nullptr;
    void (APIENTRY *enableClient_)(GLenum)=nullptr;
    void (APIENTRY *disableClient_)(GLenum)=nullptr;
    void (APIENTRY *color_)(GLubyte,GLubyte,GLubyte,GLubyte)=nullptr;
    void (APIENTRY *vertex_)(GLint,GLenum,GLsizei,const GLvoid*)=nullptr;
    void (APIENTRY *draw_)(GLenum,GLint,GLsizei)=nullptr;
};
} // namespace tak
