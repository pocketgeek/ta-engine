// guitool <install-dir> <screen> <out.png> [backdrop.pcx] [--ingame]
// Renders one Total Annihilation .GUI screen (gui/tagui + client/taguiview) with
// SDL's software renderer to a PNG at 640x480, and lists its gadgets. A visual
// check of the GUI runtime against the retail art; no window, no game.
#include "client/taguiview.h"
#include "gui/tagui.h"
#include "hpi/hpi.h"
#include "util/png.h"

#include <SDL.h>

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: guitool <install-dir> <screen> <out.png> [backdrop.pcx] [--ingame]\n");
        return 2;
    }
    bool ingame = false;
    std::string backdrop;
    for (int i = 4; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ingame") == 0) ingame = true;
        else backdrop = argv[i];
    }
    auto vfs = ta::hpi::mountRetailRoot(argv[1]);
    ta::tagui::Screen screen = ta::tagui::load(vfs, argv[2]);
    for (const auto& g : screen.gadgets)
        std::printf("%2d type %2d %-16s %4d,%4d %3dx%-3d attribs 0x%x%s%s\n", g.index, int(g.type), g.name.c_str(),
                    g.x, g.y, g.w, g.h, g.attribs, g.text.empty() ? "" : "  text=", g.text.c_str());
    SDL_SetMainReady();
    SDL_Init(0);
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, 640, 480, 32, SDL_PIXELFORMAT_ABGR8888);
    SDL_Renderer* ren = SDL_CreateSoftwareRenderer(surf);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    {
        ta::TaGuiView view(ren, vfs, std::move(screen), !ingame);
        if (!backdrop.empty()) view.setBackdrop(backdrop);
        view.drawSurface = [&](const ta::tagui::Gadget&, SDL_FRect r) {
            SDL_SetRenderDrawColor(ren, 0, 120, 0, 255);
            SDL_RenderDrawRectF(ren, &r);
        };
        view.draw(ta::GuiLayout(640, 480));
    }
    SDL_RenderPresent(ren);
    std::vector<uint8_t> rgba(size_t(640) * 480 * 4);
    SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ABGR8888, rgba.data(), 640 * 4);
    ta::png::write(argv[3], 640, 480, rgba);
    SDL_DestroyRenderer(ren);
    SDL_FreeSurface(surf);
    SDL_Quit();
    std::printf("wrote %s\n", argv[3]);
    return 0;
}
