#pragma once

// Indexed structure palette on SDL's OpenGL backend (workstream M).
//
// The baked palette pages (client/palettesheets.h) hold every frame expanded
// into its 20 shade bands as RGBA. This draws the same polygons from the
// frame's indices instead: one 512x512 (index, owner, alpha) page per layout
// page, a LUT of palette[shade[level][index]] per bank, and a GLSL program that
// replaces SDL's texture program for those draws only (GeometrySubmit's
// NativeProgram hook: SDL primes target/viewport/scissor/projection/blend,
// the program draws, SDL's program and bindings are restored).
//
// Exactness. The polygon's texture coordinates are the baked ones, with the
// sheet id added as an exact integer offset (palette::encodeSheet); the vertex
// shader removes it bit-exactly, so the interpolated coordinate is the very
// value SDL's program would have sampled the baked page with. Which texel
// NEAREST sampling picks for it is left to the texture unit: a 2048x1 texture
// holding each column's own number is sampled at (u, .5) and (v, .5), so the
// shader learns the baked page texel the hardware would have read, whatever
// its sub-texel rounding. palette::decodeTexel then names the band and the
// texel inside it (the CPU version is tested against the baked expansion),
// and the colour is fetched from the LUT: RGB from an RGBA8 texel, alpha from
// the index page's alpha byte -- the same normalized values the baked texel
// holds. A texel outside the polygon's own sheet (a coordinate on, or by float
// overshoot just past, the sheet's outer edge) is what the baked page holds
// there: the owner recorded at index texel (X/4, Y/5) names the neighbouring
// sheet and the decode uses its entry; no owner reads transparent black (page
// memory no sheet was written to; palette::IndexPageModel is the CPU version).
// Output = colour * vertex colour, SDL's exact expression. Bilinear
// (Texture Filtering Units/Everything) filters resolved COLOURS of the four
// texels, never indices; its weights cannot be bit-identical to the texture
// unit's, so the linear program is experimental (GameView keeps the baked
// pages for linear unit filtering unless TAK_PALETTE_INDEXED_LINEAR).
//
// Lifetime. GL objects belong to the GL context they were made in; when the
// current context changes (renderer recreated) they are forgotten, not
// deleted, and rebuilt on demand. Every GL texture is counted in gpuvram.
// Render thread only.

#include <SDL.h>
#include <SDL_opengl.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "client/geometrysubmit.h"
#include "client/gpuvram.h"
#include "client/palettesheets.h"

namespace tak {
namespace palette = render::palette;

class PaletteIndexedGL {
public:
    PaletteIndexedGL() = default;
    PaletteIndexedGL(const PaletteIndexedGL&) = delete;
    PaletteIndexedGL& operator=(const PaletteIndexedGL&) = delete;
    ~PaletteIndexedGL() { release(); }

    // The indexed path on this renderer: the opengl backend, the GL 2.0 entry
    // points, and both programs compiled and linked. Rebuilds after a context
    // change; a compile/link failure is remembered for that context.
    bool ready(SDL_Renderer* renderer) {
        SDL_RendererInfo info{};
        if (SDL_GetRendererInfo(renderer, &info) != 0 || !info.name || std::strcmp(info.name, "opengl")) return false;
        const auto context = SDL_GL_GetCurrentContext();
        if (!context) return false;
        if (context != context_) { forget(); context_ = context; usable_ = load() && build(); }
        return usable_;
    }
    bool usable() const { return usable_; }
    // Debug classification (TAK_PALETTE_MARK_EDGES): the programs paint a decode
    // outside its sheet magenta (the owner-lookup path; shows where it runs).
    // Takes effect when the programs are next built.
    void setMarkOutside(bool mark) { markOutside_ = mark; }
    const std::string& error() const { return error_; }

    // Index pages, one per layout page. create() makes a zeroed 512x512 RGBA page
    // (no owner anywhere until a frame is uploaded); false on budget or GL failure.
    size_t pages() const { return pages_.size(); }
    bool createPage() {
        if (!usable_) return false;
        if (gpuvram::blocked() || !gpuvram::wouldFit(palette::kIndexPageBytes)) return false;
        const std::vector<uint8_t> zero(palette::kIndexPageBytes, 0);
        GLuint name = makeTexture(palette::kIndexPageSide, palette::kIndexPageSide, GL_RGBA8, GL_RGBA, zero.data());
        if (!name) { gpuvram::noteFail(); return false; }
        gpuvram::addBuffer(palette::kIndexPageBytes);
        bytes_ += palette::kIndexPageBytes;
        auto page = std::make_unique<Page>();
        page->owner = this;
        page->texture = name;
        pages_.push_back(std::move(page));
        return true;
    }
    // One frame's texels (palette::indexTexels: index, owner = sheet + 1, alpha)
    // at its indexed rectangle (x, y), and its sheet-table entry. Upload the
    // entry first: a frame's owner texels name it. False on GL error (nothing to
    // undo: the caller does not commit the rectangle, and a later frame placed
    // there overwrites any texels written).
    bool upload(size_t page, int x, int y, const palette::Frame& frame, uint32_t sheet) {
        if (page >= pages_.size() || sheet >= palette::kMaxSheetsPerPage) return false;
        palette::indexTexels(frame, sheet, scratch_);
        return subImage(pages_[page]->texture, x, y, frame.width, frame.height, GL_RGBA, scratch_.data());
    }
    bool uploadEntry(size_t page, uint32_t sheet, const palette::SheetEntry& entry) {
        if (page >= pages_.size() || sheet >= palette::kMaxSheetsPerPage) return false;
        const auto texels = palette::entryTexels(entry);
        const palette::Rect t = palette::tableTexels(sheet);
        return subImage(pages_[page]->texture, t.x, t.y, t.w, t.h, GL_RGBA, texels.data());
    }
    // LUT rows of one bank (bank < kMaxBanks).
    bool setBank(int bank, const palette::Bank& source) {
        if (!usable_ || bank < 0 || bank >= palette::kMaxBanks) return false;
        static uint8_t rows[palette::kShadeLevels][256][4];
        palette::bankLut(source, rows);
        return subImage(lut_, 0, bank * palette::kShadeLevels, 256, palette::kShadeLevels, GL_RGBA, &rows[0][0][0]);
    }
    // The program binding for one page (stable address while the page lives).
    NativeProgram* program(size_t page, bool linear) {
        if (page >= pages_.size() || !usable_) return nullptr;
        pages_[page]->linear = linear;
        return pages_[page].get();
    }
    // ---- pass use (NativePassGL, workstream N) -----------------------------------
    // The program and the textures (units 1-3: index page, identity, LUT) for one
    // page's runs inside a native pass; NativePassGL binds them and restores the
    // state SDL primed when the pass ends. False for an unknown page.
    bool passBinding(size_t page, bool linear, GLuint& program, GLuint (&textures)[3]) {
        if (page >= pages_.size() || !usable_) return false;
        program = linear ? linear_ : nearest_;
        textures[0] = pages_[page]->texture;
        textures[1] = identity_;
        textures[2] = lut_;
        ++draws_;
        return true;
    }

    // Tracked GPU bytes: index pages + identity + LUT.
    size_t gpuBytes() const { return bytes_; }
    uint64_t draws() const { return draws_; }

    // Delete every GL object (when the context that made them is current) and
    // return their tracked bytes. The next ready() rebuilds the programs.
    void release() {
        if (context_ && SDL_GL_GetCurrentContext() == context_ && deleteTextures_) {
            for (auto& page : pages_) deleteTextures_(1, &page->texture);
            if (identity_) deleteTextures_(1, &identity_);
            if (lut_) deleteTextures_(1, &lut_);
            for (GLuint program : {nearest_, linear_}) if (program && deleteProgram_) deleteProgram_(program);
        }
        forget();
    }
    // Drop the index pages only (the layout is being rebuilt); programs,
    // identity and LUT stay.
    void releasePages() {
        if (context_ && SDL_GL_GetCurrentContext() == context_ && deleteTextures_)
            for (auto& page : pages_) deleteTextures_(1, &page->texture);
        for (size_t i = 0; i < pages_.size(); ++i) { gpuvram::removeBuffer(palette::kIndexPageBytes); bytes_ -= palette::kIndexPageBytes; }
        pages_.clear();
    }

private:
    struct Page final : NativeProgram {
        PaletteIndexedGL* owner = nullptr;
        GLuint texture = 0;
        bool linear = false;
        GLint savedProgram = 0, savedActive = 0, savedBinding[3] = {0, 0, 0};
        bool install() override { return owner->install(*this); }
        void restore() override { owner->restore(*this); }
    };
    static constexpr GLenum kUnits[3] = {GL_TEXTURE1, GL_TEXTURE2, GL_TEXTURE3};
    bool install(Page& page) {
        getInteger_(GL_CURRENT_PROGRAM, &page.savedProgram);
        getInteger_(GL_ACTIVE_TEXTURE, &page.savedActive);
        const GLuint textures[3] = {page.texture, identity_, lut_};
        for (int i = 0; i < 3; ++i) {
            activeTexture_(kUnits[i]);
            getInteger_(GL_TEXTURE_BINDING_2D, &page.savedBinding[i]);
            bindTexture_(GL_TEXTURE_2D, textures[i]);
        }
        activeTexture_(GLenum(page.savedActive));
        useProgram_(page.linear ? linear_ : nearest_);
        ++draws_;
        return true;
    }
    void restore(Page& page) {
        useProgram_(GLuint(page.savedProgram));
        for (int i = 0; i < 3; ++i) { activeTexture_(kUnits[i]); bindTexture_(GL_TEXTURE_2D, GLuint(page.savedBinding[i])); }
        activeTexture_(GLenum(page.savedActive));
    }

    void forget() {
        for (size_t i = 0; i < pages_.size(); ++i) gpuvram::removeBuffer(palette::kIndexPageBytes);
        if (identity_) gpuvram::removeBuffer(size_t(palette::kPageSide) * 4);
        if (lut_) gpuvram::removeBuffer(size_t(256) * 256 * 4);
        pages_.clear();
        identity_ = lut_ = 0;
        nearest_ = linear_ = 0;
        bytes_ = 0;
        context_ = nullptr;
        usable_ = false;
    }

    // Texture unit 1 is used for creation and uploads; its binding, the active
    // unit and the unpack state are restored, so neither SDL's cached bindings
    // nor its later uploads see a difference.
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
    bool subImage(GLuint texture, int x, int y, int w, int h, GLenum format, const void* pixels) {
        if (!usable_ || !texture) return false;
        return onScratchUnit(texture, [&] { texSubImage2D_(GL_TEXTURE_2D, 0, x, y, w, h, format, GL_UNSIGNED_BYTE, pixels); });
    }

    GLuint compile(GLenum type, const char* source) {
        const GLuint shader = createShader_(type);
        if (!shader) return 0;
        const char* sources[2] = {markOutside_ && type == GL_FRAGMENT_SHADER ? "#define TAK_MARK_OUTSIDE\n" : "", source};
        shaderSource_(shader, 2, sources, nullptr);
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
    GLuint link(const char* fragment) {
        const char* vertex = kVertexShader;
        const GLuint vs = compile(GL_VERTEX_SHADER, vertex), fs = compile(GL_FRAGMENT_SHADER, fragment);
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
        if (program) {
            // Sampler units are fixed per program; set them once.
            GLint previous = 0;
            getInteger_(GL_CURRENT_PROGRAM, &previous);
            useProgram_(program);
            uniform1i_(getUniformLocation_(program, "indexPage"), 1);
            uniform1i_(getUniformLocation_(program, "identity"), 2);
            uniform1i_(getUniformLocation_(program, "lut"), 3);
            useProgram_(GLuint(previous));
        }
        return program;
    }
    bool build() {
        while (getError_() != GL_NO_ERROR) {}
        nearest_ = link(kNearestShader);
        linear_ = nearest_ ? link(kLinearShader) : 0;
        if (!nearest_ || !linear_) {
            std::fprintf(stderr, "palette: indexed program unavailable (%s)\n", error_.c_str());
            return false;
        }
        std::vector<uint8_t> identity(size_t(palette::kPageSide) * 4);
        for (int x = 0; x < palette::kPageSide; ++x) {
            identity[size_t(x) * 4] = uint8_t(x & 255);
            identity[size_t(x) * 4 + 1] = uint8_t(x >> 8);
            identity[size_t(x) * 4 + 3] = 255;
        }
        const std::vector<uint8_t> zero(size_t(256) * 256 * 4, 0);
        identity_ = makeTexture(palette::kPageSide, 1, GL_RGBA8, GL_RGBA, identity.data());
        lut_ = identity_ ? makeTexture(256, 256, GL_RGBA8, GL_RGBA, zero.data()) : 0;
        if (!identity_ || !lut_) {
            error_ = "identity/LUT texture";
            if (identity_) deleteTextures_(1, &identity_);
            identity_ = 0;
            lut_ = 0;
            std::fprintf(stderr, "palette: indexed program unavailable (%s)\n", error_.c_str());
            return false;
        }
        gpuvram::addBuffer(size_t(palette::kPageSide) * 4);
        gpuvram::addBuffer(size_t(256) * 256 * 4);
        bytes_ += size_t(palette::kPageSide) * 4 + size_t(256) * 256 * 4;
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
            proc(texParameteri_, "glTexParameteri") && proc(pixelStore_, "glPixelStorei") &&
            proc(createShader_, "glCreateShader") && proc(shaderSource_, "glShaderSource") &&
            proc(compileShader_, "glCompileShader") && proc(getShaderiv_, "glGetShaderiv") &&
            proc(getShaderInfoLog_, "glGetShaderInfoLog") && proc(deleteShader_, "glDeleteShader") &&
            proc(createProgram_, "glCreateProgram") && proc(attachShader_, "glAttachShader") &&
            proc(linkProgram_, "glLinkProgram") && proc(getProgramiv_, "glGetProgramiv") &&
            proc(getProgramInfoLog_, "glGetProgramInfoLog") && proc(deleteProgram_, "glDeleteProgram") &&
            proc(useProgram_, "glUseProgram") && proc(getUniformLocation_, "glGetUniformLocation") &&
            proc(uniform1i_, "glUniform1i");
        if (!proc(bindBuffer_, "glBindBuffer")) bindBuffer_ = nullptr;
        if (!ok) { error_ = "GL 2.0 entry points"; return false; }
        const auto* version = reinterpret_cast<const char*>(getString_(GL_VERSION));
        if (!version || std::atoi(version) < 2) { error_ = "GL version"; return false; }
        return true;
    }

    // SDL's TEXTURE_VERTEX_SHADER plus the sheet id: coordinates arrive as
    // c + 2n (palette::encodeSheet); c is recovered exactly.
    static constexpr const char* kVertexShader =
        "varying vec4 v_color;\n"
        "varying vec2 v_texCoord;\n"
        "varying vec2 v_sheet;\n"
        "\n"
        "void main()\n"
        "{\n"
        "    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
        "    v_color = gl_Color;\n"
        "    vec2 coord = vec2(gl_MultiTexCoord0);\n"
        "    vec2 sheet = floor(coord * 0.5);\n"
        "    v_texCoord = coord - 2.0 * sheet;\n"
        "    v_sheet = sheet;\n"
        "}";
#define TAK_PALETTE_SHADER_COMMON \
        "varying vec4 v_color;\n" \
        "varying vec2 v_texCoord;\n" \
        "varying vec2 v_sheet;\n" \
        "uniform sampler2D indexPage;\n" \
        "uniform sampler2D identity;\n" \
        "uniform sampler2D lut;\n" \
        "float byteOf(float c) { return floor(c * 255.0 + 0.5); }\n" \
        "vec4 pageTexel(float x, float y) { return texture2D(indexPage, (vec2(x, y) + 0.5) * (1.0 / 512.0)); }\n" \
        "float word(vec4 t) { return byteOf(t.r) + 256.0 * byteOf(t.a); }\n" \
        "/* sheet table: four texels per sheet from row 410 (palette::tableTexels) */\n" \
        "void entryOf(float sheet, out float x0q, out float y0q, out float w, out float h, out float bank) {\n" \
        "    float base = sheet * 4.0;\n" \
        "    float row = 410.0 + floor(base * (1.0 / 512.0));\n" \
        "    float col = base - 512.0 * floor(base * (1.0 / 512.0));\n" \
        "    x0q = word(pageTexel(col, row));\n" \
        "    y0q = word(pageTexel(col + 1.0, row));\n" \
        "    w = word(pageTexel(col + 2.0, row));\n" \
        "    vec4 t = pageTexel(col + 3.0, row);\n" \
        "    float a = byteOf(t.a);\n" \
        "    bank = floor(a * 0.5);\n" \
        "    h = byteOf(t.r) + 256.0 * (a - 2.0 * bank);\n" \
        "}\n" \
        "void sheetEntry(out float x0q, out float y0q, out float w, out float h, out float bank) {\n" \
        "    entryOf(floor(v_sheet.x + 0.5) + 4096.0 * floor(v_sheet.y + 0.5), x0q, y0q, w, h, bank);\n" \
        "}\n" \
        "/* palette::IndexPageModel::resolve: baked page texel (X, Y) sampled by a polygon of the sheet */\n" \
        "vec4 resolve(float X, float Y, float x0q, float y0q, float w, float h, float bank) {\n" \
        "    float dx = X - x0q * 4.0, dy = Y - y0q * 5.0;\n" \
        "    if (dx < 0.0 || dy < 0.0 || dx >= 4.0 * w || dy >= 5.0 * h) {\n" \
        "#ifdef TAK_MARK_OUTSIDE\n" \
        "        return vec4(1.0, 0.0, 1.0, 1.0);\n" \
        "#endif\n" \
        "        /* outside the own sheet: the sheet the baked page holds there (owner + 1 at (X/4, Y/5)) */\n" \
        "        vec4 o = pageTexel(floor(X * 0.25), floor((Y + 0.5) / 5.0));\n" \
        "        float owner = byteOf(o.g) + 256.0 * byteOf(o.b);\n" \
        "        if (owner < 0.5) return vec4(0.0);\n" \
        "        entryOf(owner - 1.0, x0q, y0q, w, h, bank);\n" \
        "        dx = X - x0q * 4.0;\n" \
        "        dy = Y - y0q * 5.0;\n" \
        "    }\n" \
        "    float col = clamp(floor((dx + 0.5) / w), 0.0, 3.0);\n" \
        "    float row = clamp(floor((dy + 0.5) / h), 0.0, 4.0);\n" \
        "    float ix = clamp(dx - col * w, 0.0, w - 1.0);\n" \
        "    float iy = clamp(dy - row * h, 0.0, h - 1.0);\n" \
        "    vec4 la = pageTexel(x0q + ix, y0q + iy);\n" \
        "    float level = 5.0 + row * 4.0 + col;\n" \
        "    vec3 rgb = texture2D(lut, vec2((byteOf(la.r) + 0.5) * (1.0 / 256.0),\n" \
        "                                   (bank * 32.0 + level + 0.5) * (1.0 / 256.0))).rgb;\n" \
        "    return vec4(rgb, la.a);\n" \
        "}\n"
    // NEAREST: the texture unit chooses the texel (identity lookups).
    static constexpr const char* kNearestShader =
        TAK_PALETTE_SHADER_COMMON
        "void main()\n"
        "{\n"
        "    float x0q, y0q, w, h, bank;\n"
        "    sheetEntry(x0q, y0q, w, h, bank);\n"
        "    vec4 cx = texture2D(identity, vec2(v_texCoord.x, 0.5));\n"
        "    vec4 cy = texture2D(identity, vec2(v_texCoord.y, 0.5));\n"
        "    float X = byteOf(cx.r) + 256.0 * byteOf(cx.g);\n"
        "    float Y = byteOf(cy.r) + 256.0 * byteOf(cy.g);\n"
        "    gl_FragColor = resolve(X, Y, x0q, y0q, w, h, bank) * v_color;\n"
        "}";
    // LINEAR (experimental): four resolved colours (neighbouring sheets
    // included, as the baked page bleeds them), 8-bit sub-texel weights,
    // page-edge clamp.
    static constexpr const char* kLinearShader =
        TAK_PALETTE_SHADER_COMMON
        "void main()\n"
        "{\n"
        "    float x0q, y0q, w, h, bank;\n"
        "    sheetEntry(x0q, y0q, w, h, bank);\n"
        "    vec2 p = v_texCoord * 2048.0 - 0.5;\n"
        "    vec2 b = floor(p);\n"
        "    vec2 f = floor((p - b) * 256.0) * (1.0 / 256.0);\n"
        "    vec2 b0 = clamp(b, 0.0, 2047.0), b1 = clamp(b + 1.0, 0.0, 2047.0);\n"
        "    vec4 c00 = resolve(b0.x, b0.y, x0q, y0q, w, h, bank);\n"
        "    vec4 c10 = resolve(b1.x, b0.y, x0q, y0q, w, h, bank);\n"
        "    vec4 c01 = resolve(b0.x, b1.y, x0q, y0q, w, h, bank);\n"
        "    vec4 c11 = resolve(b1.x, b1.y, x0q, y0q, w, h, bank);\n"
        "    gl_FragColor = mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y) * v_color;\n"
        "}";
#undef TAK_PALETTE_SHADER_COMMON
    std::vector<std::unique_ptr<Page>> pages_;
    std::vector<uint8_t> scratch_;   // one frame's RGBA index texels (upload)
    GLuint identity_ = 0, lut_ = 0, nearest_ = 0, linear_ = 0;
    size_t bytes_ = 0;
    uint64_t draws_ = 0;
    SDL_GLContext context_ = nullptr;
    bool usable_ = false, markOutside_ = false;
    std::string error_;

    const GLubyte* (APIENTRY* getString_)(GLenum) = nullptr;
    void (APIENTRY* getInteger_)(GLenum, GLint*) = nullptr;
    GLenum (APIENTRY* getError_)() = nullptr;
    void (APIENTRY* activeTexture_)(GLenum) = nullptr;
    void (APIENTRY* bindTexture_)(GLenum, GLuint) = nullptr;
    void (APIENTRY* genTextures_)(GLsizei, GLuint*) = nullptr;
    void (APIENTRY* deleteTextures_)(GLsizei, const GLuint*) = nullptr;
    void (APIENTRY* texImage2D_)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr;
    void (APIENTRY* texSubImage2D_)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*) = nullptr;
    void (APIENTRY* texParameteri_)(GLenum, GLenum, GLint) = nullptr;
    void (APIENTRY* pixelStore_)(GLenum, GLint) = nullptr;
    void (APIENTRY* bindBuffer_)(GLenum, GLuint) = nullptr;
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
};

}  // namespace tak
