#include "client/shadowmask.h"
#include "client/shadowopaque.h"
#include <SDL.h>
#include <array>
#include <cstdio>
#include <cstdlib>

// Compare atlas sampling against standalone masks at subpixel positions and
// both filtering modes. Poison pixels around the padded slot detect bleed.
static bool atlasSamplingParity(SDL_Renderer* renderer) {
    constexpr int width=4,height=3,pageSide=32;
    const std::array<uint8_t,12> alpha={0,255,128,0,255,64,192,255,0,255,0,128};
    std::array<uint8_t,width*height*4> rgba{};
    for(size_t i=0;i<alpha.size();++i)rgba[i*4+3]=alpha[i];
    const auto pixels=tak::shadowMaskPixels(rgba,140);
    const auto padded=tak::paddedShadowMaskPixels(rgba,width,height,140);
    auto* single=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,width,height);
    auto* atlas=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,pageSide,pageSide);
    if(!single || !atlas) {SDL_DestroyTexture(single);SDL_DestroyTexture(atlas);return false;}
    SDL_UpdateTexture(single,nullptr,pixels.data(),width*4);
    std::array<uint8_t,pageSide*pageSide*4> poison{};
    for(size_t i=0;i<poison.size();i+=4){poison[i]=255;poison[i+3]=255;}
    SDL_UpdateTexture(atlas,nullptr,poison.data(),pageSide*4);
    const SDL_Rect slot{5,7,width+2,height+2};
    SDL_UpdateTexture(atlas,&slot,padded.data(),(width+2)*4);
    SDL_SetTextureBlendMode(single,SDL_BLENDMODE_MOD);SDL_SetTextureBlendMode(atlas,SDL_BLENDMODE_MOD);
    const SDL_Color white{255,255,255,255};
    const std::array<SDL_Vertex,9> original={{{{1.25f,1.75f},white,{0,0}},{{22.75f,2.25f},white,{1,0}},
        {{22.25f,14.75f},white,{1,1}},{{1.25f,1.75f},white,{0,0}},{{22.25f,14.75f},white,{1,1}},
        {{0.75f,14.25f},white,{0,1}},{{3.25f,3.75f},white,{0,0}},{{21.25f,5.25f},white,{1,0}},
        {{12.75f,15.25f},white,{1,1}}}};
    std::array<uint8_t,24*16*4> expected{},actual{};
    bool ok=true;
    for(auto filtering:{SDL_ScaleModeNearest,SDL_ScaleModeLinear}) {
        SDL_SetTextureScaleMode(single,filtering);SDL_SetTextureScaleMode(atlas,filtering);
        for(int packed=0;packed<2;++packed) {
            auto vertices=original;
            if(packed)for(auto& v:vertices) {
                v.tex_coord.x=(slot.x+1+v.tex_coord.x*width)/pageSide;
                v.tex_coord.y=(slot.y+1+v.tex_coord.y*height)/pageSide;
            }
            SDL_SetRenderDrawColor(renderer,200,160,120,255);SDL_RenderClear(renderer);
            ok &= SDL_RenderGeometry(renderer,packed ? atlas : single,vertices.data(),int(vertices.size()),nullptr,0)==0;
            const SDL_Rect area{0,0,24,16};
            ok &= SDL_RenderReadPixels(renderer,&area,SDL_PIXELFORMAT_RGBA32,
                packed ? actual.data() : expected.data(),24*4)==0;
        }
        int worst=0;
        for(size_t i=0;i<actual.size();++i)worst=std::max(worst,std::abs(int(actual[i])-int(expected[i])));
        if(worst>(filtering==SDL_ScaleModeNearest ? 0 : 1)) {
            std::fprintf(stderr,"atlas sampling mismatch filter=%d maxdelta=%d\n",int(filtering),worst);ok=false;
        }
    }
    SDL_DestroyTexture(single);SDL_DestroyTexture(atlas);return ok;
}

// The unit coverage target is composited once: duplicate opaque faces cannot
// darken their own unit twice, while separate units remain separate shadows.
static bool unitCoverageParity(SDL_Renderer* renderer) {
    const std::array<uint8_t,12> rgba={90,20,30,0, 40,50,60,255, 70,80,90,128};
    const auto pixels=tak::shadowCoveragePixels(rgba);
    auto* cutout=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,3,1);
    if(!cutout)return false;
    SDL_UpdateTexture(cutout,nullptr,pixels.data(),12);
    SDL_SetTextureBlendMode(cutout,SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(cutout,SDL_ScaleModeNearest);
    SDL_Texture* previous=SDL_GetRenderTarget(renderer);
    bool ok=true;
    for(int scale:{1,2}) {
        auto* coverage=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_TARGET,24*scale,16*scale);
        if(!coverage){ok=false;break;}
        SDL_SetTextureBlendMode(coverage,SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(coverage,115);
        SDL_SetTextureScaleMode(coverage,SDL_ScaleModeLinear);
        std::array<uint8_t,24*16*4> single{},duplicate{},twoUnits{};
        for(int trial=0;trial<3;++trial) {
            ok &= SDL_SetRenderTarget(renderer,coverage)==0;
            SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(renderer,0,0,0,0);SDL_RenderClear(renderer);
            const SDL_Color white{255,255,255,255};
            std::array<SDL_Vertex,6> quad={{{{0.25f,0.25f},white,{0,0}},{{23.75f,0.25f},white,{1,0}},
                {{23.75f,15.75f},white,{1,1}},{{0.25f,0.25f},white,{0,0}},
                {{23.75f,15.75f},white,{1,1}},{{0.25f,15.75f},white,{0,1}}}};
            for(auto& vertex:quad){vertex.position.x*=scale;vertex.position.y*=scale;}
            // Only duplicate the opaque middle third; fractional alpha at the
            // mask edge uses ordinary alpha-over, as in production filtering.
            ok &= SDL_RenderGeometry(renderer,cutout,quad.data(),int(quad.size()),nullptr,0)==0;
            if(trial==1) {
                SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer,0,0,0,255);
                const SDL_FRect interior{10.f*scale,4.f*scale,4.f*scale,8.f*scale};
                ok &= SDL_RenderFillRectF(renderer,&interior)==0;
            }
            ok &= SDL_SetRenderTarget(renderer,previous)==0;
            SDL_SetRenderDrawColor(renderer,200,160,120,255);SDL_RenderClear(renderer);
            const SDL_FRect dest{0,0,24,16};
            for(int i=0;i<(trial==2 ? 2 : 1);++i)
                ok &= SDL_RenderCopyF(renderer,coverage,nullptr,&dest)==0;
            const SDL_Rect area{0,0,24,16};
            auto& output=trial==0 ? single : trial==1 ? duplicate : twoUnits;
            ok &= SDL_RenderReadPixels(renderer,&area,SDL_PIXELFORMAT_RGBA32,output.data(),24*4)==0;
        }
        ok &= single==duplicate;
        for(int c=0;c<3;++c) {
            const int background=200-40*c;
            const size_t empty=(8*24+2)*4+c,full=(8*24+12)*4+c,partial=(8*24+20)*4+c;
            ok &= single[empty]==background && twoUnits[empty]==background;
            ok &= std::abs(int(single[full])-background*140/255)<=1;
            ok &= std::abs(int(twoUnits[full])-int(single[full])*140/255)<=1;
            ok &= single[partial]>single[full] && single[partial]<background;
        }
        SDL_DestroyTexture(coverage);
    }
    SDL_SetRenderTarget(renderer,previous);SDL_DestroyTexture(cutout);
    std::printf("%s: unit coverage unions faces and composites distinct units at 1x/2x AA\n",ok?"PASS":"FAIL");
    return ok;
}

// Compare native position-only submission with SDL and then draw textured and
// coloured SDL geometry to detect leaked client-array/current-colour state.
static bool opaqueSubmissionParity(SDL_Renderer* renderer) {
    auto* target=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_TARGET,64,64);
    auto* texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,1,1);
    if (!target || !texture) {SDL_DestroyTexture(target);SDL_DestroyTexture(texture);return false;}
    const std::array<uint8_t,4> texel={40,120,220,128};
    SDL_UpdateTexture(texture,nullptr,texel.data(),4);
    SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
    const SDL_Color white{255,255,255,255};
    const SDL_Vertex textured[3]={{{1,1},white,{0,0}},{{9,1},white,{1,0}},{{1,9},white,{0,1}}};
    const SDL_Vertex colored[3]={{{12,1},{200,30,80,255},{0,0}},
        {{20,1},{20,230,60,255},{0,0}},{{12,9},{80,90,250,255},{0,0}}};
    std::array<uint8_t,64*64*4> expected{},actual{};
    tak::OpaqueShadowSubmit submit;
    bool ok=true;
    for (float scale:{1.f,2.f}) for (float aa:{1.f,2.f})
        for (float offset:{0.f,0.125f,0.5f}) for (bool clipped:{false,true}) {
        SDL_FPoint points[9]={{2+offset,3+offset},{24+offset,4+offset},{9+offset,25+offset},
            {2+offset,3+offset},{24+offset,4+offset},{9+offset,25+offset},
            {4+offset,8+offset},{24+offset,27+offset},{1+offset,23+offset}};
        for (auto& point:points) {point.x*=aa;point.y*=aa;}
        for (bool native:{false,true}) {
            ok &= SDL_SetRenderTarget(renderer,target)==0;
            SDL_RenderSetScale(renderer,1,1);SDL_RenderSetViewport(renderer,nullptr);SDL_RenderSetClipRect(renderer,nullptr);
            SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(renderer,190,160,130,255);SDL_RenderClear(renderer);
            SDL_RenderSetScale(renderer,scale,scale);
            SDL_Rect viewport{1,2,int(60/scale),int(60/scale)},clip{3,4,20,21};
            SDL_RenderSetViewport(renderer,&viewport);SDL_RenderSetClipRect(renderer,clipped?&clip:nullptr);
            // Prime with a textured batch before switching to opaque coverage.
            SDL_RenderGeometry(renderer,texture,textured,3,nullptr,0);
            ok &= submit.draw(renderer,points,native)==0;
            SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_BLEND);
            SDL_RenderGeometry(renderer,nullptr,colored,3,nullptr,0);
            SDL_RenderGeometry(renderer,texture,textured,3,nullptr,0);
            SDL_RenderSetScale(renderer,1,1);SDL_RenderSetViewport(renderer,nullptr);SDL_RenderSetClipRect(renderer,nullptr);
            ok &= SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,
                native?actual.data():expected.data(),64*4)==0;
        }
        if (expected!=actual) {
            std::printf("FAIL: opaque submission scale=%.1f offset=%.3f clipped=%d\n",scale,offset,clipped);
            ok=false;
        }
    }
    SDL_RendererInfo info{};SDL_GetRendererInfo(renderer,&info);
    if (std::strcmp(info.name,"opengl")==0 && !submit.active()) {
        std::puts("FAIL: OpenGL opaque submission was not exercised");ok=false;
    }
    SDL_SetRenderTarget(renderer,nullptr);SDL_RenderSetScale(renderer,1,1);
    SDL_RenderSetViewport(renderer,nullptr);SDL_RenderSetClipRect(renderer,nullptr);
    SDL_DestroyTexture(target);SDL_DestroyTexture(texture);
    std::printf("%s: opaque shadow submission pixels and following SDL draws on %s\n",ok?"PASS":"FAIL",info.name);
    return ok;
}

// Combining adjacent mask triangles must preserve overlap darkening and the
// transparent/partially covered texels, not just their projected positions.
static bool maskedRunBatchParity() {
    const bool accelerated=std::getenv("TAK_SHADOW_TEST_ACCELERATED")!=nullptr;
    SDL_Surface* surface=nullptr;SDL_Window* window=nullptr;SDL_Renderer* renderer=nullptr;
    if (accelerated) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO)!=0) return false;
        window=SDL_CreateWindow("shadow batching test",SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,
                                24,16,SDL_WINDOW_HIDDEN);
        if (window) renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_ACCELERATED);
    } else {
        surface=SDL_CreateRGBSurfaceWithFormat(0,24,16,32,SDL_PIXELFORMAT_RGBA32);
        if (surface) renderer=SDL_CreateSoftwareRenderer(surface);
    }
    if (!renderer) {std::fprintf(stderr,"shadow renderer: %s\n",SDL_GetError());
        SDL_DestroyWindow(window);SDL_FreeSurface(surface);return false;}
    SDL_RendererInfo info{};SDL_GetRendererInfo(renderer,&info);
    const bool batch=(info.flags&SDL_RENDERER_ACCELERATED)!=0;
    const std::array<uint8_t,12> rgba={200,0,0,0,0,200,0,255,0,0,200,128};
    const auto pixels=tak::shadowMaskPixels(rgba,140);
    auto* texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,3,1);
    if (!texture) {SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_FreeSurface(surface);return false;}
    SDL_UpdateTexture(texture,nullptr,pixels.data(),12);
    SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_MOD);
    const SDL_Color white{255,255,255,255};
    const std::array<SDL_Vertex,9> vertices={{{{0,0},white,{0,0}},{{24,0},white,{1,0}},
        {{24,12},white,{1,1}},{{0,0},white,{0,0}},{{24,12},white,{1,1}},
        {{0,12},white,{0,1}},{{3,3},white,{0,0}},{{22,5},white,{1,0}},{{12,16},white,{1,1}}}};
    std::array<uint8_t,24*16*4> separate{},batched{};
    bool ok=true;
    for (int mode=0;mode<2;++mode) {
        SDL_SetRenderDrawColor(renderer,200,160,120,255);SDL_RenderClear(renderer);
        if (mode) ok &= tak::submitShadowMaskRun(renderer,texture,vertices.data(),int(vertices.size()),batch)==0;
        else for (int offset=0;offset<int(vertices.size());offset+=3)
            ok &= SDL_RenderGeometry(renderer,texture,vertices.data()+offset,3,nullptr,0)==0;
        ok &= SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,
            mode ? batched.data() : separate.data(),24*4)==0;
    }
    ok &= separate==batched;
    ok &= opaqueSubmissionParity(renderer);
    if(batch) {
        ok &= unitCoverageParity(renderer);
        const bool atlasOK=atlasSamplingParity(renderer);
        std::printf("%s: padded shadow atlas matches standalone sampling on %s\n",atlasOK?"PASS":"FAIL",info.name);
        ok &= atlasOK;
    }
    std::printf("%s: ordered mask run matches separate triangles on %s\n",ok?"PASS":"FAIL",info.name);
    SDL_DestroyTexture(texture);SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_FreeSurface(surface);
    return ok;
}

int main() {
    // A cutout quad: transparent corners, opaque centre, partial edge coverage.
    // Exercise the same MOD textured geometry submission as the unit renderer.
    if (SDL_Init(0)!=0) return 1;
    auto* surface=SDL_CreateRGBSurfaceWithFormat(0,3,1,32,SDL_PIXELFORMAT_RGBA32);
    if (!surface) return 1;
    auto* renderer=SDL_CreateSoftwareRenderer(surface);
    if (!renderer) return 1;
    const std::array<uint8_t,12> rgba={200,0,0,0, 0,200,0,255, 0,0,200,128};
    const auto pixels=tak::shadowMaskPixels(rgba,140);
    auto* texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,3,1);
    if (!renderer || !texture) return 1;
    SDL_UpdateTexture(texture,nullptr,pixels.data(),12);
    SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_MOD);
    SDL_SetRenderDrawColor(renderer,200,160,120,255);
    SDL_RenderClear(renderer);
    const SDL_Color white{255,255,255,255};
    SDL_Vertex quad[4]={{{0,0},white,{0,0}},{{3,0},white,{1,0}},
                        {{3,1},white,{1,1}},{{0,1},white,{0,1}}};
    const int indices[6]={0,1,2,0,2,3};
    if (SDL_RenderGeometry(renderer,texture,quad,4,indices,6)!=0) return 1;
    std::array<uint8_t,12> out{};
    SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,out.data(),12);
    bool ok=true;
    for (int c=0;c<3;++c) {
        const int background=200-40*c;
        ok &= out[c]==background; // outside the silhouette must be untouched
        ok &= std::abs(int(out[4+c])-background*140/255)<=1;
        ok &= out[8+c]>out[4+c] && out[8+c]<out[c];
    }
    ok &= maskedRunBatchParity();
    SDL_DestroyTexture(texture);SDL_DestroyRenderer(renderer);SDL_FreeSurface(surface);SDL_Quit();
    std::puts(ok?"PASS: cutout shadow preserves background and darkens only covered pixels":"FAIL: shadow mask compositing");
    return ok?0:1;
}
