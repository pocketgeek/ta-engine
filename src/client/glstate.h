#pragma once

// GL state snapshot for native passes on SDL's OpenGL backend (workstream N).
//
// SDL's GL renderer caches what it last set (blend, program, unit-0 texture and
// its enable, client arrays, colour, viewport, scissor, target; SDL_render_gl.c
// drawstate) and only re-issues what it believes changed. A native pass that
// changes anything SDL cached must therefore put it back exactly. This captures
// every piece of state a pass could plausibly touch, so a verifier can compare
// the state right after SDL primed the pass with the state after the pass was
// closed -- equal means SDL's cache is still true -- and print what differs.
//
// Debug verification and tests only: capture() issues dozens of glGet calls.
// Render thread, with the renderer's context current.

#include <SDL.h>
#include <SDL_opengl.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace tak {

class GLStateSnapshot {
public:
    // Loads the entry points for the current context (cheap after the first call
    // in a context). False without a context or the GL 1.3 entry points.
    bool load() {
        const auto context = SDL_GL_GetCurrentContext();
        if (!context) return false;
        if (context == context_) return loaded_;
        context_ = context;
        loaded_ = proc(getString_, "glGetString") && proc(getInteger_, "glGetIntegerv") &&
                  proc(getFloat_, "glGetFloatv") && proc(isEnabled_, "glIsEnabled") &&
                  proc(getPointer_, "glGetPointerv") && proc(getError_, "glGetError") &&
                  proc(activeTexture_, "glActiveTexture");
        if (!proc(clientActiveTexture_, "glClientActiveTexture")) clientActiveTexture_ = nullptr;
        const auto* version = loaded_ ? reinterpret_cast<const char*>(getString_(GL_VERSION)) : nullptr;
        major_ = version ? std::atoi(version) : 0;
        minor_ = 0;
        if (const char* dot = version ? std::strchr(version, '.') : nullptr) minor_ = std::atoi(dot + 1);
        GLint units = 0;
        if (loaded_) getInteger_(GL_MAX_TEXTURE_IMAGE_UNITS, &units);
        units_ = std::max(1, std::min(int(units), kMaxUnits));
        return loaded_;
    }

    struct Values {
        std::vector<double> v;
        std::vector<const char*> name;   // static strings, one per value
        bool operator==(const Values& o) const { return v == o.v; }
    };

    // Everything below, in a fixed order. The active texture unit is restored.
    Values capture() {
        Values s;
        if (!load()) return s;
        auto i = [&](const char* name, GLenum e, int n = 1) {
            GLint buf[16] = {};
            getInteger_(e, buf);
            for (int k = 0; k < n; ++k) { s.v.push_back(double(buf[k])); s.name.push_back(name); }
        };
        auto f = [&](const char* name, GLenum e, int n) {
            GLfloat buf[16] = {};
            getFloat_(e, buf);
            for (int k = 0; k < n; ++k) { s.v.push_back(double(buf[k])); s.name.push_back(name); }
        };
        auto on = [&](const char* name, GLenum e) { s.v.push_back(isEnabled_(e) ? 1.0 : 0.0); s.name.push_back(name); };
        auto ptr = [&](const char* name, GLenum e) {
            void* p = nullptr;
            getPointer_(e, &p);
            s.v.push_back(double(reinterpret_cast<uintptr_t>(p)));
            s.name.push_back(name);
        };
        // Program, framebuffers, viewport, scissor.
        i("program", GL_CURRENT_PROGRAM);
        i("draw_framebuffer", 0x8CA6 /* GL_DRAW_FRAMEBUFFER_BINDING / FRAMEBUFFER_BINDING_EXT */);
        if (atLeast(3, 0)) i("read_framebuffer", 0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */);
        i("viewport", GL_VIEWPORT, 4);
        on("scissor_test", GL_SCISSOR_TEST);
        i("scissor_box", GL_SCISSOR_BOX, 4);
        // Blending and other per-fragment state SDL relies on staying off/default.
        on("blend", GL_BLEND);
        i("blend_src_rgb", GL_BLEND_SRC_RGB);
        i("blend_dst_rgb", GL_BLEND_DST_RGB);
        i("blend_src_alpha", GL_BLEND_SRC_ALPHA);
        i("blend_dst_alpha", GL_BLEND_DST_ALPHA);
        i("blend_equation_rgb", GL_BLEND_EQUATION_RGB);
        i("blend_equation_alpha", GL_BLEND_EQUATION_ALPHA);
        f("blend_color", GL_BLEND_COLOR, 4);
        i("color_writemask", GL_COLOR_WRITEMASK, 4);
        on("depth_test", GL_DEPTH_TEST);
        on("stencil_test", GL_STENCIL_TEST);
        on("cull_face", GL_CULL_FACE);
        on("alpha_test", GL_ALPHA_TEST);
        // Matrices and stacks (SDL sets the projection; modelview stays identity).
        i("matrix_mode", GL_MATRIX_MODE);
        f("modelview", GL_MODELVIEW_MATRIX, 16);
        f("projection", GL_PROJECTION_MATRIX, 16);
        i("modelview_depth", GL_MODELVIEW_STACK_DEPTH);
        i("projection_depth", GL_PROJECTION_STACK_DEPTH);
        i("attrib_depth", GL_ATTRIB_STACK_DEPTH);
        i("client_attrib_depth", GL_CLIENT_ATTRIB_STACK_DEPTH);
        // Current vertex attributes (fixed-function colour SDL sets for solid draws).
        f("current_color", GL_CURRENT_COLOR, 4);
        f("current_texcoord", GL_CURRENT_TEXTURE_COORDS, 4);
        // Buffers and vertex state (SDL draws from client arrays, buffer 0).
        i("array_buffer", GL_ARRAY_BUFFER_BINDING);
        i("element_array_buffer", GL_ELEMENT_ARRAY_BUFFER_BINDING);
        i("pixel_unpack_buffer", GL_PIXEL_UNPACK_BUFFER_BINDING);
        if (atLeast(3, 0)) i("vertex_array_object", 0x85B5 /* GL_VERTEX_ARRAY_BINDING */);
        if (clientActiveTexture_) i("client_active_texture", GL_CLIENT_ACTIVE_TEXTURE);
        on("vertex_array", GL_VERTEX_ARRAY);
        on("color_array", GL_COLOR_ARRAY);
        on("texcoord_array", GL_TEXTURE_COORD_ARRAY);
        on("normal_array", GL_NORMAL_ARRAY);
        i("vertex_array_size", GL_VERTEX_ARRAY_SIZE);
        i("vertex_array_type", GL_VERTEX_ARRAY_TYPE);
        i("vertex_array_stride", GL_VERTEX_ARRAY_STRIDE);
        i("vertex_array_buffer", GL_VERTEX_ARRAY_BUFFER_BINDING);
        ptr("vertex_array_pointer", GL_VERTEX_ARRAY_POINTER);
        i("color_array_size", GL_COLOR_ARRAY_SIZE);
        i("color_array_type", GL_COLOR_ARRAY_TYPE);
        i("color_array_stride", GL_COLOR_ARRAY_STRIDE);
        i("color_array_buffer", GL_COLOR_ARRAY_BUFFER_BINDING);
        ptr("color_array_pointer", GL_COLOR_ARRAY_POINTER);
        i("texcoord_array_size", GL_TEXTURE_COORD_ARRAY_SIZE);
        i("texcoord_array_type", GL_TEXTURE_COORD_ARRAY_TYPE);
        i("texcoord_array_stride", GL_TEXTURE_COORD_ARRAY_STRIDE);
        i("texcoord_array_buffer", GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING);
        ptr("texcoord_array_pointer", GL_TEXTURE_COORD_ARRAY_POINTER);
        // Pixel transfer (texture uploads SDL issues later).
        i("unpack_alignment", GL_UNPACK_ALIGNMENT);
        i("unpack_row_length", GL_UNPACK_ROW_LENGTH);
        // Texture units: active unit, then per unit the 2D binding, its
        // fixed-function enable and (GL 3.3) the sampler object.
        GLint active = GL_TEXTURE0;
        getInteger_(GL_ACTIVE_TEXTURE, &active);
        s.v.push_back(double(active));
        s.name.push_back("active_texture");
        for (int u = 0; u < units_; ++u) {
            activeTexture_(GLenum(GL_TEXTURE0 + u));
            i(kUnitBinding[u], GL_TEXTURE_BINDING_2D);
            if (u < kFixedUnits) on(kUnitEnable[u], GL_TEXTURE_2D);
            if (atLeast(3, 3)) i(kUnitSampler[u], 0x8919 /* GL_SAMPLER_BINDING */);
        }
        activeTexture_(GLenum(active));
        return s;
    }

    // Drains pending GL errors; returns the first one (GL_NO_ERROR if none).
    GLenum drainErrors() {
        if (!load()) return GL_NO_ERROR;
        GLenum first = GL_NO_ERROR;
        for (int k = 0; k < 32; ++k) {
            const GLenum e = getError_();
            if (e == GL_NO_ERROR) break;
            if (first == GL_NO_ERROR) first = e;
        }
        return first;
    }

    // "name[i]: before -> after" for every differing value (at most `limit`).
    static std::string diff(const Values& before, const Values& after, int limit = 8) {
        std::string out;
        if (before.v.size() != after.v.size()) return "snapshot sizes differ";
        int shown = 0;
        for (size_t k = 0; k < before.v.size(); ++k) {
            if (before.v[k] == after.v[k]) continue;
            if (shown++ >= limit) { out += " ..."; break; }
            char line[160];
            std::snprintf(line, sizeof line, "%s%s: %.9g -> %.9g", out.empty() ? "" : "; ", before.name[k],
                          before.v[k], after.v[k]);
            out += line;
        }
        return out;
    }

private:
    static constexpr int kMaxUnits = 8, kFixedUnits = 5;
    static constexpr const char* kUnitBinding[kMaxUnits] = {"unit0_texture", "unit1_texture", "unit2_texture",
        "unit3_texture", "unit4_texture", "unit5_texture", "unit6_texture", "unit7_texture"};
    static constexpr const char* kUnitEnable[kFixedUnits] = {"unit0_texture_2d", "unit1_texture_2d",
        "unit2_texture_2d", "unit3_texture_2d", "unit4_texture_2d"};
    static constexpr const char* kUnitSampler[kMaxUnits] = {"unit0_sampler", "unit1_sampler", "unit2_sampler",
        "unit3_sampler", "unit4_sampler", "unit5_sampler", "unit6_sampler", "unit7_sampler"};
    bool atLeast(int major, int minor) const { return major_ > major || (major_ == major && minor_ >= minor); }
    template <class T> static bool proc(T& out, const char* name) {
        out = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
        return out != nullptr;
    }
    SDL_GLContext context_ = nullptr;
    bool loaded_ = false;
    int major_ = 0, minor_ = 0, units_ = 1;
    const GLubyte* (APIENTRY* getString_)(GLenum) = nullptr;
    void (APIENTRY* getInteger_)(GLenum, GLint*) = nullptr;
    void (APIENTRY* getFloat_)(GLenum, GLfloat*) = nullptr;
    GLboolean (APIENTRY* isEnabled_)(GLenum) = nullptr;
    void (APIENTRY* getPointer_)(GLenum, void**) = nullptr;
    GLenum (APIENTRY* getError_)() = nullptr;
    void (APIENTRY* activeTexture_)(GLenum) = nullptr;
    void (APIENTRY* clientActiveTexture_)(GLenum) = nullptr;
};

}  // namespace tak
