#include <map>
#include "client/modelview.h"

#include "util/strcase.h"

#include "client/gpuvram.h"
#include "gaf/gaf.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

ModelView::ModelView(SDL_Renderer* ren, const std::string& path, const std::string& texDir,
                     const std::string& palettePath, const std::string& cobPath,
                     const std::string& anim, uint32_t staticMask)
    : ren_(ren), model_(ta::tdo::load(path)) {
    if (!texDir.empty() && !palettePath.empty()) loadTextures(texDir, palettePath);
    if (!cobPath.empty() && !anim.empty()) {
        vm_ = std::make_unique<ta::cob::Vm>(ta::cob::load(cobPath));
        for (int i = 0; i < 32; ++i)
            if (staticMask & (1u << i)) vm_->setStatic(i, 1);
        // Run Create FIRST, exactly as the game does when a unit enters play: it is
        // the one-shot setup script that hides the pieces a unit isn't using yet
        // (muzzle flares, alternate weapons, corpse geometry). Skipping it leaves
        // those pieces floating beside the model -- which looks like broken geometry
        // and isn't.
        if (vm_->start("Create")) {
            for (int i = 0; i < 30; ++i) vm_->tick(1.0f / 30.0f);
        }
        if (!vm_->start(anim))
            std::fprintf(stderr, "no script '%s' in %s\n", anim.c_str(),
                         cobPath.c_str());
        // Map piece numbers to lowercase object names.
        for (const auto& p : vm_->file().pieces) {
            std::string n = p;
            std::transform(n.begin(), n.end(), n.begin(), ::tolower);
            pieceNames_.push_back(n);
        }
    }
}

void ModelView::input(const SDL_Event& e) {
    if (e.type == SDL_MOUSEMOTION && (e.motion.state & SDL_BUTTON_LMASK)) {
        yaw_ += e.motion.xrel * 0.01f;
        pitch_ = std::clamp(pitch_ + e.motion.yrel * 0.01f, -1.4f, 1.4f);
        spin_ = false;
    } else if (e.type == SDL_MOUSEWHEEL) {
        zoom_ *= e.wheel.y > 0 ? 1.15f : 0.87f;
    }
}

void ModelView::draw(int winW, int winH, float dt) {
    if (spin_) yaw_ += dt * 0.8f;
    if (vm_) vm_->tick(dt);

    tris_.clear();
    walk(model_.root, Xform{});
    if (tris_.empty()) return;

    // Center and fit every frame (cheap, and stays correct as it spins).
    float lox = 1e9f, hix = -1e9f, loy = 1e9f, hiy = -1e9f;
    for (auto& t : tris_)
        for (auto& v : t.v) {
            lox = std::min(lox, v.position.x); hix = std::max(hix, v.position.x);
            loy = std::min(loy, v.position.y); hiy = std::max(hiy, v.position.y);
        }
    if (!fitted_) {
        fit_ = 0.8f * std::min(winW, winH) /
               std::max({hix - lox, hiy - loy, 1e-3f});
        fitted_ = true;
    }
    std::stable_sort(tris_.begin(), tris_.end(),
              [](const Tri& a, const Tri& b) { return a.depth > b.depth; });
    float s = fit_ * zoom_;
    float cx = (lox + hix) / 2, cy = (loy + hiy) / 2;
    for (auto& t : tris_) {
        SDL_Vertex v[3];
        for (int i = 0; i < 3; ++i) {
            v[i] = t.v[i];
            v[i].position.x = (v[i].position.x - cx) * s + winW / 2.0f;
            v[i].position.y = (v[i].position.y - cy) * s + winH / 2.0f;
        }
        SDL_RenderGeometry(ren_, t.tex, v, 3, nullptr, 0);
    }
}

void ModelView::loadTextures(const std::string& texDir, const std::string& palettePath) {
    auto fallback = ta::gaf::Palette::load(palettePath);
    // Each faction's texture bank has its OWN palette (palettes/<side>_textures.pcx),
    // exactly as the game loads them (GameView::loadTextures). Decoding every bank
    // against one palette is what made models in this viewer come out speckled --
    // the geometry was fine, the colours were being read from the wrong table.
    std::map<std::string, ta::gaf::Palette> banks;
    std::filesystem::path palDir = std::filesystem::path(palettePath).parent_path();
    for (const char* side : {"ara", "tar", "ver", "zon", "aid", "cre", "mon", "npc", "lif", "mis"}) {
        std::filesystem::path pp = palDir / (std::string(side) + "_textures.pcx");
        try { if (std::filesystem::exists(pp)) banks[side] = ta::gaf::Palette::load(pp.string()); }
        catch (const std::exception&) {}
    }
    for (const auto& e : std::filesystem::directory_iterator(texDir)) {
        // Case-insensitive: TA ships .GAF in upper case, and a case-sensitive
        // test here loaded zero textures and rendered every model flat grey.
        if (!ta::iendsWith(e.path().filename().string(), ".gaf")) continue;
        std::string stem = e.path().stem().string();
        std::transform(stem.begin(), stem.end(), stem.begin(), ::tolower);
        const ta::gaf::Palette* pal = &fallback;
        if (auto it = banks.find(stem.substr(0, 3)); it != banks.end()) pal = &it->second;
        try {
            for (auto& seq : ta::gaf::load(e.path(), *pal, 5)) {
                if (seq.frames.empty()) continue;
                auto& f = seq.frames[0];
                if (f.width == 0 || f.height == 0) continue;
                std::string name = seq.name;
                std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                if (textures_.count(name)) continue;
                SDL_Texture* t = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32,
                                                   SDL_TEXTUREACCESS_STATIC,
                                                   f.width, f.height);
                SDL_UpdateTexture(t, nullptr, f.rgba.data(), f.width * 4);
                textures_[name] = t;
            }
        } catch (const std::exception&) { /* skip odd banks */ }
    }
    std::printf("loaded %zu textures\n", textures_.size());
}

void ModelView::project(float x, float y, float z, SDL_FPoint& out, float& depth) const {
    float cx = std::cos(yaw_), sx = std::sin(yaw_);
    float rx = x * cx + z * sx;
    float rz = -x * sx + z * cx;
    float cy = std::cos(pitch_), sy = std::sin(pitch_);
    float ry = y * cy - rz * sy;
    depth = rz * cy + y * sy;
    out = {rx, -ry};
}

const ta::cob::PieceState* ModelView::pieceFor(const std::string& objName) const {
    if (!vm_) return nullptr;
    std::string n = objName;
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    for (size_t i = 0; i < pieceNames_.size(); ++i)
        if (pieceNames_[i] == n) return &vm_->pieces()[i];
    return nullptr;
}

void ModelView::walk(const ta::tdo::Object& o, const Xform& parent) {
    const ta::cob::PieceState* ps = pieceFor(o.name);
    if (ps && !ps->visible) return;
    float rr[3];
    Xform xf = parent.then(o.x + (ps ? ps->move[0] : 0),
                           o.y + (ps ? ps->move[1] : 0),
                           o.z + (ps ? ps->move[2] : 0),
                           scriptRot(ps, rr));
    for (const auto& p : o.primitives) {
        if (p.indices.size() < 3) continue;
        SDL_Texture* tex = nullptr;
        if (!p.texture.empty()) {
            std::string name = p.texture;
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            auto it = textures_.find(name);
            if (it != textures_.end()) tex = it->second;
        }
        // Fan-triangulate; quads get proper corner UVs.
        for (size_t i = 1; i + 1 < p.indices.size(); ++i) {
            size_t idx[3] = {0, i, i + 1};
            Tri tri{};
            tri.tex = tex;
            float depth = 0;
            for (int k = 0; k < 3; ++k) {
                size_t vi = size_t(p.indices[idx[k]]) * 3;
                if (vi + 2 >= o.vertices.size()) { tri.tex = nullptr; break; }
                float w[3], d;
                xf.apply(o.vertices[vi], o.vertices[vi + 1], o.vertices[vi + 2], w);
                project(w[0], w[1], w[2], tri.v[k].position, d);
                depth += d;
                static const SDL_FPoint uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
                tri.v[k].tex_coord = uv[idx[k] & 3];
                tri.v[k].color = tex ? SDL_Color{255, 255, 255, 255}
                                     : SDL_Color{170, 170, 180, 255};
            }
            tri.depth = depth / 3;
            tris_.push_back(tri);
        }
    }
    for (const auto& c : o.children) walk(c, xf);
}

void ModelView::advance(float seconds) {
    // --time also PINS the camera. The viewer spins by default, and the spin is
    // driven by real frame dt, so two captures at different sim times differed by
    // yaw drift as much as by the animation -- which makes an automated
    // "did this animation actually move anything" diff meaningless. A timed capture
    // is a measurement, so it gets a fixed camera.
    spin_ = false;
    if (!vm_) return;
    for (float t = 0; t < seconds; t += 1.0f / 30.0f) vm_->tick(1.0f / 30.0f);
}
