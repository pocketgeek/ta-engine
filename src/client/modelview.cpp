#include <map>
#include "client/modelview.h"
#include "client/modelpiece.h"
#include <cctype>
#include <stdexcept>

#include "client/gpuvram.h"
#include "gaf/gaf.h"
#include "util/virtualpath.h"
#include <set>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

void ModelView::TextureDeleter::operator()(SDL_Texture* texture) const {
    gpuvram::destroy(texture);
}

ModelView::ModelView(SDL_Renderer* ren, const std::string& path, const std::string& texDir,
                     const std::string& palettePath, const std::string& cobPath,
                     const std::string& anim, uint32_t staticMask)
    : ren_(ren), model_(tak::tdo::load(path)) {
    if (!texDir.empty() && !palettePath.empty()) loadTextures(texDir, palettePath);
    if (!cobPath.empty() && !anim.empty()) {
        initializeScript(tak::cob::load(cobPath),anim,staticMask);
    }
}

void ModelView::initializeScript(tak::cob::File file,const std::string& anim,uint32_t staticMask) {
        vm_ = std::make_unique<tak::cob::Vm>(std::move(file));
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
        if (!anim.empty() && !vm_->start(anim))
            std::fprintf(stderr, "no script '%s' in model preview\n", anim.c_str());
        // Map piece numbers to lowercase object names.
        for (const auto& p : vm_->file().pieces) {
            std::string n = p;
            std::transform(n.begin(), n.end(), n.begin(), ::tolower);
            pieceNames_.push_back(n);
        }
}

ModelView::Asset ModelView::loadAsset(const tak::hpi::Vfs& vfs,const std::string& type,const std::atomic_bool* cancel) {
    auto check=[&] {if(cancel && cancel->load())throw std::runtime_error("Preview cancelled");};
    auto lower=[](std::string s) {for(char& c:s)c=char(std::tolower(static_cast<unsigned char>(c)));return s;};
    check();const auto id=lower(type);Asset result;
    result.model=tak::tdo::load(vfs.read("objects3d/"+id+".3do"));
    if(const auto bytes=vfs.tryRead("scripts/"+id+".cob"))result.script=tak::cob::load(*bytes,id);
    std::map<std::string,tak::gaf::Palette> palettes;
    for(const char* side:{"ara","tar","ver","zon","cre","aid","mon","npc","lif","mis"}) {
        const auto path="palettes/"+std::string(side)+"_textures.pcx";
        if(const auto bytes=vfs.tryRead(path))palettes.emplace(side,tak::gaf::Palette::fromBytes(*bytes,path));
    }
    if(!palettes.count("ara"))throw std::runtime_error("Missing model texture palette");
    result.palette=palettes.count(id.substr(0,3))?palettes.at(id.substr(0,3)):palettes.at("ara");
    std::set<std::string> needed;
    const auto gather=[&](auto&& self,const tak::tdo::Object& object,bool root)->void {
        if(!skipLiveModelPiece(lower(object.name),root))for(size_t i=0;i<object.primitives.size();++i) {
            const auto& primitive=object.primitives[i];
            if(int32_t(i)!=object.selectionPrimitive && primitive.indices.size()>=3 && !primitive.texture.empty())needed.insert(lower(primitive.texture));
        }
        for(const auto& child:object.children)self(self,child,false);
    };
    gather(gather,result.model.root,true);
    for(const auto& path:vfs.list("textures")) {
        check();if(needed.empty())break;
        if(tak::vpath::extension(path)!=".gaf")continue;
        const auto bank=lower(tak::vpath::stem(path)).substr(0,3);
        const auto& palette=palettes.count(bank)?palettes.at(bank):palettes.at("ara");
        for(auto& sequence:tak::gaf::load(vfs.read(path),palette,5,path)) {
            const auto name=lower(sequence.name);
            if(!sequence.frames.empty() && needed.erase(name))result.textures.emplace(name,std::move(sequence.frames.front()));
        }
    }
    result.missingTextures.assign(needed.begin(),needed.end());
    check();return result;
}

ModelView::ModelView(SDL_Renderer* renderer,Asset asset):ren_(renderer),model_(std::move(asset.model)),palette_(asset.palette) {
    liveModel_=true;
    for(auto& [name,frame]:asset.textures) {
        if(frame.width<=0 || frame.height<=0)continue;
        std::unique_ptr<SDL_Texture,TextureDeleter> texture(gpuvram::create(ren_,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,frame.width,frame.height));
        if(!texture || SDL_UpdateTexture(texture.get(),nullptr,frame.rgba.data(),frame.width*4)!=0)throw std::runtime_error(SDL_GetError());
        SDL_SetTextureBlendMode(texture.get(),SDL_BLENDMODE_BLEND);
        textures_.emplace(name,std::move(texture));
    }
    // Preview initialization hides unused pieces. This is a visual inspector,
    // not a second gameplay world or a claim of complete unit behavior playback.
    if(asset.script)initializeScript(std::move(*asset.script),"",0);
}

void ModelView::input(const SDL_Event& e) {
    if (e.type == SDL_MOUSEMOTION && (e.motion.state & SDL_BUTTON_LMASK)) {
        yaw_ += e.motion.xrel * 0.01f;
        pitch_ = std::clamp(pitch_ + e.motion.yrel * 0.01f, -1.4f, 1.4f);
        spin_ = false;
    } else if (e.type == SDL_MOUSEWHEEL) {
        if(e.wheel.y)zoom_=std::clamp(zoom_*(e.wheel.y>0?1.15f:0.87f),0.1f,10.f);
    }
}

void ModelView::draw(int winW, int winH, float dt) {
    if(winW!=lastW_ || winH!=lastH_) {fitted_=false;lastW_=winW;lastH_=winH;}
    if (spin_) yaw_ += dt * 0.8f;
    if (vm_) vm_->tick(dt);

    tris_.clear();
    walk(model_.root, Xform{},true);
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
    auto fallback = tak::gaf::Palette::load(palettePath);
    // Each faction's texture bank has its OWN palette (palettes/<side>_textures.pcx),
    // exactly as the game loads them (GameView::loadTextures). Decoding every bank
    // against one palette is what made models in this viewer come out speckled --
    // the geometry was fine, the colours were being read from the wrong table.
    std::map<std::string, tak::gaf::Palette> banks;
    std::filesystem::path palDir = std::filesystem::path(palettePath).parent_path();
    for (const char* side : {"ara", "tar", "ver", "zon", "aid", "cre", "mon", "npc", "lif", "mis"}) {
        std::filesystem::path pp = palDir / (std::string(side) + "_textures.pcx");
        try { if (std::filesystem::exists(pp)) banks[side] = tak::gaf::Palette::load(pp.string()); }
        catch (const std::exception&) {}
    }
    for (const auto& e : std::filesystem::directory_iterator(texDir)) {
        if (e.path().extension() != ".gaf") continue;
        std::string stem = e.path().stem().string();
        std::transform(stem.begin(), stem.end(), stem.begin(), ::tolower);
        const tak::gaf::Palette* pal = &fallback;
        if (auto it = banks.find(stem.substr(0, 3)); it != banks.end()) pal = &it->second;
        try {
            for (auto& seq : tak::gaf::load(e.path(), *pal, 5)) {
                if (seq.frames.empty()) continue;
                auto& f = seq.frames[0];
                if (f.width == 0 || f.height == 0) continue;
                std::string name = seq.name;
                std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                if (textures_.count(name)) continue;
                std::unique_ptr<SDL_Texture,TextureDeleter> t(gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32,
                                                   SDL_TEXTUREACCESS_STATIC,
                                                   f.width, f.height));
                if(t) SDL_UpdateTexture(t.get(), nullptr, f.rgba.data(), f.width * 4);
                textures_[name] = std::move(t);
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

const tak::cob::PieceState* ModelView::pieceFor(const std::string& objName) const {
    if (!vm_) return nullptr;
    std::string n = objName;
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    for (size_t i = 0; i < pieceNames_.size(); ++i)
        if (pieceNames_[i] == n) return &vm_->pieces()[i];
    return nullptr;
}

void ModelView::walk(const tak::tdo::Object& o, const Xform& parent,bool root) {
    const tak::cob::PieceState* ps = pieceFor(o.name);
    Xform xf = scriptTransform(parent,o.x,o.y,o.z,ps);
    std::string name=o.name;for(char& c:name)c=char(std::tolower(static_cast<unsigned char>(c)));
    const bool hidden=(ps && !ps->visible) || (liveModel_ && skipLiveModelPiece(name,root));
    for (size_t pi=0;!hidden && pi<o.primitives.size();++pi) {
        if(liveModel_ && int32_t(pi)==o.selectionPrimitive)continue;
        const auto& p=o.primitives[pi];
        if (p.indices.size() < 3) continue;
        SDL_Texture* tex = nullptr;
        if (!p.texture.empty()) {
            std::string name = p.texture;
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            auto it = textures_.find(name);
            if (it != textures_.end()) tex = it->second.get();
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
                tri.v[k].color = tex ? SDL_Color{255,255,255,255} : SDL_Color{170,170,180,255};
                if(!tex && palette_ && !p.texture.empty())tri.v[k].color={255,0,255,255};
                else if(!tex && palette_ && p.colorIndex<256) {
                    const auto* c=palette_->rgba[p.colorIndex];tri.v[k].color={c[0],c[1],c[2],255};
                }
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
