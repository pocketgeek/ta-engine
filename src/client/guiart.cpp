#include "client/guiart.h"

#include "client/gpuvram.h"
#include "client/artscale.h"
#include "hpi/hpi.h"

namespace ta {

gaf::Palette guiPalette(const hpi::Vfs& vfs, const std::string& gafName) {
    std::string pp = "anims/" + gafName + ".pcx";
    try { return gaf::Palette::fromBytes(vfs.read(pp), pp); } catch (...) {}
    try { return gaf::Palette::fromBytes(vfs.read("palettes/guipal.pal"),
                                         "palettes/guipal.pal"); } catch (...) {}
    return {};
}

SDL_Texture* gafTexture(SDL_Renderer* ren, const hpi::Vfs& vfs, const std::string& gafName,
                        const std::string& seq, int frame, bool keyBlack) {
    if (gafName.empty() || seq.empty()) return nullptr;
    std::string base = gafName;
    if (base.size() >= 4 && base.substr(base.size() - 4) == ".gaf")
        base = base.substr(0, base.size() - 4);
    std::string gp = "anims/" + base + ".gaf";
    try {
        auto pal = guiPalette(vfs, base);
        for (auto& sq : gaf::load(vfs.read(gp), pal, -1, gp)) {
            if (sq.name != seq) continue;
            if (sq.frames.empty()) return nullptr;
            if (frame < 0 || size_t(frame) >= sq.frames.size()) frame = 0;
            auto& f = sq.frames[size_t(frame)];
            if (f.width == 0 || f.height == 0) return nullptr;
            // (texture built below, after the colour-key pass -- see makeTexture)
            // The cut-out is the palette's near-black entry (2,2,2 in loadingbg), not a
            // hard zero -- so key on a small threshold rather than exact black.
            if (keyBlack)
                for (size_t i = 0; i + 3 < f.rgba.size(); i += 4)
                    if (f.rgba[i] <= 8 && f.rgba[i + 1] <= 8 && f.rgba[i + 2] <= 8)
                        f.rgba[i + 3] = 0;
            return ta::art::makeTexture(ren, f.rgba, f.width, f.height);
        }
    } catch (...) {}
    return nullptr;
}

}  // namespace ta
