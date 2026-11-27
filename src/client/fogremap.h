#pragma once

// TotalA.exe's fog of war on the main view (docs/retail-fog.md section 3.1),
// on SDL's OpenGL backend: one native draw over the map view that reads back
// what has been drawn and re-colours it.
//
// Retail (0x4848e0, once a frame): a grid of vertices, one per 32 px cell
// centre, each holding two 4-bit masks of which of the four cells around it
// are dark -- grey (not in view) and black (never explored). Each vertex's
// 32x32 square is filled whole for mask 15, or gets the anims\fog.gaf piece
// Gray/Black<v>[mask-1] (v = (column + row) & 3, anchored to the world) for
// masks 1..14: bit 1 the top-left cell, 2 top-right, 4 bottom-left, 8
// bottom-right (read off the pieces' coverage). Black pixels are palette 0;
// grey pixels are the terrain through the GRAY TABLE (0x4bad30: for each
// colour the palette entry nearest its grey (r+g+b)/3) -- desaturated, not
// darkened -- or, with DitheredFog, every other pixel black (0x4bff20). No
// motion, no tint.
//
// Here the same rule runs per fragment: the cell plane (unexplored / out of
// view / in view, one byte a cell), an atlas of the 8 x 14 pieces, and a
// 256-entry LUT from a grey level to its nearest palette colour (for a palette
// colour that is exactly retail's table; a lit or filtered pixel uses its own
// grey). The view drawn so far is copied (glCopyTexSubImage2D) when the
// program installs, inside SDL's priming, so it is exactly SDL's target.
//
// Lifetime and state: as client/paletteindexed.h -- GL objects belong to the
// context they were made in (forgotten, not deleted, on a change); install()
// saves what it changes and restore() puts it back. Render thread only.

#include <SDL.h>
#include <SDL_opengl.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "client/geometrysubmit.h"
#include "client/gpuvram.h"

namespace ta {

class FogRemapGL final : public NativeProgram {
public:
    static constexpr int kPiece = 32, kMasks = 14, kVariants = 4;
    static constexpr int kAtlasW = kPiece * kMasks, kAtlasH = kPiece * kVariants * 2;   // Gray rows, then Black

    FogRemapGL() = default;
    FogRemapGL(const FogRemapGL&) = delete;
    FogRemapGL& operator=(const FogRemapGL&) = delete;
    ~FogRemapGL() { release(); }

    // The pieces: `atlas` is kAtlasW x kAtlasH, one byte a pixel (1 = fog):
    // row band g*kVariants+v holds Gray<v+1> (g = 0) or Black<v+1> (g = 1),
    // column band m-1 the piece for mask m. `greyLut` is 256 RGB triples.
    void setArt(std::vector<uint8_t> atlas, std::vector<uint8_t> greyLut) {
        atlas_ = std::move(atlas); lut_ = std::move(greyLut); artDirty_ = true;
    }
    bool hasArt() const { return atlas_.size() == size_t(kAtlasW) * kAtlasH && lut_.size() == 256 * 3; }

    bool ready(SDL_Renderer* renderer) {
        SDL_RendererInfo info{};
        if (SDL_GetRendererInfo(renderer, &info) != 0 || !info.name || std::strcmp(info.name, "opengl")) return false;
        const auto context = SDL_GL_GetCurrentContext();
        if (!context) return false;
        if (context != context_) { forget(); context_ = context; usable_ = load() && build(); }
        if (usable_ && artDirty_ && hasArt()) usable_ = uploadArt();
        return usable_ && hasArt();
    }

    // The cell plane (retailfog kUnexplored / kExplored / kInView), re-sent when
    // its generation changes.
    bool setCells(const std::vector<uint8_t>& plane, int w, int h, uint64_t generation) {
        if (!usable_ || w <= 0 || h <= 0 || plane.size() != size_t(w) * h) return false;
        if (generation == cellsGen_ && w == cellsW_ && h == cellsH_) return true;
        if (w != cellsW_ || h != cellsH_) {
            if (cells_) { deleteTextures_(1, &cells_); gpuvram::removeBuffer(size_t(cellsW_) * cellsH_); }
            cells_ = makeTexture(w, h, GL_LUMINANCE, GL_LUMINANCE, nullptr);
            if (!cells_) return false;
            gpuvram::addBuffer(size_t(w) * h);
            cellsW_ = w; cellsH_ = h;
        }
        if (!subImage(cells_, w, h, GL_LUMINANCE, plane.data())) return false;
        cellsGen_ = generation;
        return true;
    }
    void setDithered(bool on) { dithered_ = on; }
    // Map pixels at the view's top-left, its zoom, and the plane's cells (for
    // the shader's map-space reckoning, carried by the quad's coordinates).
    uint64_t draws() const { return draws_; }

    bool install() override {
        getInteger_(GL_CURRENT_PROGRAM, &savedProgram_);
        getInteger_(GL_ACTIVE_TEXTURE, &savedActive_);
        GLint viewport[4] = {0, 0, 0, 0};
        getInteger_(GL_VIEWPORT, viewport);
        if (viewport[2] <= 0 || viewport[3] <= 0) return false;
        if (viewport[2] > sceneW_ || viewport[3] > sceneH_) {
            if (scene_) { deleteTextures_(1, &scene_); gpuvram::removeBuffer(size_t(sceneW_) * sceneH_ * 4); }
            sceneW_ = std::max(viewport[2], sceneW_); sceneH_ = std::max(viewport[3], sceneH_);
            scene_ = makeTexture(sceneW_, sceneH_, GL_RGBA8, GL_RGBA, nullptr);
            if (!scene_) { sceneW_ = sceneH_ = 0; return false; }
            gpuvram::addBuffer(size_t(sceneW_) * sceneH_ * 4);
        }
        const GLuint textures[4] = {scene_, cells_, atlas_tex_, lutTex_};
        for (int i = 0; i < 4; ++i) {
            activeTexture_(kUnits[i]);
            getInteger_(GL_TEXTURE_BINDING_2D, &savedBinding_[i]);
            bindTexture_(GL_TEXTURE_2D, textures[i]);
        }
        // What SDL has drawn so far into its target, viewport-aligned.
        activeTexture_(kUnits[0]);
        copyTexSubImage2D_(GL_TEXTURE_2D, 0, 0, 0, viewport[0], viewport[1], viewport[2], viewport[3]);
        activeTexture_(GLenum(savedActive_));
        useProgram_(program_);
        uniform4f_(locView_, float(viewport[0]), float(viewport[1]), 1.0f / float(sceneW_), 1.0f / float(sceneH_));
        uniform4f_(locCells_, float(cellsW_), float(cellsH_), dithered_ ? 1.0f : 0.0f, 0.0f);
        ++draws_;
        return true;
    }
    void restore() override {
        useProgram_(GLuint(savedProgram_));
        for (int i = 0; i < 4; ++i) { activeTexture_(kUnits[i]); bindTexture_(GL_TEXTURE_2D, GLuint(savedBinding_[i])); }
        activeTexture_(GLenum(savedActive_));
    }

    void release() {
        if (context_ && SDL_GL_GetCurrentContext() == context_ && deleteTextures_) {
            for (GLuint t : {scene_, cells_, atlas_tex_, lutTex_}) if (t) deleteTextures_(1, &t);
            if (program_ && deleteProgram_) deleteProgram_(program_);
        }
        forget();
    }

private:
    static constexpr GLenum kUnits[4] = {GL_TEXTURE1, GL_TEXTURE2, GL_TEXTURE3, GL_TEXTURE4};

    void forget() {
        if (scene_) gpuvram::removeBuffer(size_t(sceneW_) * sceneH_ * 4);
        if (cells_) gpuvram::removeBuffer(size_t(cellsW_) * cellsH_);
        if (atlas_tex_) gpuvram::removeBuffer(size_t(kAtlasW) * kAtlasH);
        if (lutTex_) gpuvram::removeBuffer(256 * 4);
        scene_ = cells_ = atlas_tex_ = lutTex_ = program_ = 0;
        sceneW_ = sceneH_ = cellsW_ = cellsH_ = 0;
        cellsGen_ = ~uint64_t(0);
        context_ = nullptr; usable_ = false; artDirty_ = true;
    }

    bool uploadArt() {
        if (!atlas_tex_) {
            atlas_tex_ = makeTexture(kAtlasW, kAtlasH, GL_LUMINANCE, GL_LUMINANCE, nullptr);
            if (!atlas_tex_) return false;
            gpuvram::addBuffer(size_t(kAtlasW) * kAtlasH);
        }
        if (!lutTex_) {
            lutTex_ = makeTexture(256, 1, GL_RGBA8, GL_RGBA, nullptr);
            if (!lutTex_) return false;
            gpuvram::addBuffer(256 * 4);
        }
        std::vector<uint8_t> atlas(atlas_.size());
        for (size_t i = 0; i < atlas.size(); ++i) atlas[i] = atlas_[i] ? 255 : 0;
        std::vector<uint8_t> lut(256 * 4);
        for (int i = 0; i < 256; ++i) {
            lut[size_t(i) * 4] = lut_[size_t(i) * 3]; lut[size_t(i) * 4 + 1] = lut_[size_t(i) * 3 + 1];
            lut[size_t(i) * 4 + 2] = lut_[size_t(i) * 3 + 2]; lut[size_t(i) * 4 + 3] = 255;
        }
        if (!subImage(atlas_tex_, kAtlasW, kAtlasH, GL_LUMINANCE, atlas.data()) ||
            !subImage(lutTex_, 256, 1, GL_RGBA, lut.data())) return false;
        artDirty_ = false;
        return true;
    }

    template <class F> bool onScratchUnit(GLuint texture, F&& f) {
        while (getError_() != GL_NO_ERROR) {}
        GLint active = 0, binding = 0, alignment = 4, rowLength = 0, unpackBuffer = 0;
        getInteger_(GL_ACTIVE_TEXTURE, &active);
        activeTexture_(GL_TEXTURE1);
        getInteger_(GL_TEXTURE_BINDING_2D, &binding);
        getInteger_(GL_UNPACK_ALIGNMENT, &alignment);
        getInteger_(GL_UNPACK_ROW_LENGTH, &rowLength);
        if (bindBuffer_) { getInteger_(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer); if (unpackBuffer) bindBuffer_(GL_PIXEL_UNPACK_BUFFER, 0); }
        pixelStore_(GL_UNPACK_ALIGNMENT, 1);
        pixelStore_(GL_UNPACK_ROW_LENGTH, 0);
        bindTexture_(GL_TEXTURE_2D, texture);
        f();
        const bool ok = getError_() == GL_NO_ERROR;
        bindTexture_(GL_TEXTURE_2D, GLuint(binding));
        pixelStore_(GL_UNPACK_ALIGNMENT, alignment);
        pixelStore_(GL_UNPACK_ROW_LENGTH, rowLength);
        if (unpackBuffer) bindBuffer_(GL_PIXEL_UNPACK_BUFFER, GLuint(unpackBuffer));
        activeTexture_(GLenum(active));
        return ok;
    }
    GLuint makeTexture(int w, int h, GLint internal, GLenum format, const void* pixels) {
        GLuint name = 0;
        genTextures_(1, &name);
        if (!name) return 0;
        const bool ok = onScratchUnit(name, [&] {
            texParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            texParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            texParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            texParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            texImage2D_(GL_TEXTURE_2D, 0, internal, w, h, 0, format, GL_UNSIGNED_BYTE, pixels);
        });
        if (!ok) { deleteTextures_(1, &name); return 0; }
        return name;
    }
    bool subImage(GLuint texture, int w, int h, GLenum format, const void* pixels) {
        return onScratchUnit(texture, [&] { texSubImage2D_(GL_TEXTURE_2D, 0, 0, 0, w, h, format, GL_UNSIGNED_BYTE, pixels); });
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
        while (getError_() != GL_NO_ERROR) {}
        const GLuint vs = compile(GL_VERTEX_SHADER, kVertex), fs = compile(GL_FRAGMENT_SHADER, kFragment);
        if (vs && fs) {
            program_ = createProgram_();
            attachShader_(program_, vs);
            attachShader_(program_, fs);
            linkProgram_(program_);
            GLint ok = 0;
            getProgramiv_(program_, GL_LINK_STATUS, &ok);
            if (!ok) {
                char log[1024] = {};
                getProgramInfoLog_(program_, sizeof log, nullptr, log);
                error_ = std::string("link: ") + log;
                deleteProgram_(program_);
                program_ = 0;
            }
        }
        if (vs) deleteShader_(vs);
        if (fs) deleteShader_(fs);
        if (!program_) { std::fprintf(stderr, "fog: retail fog program unavailable (%s)\n", error_.c_str()); return false; }
        GLint previous = 0;
        getInteger_(GL_CURRENT_PROGRAM, &previous);
        useProgram_(program_);
        uniform1i_(getUniformLocation_(program_, "scene"), 1);
        uniform1i_(getUniformLocation_(program_, "cells"), 2);
        uniform1i_(getUniformLocation_(program_, "atlas"), 3);
        uniform1i_(getUniformLocation_(program_, "lut"), 4);
        locView_ = getUniformLocation_(program_, "view");
        locCells_ = getUniformLocation_(program_, "cellInfo");
        useProgram_(GLuint(previous));
        return true;
    }

    template <class T> static bool proc(T& out, const char* name) {
        out = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
        return out != nullptr;
    }
    bool load() {
        bool ok = proc(getString_, "glGetString") && proc(getInteger_, "glGetIntegerv") && proc(getError_, "glGetError") &&
            proc(activeTexture_, "glActiveTexture") && proc(bindTexture_, "glBindTexture") &&
            proc(genTextures_, "glGenTextures") && proc(deleteTextures_, "glDeleteTextures") &&
            proc(texImage2D_, "glTexImage2D") && proc(texSubImage2D_, "glTexSubImage2D") &&
            proc(copyTexSubImage2D_, "glCopyTexSubImage2D") &&
            proc(texParameteri_, "glTexParameteri") && proc(pixelStore_, "glPixelStorei") &&
            proc(createShader_, "glCreateShader") && proc(shaderSource_, "glShaderSource") &&
            proc(compileShader_, "glCompileShader") && proc(getShaderiv_, "glGetShaderiv") &&
            proc(getShaderInfoLog_, "glGetShaderInfoLog") && proc(deleteShader_, "glDeleteShader") &&
            proc(createProgram_, "glCreateProgram") && proc(attachShader_, "glAttachShader") &&
            proc(linkProgram_, "glLinkProgram") && proc(getProgramiv_, "glGetProgramiv") &&
            proc(getProgramInfoLog_, "glGetProgramInfoLog") && proc(deleteProgram_, "glDeleteProgram") &&
            proc(useProgram_, "glUseProgram") && proc(getUniformLocation_, "glGetUniformLocation") &&
            proc(uniform1i_, "glUniform1i") && proc(uniform4f_, "glUniform4f");
        if (!proc(bindBuffer_, "glBindBuffer")) bindBuffer_ = nullptr;
        if (!ok) { error_ = "GL 2.0 entry points"; return false; }
        const auto* version = reinterpret_cast<const char*>(getString_(GL_VERSION));
        if (!version || std::atoi(version) < 2) { error_ = "GL version"; return false; }
        return true;
    }

    // The quad's texture coordinates are MAP pixels (x, projected z).
    static constexpr const char* kVertex =
        "varying vec2 v_map;\n"
        "void main() {\n"
        "    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
        "    v_map = vec2(gl_MultiTexCoord0);\n"
        "}";
    static constexpr const char* kFragment =
        "varying vec2 v_map;\n"
        "uniform sampler2D scene;\n"
        "uniform sampler2D cells;\n"
        "uniform sampler2D atlas;\n"
        "uniform sampler2D lut;\n"
        "uniform vec4 view;      /* viewport x, y; 1/scene w, 1/scene h */\n"
        "uniform vec4 cellInfo;  /* cells w, h; dithered */\n"
        "float cls(vec2 c) {\n"
        "    c = clamp(c, vec2(0.0), cellInfo.xy - 1.0);   /* the edge masks copied outward */\n"
        "    return floor(texture2D(cells, (c + 0.5) / cellInfo.xy).r * 255.0 + 0.5);\n"
        "}\n"
        "float piece(float band, float mask, vec2 l) {\n"
        "    if (mask < 0.5) return 0.0;\n"
        "    if (mask > 14.5) return 1.0;\n"
        "    vec2 t = vec2((mask - 1.0) * 32.0 + l.x, band * 32.0 + l.y) + 0.5;\n"
        "    return texture2D(atlas, t / vec2(448.0, 256.0)).r > 0.5 ? 1.0 : 0.0;\n"
        "}\n"
        "void main() {\n"
        "    vec2 g = v_map - 16.0;\n"
        "    vec2 ij = floor(g / 32.0);\n"
        "    vec2 l = clamp(floor(g - ij * 32.0), 0.0, 31.0);\n"
        "    float c00 = cls(ij), c10 = cls(ij + vec2(1.0, 0.0)), c01 = cls(ij + vec2(0.0, 1.0)), c11 = cls(ij + vec2(1.0));\n"
        "    float black = (c00 < 0.5 ? 1.0 : 0.0) + (c10 < 0.5 ? 2.0 : 0.0) + (c01 < 0.5 ? 4.0 : 0.0) + (c11 < 0.5 ? 8.0 : 0.0);\n"
        "    float grey = (c00 < 1.5 ? 1.0 : 0.0) + (c10 < 1.5 ? 2.0 : 0.0) + (c01 < 1.5 ? 4.0 : 0.0) + (c11 < 1.5 ? 8.0 : 0.0);\n"
        "    float v = mod(ij.x + ij.y, 4.0);\n"
        "    vec4 colour = texture2D(scene, (gl_FragCoord.xy - view.xy) * view.zw);\n"
        "    if (piece(v, grey, l) > 0.5) {\n"
        "        if (cellInfo.z > 0.5) {\n"
        "            vec2 p = floor(v_map);\n"
        "            if (mod(p.x + p.y, 2.0) > 0.5) colour = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "        } else {\n"
        "            float level = floor((colour.r + colour.g + colour.b) * 255.0 / 3.0);\n"
        "            colour = vec4(texture2D(lut, vec2((level + 0.5) / 256.0, 0.5)).rgb, 1.0);\n"
        "        }\n"
        "    }\n"
        "    if (piece(4.0 + v, black, l) > 0.5) colour = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "    gl_FragColor = colour;\n"
        "}";

    SDL_GLContext context_ = nullptr;
    bool usable_ = false, artDirty_ = true, dithered_ = false;
    std::string error_;
    std::vector<uint8_t> atlas_, lut_;
    GLuint program_ = 0, scene_ = 0, cells_ = 0, atlas_tex_ = 0, lutTex_ = 0;
    int sceneW_ = 0, sceneH_ = 0, cellsW_ = 0, cellsH_ = 0;
    uint64_t cellsGen_ = ~uint64_t(0), draws_ = 0;
    GLint locView_ = -1, locCells_ = -1;
    GLint savedProgram_ = 0, savedActive_ = 0, savedBinding_[4] = {0, 0, 0, 0};

    using GetString = const GLubyte*(APIENTRY*)(GLenum);
    using GetInteger = void(APIENTRY*)(GLenum, GLint*);
    using GetError = GLenum(APIENTRY*)();
    using ActiveTexture = void(APIENTRY*)(GLenum);
    using BindTexture = void(APIENTRY*)(GLenum, GLuint);
    using GenTextures = void(APIENTRY*)(GLsizei, GLuint*);
    using DeleteTextures = void(APIENTRY*)(GLsizei, const GLuint*);
    using TexImage2D = void(APIENTRY*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
    using TexSubImage2D = void(APIENTRY*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
    using CopyTexSubImage2D = void(APIENTRY*)(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei);
    using TexParameteri = void(APIENTRY*)(GLenum, GLenum, GLint);
    using PixelStore = void(APIENTRY*)(GLenum, GLint);
    using BindBuffer = void(APIENTRY*)(GLenum, GLuint);
    using CreateShader = GLuint(APIENTRY*)(GLenum);
    using ShaderSource = void(APIENTRY*)(GLuint, GLsizei, const char* const*, const GLint*);
    using CompileShader = void(APIENTRY*)(GLuint);
    using GetShaderiv = void(APIENTRY*)(GLuint, GLenum, GLint*);
    using GetInfoLog = void(APIENTRY*)(GLuint, GLsizei, GLsizei*, char*);
    using DeleteShader = void(APIENTRY*)(GLuint);
    using CreateProgram = GLuint(APIENTRY*)();
    using AttachShader = void(APIENTRY*)(GLuint, GLuint);
    using LinkProgram = void(APIENTRY*)(GLuint);
    using DeleteProgram = void(APIENTRY*)(GLuint);
    using UseProgram = void(APIENTRY*)(GLuint);
    using GetUniformLocation = GLint(APIENTRY*)(GLuint, const char*);
    using Uniform1i = void(APIENTRY*)(GLint, GLint);
    using Uniform4f = void(APIENTRY*)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
    GetString getString_ = nullptr; GetInteger getInteger_ = nullptr; GetError getError_ = nullptr;
    ActiveTexture activeTexture_ = nullptr; BindTexture bindTexture_ = nullptr; GenTextures genTextures_ = nullptr;
    DeleteTextures deleteTextures_ = nullptr; TexImage2D texImage2D_ = nullptr; TexSubImage2D texSubImage2D_ = nullptr;
    CopyTexSubImage2D copyTexSubImage2D_ = nullptr;
    TexParameteri texParameteri_ = nullptr; PixelStore pixelStore_ = nullptr; BindBuffer bindBuffer_ = nullptr;
    CreateShader createShader_ = nullptr; ShaderSource shaderSource_ = nullptr; CompileShader compileShader_ = nullptr;
    GetShaderiv getShaderiv_ = nullptr; GetInfoLog getShaderInfoLog_ = nullptr; DeleteShader deleteShader_ = nullptr;
    CreateProgram createProgram_ = nullptr; AttachShader attachShader_ = nullptr; LinkProgram linkProgram_ = nullptr;
    GetShaderiv getProgramiv_ = nullptr; GetInfoLog getProgramInfoLog_ = nullptr; DeleteProgram deleteProgram_ = nullptr;
    UseProgram useProgram_ = nullptr; GetUniformLocation getUniformLocation_ = nullptr; Uniform1i uniform1i_ = nullptr;
    Uniform4f uniform4f_ = nullptr;
};

}  // namespace ta
