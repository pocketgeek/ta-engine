#pragma once
#include <SDL.h>
#include <SDL_opengl.h>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <span>
#include <cstdint>
#include <vector>
#include "client/gpuvram.h"
#include "client/rendertelemetry.h"

namespace tak {
// A program the caller owns that replaces SDL's for one native draw (workstream
// M: the indexed palette program, client/paletteindexed.h). GeometrySubmit primes
// SDL's state as usual (target, viewport, scissor, projection, blend), then
// install() saves whatever GL state it is about to change and binds its program
// and textures; restore() puts exactly that state back, so SDL's cached program
// and texture bindings are true again before SDL draws anything else. A draw
// with a program always runs natively (it has no SDL equivalent): when this
// renderer cannot, draw()/drawBuffer() return 1 without drawing.
struct NativeProgram {
    virtual bool install() = 0;
    virtual void restore() = 0;
protected:
    ~NativeProgram() = default;
};
// SDL's compatibility OpenGL backend copies/reformats every geometry vertex.
// Large already-packed batches can use client arrays directly. Other renderers,
// non-normalized textures and fractional output scales retain the SDL path.
class GeometrySubmit {
public:
    GeometrySubmit()=default;
    GeometrySubmit(const GeometrySubmit&)=delete;
    GeometrySubmit& operator=(const GeometrySubmit&)=delete;
    ~GeometrySubmit() { clear(); }
    void clear() {
        if(buffer_ && SDL_GL_GetCurrentContext()==context_ && deleteBuffers_)deleteBuffers_(1,&buffer_);
        gpuvram::removeBuffer(bufferBytes_);bufferBytes_=0;buffer_=0;revision_=0;seenRevision_=0;
    }
    uint64_t uploads() const { return uploads_; }
    // A nonzero revision retains this mesh on compatible OpenGL backends.
    // Callers increment it whenever any vertex changes; zero keeps streaming.
    int draw(SDL_Renderer* renderer,SDL_Texture* texture,std::span<const SDL_Vertex> vertices,bool direct=true,uint64_t revision=0,SDL_FPoint offset={0,0},NativeProgram* program=nullptr) {
        if(vertices.empty())return 0;
        auto normal=[&] {
            auto source=vertices;
            if(offset.x!=0 || offset.y!=0) {
                translated_.assign(vertices.begin(),vertices.end());
                for(auto& v:translated_){v.position.x+=offset.x;v.position.y+=offset.y;}
                source=translated_;
            }
            render::countSdlGeometry(source.size(),source.size_bytes());
            return SDL_RenderGeometry(renderer,texture,source.data(),int(source.size()),nullptr,0);
        };
        if(!program && (!direct || vertices.size()<kNativeMinVertices))return normal();
        float sx=1,sy=1;
        if(!nativeReady(renderer,texture,sx,sy))return program ? 1 : normal();
        if(!prime(renderer,texture))return -1;
        GLint buffer=0,mode=0;
        getInteger_(GL_ARRAY_BUFFER_BINDING,&buffer);getInteger_(GL_MATRIX_MODE,&mode);
        pushAttrib_(GL_CURRENT_BIT);pushClientAttrib_(GL_CLIENT_VERTEX_ARRAY_BIT);
        bool retained=false;
        const size_t bytes=vertices.size_bytes();
        // Moving cameras rebuild every frame. Do not allocate a new persistent
        // buffer until the mesh actually survives into a second draw.
        const bool stable=revision && revision==seenRevision_;
        if(buffer_ && (revision_!=revision || bufferBytes_!=bytes))clear();
        seenRevision_=revision;
        if(stable && genBuffers_ && bytes<=(size_t(64)<<20)) {
            if(!buffer_ && !gpuvram::blocked() && gpuvram::wouldFit(bytes)) {
                genBuffers_(1,&buffer_);bindBuffer_(GL_ARRAY_BUFFER,buffer_);
                bufferData_(GL_ARRAY_BUFFER,GLsizeiptr(bytes),vertices.data(),GL_STATIC_DRAW);
                GLint allocated=0;getBufferParameter_(GL_ARRAY_BUFFER,GL_BUFFER_SIZE,&allocated);
                if(size_t(allocated)==bytes) {
                    bufferBytes_=bytes;gpuvram::addBuffer(bytes);revision_=revision;++uploads_;
                    render::countUploadBytes(bytes);
                } else {clear();gpuvram::noteFail();}
            }
            retained=buffer_ && revision_==revision;
        }
        bindBuffer_(GL_ARRAY_BUFFER,retained?buffer_:0);
        matrixMode_(GL_MODELVIEW);pushMatrix_();scale_(sx,sy,1);translate_(offset.x,offset.y,0);
        enable_(GL_VERTEX_ARRAY);enable_(GL_COLOR_ARRAY);
        if(texture)enable_(GL_TEXTURE_COORD_ARRAY);else disable_(GL_TEXTURE_COORD_ARRAY);
        vertex_(2,GL_FLOAT,sizeof(SDL_Vertex),retained?reinterpret_cast<const void*>(offsetof(SDL_Vertex,position)):&vertices[0].position);
        color_(4,GL_UNSIGNED_BYTE,sizeof(SDL_Vertex),retained?reinterpret_cast<const void*>(offsetof(SDL_Vertex,color)):&vertices[0].color);
        if(texture)texcoord_(2,GL_FLOAT,sizeof(SDL_Vertex),retained?reinterpret_cast<const void*>(offsetof(SDL_Vertex,tex_coord)):&vertices[0].tex_coord);
        const bool installed=!program || program->install();
        if(installed) {
            draw_(GL_TRIANGLES,0,GLsizei(vertices.size()));
            if(program)program->restore();
            render::countNativeDraw(vertices.size(),bytes,retained);
        }
        popMatrix_();matrixMode_(GLenum(mode));
        popClientAttrib_();bindBuffer_(GL_ARRAY_BUFFER,GLuint(buffer));popAttrib_();
        used_=true;
        return installed ? 0 : 1;
    }
    bool active() const { return used_; }
    // Batches smaller than this take the SDL path: priming the native state
    // (a degenerate SDL draw + flush) costs more than SDL's conversion.
    static constexpr size_t kNativeMinVertices=512;
    // Can this renderer draw natively right now (the opengl backend, an exact
    // integer output scale, the GL entry points)? Texture checks come per draw.
    bool nativeCapable(SDL_Renderer* renderer) {
        float sx=1,sy=1;
        return nativeReady(renderer,nullptr,sx,sy);
    }
    // Draw ranges of a GL buffer of SDL_Vertex records that the caller owns (the
    // retained body arena, client/vertexarena.h) with exactly the state draw()
    // installs for client arrays; ranges execute in order (glMultiDrawArrays
    // behaves as consecutive glDrawArrays). Returns 0 when drawn, 1 when this
    // renderer, scale or texture cannot draw natively (the caller then draws the
    // same vertices through draw(), which takes the SDL path for the same
    // reason), -1 on an SDL error.
    int drawBuffer(SDL_Renderer* renderer,SDL_Texture* texture,GLuint vertexBuffer,
                   std::span<const GLint> firsts,std::span<const GLsizei> counts,size_t vertices,
                   NativeProgram* program=nullptr) {
        if(firsts.empty() || firsts.size()!=counts.size())return 0;
        float sx=1,sy=1;
        if(!vertexBuffer || !nativeReady(renderer,texture,sx,sy))return 1;
        if(!prime(renderer,texture))return -1;
        GLint buffer=0,mode=0;
        getInteger_(GL_ARRAY_BUFFER_BINDING,&buffer);getInteger_(GL_MATRIX_MODE,&mode);
        pushAttrib_(GL_CURRENT_BIT);pushClientAttrib_(GL_CLIENT_VERTEX_ARRAY_BIT);
        bindBuffer_(GL_ARRAY_BUFFER,vertexBuffer);
        matrixMode_(GL_MODELVIEW);pushMatrix_();scale_(sx,sy,1);
        enable_(GL_VERTEX_ARRAY);enable_(GL_COLOR_ARRAY);
        if(texture)enable_(GL_TEXTURE_COORD_ARRAY);else disable_(GL_TEXTURE_COORD_ARRAY);
        vertex_(2,GL_FLOAT,sizeof(SDL_Vertex),reinterpret_cast<const void*>(offsetof(SDL_Vertex,position)));
        color_(4,GL_UNSIGNED_BYTE,sizeof(SDL_Vertex),reinterpret_cast<const void*>(offsetof(SDL_Vertex,color)));
        if(texture)texcoord_(2,GL_FLOAT,sizeof(SDL_Vertex),reinterpret_cast<const void*>(offsetof(SDL_Vertex,tex_coord)));
        const bool installed=!program || program->install();
        if(installed) {
            if(multiDraw_ && firsts.size()>1)multiDraw_(GL_TRIANGLES,firsts.data(),counts.data(),GLsizei(firsts.size()));
            else for(size_t i=0;i<firsts.size();++i)draw_(GL_TRIANGLES,firsts[i],counts[i]);
            if(program)program->restore();
            render::countNativeDraw(vertices,vertices*sizeof(SDL_Vertex),true);
        }
        popMatrix_();matrixMode_(GLenum(mode));
        popClientAttrib_();bindBuffer_(GL_ARRAY_BUFFER,GLuint(buffer));popAttrib_();
        used_=true;
        return installed ? 0 : 1;
    }
    bool multiDraw() const { return multiDraw_!=nullptr; }

    // ---- native pass (workstream M) ------------------------------------------
    // Several consecutive commands drawn natively under ONE priming: beginPass()
    // primes SDL's state with `prime` (target, viewport, scissor, projection, the
    // prime texture's blend mode -- every command of the pass must share it) and
    // saves the vertex-array state; passDraw()/passDrawBuffer() draw with whatever
    // program and textures the caller installed (it owns and restores those);
    // endPass() restores the vertex-array, matrix and buffer state. Nothing SDL
    // queues may run in between: the caller issues no SDL calls inside a pass.
    // Workstream N splits the opening in two so the pass owner (NativePassGL) can
    // check and snapshot the primed state before anything changes it:
    // primePass() = readiness + SDL priming (GL state is then exactly SDL's),
    // openPass() = save and set the vertex-array/matrix state.
    bool beginPass(SDL_Renderer* renderer,SDL_Texture* prime) {
        return primePass(renderer,prime) && openPass();
    }
    bool primePass(SDL_Renderer* renderer,SDL_Texture* prime) {
        if(inPass_ || primed_ || !prime || !nativeReady(renderer,prime,passScaleX_,passScaleY_))return false;
        if(!GeometrySubmit::prime(renderer,prime))return false;
        primed_=true;
        return true;
    }
    bool openPass() {
        if(!primed_)return false;
        primed_=false;
        getInteger_(GL_ARRAY_BUFFER_BINDING,&passBuffer_);getInteger_(GL_MATRIX_MODE,&passMode_);
        pushAttrib_(GL_CURRENT_BIT);pushClientAttrib_(GL_CLIENT_VERTEX_ARRAY_BIT);
        matrixMode_(GL_MODELVIEW);pushMatrix_();scale_(passScaleX_,passScaleY_,1);
        enable_(GL_VERTEX_ARRAY);enable_(GL_COLOR_ARRAY);enable_(GL_TEXTURE_COORD_ARRAY);
        passArrays_=kNoArrays;
        inPass_=true;used_=true;
        return true;
    }
    // A primed pass the owner decided not to open: nothing was changed after SDL's
    // priming, so there is nothing to restore.
    void cancelPass() { primed_=false; }
    bool inPass() const { return inPass_; }
    void passDraw(std::span<const SDL_Vertex> vertices) {
        if(!inPass_ || vertices.empty())return;
        if(passArrays_!=0)bindBuffer_(GL_ARRAY_BUFFER,0);
        vertex_(2,GL_FLOAT,sizeof(SDL_Vertex),&vertices[0].position);
        color_(4,GL_UNSIGNED_BYTE,sizeof(SDL_Vertex),&vertices[0].color);
        texcoord_(2,GL_FLOAT,sizeof(SDL_Vertex),&vertices[0].tex_coord);
        passArrays_=0;
        draw_(GL_TRIANGLES,0,GLsizei(vertices.size()));
        render::countNativeDraw(vertices.size(),vertices.size_bytes(),false);
    }
    void passDrawBuffer(GLuint vertexBuffer,std::span<const GLint> firsts,std::span<const GLsizei> counts,size_t vertices) {
        if(!inPass_ || firsts.empty() || firsts.size()!=counts.size())return;
        // Consecutive draws from the same buffer keep its binding and array
        // pointers (offsets into it): set them only when the source changes.
        if(passArrays_!=vertexBuffer) {
            bindBuffer_(GL_ARRAY_BUFFER,vertexBuffer);
            vertex_(2,GL_FLOAT,sizeof(SDL_Vertex),reinterpret_cast<const void*>(offsetof(SDL_Vertex,position)));
            color_(4,GL_UNSIGNED_BYTE,sizeof(SDL_Vertex),reinterpret_cast<const void*>(offsetof(SDL_Vertex,color)));
            texcoord_(2,GL_FLOAT,sizeof(SDL_Vertex),reinterpret_cast<const void*>(offsetof(SDL_Vertex,tex_coord)));
            passArrays_=vertexBuffer;
        }
        if(multiDraw_ && firsts.size()>1)multiDraw_(GL_TRIANGLES,firsts.data(),counts.data(),GLsizei(firsts.size()));
        else for(size_t i=0;i<firsts.size();++i)draw_(GL_TRIANGLES,firsts[i],counts[i]);
        render::countNativeDraw(vertices,vertices*sizeof(SDL_Vertex),true);
    }
    void endPass() {
        if(!inPass_)return;
        popMatrix_();matrixMode_(GLenum(passMode_));
        popClientAttrib_();bindBuffer_(GL_ARRAY_BUFFER,GLuint(passBuffer_));popAttrib_();
        inPass_=false;
    }
    // The GL name of an SDL texture this renderer can sample natively (normalized
    // coordinates), else 0. SDL_GL_BindTexture/UnbindTexture leave SDL's own
    // binding cache consistent (the next SDL draw rebinds). Not inside a pass.
    GLuint textureName(SDL_Renderer* renderer,SDL_Texture* texture) {
        float sx=1,sy=1;
        if(inPass_ || primed_ || !texture || !nativeReady(renderer,nullptr,sx,sy))return 0;
        float tw=0,th=0;
        if(SDL_GL_BindTexture(texture,&tw,&th)!=0)return 0;
        GLint name=0;
        getInteger_(GL_TEXTURE_BINDING_2D,&name);
        SDL_GL_UnbindTexture(texture);
        return tw==1 && th==1 ? GLuint(name) : 0;
    }
private:
    bool inPass_=false,primed_=false;
    GLint passBuffer_=0,passMode_=0;
    // Inside a pass: the source the array pointers were last set for (0 = client
    // memory, set per draw), kNoArrays right after openPass.
    static constexpr GLuint kNoArrays=~GLuint(0);
    GLuint passArrays_=kNoArrays;
    float passScaleX_=1,passScaleY_=1;
    bool nativeReady(SDL_Renderer* renderer,SDL_Texture* texture,float& sx,float& sy) {
        SDL_RendererInfo info{};
        if(SDL_GetRendererInfo(renderer,&info)!=0 || !info.name || std::strcmp(info.name,"opengl"))return false;
        SDL_RenderGetScale(renderer,&sx,&sy);
        // Exact integer output scales share the same projection setup.
        // Fractional sampling retains the SDL path.
        if((sx!=1 && sx!=2 && sx!=4) || (sy!=1 && sy!=2 && sy!=4))return false;
        const auto context=SDL_GL_GetCurrentContext();
        if(!context)return false;
        if(!checked_ || context!=context_) { clear();checked_=true;context_=context;available_=load(); }
        if(!available_)return false;
        if(texture) {
            float tw=0,th=0;
            if(SDL_GL_BindTexture(texture,&tw,&th)!=0)return false;
            SDL_GL_UnbindTexture(texture);
            if(tw!=1 || th!=1)return false;
        }
        return true;
    }
    // The degenerate SDL draw installs the exact shader, blend, target,
    // viewport and scissor state without changing any pixels.
    static bool prime(SDL_Renderer* renderer,SDL_Texture* texture) {
        const SDL_Vertex prime[3]={{{0,0},{255,255,255,255},{0,0}},
                                  {{0,0},{255,255,255,255},{0,0}},
                                  {{0,0},{255,255,255,255},{0,0}}};
        render::countPrime();
        return SDL_RenderGeometry(renderer,texture,prime,3,nullptr,0)==0 && SDL_RenderFlush(renderer)==0;
    }
    template<class T> static bool proc(T& out,const char* name) {
        out=reinterpret_cast<T>(SDL_GL_GetProcAddress(name));return out!=nullptr;
    }
    bool load() {
        if(!proc(getString_,"glGetString") || !proc(getInteger_,"glGetIntegerv") ||
           !proc(pushAttrib_,"glPushAttrib") || !proc(popAttrib_,"glPopAttrib") ||
           !proc(pushClientAttrib_,"glPushClientAttrib") || !proc(popClientAttrib_,"glPopClientAttrib") ||
           !proc(bindBuffer_,"glBindBuffer") || !proc(enable_,"glEnableClientState") ||
           !proc(disable_,"glDisableClientState") || !proc(vertex_,"glVertexPointer") ||
           !proc(color_,"glColorPointer") || !proc(texcoord_,"glTexCoordPointer") ||
           !proc(matrixMode_,"glMatrixMode") || !proc(pushMatrix_,"glPushMatrix") ||
           !proc(popMatrix_,"glPopMatrix") || !proc(translate_,"glTranslatef") || !proc(scale_,"glScalef") || !proc(draw_,"glDrawArrays"))return false;
        // Optional retained buffers do not affect the existing direct fallback.
        if(!proc(genBuffers_,"glGenBuffers") || !proc(deleteBuffers_,"glDeleteBuffers") ||
           !proc(bufferData_,"glBufferData") || !proc(getBufferParameter_,"glGetBufferParameteriv"))genBuffers_=nullptr;
        // Optional (GL 1.4): ranges of one buffer in one call; else one draw per range.
        if(!proc(multiDraw_,"glMultiDrawArrays"))multiDraw_=nullptr;
        const auto* version=reinterpret_cast<const char*>(getString_(GL_VERSION));
        return version && (std::atoi(version)<3 || SDL_GL_ExtensionSupported("GL_ARB_compatibility"));
    }
    std::vector<SDL_Vertex> translated_;
    GLuint buffer_=0;
    size_t bufferBytes_=0;
    uint64_t revision_=0,seenRevision_=0,uploads_=0;
    void (APIENTRY *genBuffers_)(GLsizei,GLuint*)=nullptr;
    void (APIENTRY *deleteBuffers_)(GLsizei,const GLuint*)=nullptr;
    void (APIENTRY *bufferData_)(GLenum,GLsizeiptr,const void*,GLenum)=nullptr;
    void (APIENTRY *getBufferParameter_)(GLenum,GLenum,GLint*)=nullptr;
    bool checked_=false,available_=false,used_=false;
    SDL_GLContext context_=nullptr;
    const GLubyte* (APIENTRY *getString_)(GLenum)=nullptr;
    void (APIENTRY *getInteger_)(GLenum,GLint*)=nullptr;
    void (APIENTRY *pushAttrib_)(GLbitfield)=nullptr;
    void (APIENTRY *popAttrib_)()=nullptr;
    void (APIENTRY *pushClientAttrib_)(GLbitfield)=nullptr;
    void (APIENTRY *popClientAttrib_)()=nullptr;
    void (APIENTRY *bindBuffer_)(GLenum,GLuint)=nullptr;
    void (APIENTRY *enable_)(GLenum)=nullptr;
    void (APIENTRY *disable_)(GLenum)=nullptr;
    void (APIENTRY *vertex_)(GLint,GLenum,GLsizei,const GLvoid*)=nullptr;
    void (APIENTRY *color_)(GLint,GLenum,GLsizei,const GLvoid*)=nullptr;
    void (APIENTRY *texcoord_)(GLint,GLenum,GLsizei,const GLvoid*)=nullptr;
    void (APIENTRY *matrixMode_)(GLenum)=nullptr;
    void (APIENTRY *pushMatrix_)()=nullptr;
    void (APIENTRY *popMatrix_)()=nullptr;
    void (APIENTRY *translate_)(GLfloat,GLfloat,GLfloat)=nullptr;
    void (APIENTRY *scale_)(GLfloat,GLfloat,GLfloat)=nullptr;
    void (APIENTRY *draw_)(GLenum,GLint,GLsizei)=nullptr;
    void (APIENTRY *multiDraw_)(GLenum,const GLint*,const GLsizei*,GLsizei)=nullptr;
};
}
