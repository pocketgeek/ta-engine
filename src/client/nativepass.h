#pragma once

// Explicitly owned native passes on SDL's OpenGL backend (workstream N).
//
// A pass draws several consecutive commands natively under ONE SDL priming
// instead of priming (a degenerate SDL draw + SDL_RenderFlush) and saving/
// restoring GL state once per command. The contract with SDL is the documented
// one for mixing native GL with the render API: SDL_RenderFlush() before native
// calls, and every piece of GL state SDL caches is exactly what SDL left when
// SDL draws again. Nothing is assumed to survive an SDL call: a pass is primed
// afresh every time it opens, and no SDL call may be made while it is open.
//
//   begin()  GeometrySubmit::primePass: SDL sets target, viewport, scissor,
//            projection, blend and its program for the prime texture, then
//            flushes -- GL state is now exactly SDL's. The primed state is
//            checked (SDL uses shaders, blending is the prime's mode with the
//            documented factors, ADD equation); anything unexpected refuses the
//            pass, and the caller draws command by command as before.
//            Then GeometrySubmit::openPass saves the vertex-array, colour,
//            matrix and buffer state, and this saves the program, the active
//            unit, units 1-4 and the blend factors.
//   use*()   select the program and texture of the next command: an
//            SDL-identical RGBA program sampling unit 4 (useTexture), or a
//            caller's program sampling units 1-3 (useProgram: the indexed
//            palette). Unit 0 -- SDL's texture binding -- is never touched.
//   end()    restores what begin() saved, in reverse, so SDL's cache is true.
//
// Joining textures (joinable()): SDL would draw them with SHADER_RGBA (ARGB8888 /
// ABGR8888), in BLEND or the caller's premultiplied mode (ONE, ONE_MINUS_SRC_ALPHA;
// the distant-image pages), with normalized coordinates and no colour/alpha mod.
// SDL 2.32's geometry path ignores texture mods (GL_QueueGeometry copies vertex
// colours), but another SDL (SDL3 via sdl2-compat) may fold them in, so modded
// textures stay on SDL's path rather than depend on that.
//
// Verification (setVerify): a GLStateSnapshot right after SDL's priming and
// again after end(); any difference, or a GL error raised inside the pass, is a
// failure (counted, described by lastFailure()).
//
// Lifetime: the plain program belongs to the GL context it was linked in; on a
// context change it is forgotten (not deleted) and rebuilt by ready(). Render
// thread only.

#include <SDL.h>
#include <SDL_opengl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>

#include "client/geometrysubmit.h"
#include "client/glstate.h"

namespace tak {

class NativePassGL {
public:
    NativePassGL() = default;
    NativePassGL(const NativePassGL&) = delete;
    NativePassGL& operator=(const NativePassGL&) = delete;
    ~NativePassGL() { release(); }

    // The opengl backend, the GL 2.0 entry points and the plain program linked
    // (per context; a link failure is remembered for that context).
    bool ready(SDL_Renderer* renderer) {
        SDL_RendererInfo info{};
        if (SDL_GetRendererInfo(renderer, &info) != 0 || !info.name || std::strcmp(info.name, "opengl")) return false;
        const auto context = SDL_GL_GetCurrentContext();
        if (!context) return false;
        if (context != context_) { forget(); context_ = context; usable_ = load() && build(); }
        return usable_;
    }
    bool usable() const { return usable_; }
    const std::string& error() const { return error_; }

    struct Texture { GLuint name = 0; bool premultiplied = false; };
    // Can draws of `texture` join a pass (see the header comment)? name 0 = no.
    // Not while a pass is primed or open (it may make SDL bind the texture).
    Texture joinable(GeometrySubmit& submit, SDL_Renderer* renderer, SDL_Texture* texture,
                     SDL_BlendMode premultipliedMode) {
        Texture out;
        Uint32 format = 0;
        SDL_BlendMode blend = SDL_BLENDMODE_INVALID;
        Uint8 r = 0, g = 0, b = 0, a = 0;
        if (!texture || SDL_QueryTexture(texture, &format, nullptr, nullptr, nullptr) != 0) return out;
        if (format != SDL_PIXELFORMAT_ARGB8888 && format != SDL_PIXELFORMAT_ABGR8888) return out;
        if (SDL_GetTextureBlendMode(texture, &blend) != 0) return out;
        if (blend != SDL_BLENDMODE_BLEND && blend != premultipliedMode) return out;
        if (SDL_GetTextureColorMod(texture, &r, &g, &b) != 0 || r != 255 || g != 255 || b != 255) return out;
        if (SDL_GetTextureAlphaMod(texture, &a) != 0 || a != 255) return out;
        out.name = submit.textureName(renderer, texture);
        out.premultiplied = blend != SDL_BLENDMODE_BLEND;
        return out;
    }

    // Opens a pass primed with `prime` (a texture of the pass; its blend mode is
    // BLEND, or premultiplied when `primePremultiplied`). False: nothing is open
    // and no GL state was changed after SDL's own priming.
    bool begin(GeometrySubmit& submit, SDL_Renderer* renderer, SDL_Texture* prime, bool primePremultiplied) {
        if (!usable_ || open_) return false;
        ++attempts_;
        if (!submit.primePass(renderer, prime)) { ++refusedPrime_; return false; }
        if (verify_) { snapshot_.drainErrors(); before_ = snapshot_.capture(); }
        GLint program = 0, equationRgb = 0, equationAlpha = 0;
        getInteger_(GL_CURRENT_PROGRAM, &program);
        getInteger_(GL_BLEND_EQUATION_RGB, &equationRgb);
        getInteger_(GL_BLEND_EQUATION_ALPHA, &equationAlpha);
        for (int k = 0; k < 4; ++k) getInteger_(kBlendQueries[k], &primedBlend_[k]);
        const GLint* expected = primePremultiplied ? kPremultiplied : kBlend;
        bool ok = program != 0 && isEnabled_(GL_BLEND) && equationRgb == GL_FUNC_ADD && equationAlpha == GL_FUNC_ADD;
        for (int k = 0; k < 4; ++k) ok = ok && primedBlend_[k] == expected[k];
        if (!ok) {
            // SDL without shaders (fixed function: our program would not be its
            // arithmetic) or an unexpected blend: draw command by command instead.
            // Not this context's way of drawing: no further passes in it (each
            // attempt would cost a priming), logged once.
            submit.cancelPass();
            ++refusedState_;
            usable_ = false;
            error_ = program == 0 ? "SDL draws without shaders" : "unexpected primed blend state";
            std::fprintf(stderr, "native pass: disabled (%s)\n", error_.c_str());
            return false;
        }
        if (!submit.openPass()) { ++refusedPrime_; return false; }
        savedProgram_ = program;
        getInteger_(GL_ACTIVE_TEXTURE, &savedActive_);
        for (int i = 0; i < kUnits; ++i) {
            activeTexture_(GLenum(GL_TEXTURE1 + i));
            getInteger_(GL_TEXTURE_BINDING_2D, &savedBinding_[i]);
            bound_[i] = GLuint(savedBinding_[i]);
        }
        activeTexture_(GLenum(savedActive_));
        primedPremultiplied_ = premultiplied_ = primePremultiplied;
        current_ = GLuint(program);
        open_ = true;
        ++passes_;
        return true;
    }
    bool open() const { return open_; }

    // The next draws sample `name` (a joinable texture) with the plain program.
    void useTexture(GLuint name, bool premultiplied) {
        if (!open_) return;
        bind(3, name);
        selectProgram(plain_);
        useBlend(premultiplied);
    }
    // The next draws use the caller's program, sampling `textures` from units
    // 1-3, blended like BLEND (the indexed palette program).
    void useProgram(GLuint program, const GLuint (&textures)[3]) {
        if (!open_) return;
        for (int i = 0; i < 3; ++i) bind(i, textures[i]);
        selectProgram(program);
        useBlend(false);
    }

    void end(GeometrySubmit& submit) {
        if (!open_) return;
        useBlend(primedPremultiplied_);
        selectProgram(GLuint(savedProgram_));
        for (int i = 0; i < kUnits; ++i) bind(i, GLuint(savedBinding_[i]));
        activeTexture_(GLenum(savedActive_));
        submit.endPass();
        open_ = false;
        if (verify_) {
            const auto after = snapshot_.capture();
            const GLenum error = snapshot_.drainErrors();
            ++verified_;
            if (!(after == before_) || error != GL_NO_ERROR) {
                ++verifyFailures_;
                char code[48];
                std::snprintf(code, sizeof code, "GL error 0x%04x; ", unsigned(error));
                lastFailure_ = (error != GL_NO_ERROR ? std::string(code) : std::string()) +
                               GLStateSnapshot::diff(before_, after);
            }
        }
    }

    // Debug/tests: compare the GL state after each pass with the state SDL primed.
    void setVerify(bool verify) { verify_ = verify; }
    uint64_t verified() const { return verified_; }
    uint64_t verifyFailures() const { return verifyFailures_; }
    const std::string& lastFailure() const { return lastFailure_; }
    // Opened passes; begin() calls refused at SDL's priming / by the state check.
    uint64_t passes() const { return passes_; }
    uint64_t attempts() const { return attempts_; }
    uint64_t refusedPrime() const { return refusedPrime_; }
    uint64_t refusedState() const { return refusedState_; }

    void release() {
        if (context_ && SDL_GL_GetCurrentContext() == context_ && plain_ && deleteProgram_) deleteProgram_(plain_);
        forget();
    }

private:
    static constexpr int kUnits = 4;   // GL_TEXTURE1..4: program textures 1-3, plain texture 4
    static constexpr GLenum kBlendQueries[4] = {GL_BLEND_SRC_RGB, GL_BLEND_DST_RGB, GL_BLEND_SRC_ALPHA, GL_BLEND_DST_ALPHA};
    // SDL_BLENDMODE_BLEND as SDL_blendmode.h documents it (dstRGB = srcRGB*srcA +
    // dstRGB*(1-srcA), dstA = srcA + dstA*(1-srcA)), and the premultiplied mode.
    static constexpr GLint kBlend[4] = {GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA};
    static constexpr GLint kPremultiplied[4] = {GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA};

    void bind(int unit, GLuint texture) {
        if (bound_[unit] == texture) return;
        activeTexture_(GLenum(GL_TEXTURE1 + unit));
        bindTexture_(GL_TEXTURE_2D, texture);
        activeTexture_(GLenum(savedActive_));
        bound_[unit] = texture;
    }
    void selectProgram(GLuint program) {
        if (current_ == program) return;
        useProgram_(program);
        current_ = program;
    }
    void useBlend(bool premultiplied) {
        if (premultiplied == premultiplied_) return;
        const GLint* f = premultiplied ? kPremultiplied : kBlend;
        blendFuncSeparate_(GLenum(f[0]), GLenum(f[1]), GLenum(f[2]), GLenum(f[3]));
        premultiplied_ = premultiplied;
    }

    void forget() {
        plain_ = 0;
        open_ = false;
        context_ = nullptr;
        usable_ = false;
    }

    bool load() {
        const bool ok = proc(getString_, "glGetString") && proc(getInteger_, "glGetIntegerv") &&
            proc(isEnabled_, "glIsEnabled") && proc(activeTexture_, "glActiveTexture") &&
            proc(bindTexture_, "glBindTexture") && proc(createShader_, "glCreateShader") &&
            proc(shaderSource_, "glShaderSource") && proc(compileShader_, "glCompileShader") &&
            proc(getShaderiv_, "glGetShaderiv") && proc(getShaderInfoLog_, "glGetShaderInfoLog") &&
            proc(deleteShader_, "glDeleteShader") && proc(createProgram_, "glCreateProgram") &&
            proc(attachShader_, "glAttachShader") && proc(linkProgram_, "glLinkProgram") &&
            proc(getProgramiv_, "glGetProgramiv") && proc(getProgramInfoLog_, "glGetProgramInfoLog") &&
            proc(deleteProgram_, "glDeleteProgram") && proc(useProgram_, "glUseProgram") &&
            proc(getUniformLocation_, "glGetUniformLocation") && proc(uniform1i_, "glUniform1i") &&
            proc(blendFuncSeparate_, "glBlendFuncSeparate");
        if (!ok) { error_ = "GL 2.0 entry points"; return false; }
        const auto* version = reinterpret_cast<const char*>(getString_(GL_VERSION));
        if (!version || std::atoi(version) < 2) { error_ = "GL version"; return false; }
        return true;
    }
    GLuint compile(GLenum type, const char* source) {
        const GLuint shader = createShader_(type);
        if (!shader) return 0;
        shaderSource_(shader, 1, &source, nullptr);
        compileShader_(shader);
        GLint ok = 0;
        getShaderiv_(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024] = {};
            getShaderInfoLog_(shader, sizeof log, nullptr, log);
            error_ = std::string("compile: ") + log;
            deleteShader_(shader);
            return 0;
        }
        return shader;
    }
    bool build() {
        const GLuint vs = compile(GL_VERTEX_SHADER, kPlainVertexShader);
        const GLuint fs = compile(GL_FRAGMENT_SHADER, kPlainFragmentShader);
        GLuint program = 0;
        if (vs && fs) {
            program = createProgram_();
            attachShader_(program, vs);
            attachShader_(program, fs);
            linkProgram_(program);
            GLint ok = 0;
            getProgramiv_(program, GL_LINK_STATUS, &ok);
            if (!ok) {
                char log[1024] = {};
                getProgramInfoLog_(program, sizeof log, nullptr, log);
                error_ = std::string("link: ") + log;
                deleteProgram_(program);
                program = 0;
            }
        }
        if (vs) deleteShader_(vs);
        if (fs) deleteShader_(fs);
        if (!program) {
            std::fprintf(stderr, "native pass: plain program unavailable (%s)\n", error_.c_str());
            return false;
        }
        GLint previous = 0;
        getInteger_(GL_CURRENT_PROGRAM, &previous);
        useProgram_(program);
        uniform1i_(getUniformLocation_(program, "plainTex"), 4);
        useProgram_(GLuint(previous));
        plain_ = program;
        return true;
    }
    template <class T> static bool proc(T& out, const char* name) {
        out = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
        return out != nullptr;
    }

    // SDL's TEXTURE_VERTEX_SHADER and SHADER_RGBA (render/opengl/SDL_shaders_gl.c,
    // SDL 2.32), the sampler on unit 4.
    static constexpr const char* kPlainVertexShader =
        "varying vec4 v_color;\n"
        "varying vec2 v_texCoord;\n"
        "\n"
        "void main()\n"
        "{\n"
        "    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
        "    v_color = gl_Color;\n"
        "    v_texCoord = vec2(gl_MultiTexCoord0);\n"
        "}";
    static constexpr const char* kPlainFragmentShader =
        "varying vec4 v_color;\n"
        "varying vec2 v_texCoord;\n"
        "uniform sampler2D plainTex;\n"
        "\n"
        "void main()\n"
        "{\n"
        "    gl_FragColor = texture2D(plainTex, v_texCoord) * v_color;\n"
        "}";

    GLuint plain_ = 0, current_ = 0, bound_[kUnits] = {0, 0, 0, 0};
    GLint savedProgram_ = 0, savedActive_ = GL_TEXTURE0, savedBinding_[kUnits] = {0, 0, 0, 0}, primedBlend_[4] = {0, 0, 0, 0};
    bool open_ = false, premultiplied_ = false, primedPremultiplied_ = false;
    SDL_GLContext context_ = nullptr;
    bool usable_ = false, verify_ = false;
    std::string error_, lastFailure_;
    GLStateSnapshot snapshot_;
    GLStateSnapshot::Values before_;
    uint64_t passes_ = 0, attempts_ = 0, refusedPrime_ = 0, refusedState_ = 0, verified_ = 0, verifyFailures_ = 0;

    const GLubyte* (APIENTRY* getString_)(GLenum) = nullptr;
    void (APIENTRY* getInteger_)(GLenum, GLint*) = nullptr;
    GLboolean (APIENTRY* isEnabled_)(GLenum) = nullptr;
    void (APIENTRY* activeTexture_)(GLenum) = nullptr;
    void (APIENTRY* bindTexture_)(GLenum, GLuint) = nullptr;
    GLuint (APIENTRY* createShader_)(GLenum) = nullptr;
    void (APIENTRY* shaderSource_)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
    void (APIENTRY* compileShader_)(GLuint) = nullptr;
    void (APIENTRY* getShaderiv_)(GLuint, GLenum, GLint*) = nullptr;
    void (APIENTRY* getShaderInfoLog_)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void (APIENTRY* deleteShader_)(GLuint) = nullptr;
    GLuint (APIENTRY* createProgram_)() = nullptr;
    void (APIENTRY* attachShader_)(GLuint, GLuint) = nullptr;
    void (APIENTRY* linkProgram_)(GLuint) = nullptr;
    void (APIENTRY* getProgramiv_)(GLuint, GLenum, GLint*) = nullptr;
    void (APIENTRY* getProgramInfoLog_)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void (APIENTRY* deleteProgram_)(GLuint) = nullptr;
    void (APIENTRY* useProgram_)(GLuint) = nullptr;
    GLint (APIENTRY* getUniformLocation_)(GLuint, const GLchar*) = nullptr;
    void (APIENTRY* uniform1i_)(GLint, GLint) = nullptr;
    void (APIENTRY* blendFuncSeparate_)(GLenum, GLenum, GLenum, GLenum) = nullptr;
};

}  // namespace tak
