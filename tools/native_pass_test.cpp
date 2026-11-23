// Workstream N: explicitly owned native passes (client/nativepass.h) on SDL's
// OpenGL backend.
//
// The same commands -- random triangles of BLEND and premultiplied textures,
// random vertex colours and alpha, nearest and linear sampling -- are drawn once
// command by command through SDL and once inside ONE native pass (client arrays
// and a GL buffer), with SDL work queued before the pass and SDL draws after it
// that would show any leaked state (texture, program, blend, arrays, matrices,
// colour). Every case must be pixel-identical, and the pass verifier (a
// GLStateSnapshot after SDL's priming and after end()) must find the GL state
// SDL left -- across output scales 1/2/4, a logical size (letterboxed viewport),
// the window and a render target, clip on/off, a viewport offset, a window
// resize between frames, and a renderer destroyed and recreated (a new GL
// context: the pass rebuilds its program).
//
// Also: joinable() admits only what SDL would draw with its RGBA program in
// BLEND or the premultiplied mode (refuses RGB888, ADD/MOD/NONE, colour or alpha
// mod); begin() refuses a fractional scale without changing any state; a
// deliberately leaked binding inside a pass is reported by the verifier (so the
// check can fail); an SDL renderer without shaders (SDL_HINT_RENDER_OPENGL_SHADERS
// =0, fixed function) refuses the pass once and then for the whole context, and
// the command-by-command fallback is identical; the software renderer refuses.
//
// Accelerated runs need TAK_GEOMETRY_TEST_ACCELERATED=1 (SDL_VIDEODRIVER=offscreen
// is fine); without it the test checks the software refusal only.
#include "client/nativepass.h"

#include <array>
#include <cstdio>
#include <random>
#include <vector>

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

const SDL_BlendMode kPremultiplied = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE,
    SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
    SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD);

struct Command { int kind; std::vector<SDL_Vertex> v; };   // kind: texture index

SDL_Texture* makeTexture(SDL_Renderer* r, std::mt19937& rng, Uint32 format, SDL_BlendMode blend, bool premultiply) {
    SDL_Texture* t = SDL_CreateTexture(r, format, SDL_TEXTUREACCESS_STATIC, 9, 7);
    if (!t) return nullptr;
    std::vector<uint8_t> px(9 * 7 * 4);
    for (size_t i = 0; i < px.size(); i += 4) {
        const uint8_t a = uint8_t(rng() % 3 == 0 ? 255 : rng());
        for (int c = 0; c < 3; ++c) px[i + c] = premultiply ? uint8_t(rng() % (a + 1)) : uint8_t(rng());
        px[i + 3] = a;
    }
    SDL_UpdateTexture(t, nullptr, px.data(), 9 * 4);
    SDL_SetTextureBlendMode(t, blend);
    return t;
}

std::vector<Command> makeCommands(std::mt19937& rng, int kinds, int count, float w, float h) {
    std::vector<Command> out;
    for (int c = 0; c < count; ++c) {
        Command cmd;
        cmd.kind = int(rng() % unsigned(kinds));
        const int tris = 1 + int(rng() % 9);
        for (int t = 0; t < tris * 3; ++t)
            cmd.v.push_back({{float(rng() % unsigned(w * 8)) / 8.0f - 6, float(rng() % unsigned(h * 8)) / 8.0f - 6},
                             {uint8_t(rng()), uint8_t(rng()), uint8_t(rng()), uint8_t(rng() % 2 ? 255 : rng())},
                             {float(rng() % 1000) / 999.0f, float(rng() % 1000) / 999.0f}});
        out.push_back(std::move(cmd));
    }
    return out;
}

struct GL {
    void (APIENTRY* genBuffers)(GLsizei, GLuint*) = nullptr;
    void (APIENTRY* bindBuffer)(GLenum, GLuint) = nullptr;
    void (APIENTRY* bufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
    void (APIENTRY* deleteBuffers)(GLsizei, const GLuint*) = nullptr;
    void (APIENTRY* activeTexture)(GLenum) = nullptr;
    void (APIENTRY* bindTexture)(GLenum, GLuint) = nullptr;
    void load() {
        genBuffers = reinterpret_cast<decltype(genBuffers)>(SDL_GL_GetProcAddress("glGenBuffers"));
        bindBuffer = reinterpret_cast<decltype(bindBuffer)>(SDL_GL_GetProcAddress("glBindBuffer"));
        bufferData = reinterpret_cast<decltype(bufferData)>(SDL_GL_GetProcAddress("glBufferData"));
        deleteBuffers = reinterpret_cast<decltype(deleteBuffers)>(SDL_GL_GetProcAddress("glDeleteBuffers"));
        activeTexture = reinterpret_cast<decltype(activeTexture)>(SDL_GL_GetProcAddress("glActiveTexture"));
        bindTexture = reinterpret_cast<decltype(bindTexture)>(SDL_GL_GetProcAddress("glBindTexture"));
    }
};

// One renderer's worth of cases. Returns the number of compared cases.
int runCases(SDL_Window* window, SDL_Renderer* renderer, std::mt19937& rng, int W, int H, bool resize) {
    tak::GeometrySubmit submit;
    tak::NativePassGL pass;
    CHECK(pass.ready(renderer));
    pass.setVerify(true);
    GL gl;
    gl.load();
    SDL_Texture* target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, W, H);
    // A second, differently sized target: from frame 1 on, the offscreen cases draw
    // into it (SDL re-derives viewport and projection for the new size at the next
    // flush -- the pass's priming -- as it does for a resized window).
    SDL_Texture* target2 = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, W - 29, H + 13);
    SDL_Texture* blendA = makeTexture(renderer, rng, SDL_PIXELFORMAT_ABGR8888, SDL_BLENDMODE_BLEND, false);
    SDL_Texture* blendB = makeTexture(renderer, rng, SDL_PIXELFORMAT_ARGB8888, SDL_BLENDMODE_BLEND, false);
    SDL_Texture* premul = makeTexture(renderer, rng, SDL_PIXELFORMAT_ABGR8888, kPremultiplied, true);
    SDL_Texture* premulB = makeTexture(renderer, rng, SDL_PIXELFORMAT_ARGB8888, kPremultiplied, true);
    CHECK(target && blendA && blendB && premul && premulB);
    SDL_Texture* tex[4] = {blendA, blendB, premul, premulB};

    // ---- joinable(): what SDL would draw with SHADER_RGBA in BLEND / premultiplied ----
    {
        CHECK(pass.joinable(submit, renderer, blendA, kPremultiplied).name != 0);
        CHECK(!pass.joinable(submit, renderer, blendA, kPremultiplied).premultiplied);
        CHECK(pass.joinable(submit, renderer, premul, kPremultiplied).name != 0);
        CHECK(pass.joinable(submit, renderer, premul, kPremultiplied).premultiplied);
        SDL_Texture* rgb = makeTexture(renderer, rng, SDL_PIXELFORMAT_RGB888, SDL_BLENDMODE_BLEND, false);
        CHECK(rgb && pass.joinable(submit, renderer, rgb, kPremultiplied).name == 0);   // SDL's SHADER_RGB
        SDL_Texture* other = makeTexture(renderer, rng, SDL_PIXELFORMAT_ABGR8888, SDL_BLENDMODE_ADD, false);
        CHECK(pass.joinable(submit, renderer, other, kPremultiplied).name == 0);
        SDL_SetTextureBlendMode(other, SDL_BLENDMODE_MOD);
        CHECK(pass.joinable(submit, renderer, other, kPremultiplied).name == 0);
        SDL_SetTextureBlendMode(other, SDL_BLENDMODE_NONE);
        CHECK(pass.joinable(submit, renderer, other, kPremultiplied).name == 0);
        SDL_SetTextureBlendMode(other, SDL_BLENDMODE_BLEND);
        CHECK(pass.joinable(submit, renderer, other, kPremultiplied).name != 0);
        SDL_SetTextureColorMod(other, 255, 254, 255);
        CHECK(pass.joinable(submit, renderer, other, kPremultiplied).name == 0);
        SDL_SetTextureColorMod(other, 255, 255, 255);
        SDL_SetTextureAlphaMod(other, 128);
        CHECK(pass.joinable(submit, renderer, other, kPremultiplied).name == 0);
        CHECK(pass.joinable(submit, renderer, nullptr, kPremultiplied).name == 0);
        SDL_DestroyTexture(rgb);
        SDL_DestroyTexture(other);
    }

    struct Setup { float scale; bool logical; };
    int cases = 0, refusedCases = 0;
    bool resized = false;
    for (int frame = 0; frame < (resize ? 2 : 1); ++frame) {
        if (frame == 1) {
            // A resized window between frames: SDL re-derives viewport/projection from
            // the new drawable size at its next flush (the pass's priming).
            SDL_SetWindowSize(window, W - 37, H - 21);
            int ow = 0, oh = 0;
            for (int wait = 0; wait < 20; ++wait) {   // windowing drivers apply it asynchronously
                SDL_PumpEvents();
                SDL_RenderPresent(renderer);
                SDL_GetRendererOutputSize(renderer, &ow, &oh);
                if (ow == W - 37 && oh == H - 21) break;
                SDL_Delay(25);
            }
            // SDL's offscreen driver keeps its EGL surface size: the resize is then not
            // exercised (reported); a windowing driver (x11/wayland) resizes the drawable.
            resized = ow == W - 37 && oh == H - 21;
            std::printf("resize: output %dx%d (%s)\n", ow, oh, resized ? "resized" : "driver kept its surface size");
        }
        for (Setup setup : {Setup{1, false}, Setup{2, false}, Setup{4, false}, Setup{0, true}})
        for (bool offscreen : {false, true})
        for (bool clipped : {false, true})
        for (bool buffer : {false, true})
        for (bool premultipliedOnly : {false, true})
        for (auto filter : {SDL_ScaleModeNearest, SDL_ScaleModeLinear}) {
            int ow = 0, oh = 0;
            SDL_Texture* into = offscreen ? (frame == 1 ? target2 : target) : nullptr;
            SDL_SetRenderTarget(renderer, into);
            SDL_RenderSetLogicalSize(renderer, 0, 0);
            if (into) SDL_QueryTexture(into, nullptr, nullptr, &ow, &oh);
            else SDL_GetRendererOutputSize(renderer, &ow, &oh);
            const auto commands = makeCommands(rng, premultipliedOnly ? 2 : 4, 30, float(ow) / std::max(1.0f, setup.scale),
                                               float(oh) / std::max(1.0f, setup.scale));
            for (auto* t : tex) SDL_SetTextureScaleMode(t, filter);
            std::vector<SDL_Vertex> all;
            std::vector<GLint> first;
            for (const auto& c : commands) { first.push_back(GLint(all.size())); all.insert(all.end(), c.v.begin(), c.v.end()); }
            GLuint vbo = 0;
            if (buffer) {
                gl.genBuffers(1, &vbo);
                gl.bindBuffer(GL_ARRAY_BUFFER, vbo);
                gl.bufferData(GL_ARRAY_BUFFER, GLsizeiptr(all.size() * sizeof(SDL_Vertex)), all.data(), GL_STATIC_DRAW);
                gl.bindBuffer(GL_ARRAY_BUFFER, 0);
            }
            std::array<std::vector<uint8_t>, 2> image;
            for (int path = 0; path < 2; ++path) {   // 0 SDL, 1 native pass
                SDL_SetRenderTarget(renderer, into);
                SDL_RenderSetLogicalSize(renderer, 0, 0);
                SDL_RenderSetScale(renderer, 1, 1);
                SDL_RenderSetViewport(renderer, nullptr);
                SDL_RenderSetClipRect(renderer, nullptr);
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
                SDL_SetRenderDrawColor(renderer, 30, 60, 90, 255);
                SDL_RenderClear(renderer);
                if (setup.logical) SDL_RenderSetLogicalSize(renderer, ow / 2 - 3, oh / 2);   // scale 2, letterboxed
                else {
                    SDL_RenderSetScale(renderer, setup.scale, setup.scale);
                    const SDL_Rect viewport{3, 2, int(float(ow - 9) / setup.scale), int(float(oh - 5) / setup.scale)};
                    SDL_RenderSetViewport(renderer, &viewport);
                }
                const SDL_Rect clip{5, 4, 61, 47};
                SDL_RenderSetClipRect(renderer, clipped ? &clip : nullptr);
                const SDL_FRect before{2, 2, 9, 9};
                SDL_RenderCopyF(renderer, blendB, nullptr, &before);   // queued SDL work ahead of the pass
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 200, 10, 70, 100);
                const SDL_FRect solid{7, 9, 11, 5};
                SDL_RenderFillRectF(renderer, &solid);
                auto texOf = [&](const Command& c) { return premultipliedOnly ? tex[2 + c.kind] : tex[c.kind]; };
                if (path == 0) {
                    for (const auto& c : commands) SDL_RenderGeometry(renderer, texOf(c), c.v.data(), int(c.v.size()), nullptr, 0);
                } else {
                    GLuint names[4];
                    for (int k = 0; k < 4; ++k) names[k] = pass.joinable(submit, renderer, tex[k], kPremultiplied).name;
                    SDL_Texture* prime = premultipliedOnly ? premul : blendA;
                    float sx = 0, sy = 0;
                    SDL_RenderGetScale(renderer, &sx, &sy);
                    const bool integral = (sx == 1 || sx == 2 || sx == 4) && (sy == 1 || sy == 2 || sy == 4);
                    if (!pass.begin(submit, renderer, prime, premultipliedOnly)) {
                        // Refused (a fractional scale): the caller's fallback, command by command.
                        CHECK(!integral);
                        ++refusedCases;
                        for (const auto& c : commands) SDL_RenderGeometry(renderer, texOf(c), c.v.data(), int(c.v.size()), nullptr, 0);
                    } else for (size_t c = 0; c < commands.size(); ++c) {
                        const int k = premultipliedOnly ? 2 + commands[c].kind : commands[c].kind;
                        pass.useTexture(names[k], k >= 2);
                        // Buffer cases mix the two sources inside one pass (arena and
                        // assembled commands): every third command from client memory.
                        if (buffer && c % 3 != 1) {
                            const GLsizei count = GLsizei(commands[c].v.size());
                            submit.passDrawBuffer(vbo, std::span(&first[c], 1), std::span(&count, 1), commands[c].v.size());
                        } else submit.passDraw(commands[c].v);
                    }
                    if (pass.open()) pass.end(submit);
                }
                // SDL draws after the pass: leaked program, bindings, blend, arrays, buffer, colour.
                const SDL_FRect after1{40, 30, 13, 11}, after2{20, 50, 17, 9};
                SDL_RenderCopyF(renderer, premul, nullptr, &after1);
                SDL_RenderCopyF(renderer, blendA, nullptr, &after2);
                SDL_RenderGeometry(renderer, nullptr, commands[1].v.data(), 3, nullptr, 0);
                SDL_SetRenderDrawColor(renderer, 10, 220, 30, 180);
                const SDL_FRect solid2{30, 12, 9, 13};
                SDL_RenderFillRectF(renderer, &solid2);
                SDL_RenderDrawLineF(renderer, 1, 1, 60, 40);
                SDL_RenderSetLogicalSize(renderer, 0, 0);
                SDL_RenderSetScale(renderer, 1, 1);
                SDL_RenderSetViewport(renderer, nullptr);
                SDL_RenderSetClipRect(renderer, nullptr);
                image[size_t(path)].resize(size_t(ow) * oh * 4);
                CHECK(SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGBA32, image[size_t(path)].data(), ow * 4) == 0);
            }
            if (vbo) gl.deleteBuffers(1, &vbo);
            int differing = 0;
            for (size_t i = 0; i < image[0].size(); i += 4) differing += std::memcmp(&image[0][i], &image[1][i], 4) != 0;
            if (differing)
                std::printf("FAIL frame=%d scale=%g logical=%d target=%d clip=%d buffer=%d premultiplied=%d filter=%d: %d pixels differ\n",
                            frame, setup.scale, setup.logical, offscreen, clipped, buffer, premultipliedOnly, int(filter), differing);
            CHECK(differing == 0);
            ++cases;
        }
    }
    CHECK(pass.passes() == uint64_t(cases - refusedCases));
    CHECK(pass.verified() == uint64_t(cases - refusedCases));
    std::printf("%d cases, %d refused (fractional logical scale) and drawn by SDL%s\n", cases, refusedCases,
                resize ? (resized ? ", window resized between frames" : ", resize NOT exercised") : "");
    CHECK(pass.verifyFailures() == 0);
    if (pass.verifyFailures()) std::printf("  state: %s\n", pass.lastFailure().c_str());

    // ---- a fractional scale: refused before any state change, SDL draws instead ----
    {
        SDL_SetRenderTarget(renderer, target);
        SDL_RenderSetScale(renderer, 1.25f, 1.25f);
        tak::GLStateSnapshot snap;
        SDL_RenderFlush(renderer);
        const auto before = snap.capture();
        CHECK(!pass.begin(submit, renderer, blendA, false));
        CHECK(!pass.open());
        CHECK(snap.capture() == before);
        CHECK(pass.refusedPrime() == uint64_t(refusedCases) + 1);
        SDL_RenderSetScale(renderer, 1, 1);
    }

    // ---- the verifier can fail: a binding leaked inside a pass is reported ----
    {
        SDL_SetRenderTarget(renderer, target);
        const uint64_t failuresBefore = pass.verifyFailures();
        GLuint leaked = pass.joinable(submit, renderer, blendB, kPremultiplied).name;
        CHECK(pass.begin(submit, renderer, blendA, false));
        GLint previous = 0;
        auto getInteger = reinterpret_cast<void (APIENTRY*)(GLenum, GLint*)>(SDL_GL_GetProcAddress("glGetIntegerv"));
        gl.activeTexture(GL_TEXTURE5);
        getInteger(GL_TEXTURE_BINDING_2D, &previous);
        gl.bindTexture(GL_TEXTURE_2D, leaked);
        gl.activeTexture(GL_TEXTURE0);
        pass.end(submit);
        CHECK(pass.verifyFailures() == failuresBefore + 1);
        CHECK(pass.lastFailure().find("unit5_texture") != std::string::npos);
        std::printf("leak detected: %s\n", pass.lastFailure().c_str());
        gl.activeTexture(GL_TEXTURE5);
        gl.bindTexture(GL_TEXTURE_2D, GLuint(previous));
        gl.activeTexture(GL_TEXTURE0);
    }
    SDL_SetRenderTarget(renderer, nullptr);
    pass.release();
    submit.clear();
    for (auto* t : tex) SDL_DestroyTexture(t);
    SDL_DestroyTexture(target);
    SDL_DestroyTexture(target2);
    return cases;
}
}  // namespace

int main() {
    const bool accelerated = std::getenv("TAK_GEOMETRY_TEST_ACCELERATED");
    if (!accelerated) {
        SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, 64, 64, 32, SDL_PIXELFORMAT_RGBA32);
        SDL_Renderer* renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
        if (!renderer) { std::printf("%s\n", SDL_GetError()); return 2; }
        tak::NativePassGL pass;
        tak::GeometrySubmit submit;
        CHECK(!pass.ready(renderer));
        SDL_Texture* t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, 4, 4);
        CHECK(!pass.begin(submit, renderer, t, false));
        CHECK(pass.joinable(submit, renderer, t, kPremultiplied).name == 0);
        SDL_DestroyTexture(t);
        SDL_DestroyRenderer(renderer);
        SDL_FreeSurface(surface);
        if (failures) return 1;
        std::printf("PASS: native pass (software renderer refused; set TAK_GEOMETRY_TEST_ACCELERATED=1 for GL)\n");
        return 0;
    }
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 2;
    constexpr int W = 192, H = 144;
    SDL_Window* window = SDL_CreateWindow("native pass", 0, 0, W, H, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED) : nullptr;
    if (!renderer) { std::printf("%s\n", SDL_GetError()); return 2; }
    SDL_RendererInfo info{};
    SDL_GetRendererInfo(renderer, &info);
    if (std::strcmp(info.name, "opengl") != 0) { std::printf("SKIP: renderer %s is not opengl\n", info.name); return 0; }
    auto getString = reinterpret_cast<const GLubyte* (APIENTRY*)(GLenum)>(SDL_GL_GetProcAddress("glGetString"));
    std::printf("OpenGL renderer: %s, %s\n", reinterpret_cast<const char*>(getString(GL_RENDERER)),
                reinterpret_cast<const char*>(getString(GL_VERSION)));
    std::mt19937 rng(9091);
    const size_t baseline = gpuvram::bytes();
    int cases = runCases(window, renderer, rng, W, H, true);

    // ---- renderer destroyed and recreated: a new context, the pass rebuilds ----
    SDL_DestroyRenderer(renderer);
    SDL_SetWindowSize(window, W, H);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    CHECK(renderer != nullptr);
    if (renderer) cases += runCases(window, renderer, rng, W, H, false);

    // ---- SDL without shaders (fixed function): the pass refuses once, for the context ----
    SDL_DestroyRenderer(renderer);
    SDL_SetHint(SDL_HINT_RENDER_OPENGL_SHADERS, "0");
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    CHECK(renderer != nullptr);
    if (renderer) {
        tak::GeometrySubmit submit;
        tak::NativePassGL pass;
        SDL_Texture* t = makeTexture(renderer, rng, SDL_PIXELFORMAT_ABGR8888, SDL_BLENDMODE_BLEND, false);
        CHECK(pass.ready(renderer));   // the program itself still links
        CHECK(!pass.begin(submit, renderer, t, false));
        CHECK(pass.refusedState() == 1 && !pass.usable());
        CHECK(!pass.begin(submit, renderer, t, false));   // no second attempt (no second priming)
        CHECK(pass.attempts() == 1);
        // The caller's fallback after a refused begin: SDL draws, nothing leaked.
        auto commands = makeCommands(rng, 1, 10, W, H);
        std::array<std::vector<uint8_t>, 2> image;
        for (int path = 0; path < 2; ++path) {
            SDL_SetRenderDrawColor(renderer, 30, 60, 90, 255);
            SDL_RenderClear(renderer);
            if (path == 1) CHECK(!pass.begin(submit, renderer, t, false));
            for (const auto& c : commands) SDL_RenderGeometry(renderer, t, c.v.data(), int(c.v.size()), nullptr, 0);
            image[size_t(path)].resize(size_t(W) * H * 4);
            CHECK(SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGBA32, image[size_t(path)].data(), W * 4) == 0);
        }
        CHECK(image[0] == image[1]);
        SDL_DestroyTexture(t);
        pass.release();
        submit.clear();
        SDL_DestroyRenderer(renderer);
    }
    SDL_ResetHint(SDL_HINT_RENDER_OPENGL_SHADERS);
    CHECK(gpuvram::bytes() == baseline);
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("PASS: native pass (%d cases pixel-identical, GL state verified after every pass)\n", cases);
    return 0;
}
