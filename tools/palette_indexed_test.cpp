// Workstream M: the indexed palette program (client/paletteindexed.h) against
// the baked pages it replaces, on SDL's OpenGL backend.
//
// A random roster (frames 1..80 px, three banks with random palettes and shade
// tables) is laid out by palette::PageLayout and held twice: as baked 2048x2048
// RGBA pages, and as indexed pages + LUT. The same triangles -- band corners
// and arbitrary whole-texel coordinates, magnified, minified, slivers, random
// vertex colours and alpha -- are drawn once through SDL with the baked page
// and once through GeometrySubmit with the indexed program, followed by plain
// SDL draws that would show leaked GL state. NEAREST must be pixel-identical in
// every case (scales 1/2/4, window and target, clip on/off, client arrays and a
// GL buffer) -- including samples outside the triangle's own sheet, which read
// the neighbouring sheet (or the zeroed unused page area) exactly as the baked
// page does; a second program set with the outside decodes marked counts them,
// so the test shows that path ran. LINEAR (experimental) is reported, not
// required. Also: rebuild
// after release(), page creation refused under the GPU budget, no tracked-byte
// leak, and ready() refuses the software renderer.
//
// Accelerated runs need TAK_GEOMETRY_TEST_ACCELERATED=1 (SDL_VIDEODRIVER=offscreen
// is fine); without it the test checks the software refusal only.
#include "client/paletteindexed.h"
#include "client/nativepass.h"

#include <array>
#include <cstdio>
#include <random>
#include <vector>

using namespace tak::render;

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

struct Sheet { size_t frame; size_t page; palette::Rect rect; uint32_t id; int bank; };
}  // namespace

int main() {
    const bool accelerated = std::getenv("TAK_GEOMETRY_TEST_ACCELERATED");
    if (!accelerated) {
        SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, 64, 64, 32, SDL_PIXELFORMAT_RGBA32);
        SDL_Renderer* renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
        if (!renderer) { std::printf("%s\n", SDL_GetError()); return 2; }
        tak::PaletteIndexedGL indexed;
        CHECK(!indexed.ready(renderer));
        SDL_DestroyRenderer(renderer);
        SDL_FreeSurface(surface);
        if (failures) return 1;
        std::printf("PASS: palette indexed (software renderer refused; set TAK_GEOMETRY_TEST_ACCELERATED=1 for GL)\n");
        return 0;
    }
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 2;
    constexpr int W = 256, H = 192;
    SDL_Window* window = SDL_CreateWindow("palette parity", 0, 0, W, H, SDL_WINDOW_HIDDEN);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED) : nullptr;
    if (!renderer) { std::printf("%s\n", SDL_GetError()); return 2; }
    SDL_RendererInfo info{};
    SDL_GetRendererInfo(renderer, &info);
    if (std::strcmp(info.name, "opengl") != 0) { std::printf("SKIP: renderer %s is not opengl\n", info.name); return 0; }
    auto getString = reinterpret_cast<const GLubyte* (APIENTRY*)(GLenum)>(SDL_GL_GetProcAddress("glGetString"));
    std::printf("OpenGL renderer: %s\n", reinterpret_cast<const char*>(getString(GL_RENDERER)));

    const size_t baseline = gpuvram::bytes();
    std::mt19937 rng(7171);
    // ---- roster ----
    std::vector<palette::Bank> banks(3);
    for (auto& bank : banks) {
        for (int i = 0; i < 256; ++i) for (int c = 0; c < 4; ++c) bank.palette[i][c] = uint8_t(rng());
        bank.shades.resize(palette::kShadeTableBytes);
        for (auto& s : bank.shades) s = uint8_t(rng());
    }
    std::vector<palette::Frame> frames;
    std::vector<Sheet> sheets;
    palette::PageLayout layout;
    std::vector<uint32_t> perPage;
    for (int f = 0; f < 260; ++f) {
        palette::Frame frame;
        static const int sizes[] = {1, 2, 3, 5, 8, 13, 16, 31, 32, 47, 64, 80, 128, 256};
        frame.width = sizes[rng() % 14];
        frame.height = sizes[rng() % 14];
        frame.la.resize(size_t(frame.width) * frame.height * 2);
        for (size_t i = 0; i < frame.la.size(); i += 2) {
            frame.la[i] = uint8_t(rng());
            const int a = int(rng() % 8);
            frame.la[i + 1] = a < 5 ? 255 : a == 5 ? 0 : uint8_t(rng());
        }
        auto slot = layout.find(frame.width, frame.height);
        if (!slot) { slot = layout.addPage(); perPage.push_back(0); }
        const palette::Rect rect = layout.commit(*slot, frame.width, frame.height);
        sheets.push_back({frames.size(), slot->page, rect, perPage[slot->page]++, int(rng() % 3)});
        frames.push_back(std::move(frame));
    }
    std::printf("roster: %zu frames on %zu pages\n", frames.size(), layout.pages());

    // ---- baked pages ----
    std::vector<SDL_Texture*> baked;
    for (size_t p = 0; p < layout.pages(); ++p) {
        auto* t = gpuvram::create(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 2048, 2048);
        CHECK(t);
        // Unused page area: zero, what the game's never-written page memory reads on
        // this driver and what the indexed form defines for a texel no sheet owns.
        const std::vector<uint8_t> fill(size_t(2048) * 2048 * 4, 0);
        SDL_UpdateTexture(t, nullptr, fill.data(), 2048 * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        baked.push_back(t);
    }
    std::vector<uint8_t> expanded;
    for (const auto& s : sheets) {
        palette::expandSheet(frames[s.frame], banks[size_t(s.bank)], expanded);
        SDL_Rect r{s.rect.x, s.rect.y, s.rect.w, s.rect.h};
        CHECK(SDL_UpdateTexture(baked[s.page], &r, expanded.data(), s.rect.w * 4) == 0);
    }
    // ---- indexed pages (and a marked copy: outside decodes magenta) ----
    tak::PaletteIndexedGL indexed, marked;
    marked.setMarkOutside(true);
    CHECK(indexed.ready(renderer));
    if (!indexed.ready(renderer)) { std::printf("indexed unavailable: %s\n", indexed.error().c_str()); return 1; }
    CHECK(marked.ready(renderer));
    for (int b = 0; b < 3; ++b) CHECK(indexed.setBank(b, banks[size_t(b)]) && marked.setBank(b, banks[size_t(b)]));
    std::vector<SDL_Texture*> keys;
    for (size_t p = 0; p < layout.pages(); ++p) {
        CHECK(indexed.createPage() && marked.createPage());
        auto* key = gpuvram::create(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 1, 1);
        const uint8_t white[4] = {255, 255, 255, 255};
        SDL_UpdateTexture(key, nullptr, white, 4);
        SDL_SetTextureBlendMode(key, SDL_BLENDMODE_BLEND);
        keys.push_back(key);
    }
    for (const auto& s : sheets) {
        const auto& f = frames[s.frame];
        for (auto* form : {&indexed, &marked}) {
            CHECK(form->uploadEntry(s.page, s.id, palette::entryFor(s.rect, f.width, f.height, s.bank)));
            CHECK(form->upload(s.page, s.rect.x / 4, s.rect.y / 5, f, s.id));
        }
    }

    // ---- triangles, grouped by page ----
    struct Batch { std::vector<SDL_Vertex> baked, indexed; std::vector<uint32_t> sheetOf; };
    auto makeBatches = [&](int variant) {
        std::vector<Batch> batches(layout.pages());
        for (int t = 0; t < 700; ++t) {
            const uint32_t sheetIndex = uint32_t(rng() % sheets.size());
            const Sheet& s = sheets[sheetIndex];
            const auto& f = frames[s.frame];
            const int level = int(rng() % 20);
            const int bx = s.rect.x + (level % 4) * f.width, by = s.rect.y + (level / 4) * f.height;
            float scale = 0;
            switch (rng() % 4) {
                case 0: scale = 0.3f + float(rng() % 100) / 200.0f; break;   // minified
                case 1: scale = 1.0f; break;
                case 2: scale = 2.0f + float(rng() % 300) / 37.0f; break;    // magnified
                default: scale = float(rng() % 1000) / 333.0f + 0.05f; break;
            }
            const float ox = float(rng() % (W * 8)) / 8.0f - 20, oy = float(rng() % (H * 8)) / 8.0f - 20;
            SDL_Vertex v[3];
            for (int k = 0; k < 3; ++k) {
                int X, Y;
                if (variant == 0 || rng() % 2) {   // band corners (what the collector emits)
                    const int corner = int(rng() % 4);
                    X = bx + ((corner == 1 || corner == 2) ? f.width : 0);
                    Y = by + (corner >= 2 ? f.height : 0);
                } else {                            // arbitrary whole texels inside the band
                    X = bx + int(rng() % (f.width + 1));
                    Y = by + int(rng() % (f.height + 1));
                }
                const float px = ox + float(X - bx) * scale + float(rng() % 16) / 16.0f;
                const float py = oy + float(Y - by) * scale + float(rng() % 16) / 16.0f;
                const SDL_Color c{uint8_t(rng()), uint8_t(rng()), uint8_t(rng()),
                                  uint8_t(rng() % 3 ? 255 : rng())};
                v[k] = {{px, py}, c, {float(X) / 2048.0f, float(Y) / 2048.0f}};
            }
            if (rng() % 7 == 0) { v[2].position.x = v[1].position.x + 0.3f; }   // sliver
            batches[s.page].sheetOf.push_back(sheetIndex);
            for (int k = 0; k < 3; ++k) {
                batches[s.page].baked.push_back(v[k]);
                SDL_Vertex e = v[k];
                palette::encodeSheet(e.tex_coord.x, e.tex_coord.y, s.id);
                batches[s.page].indexed.push_back(e);
            }
        }
        return batches;
    };

    SDL_Texture* target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, W, H);
    SDL_Texture* plain = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 2, 2);
    const uint8_t plainPixels[16] = {200, 10, 10, 255, 10, 200, 10, 128, 10, 10, 200, 255, 250, 250, 10, 64};
    SDL_UpdateTexture(plain, nullptr, plainPixels, 8);
    int outsidePixels = 0;
    int linearHistogram[5] = {};   // max channel diff 1, 2, 3..4, 5..16, >16
    tak::GeometrySubmit submit;
    int cases = 0, linearDiffPixels = 0, linearMax = 0;
    size_t comparedPixels = 0;
    for (int variant = 0; variant < 2; ++variant)
    for (bool linear : {false, true})
    for (float scale : {1.0f, 2.0f, 4.0f})
    for (bool offscreen : {false, true})
    for (bool clipped : {false, true})
    for (int path = 0; path < 2; ++path) {
        const auto batches = makeBatches(variant);
        for (auto* t : baked) SDL_SetTextureScaleMode(t, linear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        std::array<std::vector<uint8_t>, 3> image;
        for (int form = 0; form < (linear ? 2 : 3); ++form) {
            SDL_SetRenderTarget(renderer, offscreen ? target : nullptr);
            SDL_RenderSetScale(renderer, 1, 1);
            SDL_RenderSetViewport(renderer, nullptr);
            SDL_RenderSetClipRect(renderer, nullptr);
            SDL_SetRenderDrawColor(renderer, 30, 60, 90, 255);
            SDL_RenderClear(renderer);
            SDL_RenderSetScale(renderer, scale, scale);
            const SDL_Rect clip{5, 7, 91, 63};
            SDL_RenderSetClipRect(renderer, clipped ? &clip : nullptr);
            for (size_t p = 0; p < batches.size(); ++p) {
                const auto& b = batches[p];
                if (b.baked.empty()) continue;
                tak::PaletteIndexedGL& programs = form == 2 ? marked : indexed;
                if (form == 0) {
                    CHECK(SDL_RenderGeometry(renderer, baked[p], b.baked.data(), int(b.baked.size()), nullptr, 0) == 0);
                } else if (path == 0) {
                    CHECK(submit.draw(renderer, keys[p], b.indexed, true, 0, {0, 0}, programs.program(p, linear)) == 0);
                } else {
                    // A retained GL buffer (the body arena's drawBuffer path).
                    CHECK(submit.draw(renderer, keys[p], b.indexed, true, 77 + uint64_t(p) + uint64_t(cases) * 64 + uint64_t(form) * 32,
                                      {0, 0}, programs.program(p, linear)) == 0);
                }
                // Plain SDL draws right after: leaked program, bindings, arrays.
                const SDL_FRect follow{float(10 + p * 7), 3, 6, 5};
                SDL_RenderCopyF(renderer, plain, nullptr, &follow);
                SDL_RenderCopyF(renderer, baked[p], nullptr, &follow);
                SDL_RenderGeometry(renderer, nullptr, b.baked.data(), 3, nullptr, 0);
            }
            SDL_RenderSetScale(renderer, 1, 1);
            SDL_RenderSetClipRect(renderer, nullptr);
            image[size_t(form)].resize(size_t(W) * H * 4);
            CHECK(SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGBA32, image[size_t(form)].data(), W * 4) == 0);
        }
        int differing = 0, maxDiff = 0;
        for (size_t i = 0; i < image[0].size(); i += 4) {
            int d = 0;
            for (int c = 0; c < 4; ++c) d = std::max(d, std::abs(int(image[0][i + c]) - int(image[1][i + c])));
            differing += d > 0;
            maxDiff = std::max(maxDiff, d);
        }
        comparedPixels += image[0].size() / 4;
        if (!linear) {
            // Identical everywhere, outside decodes included; the marked image shows
            // how many pixels took the owner path.
            int outside = 0;
            for (size_t i = 0; i < image[1].size(); i += 4) outside += std::memcmp(&image[1][i], &image[2][i], 4) != 0;
            outsidePixels += outside;
            if (differing) {
                int shown = 0;
                for (size_t i = 0; i < image[0].size() && shown < 6; i += 4)
                    if (std::memcmp(&image[0][i], &image[1][i], 4)) {
                        ++shown;
                        std::printf("  pixel %zu,%zu baked %d,%d,%d,%d indexed %d,%d,%d,%d%s\n", (i / 4) % W, (i / 4) / W,
                                    image[0][i], image[0][i + 1], image[0][i + 2], image[0][i + 3], image[1][i],
                                    image[1][i + 1], image[1][i + 2], image[1][i + 3],
                                    std::memcmp(&image[1][i], &image[2][i], 4) ? " (outside its sheet)" : "");
                    }
                std::printf("FAIL nearest variant=%d scale=%g target=%d clip=%d path=%d: %d pixels differ, max %d\n",
                            variant, scale, offscreen, clipped, path, differing, maxDiff);
            }
            CHECK(differing == 0);
        } else if (linear) {
            for (size_t i = 0; i < image[0].size(); i += 4) {
                int d = 0;
                for (int c = 0; c < 4; ++c) d = std::max(d, std::abs(int(image[0][i + c]) - int(image[1][i + c])));
                if (d) ++linearHistogram[d == 1 ? 0 : d == 2 ? 1 : d <= 4 ? 2 : d <= 16 ? 3 : 4];
            }
            linearDiffPixels += differing;
            linearMax = std::max(linearMax, maxDiff);
        }
        ++cases;
    }
    std::printf("nearest: identical in every case; %d pixels decoded outside their own sheet (neighbouring sheet or unused page area)\n",
                outsidePixels);
    CHECK(outsidePixels > 0);   // the owner path ran
    std::printf("linear (experimental): %d differing pixels over %d cases, max channel diff %d; by max diff 1:%d 2:%d 3-4:%d 5-16:%d >16:%d\n",
                linearDiffPixels, cases / 2, linearMax, linearHistogram[0], linearHistogram[1], linearHistogram[2],
                linearHistogram[3], linearHistogram[4]);
    std::printf("compared %zu pixels in %d cases, %llu indexed draws\n", comparedPixels, cases,
                (unsigned long long)indexed.draws());

    // ---- native pass: indexed, plain BLEND and premultiplied commands under ONE
    //      priming (GeometrySubmit::beginPass + PaletteIndexedGL pass state), client
    //      arrays and a GL buffer, against SDL drawing the same commands (any band:
    //      outside samples resolve like the baked page). ----
    {
        const SDL_BlendMode premulMode = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE,
            SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
            SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD);
        SDL_Texture* premul = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 8, 8);
        std::vector<uint8_t> pm(8 * 8 * 4);
        for (size_t i = 0; i < pm.size(); i += 4) {
            const uint8_t a = uint8_t(rng());
            pm[i + 3] = a;
            for (int c = 0; c < 3; ++c) pm[i + c] = uint8_t(rng() % (a + 1));   // premultiplied
        }
        SDL_UpdateTexture(premul, nullptr, pm.data(), 8 * 4);
        CHECK(SDL_SetTextureBlendMode(premul, premulMode) == 0);
        SDL_SetTextureBlendMode(plain, SDL_BLENDMODE_BLEND);
        auto glGenBuffers = reinterpret_cast<void (APIENTRY*)(GLsizei, GLuint*)>(SDL_GL_GetProcAddress("glGenBuffers"));
        auto glBindBuffer = reinterpret_cast<void (APIENTRY*)(GLenum, GLuint)>(SDL_GL_GetProcAddress("glBindBuffer"));
        auto glBufferData = reinterpret_cast<void (APIENTRY*)(GLenum, GLsizeiptr, const void*, GLenum)>(SDL_GL_GetProcAddress("glBufferData"));
        auto glDeleteBuffers = reinterpret_cast<void (APIENTRY*)(GLsizei, const GLuint*)>(SDL_GL_GetProcAddress("glDeleteBuffers"));
        struct Command { int kind; size_t page; std::vector<SDL_Vertex> baked, drawn; };   // 0 palette, 1 plain, 2 premul
        int passCases = 0;
        tak::NativePassGL pass;   // workstream N: the pass owner (was PaletteIndexedGL's pass state)
        CHECK(pass.ready(renderer));
        pass.setVerify(true);
        for (float scale : {1.0f, 2.0f})
        for (bool offscreen : {false, true})
        for (bool clipped : {false, true})
        for (bool buffer : {false, true}) {
            std::vector<Command> commands;
            for (int c = 0; c < 40; ++c) {
                Command cmd;
                cmd.kind = c % 3 == 0 ? 0 : int(rng() % 3);
                cmd.page = rng() % layout.pages();
                for (int t = 0; t < 6; ++t) {
                    SDL_Vertex v[3];
                    const float ox = float(rng() % (W * 4)) / 4.0f - 10, oy = float(rng() % (H * 4)) / 4.0f - 10;
                    const float sc = 0.5f + float(rng() % 600) / 100.0f;
                    if (cmd.kind == 0) {
                        const Sheet* s = nullptr;
                        do { s = &sheets[rng() % sheets.size()]; } while (s->page != cmd.page);
                        const auto& f = frames[s->frame];
                        const int col = int(rng() % 4), row = int(rng() % 5);
                        const int bx = s->rect.x + col * f.width, by = s->rect.y + row * f.height;
                        for (int k = 0; k < 3; ++k) {
                            const int corner = int(rng() % 4);
                            const int X = bx + ((corner == 1 || corner == 2) ? f.width : 0), Y = by + (corner >= 2 ? f.height : 0);
                            v[k] = {{ox + float(X - bx) * sc + float(rng() % 8) / 8.0f, oy + float(Y - by) * sc + float(rng() % 8) / 8.0f},
                                    {uint8_t(rng()), uint8_t(rng()), uint8_t(rng()), uint8_t(rng() % 2 ? 255 : rng())},
                                    {float(X) / 2048.0f, float(Y) / 2048.0f}};
                        }
                        for (int k = 0; k < 3; ++k) {
                            cmd.baked.push_back(v[k]);
                            SDL_Vertex e = v[k];
                            palette::encodeSheet(e.tex_coord.x, e.tex_coord.y, s->id);
                            cmd.drawn.push_back(e);
                        }
                    } else {
                        for (int k = 0; k < 3; ++k) {
                            v[k] = {{ox + float(rng() % 400) / 10.0f * sc, oy + float(rng() % 400) / 10.0f * sc},
                                    {uint8_t(rng()), uint8_t(rng()), uint8_t(rng()), uint8_t(rng())},
                                    {float(rng() % 1000) / 999.0f, float(rng() % 1000) / 999.0f}};
                            cmd.baked.push_back(v[k]);
                            cmd.drawn.push_back(v[k]);
                        }
                    }
                }
                commands.push_back(std::move(cmd));
            }
            // A GL buffer holding every command's drawn vertices (the arena's layout).
            std::vector<SDL_Vertex> all;
            std::vector<GLint> first;
            for (const auto& cmd : commands) { first.push_back(GLint(all.size())); all.insert(all.end(), cmd.drawn.begin(), cmd.drawn.end()); }
            GLuint vbo = 0;
            if (buffer) {
                glGenBuffers(1, &vbo);
                glBindBuffer(GL_ARRAY_BUFFER, vbo);
                glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(all.size() * sizeof(SDL_Vertex)), all.data(), GL_STATIC_DRAW);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
            }
            std::array<std::vector<uint8_t>, 2> image;
            for (int form = 0; form < 2; ++form) {
                for (auto* t : baked) SDL_SetTextureScaleMode(t, SDL_ScaleModeNearest);
                SDL_SetRenderTarget(renderer, offscreen ? target : nullptr);
                SDL_RenderSetScale(renderer, 1, 1);
                SDL_RenderSetClipRect(renderer, nullptr);
                SDL_SetRenderDrawColor(renderer, 30, 60, 90, 255);
                SDL_RenderClear(renderer);
                SDL_RenderSetScale(renderer, scale, scale);
                const SDL_Rect clip{9, 4, 87, 71};
                SDL_RenderSetClipRect(renderer, clipped ? &clip : nullptr);
                const SDL_FRect before{2, 2, 9, 9};
                SDL_RenderCopyF(renderer, premul, nullptr, &before);   // queued SDL work ahead of the pass
                auto texOf = [&](const Command& cmd) {
                    return cmd.kind == 0 ? (form == 0 ? baked[cmd.page] : keys[cmd.page]) : cmd.kind == 1 ? plain : premul;
                };
                if (form == 0) {
                    for (const auto& cmd : commands)
                        SDL_RenderGeometry(renderer, texOf(cmd), cmd.baked.data(), int(cmd.baked.size()), nullptr, 0);
                } else {
                    const GLuint plainName = submit.textureName(renderer, plain), premulName = submit.textureName(renderer, premul);
                    CHECK(plainName && premulName);
                    CHECK(pass.begin(submit, renderer, keys[commands[0].page], false));
                    for (size_t c = 0; c < commands.size(); ++c) {
                        const auto& cmd = commands[c];
                        if (cmd.kind == 0) {
                            GLuint program = 0, textures[3] = {0, 0, 0};
                            CHECK(indexed.passBinding(cmd.page, false, program, textures));
                            pass.useProgram(program, textures);
                        } else pass.useTexture(cmd.kind == 1 ? plainName : premulName, cmd.kind == 2);
                        if (buffer) {
                            const GLsizei count = GLsizei(cmd.drawn.size());
                            submit.passDrawBuffer(vbo, std::span(&first[c], 1), std::span(&count, 1), cmd.drawn.size());
                        } else submit.passDraw(cmd.drawn);
                    }
                    pass.end(submit);
                }
                // SDL draws after the pass: leaked program, bindings, blend, arrays, buffer.
                const SDL_FRect after1{40, 30, 13, 11}, after2{60, 50, 17, 9};
                SDL_RenderCopyF(renderer, premul, nullptr, &after1);
                SDL_RenderCopyF(renderer, plain, nullptr, &after2);
                SDL_RenderCopyF(renderer, baked[0], nullptr, &after2);
                SDL_RenderGeometry(renderer, nullptr, commands[1].baked.data(), 3, nullptr, 0);
                SDL_RenderSetScale(renderer, 1, 1);
                SDL_RenderSetClipRect(renderer, nullptr);
                image[size_t(form)].resize(size_t(W) * H * 4);
                CHECK(SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGBA32, image[size_t(form)].data(), W * 4) == 0);
            }
            if (vbo) glDeleteBuffers(1, &vbo);
            int differing = 0;
            for (size_t i = 0; i < image[0].size(); i += 4)
                if (std::memcmp(&image[0][i], &image[1][i], 4) != 0) {
                    if (++differing <= 4)
                        std::printf("  pixel %zu,%zu sdl %d,%d,%d,%d pass %d,%d,%d,%d\n", (i / 4) % W, (i / 4) / W,
                                    image[0][i], image[0][i + 1], image[0][i + 2], image[0][i + 3],
                                    image[1][i], image[1][i + 1], image[1][i + 2], image[1][i + 3]);
                }
            if (differing) std::printf("FAIL pass scale=%g target=%d clip=%d buffer=%d: %d pixels differ\n",
                                       scale, offscreen, clipped, buffer, differing);
            CHECK(differing == 0);
            ++passCases;
        }
        CHECK(pass.verifyFailures() == 0);
        if (pass.verifyFailures()) std::printf("  pass state: %s\n", pass.lastFailure().c_str());
        CHECK(pass.verified() == uint64_t(passCases));
        pass.release();
        SDL_DestroyTexture(premul);
        std::printf("pass: %d cases identical (palette + plain + premultiplied commands, one priming each)\n", passCases);
    }

    // ---- release / rebuild, budget refusal, accounting ----
    const size_t indexedBytes = indexed.gpuBytes();
    CHECK(indexedBytes == layout.pages() * palette::kIndexPageBytes + 2048 * 4 + 256 * 256 * 4);
    marked.release();
    indexed.release();
    CHECK(!indexed.usable());
    CHECK(indexed.ready(renderer));
    CHECK(indexed.createPage());
    indexed.releasePages();
    CHECK(indexed.pages() == 0);
    // Budget refusal: held bytes at the cap (setLimit floors at 384 MiB).
    gpuvram::setLimit(gpuvram::kFloor);
    const size_t pad = gpuvram::kFloor - std::min(gpuvram::bytes(), gpuvram::kFloor) + palette::kIndexPageBytes / 2;
    gpuvram::addBuffer(pad);
    CHECK(!gpuvram::wouldFit(palette::kIndexPageBytes));
    CHECK(!indexed.createPage());
    CHECK(indexed.pages() == 0);
    gpuvram::removeBuffer(pad);
    gpuvram::setLimit(gpuvram::kUnlimited);
    indexed.release();
    submit.clear();
    for (auto* t : baked) gpuvram::destroy(t);
    for (auto* t : keys) gpuvram::destroy(t);
    CHECK(gpuvram::bytes() == baseline);
    SDL_DestroyTexture(target);
    SDL_DestroyTexture(plain);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("PASS: palette indexed\n");
    return 0;
}
