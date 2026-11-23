#include "client/streamcapture_d3d.h"
#ifdef _WIN32
#include "client/gpuvram.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <SDL_system.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <vector>
#endif

namespace tak {
#ifndef _WIN32
struct D3DCapture::Impl {};
D3DCapture::D3DCapture() = default;
D3DCapture::~D3DCapture() = default;
int D3DCapture::get(SDL_Renderer*, int, int, int, int, video::Stream&, const video::StreamCursor&) { return -1; }
void D3DCapture::reset() {}
#else
namespace {
template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }
// The stream's size inside a w x h frame, like the OpenGL capture: never larger
// than the frame, keeping its shape, even.
void fit(int w, int h, int outW, int outH, int& tw, int& th) {
    double scale = 1.0;
    if (outW > 0 && outH > 0) scale = std::min({1.0, double(outW) / w, double(outH) / h});
    tw = std::max(2, int(w * scale) & ~1); th = std::max(2, int(h * scale) & ~1);
    if (tw > w) tw = w;
    if (th > h) th = h;
}
// Rows out of a mapped GPU surface into packed RGBA (the stream's format), with
// opaque alpha. BGRA (B8G8R8A8 / X8R8G8B8) swaps red and blue on the way.
void copyRows(std::vector<uint8_t>& out, const uint8_t* src, size_t pitch, int w, int h, bool bgra) {
    out.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = src + pitch * y;
        uint8_t* d = out.data() + size_t(y) * w * 4;
        if (!bgra) { std::memcpy(d, s, size_t(w) * 4); for (int x = 0; x < w; ++x) d[x*4+3] = 255; continue; }
        for (int x = 0; x < w; ++x) {
            uint32_t p; std::memcpy(&p, s + x * 4, 4);
            p = 0xff000000u | ((p & 0xffu) << 16) | (p & 0xff00u) | ((p >> 16) & 0xffu);
            std::memcpy(d + x * 4, &p, 4);
        }
    }
}
// SDL's Direct3D 11 renderer keeps the window's back buffer bound after drawing.
// A transparent point binds it even if the last draw went to a target texture.
bool bindBackBuffer(SDL_Renderer* ren) {
    if (SDL_GetRenderTarget(ren)) return false;
    Uint8 r, g, b, a; SDL_BlendMode mode;
    SDL_GetRenderDrawColor(ren, &r, &g, &b, &a); SDL_GetRenderDrawBlendMode(ren, &mode);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(ren, 0, 0, 0, 0);
    SDL_RenderDrawPoint(ren, 0, 0);
    SDL_SetRenderDrawColor(ren, r, g, b, a); SDL_SetRenderDrawBlendMode(ren, mode);
    return SDL_RenderFlush(ren) == 0;
}
}

struct D3DCapture::Impl {
    std::vector<uint8_t> pixels;
    // Direct3D 11: back buffer -> (mip chain, level 1 = half size) -> a ring of
    // staging textures mapped only once the GPU is done (MAP_FLAG_DO_NOT_WAIT).
    // The back buffer itself is never held between frames (it would block
    // ResizeBuffers).
    ID3D11Device* device = nullptr; // identity only, not a reference
    ID3D11Texture2D* mip = nullptr; ID3D11ShaderResourceView* mipView = nullptr;
    D3D11_TEXTURE2D_DESC mipDesc{};
    size_t counted = 0; // the mip chain, in gpuvram's total while it exists
    struct Slot { ID3D11Texture2D* texture = nullptr; bool pending = false; uint64_t serial = 0; video::StreamCursor cursor; };
    std::array<Slot, 3> slots{};
    int slotW = 0, slotH = 0; DXGI_FORMAT slotFormat = DXGI_FORMAT_UNKNOWN;
    uint64_t serial = 0;
    // Direct3D 9: back buffer -> StretchRect to the stream size -> system memory.
    // The render target is created and released within each capture: SDL resets
    // the device (resize, fullscreen, VSync) and Reset fails while any default-pool
    // resource it does not know about is alive. The system-memory surface survives.
    IDirect3DDevice9* device9 = nullptr; // identity only
    IDirect3DSurface9* system = nullptr;
    D3DSURFACE_DESC systemDesc{};

    void clear() {
        for (auto& s : slots) { release(s.texture); s = {}; }
        release(mipView); release(mip); mipDesc = {};
        gpuvram::removeBuffer(counted); counted = 0;
        slotW = slotH = 0; slotFormat = DXGI_FORMAT_UNKNOWN; device = nullptr;
        release(system); systemDesc = {}; device9 = nullptr;
    }

    int d3d11(SDL_Renderer* ren, int outW, int outH, video::Stream& stream, const video::StreamCursor& cursor) {
        ID3D11Device* dev = SDL_RenderGetD3D11Device(ren);
        if (!dev) return -1;
        if (dev != device) { clear(); device = dev; }
        ID3D11DeviceContext* ctx = nullptr; dev->GetImmediateContext(&ctx);
        int result = -1;
        ID3D11RenderTargetView* view = nullptr; ID3D11Resource* resource = nullptr;
        if (ctx && bindBackBuffer(ren)) ctx->OMGetRenderTargets(1, &view, nullptr);
        if (view) view->GetResource(&resource);
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        if (resource) resource->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            auto* back = static_cast<ID3D11Texture2D*>(resource);
            D3D11_TEXTURE2D_DESC desc{}; back->GetDesc(&desc);
            const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM;
            if ((bgra || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) && desc.SampleDesc.Count == 1 && desc.ArraySize == 1)
                result = submit11(dev, ctx, back, desc, outW, outH, stream, cursor);
        }
        release(resource); release(view); release(ctx); dev->Release();
        return result;
    }
    int submit11(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* back,
                 const D3D11_TEXTURE2D_DESC& desc, int outW, int outH, video::Stream& stream,
                 const video::StreamCursor& cursor) {
        const int bw = int(desc.Width), bh = int(desc.Height);
        int tw, th; fit(bw, bh, outW, outH, tw, th);
        // Half size or less: let the GPU average 2x2 blocks (mip level 1); the
        // encoder's scaler takes it the rest of the way.
        const bool half = bw >= 4 && bh >= 4 && tw * 2 <= bw && th * 2 <= bh;
        ID3D11Texture2D* source = back; UINT level = 0;
        int cw = bw, ch = bh;
        if (half) {
            if (!mip || mipDesc.Width != desc.Width || mipDesc.Height != desc.Height || mipDesc.Format != desc.Format) {
                release(mipView); release(mip);
                gpuvram::removeBuffer(counted); counted = 0;
                D3D11_TEXTURE2D_DESC m{}; m.Width = desc.Width; m.Height = desc.Height; m.MipLevels = 2; m.ArraySize = 1;
                m.Format = desc.Format; m.SampleDesc.Count = 1; m.Usage = D3D11_USAGE_DEFAULT;
                m.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE; m.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
                if (FAILED(dev->CreateTexture2D(&m, nullptr, &mip)) || FAILED(dev->CreateShaderResourceView(mip, nullptr, &mipView))) {
                    release(mipView); release(mip); mipDesc = {};
                } else { mipDesc = m; counted = size_t(bw) * bh * 5; gpuvram::addBuffer(counted); }
            }
            if (mip) {
                ctx->CopySubresourceRegion(mip, 0, 0, 0, 0, back, 0, nullptr);
                ctx->GenerateMips(mipView);
                source = mip; level = 1; cw = std::max(1, bw >> 1); ch = std::max(1, bh >> 1);
            }
        }
        if (cw != slotW || ch != slotH || desc.Format != slotFormat) {
            for (auto& s : slots) { release(s.texture); s = {}; }
            slotW = cw; slotH = ch; slotFormat = desc.Format;
        }
        // Newest finished readback wins; any older one is superseded.
        bool delivered = false;
        std::array<int, 3> order{0, 1, 2};
        std::sort(order.begin(), order.end(), [&](int a, int b) { return slots[a].serial > slots[b].serial; });
        for (int i : order) {
            auto& s = slots[i];
            if (!s.pending) continue;
            D3D11_MAPPED_SUBRESOURCE map{};
            const HRESULT hr = ctx->Map(s.texture, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
            if (SUCCEEDED(hr)) {
                copyRows(pixels, static_cast<const uint8_t*>(map.pData), map.RowPitch, slotW, slotH, slotFormat != DXGI_FORMAT_R8G8B8A8_UNORM);
                ctx->Unmap(s.texture, 0);
                stream.cursor(s.cursor);   // the pointer as it was when this was copied
                delivered = stream.video(pixels, slotW, slotH);
            }
            for (auto& older : slots) if (older.pending && older.serial <= s.serial) older.pending = false;
            break;
        }
        for (auto& s : slots) {
            if (s.pending) continue;
            if (!s.texture) {
                D3D11_TEXTURE2D_DESC t{}; t.Width = UINT(slotW); t.Height = UINT(slotH); t.MipLevels = 1; t.ArraySize = 1;
                t.Format = slotFormat; t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_STAGING; t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(dev->CreateTexture2D(&t, nullptr, &s.texture))) { s.texture = nullptr; return -1; }
            }
            ctx->CopySubresourceRegion(s.texture, 0, 0, 0, 0, source, level, nullptr);
            s.pending = true; s.serial = ++serial; s.cursor = cursor;
            break;
        }
        return delivered ? 1 : 0;
    }

    int d3d9(SDL_Renderer* ren, int outW, int outH, video::Stream& stream, const video::StreamCursor& cursor) {
        IDirect3DDevice9* dev = SDL_RenderGetD3D9Device(ren);
        if (!dev) return -1;
        if (dev != device9) { release(system); systemDesc = {}; device9 = dev; }
        int result = 0;
        IDirect3DSurface9 *back = nullptr, *target = nullptr;
        D3DSURFACE_DESC desc{};
        // A lost device is SDL's to reset; capture again once it is back.
        if (dev->TestCooperativeLevel() != D3D_OK || SDL_GetRenderTarget(ren) || SDL_RenderFlush(ren) != 0) goto done;
        if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || FAILED(back->GetDesc(&desc)) ||
            (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8) || desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
            result = -1; goto done;
        }
        {
            int tw, th; fit(int(desc.Width), int(desc.Height), outW, outH, tw, th);
            if (!system || systemDesc.Width != UINT(tw) || systemDesc.Height != UINT(th) || systemDesc.Format != desc.Format) {
                release(system);
                if (FAILED(dev->CreateOffscreenPlainSurface(tw, th, desc.Format, D3DPOOL_SYSTEMMEM, &system, nullptr))) { result = -1; goto done; }
                system->GetDesc(&systemDesc);
            }
            IDirect3DSurface9* source = back;
            if (UINT(tw) != desc.Width || UINT(th) != desc.Height) {
                if (FAILED(dev->CreateRenderTarget(tw, th, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr)) ||
                    FAILED(dev->StretchRect(back, nullptr, target, nullptr, D3DTEXF_LINEAR))) { result = -1; goto done; }
                source = target;
            }
            D3DLOCKED_RECT lock{};
            if (FAILED(dev->GetRenderTargetData(source, system)) || FAILED(system->LockRect(&lock, nullptr, D3DLOCK_READONLY))) goto done;
            copyRows(pixels, static_cast<const uint8_t*>(lock.pBits), size_t(lock.Pitch), tw, th, true);
            system->UnlockRect();
            stream.cursor(cursor);
            result = stream.video(pixels, tw, th) ? 1 : 0;
        }
    done:
        release(target); release(back); dev->Release();
        return result;
    }
};
D3DCapture::D3DCapture(): p_(std::make_unique<Impl>()) {}
D3DCapture::~D3DCapture() { p_->clear(); }
void D3DCapture::reset() { p_->clear(); std::vector<uint8_t>().swap(p_->pixels); }
int D3DCapture::get(SDL_Renderer* ren, int, int, int outW, int outH, video::Stream& stream,
                    const video::StreamCursor& cursor) {
    SDL_RendererInfo info{};
    if (SDL_GetRendererInfo(ren, &info) != 0 || !info.name) return -1;
    if (!std::strcmp(info.name, "direct3d11")) return p_->d3d11(ren, outW, outH, stream, cursor);
    if (!std::strcmp(info.name, "direct3d")) return p_->d3d9(ren, outW, outH, stream, cursor);
    return -1;
}
#endif
}
